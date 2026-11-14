/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

/*
 * MediaTek MT7986 Ethernet frame engine (NETSYS v2).
 *
 * The frame engine has two GMACs behind the GDM1/GDM2 forwarding blocks
 * and one set of DMA engines that both share.  Transmit uses QDMA: a
 * linked list of descriptors, each frame tagged with its forward port,
 * plus a free queue of buffers that QDMA uses internally.  Receive uses
 * PDMA ring 0, and the descriptor tells which GDM the frame came from.
 * Each enabled "mediatek,eth-mac" child gets an ifnet.
 *
 * GMACs connect through the SGMII PCS of SGMIISYS0/1 (syscons, as are the
 * ETHSYS system and reset registers).  Only fixed links in 2500base-x or
 * 1000base-x are supported so far; on the Banana Pi BPI-R3 that is GMAC0,
 * wired to the CPU port of an MT7531 switch.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/endian.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>

#include <machine/atomic.h>
#include <machine/bus.h>

#include <net/bpf.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net/if_var.h>
#include <net/if_vlan_var.h>

#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/tcp.h>

#include <dev/clk/clk.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/syscon/syscon.h>

#include <arm64/mediatek/if_mtkethreg.h>

#include "syscon_if.h"

#define	MTKETH_NMACS		2
#define	MTKETH_TX_RING		1024
#define	MTKETH_TX_MAXSEGS	35	/* a 64 KB TSO frame in clusters */
#define	MTKETH_TX_MAXSIZE	(IP_MAXPACKET + ETHER_HDR_LEN + \
				    ETHER_VLAN_ENCAP_LEN)
#define	MTKETH_TX_MINSEG	16	/* shortest segment TX DMA takes */
#define	MTKETH_FQ_CNT		2048
#define	MTKETH_FQ_PAGE		2048
#define	MTKETH_RX_RING		1024
#define	MTKETH_RX_BUF		MCLBYTES
#define	MTKETH_RX_BUDGET	256
#define	MTKETH_CSUM_FEATURES	(CSUM_IP | CSUM_TCP | CSUM_UDP | \
				    CSUM_TCP_IPV6 | CSUM_UDP_IPV6)

/* Interrupt moderation: RX after 8 frames or 20 us, TX after 16 or 40 us. */
#define	MTKETH_DELAY_INT						\
	(DLY_RX_EN | (8u << DLY_RX_PINT_SHIFT) | (1u << DLY_RX_PTIME_SHIFT) | \
	 DLY_TX_EN | (16u << DLY_TX_PINT_SHIFT) | (2u << DLY_TX_PTIME_SHIFT))

#define	RD4(sc, off)		bus_read_4((sc)->res, (off))
#define	WR4(sc, off, val)	bus_write_4((sc)->res, (off), (val))
#define	MD4(sc, off, clr, set)						\
	WR4((sc), (off), (RD4((sc), (off)) & ~(clr)) | (set))

#define	MTKETH_LOCK(sc)		mtx_lock(&(sc)->mtx)
#define	MTKETH_UNLOCK(sc)	mtx_unlock(&(sc)->mtx)
#define	MTKETH_ASSERT_LOCKED(sc) mtx_assert(&(sc)->mtx, MA_OWNED)

struct mtketh_softc;

/* GDM MIB counters, accumulated from the read-to-clear registers. */
struct mtketh_mib {
	uint64_t		rx_bytes;
	uint64_t		rx_frames;
	uint64_t		rx_overflow;
	uint64_t		rx_fcs_errors;
	uint64_t		rx_short;
	uint64_t		rx_long;
	uint64_t		rx_csum_errors;
	uint64_t		rx_pause;
	uint64_t		tx_skip;
	uint64_t		tx_collisions;
	uint64_t		tx_bytes;
	uint64_t		tx_frames;
};

struct mtketh_mac {
	struct mtketh_softc	*sc;
	int			id;
	if_t			ifp;
	struct ifmedia		media;
	struct syscon		*pcs;
	int			speed;		/* fixed link, Mbit/s */
	bool			pause;
	bool			running;
	bool			link;
	bool			oactive;
	uint8_t			eaddr[ETHER_ADDR_LEN];
	struct mtketh_mib	mib;
};

struct mtketh_buf {
	struct mbuf		*m;
	bus_dmamap_t		map;
};

struct mtketh_softc {
	device_t		dev;
	struct resource		*res;
	struct resource		*irq_res[2];	/* fe1 (TX), fe2 (RX) */
	void			*irq_cookie[2];
	struct mtx		mtx;
	struct callout		tick;
	struct syscon		*ethsys;
	struct mtketh_mac	*mac[MTKETH_NMACS];
	int			nrunning;

	/* QDMA transmit ring and free queue. */
	bus_dma_tag_t		txd_tag;
	bus_dmamap_t		txd_map;
	struct mtketh_txd	*txd;
	bus_addr_t		txd_pa;
	bus_dma_tag_t		txbuf_tag;
	struct mtketh_buf	txbuf[MTKETH_TX_RING];
	int			tx_next;	/* next free descriptor */
	int			tx_last;	/* last released descriptor */
	int			tx_free;
	bus_dma_tag_t		fq_tag;
	bus_dmamap_t		fq_map;
	struct mtketh_txd	*fq;
	bus_addr_t		fq_pa;
	bus_dma_tag_t		fqbuf_tag;
	bus_dmamap_t		fqbuf_map;
	void			*fqbuf;
	bus_addr_t		fqbuf_pa;

	/* PDMA receive ring 0. */
	bus_dma_tag_t		rxd_tag;
	bus_dmamap_t		rxd_map;
	struct mtketh_rxd	*rxd;
	bus_addr_t		rxd_pa;
	bus_dma_tag_t		rxbuf_tag;
	struct mtketh_buf	rxbuf[MTKETH_RX_RING];
	bus_dmamap_t		rx_spare;
	int			rx_idx;		/* last descriptor given back */

	/* Statistics. */
	uint64_t		rx_nombuf;
	uint64_t		rx_badport;
	uint64_t		rx_badlen;
	uint64_t		rx_l4valid;	/* rxd4 bit 24 (MediaTek SDK) */
	uint64_t		rx_l4valid_b2;	/* rxd4 bit 2 (Linux v2) */
	uint64_t		tx_defrag;
	uint64_t		tx_minseg;
	uint64_t		tx_tso;
	uint64_t		tx_full;
	uint32_t		int_status2;
};

static struct ofw_compat_data compat_data[] = {
	{ "mediatek,mt7986-eth",	1 },
	{ NULL,				0 }
};

static void	mtketh_start_locked(struct mtketh_mac *);
static int	mtketh_tx_reclaim(struct mtketh_softc *);

/* ------------------------------------------------------------- MDIO */

static void
mtketh_mdio_init(struct mtketh_softc *sc, phandle_t node)
{
	phandle_t mdio;
	uint32_t freq, div;

	freq = 2500000;
	mdio = ofw_bus_find_child(node, "mdio-bus");
	if (mdio > 0)
		OF_getencprop(mdio, "clock-frequency", &freq, sizeof(freq));
	if (freq == 0)
		freq = 2500000;
	div = MIN(howmany(25000000, freq), 63);
	MD4(sc, MAC_PPSC, PPSC_MDC_CFG_MASK,
	    PPSC_MDC_TURBO | (div << PPSC_MDC_CFG_SHIFT));
}

/* -------------------------------------------------------------- DMA */

static void
mtketh_dmamap_cb(void *arg, bus_dma_segment_t *segs, int nseg, int error)
{
	if (error == 0)
		*(bus_addr_t *)arg = segs[0].ds_addr;
}

static int
mtketh_dma_alloc(struct mtketh_softc *sc, bus_size_t size, bus_size_t align,
    bus_dma_tag_t *tag, bus_dmamap_t *map, void **va, bus_addr_t *pa)
{
	int error;

	error = bus_dma_tag_create(bus_get_dma_tag(sc->dev), align, 0,
	    BUS_SPACE_MAXADDR_32BIT, BUS_SPACE_MAXADDR, NULL, NULL,
	    size, 1, size, 0, NULL, NULL, tag);
	if (error != 0)
		return (error);
	error = bus_dmamem_alloc(*tag, va,
	    BUS_DMA_COHERENT | BUS_DMA_WAITOK | BUS_DMA_ZERO, map);
	if (error != 0)
		return (error);
	return (bus_dmamap_load(*tag, *map, *va, size, mtketh_dmamap_cb, pa,
	    BUS_DMA_NOWAIT));
}

static int
mtketh_newbuf(struct mtketh_softc *sc, int i)
{
	bus_dma_segment_t seg;
	bus_dmamap_t map;
	struct mbuf *m;
	int nsegs;

	m = m_getcl(M_NOWAIT, MT_DATA, M_PKTHDR);
	if (m == NULL)
		return (ENOBUFS);
	m->m_len = m->m_pkthdr.len = MTKETH_RX_BUF;
	if (bus_dmamap_load_mbuf_sg(sc->rxbuf_tag, sc->rx_spare, m, &seg,
	    &nsegs, BUS_DMA_NOWAIT) != 0) {
		m_freem(m);
		return (ENOBUFS);
	}
	if (sc->rxbuf[i].m != NULL) {
		bus_dmamap_sync(sc->rxbuf_tag, sc->rxbuf[i].map,
		    BUS_DMASYNC_POSTREAD);
		bus_dmamap_unload(sc->rxbuf_tag, sc->rxbuf[i].map);
	}
	map = sc->rxbuf[i].map;
	sc->rxbuf[i].map = sc->rx_spare;
	sc->rx_spare = map;
	sc->rxbuf[i].m = m;
	bus_dmamap_sync(sc->rxbuf_tag, sc->rxbuf[i].map, BUS_DMASYNC_PREREAD);
	sc->rxd[i].rxd1 = htole32((uint32_t)seg.ds_addr);
	return (0);
}

static void
mtketh_rx_rearm(struct mtketh_softc *sc, int i)
{
	sc->rxd[i].rxd3 = 0;
	sc->rxd[i].rxd4 = 0;
	sc->rxd[i].rxd2 = htole32(RXD2_PLEN0(MTKETH_RX_BUF));
}

static int
mtketh_dma_init(struct mtketh_softc *sc)
{
	bus_addr_t pa;
	int error, i;

	/* QDMA free queue: descriptors and their buffer pages. */
	error = mtketh_dma_alloc(sc, MTKETH_FQ_CNT * sizeof(struct mtketh_txd),
	    64, &sc->fq_tag, &sc->fq_map, (void **)&sc->fq, &sc->fq_pa);
	if (error != 0)
		return (error);
	error = mtketh_dma_alloc(sc, MTKETH_FQ_CNT * MTKETH_FQ_PAGE, PAGE_SIZE,
	    &sc->fqbuf_tag, &sc->fqbuf_map, &sc->fqbuf, &sc->fqbuf_pa);
	if (error != 0)
		return (error);
	for (i = 0; i < MTKETH_FQ_CNT; i++) {
		pa = sc->fq_pa + ((i + 1) % MTKETH_FQ_CNT) *
		    sizeof(struct mtketh_txd);
		sc->fq[i].txd1 = htole32(sc->fqbuf_pa + i * MTKETH_FQ_PAGE);
		sc->fq[i].txd2 = htole32(pa);
		sc->fq[i].txd3 = htole32(TXD3_PLEN0(MTKETH_FQ_PAGE));
	}
	bus_dmamap_sync(sc->fq_tag, sc->fq_map, BUS_DMASYNC_PREWRITE);

	/* QDMA transmit ring, a circular linked list. */
	error = mtketh_dma_alloc(sc, MTKETH_TX_RING * sizeof(struct mtketh_txd),
	    64, &sc->txd_tag, &sc->txd_map, (void **)&sc->txd, &sc->txd_pa);
	if (error != 0)
		return (error);
	error = bus_dma_tag_create(bus_get_dma_tag(sc->dev), 1, 0,
	    BUS_SPACE_MAXADDR_32BIT, BUS_SPACE_MAXADDR, NULL, NULL,
	    MTKETH_TX_MAXSIZE, MTKETH_TX_MAXSEGS, 0xffff, 0,
	    NULL, NULL, &sc->txbuf_tag);
	if (error != 0)
		return (error);
	for (i = 0; i < MTKETH_TX_RING; i++) {
		pa = sc->txd_pa + ((i + 1) % MTKETH_TX_RING) *
		    sizeof(struct mtketh_txd);
		sc->txd[i].txd2 = htole32(pa);
		sc->txd[i].txd3 = htole32(TXD3_LS0 | TXD3_OWNER_CPU);
		error = bus_dmamap_create(sc->txbuf_tag, 0, &sc->txbuf[i].map);
		if (error != 0)
			return (error);
	}
	bus_dmamap_sync(sc->txd_tag, sc->txd_map, BUS_DMASYNC_PREWRITE);
	sc->tx_next = 0;
	sc->tx_last = MTKETH_TX_RING - 1;
	sc->tx_free = MTKETH_TX_RING - 2;

	/* PDMA receive ring 0. */
	error = mtketh_dma_alloc(sc, MTKETH_RX_RING * sizeof(struct mtketh_rxd),
	    64, &sc->rxd_tag, &sc->rxd_map, (void **)&sc->rxd, &sc->rxd_pa);
	if (error != 0)
		return (error);
	error = bus_dma_tag_create(bus_get_dma_tag(sc->dev), 1, 0,
	    BUS_SPACE_MAXADDR_32BIT, BUS_SPACE_MAXADDR, NULL, NULL,
	    MTKETH_RX_BUF, 1, MTKETH_RX_BUF, 0, NULL, NULL, &sc->rxbuf_tag);
	if (error != 0)
		return (error);
	error = bus_dmamap_create(sc->rxbuf_tag, 0, &sc->rx_spare);
	if (error != 0)
		return (error);
	for (i = 0; i < MTKETH_RX_RING; i++) {
		error = bus_dmamap_create(sc->rxbuf_tag, 0, &sc->rxbuf[i].map);
		if (error != 0)
			return (error);
		if ((error = mtketh_newbuf(sc, i)) != 0)
			return (error);
		mtketh_rx_rearm(sc, i);
	}
	bus_dmamap_sync(sc->rxd_tag, sc->rxd_map, BUS_DMASYNC_PREWRITE);
	sc->rx_idx = MTKETH_RX_RING - 1;
	return (0);
}

static void
mtketh_dma_start(struct mtketh_softc *sc)
{
	int q;

	/* Free queue. */
	WR4(sc, QDMA_FQ_HEAD, sc->fq_pa);
	WR4(sc, QDMA_FQ_TAIL, sc->fq_pa +
	    (MTKETH_FQ_CNT - 1) * sizeof(struct mtketh_txd));
	WR4(sc, QDMA_FQ_CNT, (MTKETH_FQ_CNT << 16) | MTKETH_FQ_CNT);
	WR4(sc, QDMA_FQ_BLEN, MTKETH_FQ_PAGE << 16);

	/* Transmit ring: everything up to the last descriptor is free. */
	WR4(sc, QTX_CTX_PTR, sc->txd_pa);
	WR4(sc, QTX_DTX_PTR, sc->txd_pa);
	WR4(sc, QTX_CRX_PTR, sc->txd_pa +
	    (MTKETH_TX_RING - 1) * sizeof(struct mtketh_txd));
	WR4(sc, QTX_DRX_PTR, sc->txd_pa +
	    (MTKETH_TX_RING - 1) * sizeof(struct mtketh_txd));
	for (q = 0; q < QDMA_NQUEUES; q++) {
		WR4(sc, QTX_CFG(q), QTX_CFG_RES_THRES);
		WR4(sc, QTX_SCH(q), QTX_SCH_DEFAULT);
	}
	WR4(sc, QDMA_TX_SCH_RATE, QDMA_TX_SCH_MAX_WFQ);
	WR4(sc, QDMA_TX_SCH_RATE + 4, QDMA_TX_SCH_MAX_WFQ);
	WR4(sc, QDMA_FC_THRES, QDMA_FC_THRES_DEFAULT);
	WR4(sc, QDMA_HRED, 0);

	/* Receive ring 0: all descriptors belong to the hardware. */
	WR4(sc, PDMA_RX_BASE, sc->rxd_pa);
	WR4(sc, PDMA_RX_MAX_CNT, MTKETH_RX_RING);
	WR4(sc, PDMA_RST_IDX, PDMA_RST_DRX_IDX0);
	WR4(sc, PDMA_RX_CRX_IDX, sc->rx_idx);

	/*
	 * QDMA receive is not used (GDM forwards to PDMA), so only its
	 * transmit side is enabled.
	 */
	MD4(sc, QDMA_GLO_CFG, 0, GLO_TX_DMA_EN | GLO_TX_BT_32DW |
	    GLO_NDP_CO_PRO | GLO_RX_2B_OFFSET | GLO_TX_WB_DDONE |
	    GLO_MULTI_CNT | GLO_RESV_BUF | GLO_WCOMP_EN | GLO_DMAD_WR_WDONE |
	    GLO_CHK_DDONE_EN | GLO_LEAKY_BUCKET_EN);
	WR4(sc, PDMA_GLO_CFG, GLO_RX_DMA_EN | GLO_RX_BT_32DW | GLO_MULTI_EN);
}

static void
mtketh_dma_stop(struct mtketh_softc *sc)
{
	const uint32_t en = GLO_TX_WB_DDONE | GLO_RX_DMA_EN | GLO_TX_DMA_EN;
	const uint32_t busy = GLO_RX_DMA_BUSY | GLO_TX_DMA_BUSY;
	int i;

	MD4(sc, QDMA_GLO_CFG, en, 0);
	MD4(sc, PDMA_GLO_CFG, en, 0);
	for (i = 0; i < 1000; i++) {
		if ((RD4(sc, QDMA_GLO_CFG) & busy) == 0 &&
		    (RD4(sc, PDMA_GLO_CFG) & busy) == 0)
			return;
		DELAY(100);
	}
	device_printf(sc->dev, "DMA does not stop\n");
}

/* --------------------------------------------------------- Transmit */

/* Make the first len bytes contiguous and writable. */
static struct mbuf *
mtketh_pullup(struct mbuf *m, int len)
{
	if (!M_WRITABLE(m))
		return (m_copyup(m, len, 0));
	if (m->m_len < len)
		return (m_pullup(m, len));
	return (m);
}

/*
 * For TSO the GDM takes the MSS from the TCP checksum field, where the
 * stack left the pseudo-header sum; the hardware computes the checksums
 * of the segments itself.
 */
static int
mtketh_tso_setup(struct mbuf **mp)
{
	struct ether_header *eh;
	struct ip *ip;
	struct ip6_hdr *ip6;
	struct tcphdr *th;
	struct mbuf *m;
	int off;

	off = ETHER_HDR_LEN;
	m = mtketh_pullup(*mp, off + sizeof(struct ip6_hdr));
	if (m == NULL)
		goto fail;
	eh = mtod(m, struct ether_header *);
	switch (ntohs(eh->ether_type)) {
	case ETHERTYPE_IP:
		ip = mtodo(m, off);
		if (ip->ip_p != IPPROTO_TCP)
			goto bad;
		off += ip->ip_hl << 2;
		break;
	case ETHERTYPE_IPV6:
		ip6 = mtodo(m, off);
		if (ip6->ip6_nxt != IPPROTO_TCP)
			goto bad;
		off += sizeof(struct ip6_hdr);
		break;
	default:
		goto bad;
	}
	m = mtketh_pullup(m, off + sizeof(struct tcphdr));
	if (m == NULL)
		goto fail;
	th = mtodo(m, off);
	th->th_sum = htons(m->m_pkthdr.tso_segsz);
	*mp = m;
	return (0);
bad:
	m_freem(m);
fail:
	*mp = NULL;
	return (ENOBUFS);
}

/*
 * TX DMA must not be given a segment shorter than MTKETH_TX_MINSEG
 * bytes.  Of the clusters m_defrag() returns only the last one can be
 * short; top it up from the one before.
 */
static struct mbuf *
mtketh_tx_minseg(struct mbuf *m0)
{
	struct mbuf *m, *n, *prev;

	m = m_defrag(m0, M_NOWAIT);
	if (m == NULL) {
		m_freem(m0);
		return (NULL);
	}
	prev = NULL;
	for (n = m; n->m_next != NULL; n = n->m_next)
		prev = n;
	if (prev != NULL && n->m_len < MTKETH_TX_MINSEG &&
	    prev->m_len >= 2 * MTKETH_TX_MINSEG &&
	    M_TRAILINGSPACE(n) >= MTKETH_TX_MINSEG) {
		memmove(mtod(n, char *) + MTKETH_TX_MINSEG, mtod(n, char *),
		    n->m_len);
		memcpy(mtod(n, char *), mtod(prev, char *) + prev->m_len -
		    MTKETH_TX_MINSEG, MTKETH_TX_MINSEG);
		prev->m_len -= MTKETH_TX_MINSEG;
		n->m_len += MTKETH_TX_MINSEG;
	}
	return (m);
}

static int
mtketh_encap(struct mtketh_mac *mac, struct mbuf **mp)
{
	struct mtketh_softc *sc = mac->sc;
	bus_dma_segment_t segs[MTKETH_TX_MAXSEGS];
	bus_dmamap_t map;
	struct mbuf *m;
	uint32_t txd5, txd6;
	int error, first, i, last, nsegs, s;

	MTKETH_ASSERT_LOCKED(sc);
	if (((*mp)->m_pkthdr.csum_flags & CSUM_TSO) != 0 &&
	    (error = mtketh_tso_setup(mp)) != 0)
		return (error);
	first = sc->tx_next;
	map = sc->txbuf[first].map;
	error = bus_dmamap_load_mbuf_sg(sc->txbuf_tag, map, *mp, segs, &nsegs,
	    BUS_DMA_NOWAIT);
	if (error == EFBIG) {
		m = m_collapse(*mp, M_NOWAIT, MTKETH_TX_MAXSEGS);
		if (m == NULL) {
			m_freem(*mp);
			*mp = NULL;
			return (ENOBUFS);
		}
		*mp = m;
		sc->tx_defrag++;
		error = bus_dmamap_load_mbuf_sg(sc->txbuf_tag, map, *mp, segs,
		    &nsegs, BUS_DMA_NOWAIT);
	}
	if (error != 0) {
		m_freem(*mp);
		*mp = NULL;
		return (error);
	}
	for (s = 0; s < nsegs; s++) {
		if (segs[s].ds_len < MTKETH_TX_MINSEG)
			break;
	}
	if (s < nsegs && nsegs > 1) {
		bus_dmamap_unload(sc->txbuf_tag, map);
		*mp = mtketh_tx_minseg(*mp);
		if (*mp == NULL)
			return (ENOBUFS);
		sc->tx_minseg++;
		error = bus_dmamap_load_mbuf_sg(sc->txbuf_tag, map, *mp, segs,
		    &nsegs, BUS_DMA_NOWAIT);
		if (error != 0) {
			m_freem(*mp);
			*mp = NULL;
			return (error);
		}
	}
	if (nsegs > sc->tx_free) {
		bus_dmamap_unload(sc->txbuf_tag, map);
		return (ENOBUFS);
	}
	bus_dmamap_sync(sc->txbuf_tag, map, BUS_DMASYNC_PREWRITE);

	txd5 = 0;
	if (((*mp)->m_pkthdr.csum_flags & (MTKETH_CSUM_FEATURES |
	    CSUM_TSO)) != 0)
		txd5 = TXD5_CSUM;
	if (((*mp)->m_pkthdr.csum_flags & CSUM_TSO) != 0) {
		txd5 |= TXD5_TSO;
		sc->tx_tso++;
	}
	txd6 = 0;
	if (((*mp)->m_flags & M_VLANTAG) != 0)
		txd6 = TXD6_INS_VLAN | (*mp)->m_pkthdr.ether_vtag;
	i = first;
	last = first;
	for (s = 0; s < nsegs; s++) {
		struct mtketh_txd *d = &sc->txd[i];

		d->txd1 = htole32((uint32_t)segs[s].ds_addr);
		d->txd4 = htole32(TXD4_SWC | TXD4_QID(mac->id) |
		    TXD4_FPORT(mac->id + 1));
		/* Checksums, TSO and the VLAN tag apply to the whole frame. */
		d->txd5 = htole32(s == 0 ? txd5 : 0);
		d->txd6 = htole32(s == 0 ? txd6 : 0);
		d->txd7 = 0;
		d->txd8 = 0;
		/* Clearing the owner bit hands the descriptor over. */
		d->txd3 = htole32(TXD3_PLEN0(segs[s].ds_len) |
		    (s == nsegs - 1 ? TXD3_LS0 : 0));
		last = i;
		i = (i + 1) % MTKETH_TX_RING;
	}
	/* The mbuf and its loaded map live with the last descriptor. */
	sc->txbuf[first].map = sc->txbuf[last].map;
	sc->txbuf[last].map = map;
	sc->txbuf[last].m = *mp;
	sc->tx_next = i;
	sc->tx_free -= nsegs;
	return (0);
}

static void
mtketh_start_locked(struct mtketh_mac *mac)
{
	struct mtketh_softc *sc = mac->sc;
	if_t ifp = mac->ifp;
	struct mbuf *m;
	int queued;

	MTKETH_ASSERT_LOCKED(sc);
	if ((if_getdrvflags(ifp) & (IFF_DRV_RUNNING | IFF_DRV_OACTIVE)) !=
	    IFF_DRV_RUNNING || !mac->link)
		return;
	if (sc->tx_free < MTKETH_TX_RING / 4)
		mtketh_tx_reclaim(sc);
	queued = 0;
	while (!if_sendq_empty(ifp)) {
		m = if_dequeue(ifp);
		if (m == NULL)
			break;
		if (mtketh_encap(mac, &m) != 0) {
			if (m != NULL) {
				if_sendq_prepend(ifp, m);
				if_setdrvflagbits(ifp, IFF_DRV_OACTIVE, 0);
				mac->oactive = true;
				sc->tx_full++;
			} else
				if_inc_counter(ifp, IFCOUNTER_OERRORS, 1);
			break;
		}
		if_inc_counter(ifp, IFCOUNTER_OPACKETS, 1);
		BPF_MTAP(ifp, m);
		queued++;
	}
	if (queued > 0) {
		bus_dmamap_sync(sc->txd_tag, sc->txd_map, BUS_DMASYNC_PREWRITE);
		wmb();
		WR4(sc, QTX_CTX_PTR, sc->txd_pa +
		    sc->tx_next * sizeof(struct mtketh_txd));
	}
}

static void
mtketh_start(if_t ifp)
{
	struct mtketh_mac *mac = if_getsoftc(ifp);

	MTKETH_LOCK(mac->sc);
	mtketh_start_locked(mac);
	MTKETH_UNLOCK(mac->sc);
}

/*
 * Release the descriptors QDMA is done with: from the TX done interrupt,
 * and from the transmit path when the ring runs low.  The QDMA delay
 * settings do not moderate this interrupt (measured), and taking it only
 * while the ring is full costs more CPU than it saves.
 */
static int
mtketh_tx_reclaim(struct mtketh_softc *sc)
{
	struct mtketh_buf *b;
	uint32_t dma;
	int cpu, freed, next;

	MTKETH_ASSERT_LOCKED(sc);
	bus_dmamap_sync(sc->txd_tag, sc->txd_map, BUS_DMASYNC_POSTREAD);
	dma = (RD4(sc, QTX_DRX_PTR) - sc->txd_pa) / sizeof(struct mtketh_txd);
	if (dma >= MTKETH_TX_RING)
		return (0);
	cpu = sc->tx_last;
	freed = 0;
	while (cpu != (int)dma) {
		next = (cpu + 1) % MTKETH_TX_RING;
		if ((le32toh(sc->txd[next].txd3) & TXD3_OWNER_CPU) == 0)
			break;
		b = &sc->txbuf[next];
		if (b->m != NULL) {
			bus_dmamap_sync(sc->txbuf_tag, b->map,
			    BUS_DMASYNC_POSTWRITE);
			bus_dmamap_unload(sc->txbuf_tag, b->map);
			m_freem(b->m);
			b->m = NULL;
		}
		sc->tx_free++;
		freed++;
		cpu = next;
	}
	if (freed == 0)
		return (0);
	sc->tx_last = cpu;
	WR4(sc, QTX_CRX_PTR, sc->txd_pa + cpu * sizeof(struct mtketh_txd));
	return (freed);
}

static void
mtketh_txeof(struct mtketh_softc *sc)
{
	struct mtketh_mac *mac;
	int i;

	MTKETH_ASSERT_LOCKED(sc);
	if (mtketh_tx_reclaim(sc) == 0)
		return;
	for (i = 0; i < MTKETH_NMACS; i++) {
		mac = sc->mac[i];
		if (mac == NULL || !mac->running)
			continue;
		if (mac->oactive) {
			mac->oactive = false;
			if_setdrvflagbits(mac->ifp, 0, IFF_DRV_OACTIVE);
		}
		mtketh_start_locked(mac);
	}
}

static void
mtketh_intr_tx(void *arg)
{
	struct mtketh_softc *sc = arg;
	uint32_t status;

	MTKETH_LOCK(sc);
	status = RD4(sc, QDMA_INT_STATUS);
	WR4(sc, QDMA_INT_STATUS, status & INT_TX_DONE_DLY);
	if ((status & INT_TX_DONE_DLY) != 0)
		mtketh_txeof(sc);
	MTKETH_UNLOCK(sc);
}

/* ---------------------------------------------------------- Receive */

/*
 * Take up to MTKETH_RX_BUDGET frames off the ring.  The frames are handed
 * back per GMAC so that if_input() runs without the driver lock.
 */
static int
mtketh_rxeof(struct mtketh_softc *sc, struct mbuf **heads)
{
	struct mbuf *tails[MTKETH_NMACS];
	struct mtketh_mac *mac;
	struct mbuf *m;
	uint32_t rxd2, rxd3, rxd4;
	int done, i, len, port;

	MTKETH_ASSERT_LOCKED(sc);
	for (i = 0; i < MTKETH_NMACS; i++)
		heads[i] = tails[i] = NULL;
	done = 0;
	bus_dmamap_sync(sc->rxd_tag, sc->rxd_map, BUS_DMASYNC_POSTREAD);
	while (done < MTKETH_RX_BUDGET) {
		i = (sc->rx_idx + 1) % MTKETH_RX_RING;
		rxd2 = le32toh(sc->rxd[i].rxd2);
		if ((rxd2 & RXD2_DONE) == 0)
			break;
		rmb();
		rxd3 = le32toh(sc->rxd[i].rxd3);
		rxd4 = le32toh(sc->rxd[i].rxd4);
		len = RXD2_GET_PLEN0(rxd2);
		port = RXD4_SPORT(rxd4);
		mac = (port >= 1 && port <= MTKETH_NMACS) ?
		    sc->mac[port - 1] : NULL;
		if (mac == NULL || !mac->running ||
		    (rxd4 & RXD4_SPECIAL_TAG) != 0) {
			sc->rx_badport++;
		} else if (len < ETHER_HDR_LEN || len > MTKETH_RX_BUF ||
		    (rxd2 & RXD2_LS0) == 0) {
			sc->rx_badlen++;
			if_inc_counter(mac->ifp, IFCOUNTER_IERRORS, 1);
		} else {
			m = sc->rxbuf[i].m;
			if (mtketh_newbuf(sc, i) != 0) {
				sc->rx_nombuf++;
				if_inc_counter(mac->ifp, IFCOUNTER_IQDROPS, 1);
			} else {
				m->m_pkthdr.rcvif = mac->ifp;
				m->m_pkthdr.len = m->m_len = len;
				if ((rxd4 & RXD4_L4_VALID) != 0)
					sc->rx_l4valid++;
				if ((rxd4 & (1u << 2)) != 0)
					sc->rx_l4valid_b2++;
				/* GDM checked the TCP/UDP checksum. */
				if ((rxd4 & RXD4_L4_VALID) != 0 &&
				    (if_getcapenable(mac->ifp) &
				    (IFCAP_RXCSUM | IFCAP_RXCSUM_IPV6)) != 0) {
					m->m_pkthdr.csum_flags =
					    CSUM_DATA_VALID | CSUM_PSEUDO_HDR;
					m->m_pkthdr.csum_data = 0xffff;
				}
				if ((rxd2 & RXD2_VTAG) != 0) {
					m->m_pkthdr.ether_vtag = RXD3_TCI(rxd3);
					m->m_flags |= M_VLANTAG;
				}
				if_inc_counter(mac->ifp, IFCOUNTER_IPACKETS, 1);
				if (tails[mac->id] == NULL)
					heads[mac->id] = m;
				else
					tails[mac->id]->m_nextpkt = m;
				tails[mac->id] = m;
			}
		}
		mtketh_rx_rearm(sc, i);
		sc->rx_idx = i;
		done++;
	}
	if (done > 0) {
		bus_dmamap_sync(sc->rxd_tag, sc->rxd_map, BUS_DMASYNC_PREWRITE);
		wmb();
		WR4(sc, PDMA_RX_CRX_IDX, sc->rx_idx);
	}
	return (done);
}

static void
mtketh_intr_rx(void *arg)
{
	struct mtketh_softc *sc = arg;
	struct mbuf *heads[MTKETH_NMACS];
	struct mbuf *m;
	int done, i;

	/* Loop until the ring is empty: the status bit does not re-arm. */
	do {
		MTKETH_LOCK(sc);
		WR4(sc, PDMA_INT_STATUS, INT_RX_DONE_DLY);
		done = mtketh_rxeof(sc, heads);
		MTKETH_UNLOCK(sc);
		for (i = 0; i < MTKETH_NMACS; i++) {
			while ((m = heads[i]) != NULL) {
				heads[i] = m->m_nextpkt;
				m->m_nextpkt = NULL;
				if_input(m->m_pkthdr.rcvif, m);
			}
		}
	} while (done == MTKETH_RX_BUDGET);
}

/* ------------------------------------------------- PCS, MAC, links */

/*
 * Configure an SGMII PCS for 2500base-x or 1000base-x without in-band
 * negotiation.  The PHYA is powered down while the PCS is reset and
 * reconfigured, then the whole power register is cleared: clearing only
 * the power-down bit can leave the link up but passing no traffic.
 */
static void
mtketh_pcs_config(struct mtketh_mac *mac)
{
	struct syscon *pcs = mac->pcs;

	SYSCON_MODIFY_4(pcs, PCS_QPHY_PWR_STATE, 0, PCS_PHYA_PWD);
	SYSCON_MODIFY_4(pcs, PCS_RESERVED_0, 0, PCS_SW_RESET);
	SYSCON_MODIFY_4(pcs, PCS_ANA_RGC3, RGC3_SPEED_MASK,
	    mac->speed == 2500 ? RGC3_SPEED_3125M : RGC3_SPEED_1250M);
	SYSCON_WRITE_4(pcs, PCS_LINK_TIMER, 10000000 / 16);	/* 10 ms */
	SYSCON_MODIFY_4(pcs, PCS_SGMII_MODE, SGMII_IF_MODE_SGMII |
	    SGMII_SPEED_DUPLEX_AN | SGMII_REMOTE_FAULT_DIS, 0);
	SYSCON_MODIFY_4(pcs, PCS_CONTROL_1, PCS_BMCR_ANENABLE, 0);
	DELAY(100);
	SYSCON_WRITE_4(pcs, PCS_QPHY_PWR_STATE, 0);
	if ((SYSCON_READ_4(pcs, PCS_RESERVED_0) & PCS_SW_RESET) != 0) {
		/* Not documented to clear by itself. */
		SYSCON_MODIFY_4(pcs, PCS_RESERVED_0, PCS_SW_RESET, 0);
		if (bootverbose)
			device_printf(mac->sc->dev,
			    "GMAC%d: PCS reset released by software\n",
			    mac->id);
	}
}

static uint32_t
mtketh_mcr(struct mtketh_mac *mac, bool up)
{
	uint32_t mcr;

	mcr = MCR_MAX_RX_2048 | MCR_IPG_CFG | MCR_FORCE_MODE |
	    MCR_RX_FIFO_CLR_DIS | MCR_BACKOFF_EN | MCR_BACKPR_EN |
	    MCR_SPEED_1000 | MCR_FORCE_DPX;	/* 2.5G uses the 1000 code */
	if (mac->speed == 100)
		mcr ^= MCR_SPEED_1000 | MCR_SPEED_100;
	if (mac->pause)
		mcr |= MCR_FORCE_TX_FC | MCR_FORCE_RX_FC;
	if (up)
		mcr |= MCR_TX_EN | MCR_RX_EN | MCR_FORCE_LINK;
	return (mcr);
}

static void
mtketh_mac_config(struct mtketh_mac *mac)
{
	struct mtketh_softc *sc = mac->sc;
	int id = mac->id;

	/* Detach the GMAC from its SGMII path while the PCS changes. */
	SYSCON_MODIFY_4(sc->ethsys, ETHSYS_SYSCFG0, SYSCFG0_SGMII_SEL(id) |
	    SYSCFG0_GE_MODE_MASK(id), 0);
	mtketh_pcs_config(mac);
	SYSCON_MODIFY_4(sc->ethsys, ETHSYS_SYSCFG0, 0, SYSCFG0_SGMII_SEL(id));
	WR4(sc, MAC_MCR(id), mtketh_mcr(mac, false));
	WR4(sc, GDM_MAC_MSB(id), mac->eaddr[0] << 8 | mac->eaddr[1]);
	WR4(sc, GDM_MAC_LSB(id), mac->eaddr[2] << 24 | mac->eaddr[3] << 16 |
	    mac->eaddr[4] << 8 | mac->eaddr[5]);
}

static void
mtketh_gdm_forward(struct mtketh_softc *sc, int id, bool on)
{
	struct mtketh_mac *mac = sc->mac[id];
	uint32_t csum;

	/*
	 * With the checks on, the GDM marks good TCP/UDP checksums in rxd4
	 * and drops frames with a bad IP, TCP or UDP checksum (MIB_RX_CSUM).
	 * Without them the stack sees and verifies every frame.
	 */
	csum = 0;
	if (mac != NULL && (if_getcapenable(mac->ifp) &
	    (IFCAP_RXCSUM | IFCAP_RXCSUM_IPV6)) != 0)
		csum = GDM_ICS_EN | GDM_TCS_EN | GDM_UCS_EN;
	MD4(sc, GDM_FWD_CFG(id), GDM_DST_MASK | GDM_ICS_EN | GDM_TCS_EN |
	    GDM_UCS_EN, csum | GDM_STRP_CRC |
	    (on ? GDM_DST_PDMA : GDM_DST_DROP));
}

/*
 * Receive VLAN stripping is one switch for both GMACs; it is on while
 * either interface has VLAN_HWTAGGING enabled.
 */
static void
mtketh_vlan_strip(struct mtketh_softc *sc)
{
	uint32_t val;
	int i;

	val = 0;
	for (i = 0; i < MTKETH_NMACS; i++) {
		if (sc->mac[i] != NULL && (if_getcapenable(sc->mac[i]->ifp) &
		    IFCAP_VLAN_HWTAGGING) != 0)
			val = CDMP_EG_VLAN_STRIP;
	}
	WR4(sc, CDMP_EG_CTRL, val);
}

static void
mtketh_mib_update(struct mtketh_mac *mac)
{
	struct mtketh_softc *sc = mac->sc;
	struct mtketh_mib *mib = &mac->mib;
	bus_size_t base = GDM_MIB(mac->id);

	MTKETH_ASSERT_LOCKED(sc);
	mib->rx_bytes += RD4(sc, base + MIB_RX_BYTES);
	mib->rx_bytes += (uint64_t)RD4(sc, base + MIB_RX_BYTES + 4) << 32;
	mib->rx_frames += RD4(sc, base + MIB_RX_FRAMES);
	mib->rx_overflow += RD4(sc, base + MIB_RX_OVERFLOW);
	mib->rx_fcs_errors += RD4(sc, base + MIB_RX_FCS);
	mib->rx_short += RD4(sc, base + MIB_RX_SHORT);
	mib->rx_long += RD4(sc, base + MIB_RX_LONG);
	mib->rx_csum_errors += RD4(sc, base + MIB_RX_CSUM);
	mib->rx_pause += RD4(sc, base + MIB_RX_PAUSE);
	mib->tx_skip += RD4(sc, base + MIB_TX_SKIP);
	mib->tx_collisions += RD4(sc, base + MIB_TX_COLL);
	mib->tx_bytes += RD4(sc, base + MIB_TX_BYTES);
	mib->tx_bytes += (uint64_t)RD4(sc, base + MIB_TX_BYTES + 4) << 32;
	mib->tx_frames += RD4(sc, base + MIB_TX_FRAMES);
}

static void
mtketh_update_link(struct mtketh_mac *mac)
{
	struct mtketh_softc *sc = mac->sc;
	bool link;

	MTKETH_ASSERT_LOCKED(sc);
	link = mac->running &&
	    (SYSCON_READ_4(mac->pcs, PCS_CONTROL_1) & PCS_BMSR_LINK) != 0 &&
	    (RD4(sc, MAC_MSR(mac->id)) & MSR_LINK) != 0;
	if (link == mac->link)
		return;
	mac->link = link;
	if_link_state_change(mac->ifp, link ? LINK_STATE_UP : LINK_STATE_DOWN);
	if (link)
		mtketh_start_locked(mac);
}

static void
mtketh_tick(void *arg)
{
	struct mtketh_softc *sc = arg;
	uint32_t status2;
	int i;

	MTKETH_ASSERT_LOCKED(sc);
	status2 = RD4(sc, FE_INT_STATUS2);
	WR4(sc, FE_INT_STATUS2, status2);
	sc->int_status2 |= status2;
	mtketh_txeof(sc);
	for (i = 0; i < MTKETH_NMACS; i++) {
		if (sc->mac[i] != NULL) {
			mtketh_mib_update(sc->mac[i]);
			mtketh_update_link(sc->mac[i]);
		}
	}
	callout_reset(&sc->tick, hz, mtketh_tick, sc);
}

/* ------------------------------------------------------------ ifnet */

static void
mtketh_init_locked(struct mtketh_mac *mac)
{
	struct mtketh_softc *sc = mac->sc;

	MTKETH_ASSERT_LOCKED(sc);
	if (mac->running)
		return;
	WR4(sc, MAC_MCR(mac->id), mtketh_mcr(mac, true));
	mtketh_gdm_forward(sc, mac->id, true);
	mac->running = true;
	if (sc->nrunning++ == 0)
		callout_reset(&sc->tick, hz, mtketh_tick, sc);
	if_setdrvflagbits(mac->ifp, IFF_DRV_RUNNING, IFF_DRV_OACTIVE);
	DELAY(1000);	/* forced links come up at once */
	mtketh_update_link(mac);
}

static void
mtketh_init(void *arg)
{
	struct mtketh_mac *mac = arg;

	MTKETH_LOCK(mac->sc);
	mtketh_init_locked(mac);
	MTKETH_UNLOCK(mac->sc);
}

static void
mtketh_stop_locked(struct mtketh_mac *mac)
{
	struct mtketh_softc *sc = mac->sc;

	MTKETH_ASSERT_LOCKED(sc);
	if (!mac->running)
		return;
	mtketh_gdm_forward(sc, mac->id, false);
	WR4(sc, MAC_MCR(mac->id), mtketh_mcr(mac, false));
	mtketh_mib_update(mac);		/* the tick may stop below */
	mac->running = false;
	if (mac->link) {
		mac->link = false;
		if_link_state_change(mac->ifp, LINK_STATE_DOWN);
	}
	if (--sc->nrunning == 0)
		callout_stop(&sc->tick);
	if_setdrvflagbits(mac->ifp, 0, IFF_DRV_RUNNING | IFF_DRV_OACTIVE);
}

static int
mtketh_media_change(if_t ifp)
{
	/* Fixed links. */
	return (0);
}

static void
mtketh_media_status(if_t ifp, struct ifmediareq *ifmr)
{
	struct mtketh_mac *mac = if_getsoftc(ifp);

	MTKETH_LOCK(mac->sc);
	ifmr->ifm_status = IFM_AVALID | (mac->link ? IFM_ACTIVE : 0);
	ifmr->ifm_active = mac->media.ifm_cur->ifm_media;
	MTKETH_UNLOCK(mac->sc);
}

static uint64_t
mtketh_get_counter(if_t ifp, ift_counter cnt)
{
	struct mtketh_mac *mac = if_getsoftc(ifp);
	struct mtketh_mib *mib = &mac->mib;
	uint64_t val;

	/* Add the frames the GDM handled without the host seeing them. */
	MTKETH_LOCK(mac->sc);
	switch (cnt) {
	case IFCOUNTER_IERRORS:
		val = mib->rx_fcs_errors + mib->rx_short + mib->rx_long +
		    mib->rx_csum_errors;
		break;
	case IFCOUNTER_IQDROPS:
		val = mib->rx_overflow;
		break;
	case IFCOUNTER_OERRORS:
		val = mib->tx_skip;
		break;
	case IFCOUNTER_COLLISIONS:
		val = mib->tx_collisions;
		break;
	default:
		val = 0;
		break;
	}
	MTKETH_UNLOCK(mac->sc);
	return (val + if_get_counter_default(ifp, cnt));
}

static int
mtketh_ioctl(if_t ifp, u_long cmd, caddr_t data)
{
	struct mtketh_mac *mac = if_getsoftc(ifp);
	struct mtketh_softc *sc = mac->sc;
	struct ifreq *ifr = (struct ifreq *)data;
	int error = 0, mask;

	switch (cmd) {
	case SIOCSIFFLAGS:
		MTKETH_LOCK(sc);
		if ((if_getflags(ifp) & IFF_UP) != 0)
			mtketh_init_locked(mac);
		else
			mtketh_stop_locked(mac);
		MTKETH_UNLOCK(sc);
		break;
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		/* GDM forwards every frame to the host. */
		break;
	case SIOCSIFMEDIA:
	case SIOCGIFMEDIA:
	case SIOCGIFXMEDIA:
		error = ifmedia_ioctl(ifp, ifr, &mac->media, cmd);
		break;
	case SIOCSIFCAP:
		mask = (ifr->ifr_reqcap ^ if_getcapenable(ifp)) &
		    (IFCAP_TXCSUM | IFCAP_TXCSUM_IPV6 | IFCAP_RXCSUM |
		    IFCAP_RXCSUM_IPV6 | IFCAP_TSO4 | IFCAP_TSO6 |
		    IFCAP_VLAN_HWTAGGING | IFCAP_VLAN_HWCSUM |
		    IFCAP_VLAN_HWTSO);
		if_togglecapenable(ifp, mask);
		/* TSO relies on the checksum insertion. */
		if ((if_getcapenable(ifp) & IFCAP_TXCSUM) == 0)
			if_setcapenablebit(ifp, 0, IFCAP_TSO4);
		if ((if_getcapenable(ifp) & IFCAP_TXCSUM_IPV6) == 0)
			if_setcapenablebit(ifp, 0, IFCAP_TSO6);
		/*
		 * vlan(4) uses VLAN_HWTSO even without VLAN_HWTAGGING, which
		 * would hand us TSO frames with the tag in the data.
		 */
		if ((if_getcapenable(ifp) & IFCAP_VLAN_HWTAGGING) == 0)
			if_setcapenablebit(ifp, 0, IFCAP_VLAN_HWTSO);
		if_sethwassistbits(ifp, 0, MTKETH_CSUM_FEATURES | CSUM_TSO);
		if ((if_getcapenable(ifp) & IFCAP_TXCSUM) != 0)
			if_sethwassistbits(ifp,
			    CSUM_IP | CSUM_TCP | CSUM_UDP, 0);
		if ((if_getcapenable(ifp) & IFCAP_TXCSUM_IPV6) != 0)
			if_sethwassistbits(ifp,
			    CSUM_TCP_IPV6 | CSUM_UDP_IPV6, 0);
		if ((if_getcapenable(ifp) & IFCAP_TSO4) != 0)
			if_sethwassistbits(ifp, CSUM_IP_TSO, 0);
		if ((if_getcapenable(ifp) & IFCAP_TSO6) != 0)
			if_sethwassistbits(ifp, CSUM_IP6_TSO, 0);
		if ((mask & (IFCAP_RXCSUM | IFCAP_RXCSUM_IPV6)) != 0) {
			MTKETH_LOCK(sc);
			if (mac->running)
				mtketh_gdm_forward(sc, mac->id, true);
			MTKETH_UNLOCK(sc);
		}
		if ((mask & IFCAP_VLAN_HWTAGGING) != 0) {
			MTKETH_LOCK(sc);
			mtketh_vlan_strip(sc);
			MTKETH_UNLOCK(sc);
		}
		VLAN_CAPABILITIES(ifp);
		break;
	default:
		error = ether_ioctl(ifp, cmd, data);
		break;
	}
	return (error);
}

/* ----------------------------------------------------------- Attach */

static int
mtketh_mac_attach(struct mtketh_softc *sc, phandle_t node, int id)
{
	struct mtketh_mac *mac;
	phandle_t fixed;
	char *mode;
	uint32_t speed;
	if_t ifp;
	int media;

	if (OF_getprop_alloc(node, "phy-mode", (void **)&mode) <= 0) {
		device_printf(sc->dev, "GMAC%d: no phy-mode\n", id);
		return (ENXIO);
	}
	fixed = ofw_bus_find_child(node, "fixed-link");
	if (fixed <= 0 || (strcmp(mode, "2500base-x") != 0 &&
	    strcmp(mode, "1000base-x") != 0)) {
		device_printf(sc->dev, "GMAC%d: %s without fixed-link not "
		    "supported yet\n", id, mode);
		OF_prop_free(mode);
		return (ENXIO);
	}
	speed = strcmp(mode, "2500base-x") == 0 ? 2500 : 1000;
	OF_prop_free(mode);
	OF_getencprop(fixed, "speed", &speed, sizeof(speed));
	if ((speed != 2500 && speed != 1000) ||
	    !OF_hasprop(fixed, "full-duplex")) {
		device_printf(sc->dev, "GMAC%d: unsupported fixed-link\n", id);
		return (ENXIO);
	}
	if (id >= MTKETH_NMACS || sc->mac[id] != NULL)
		return (EINVAL);

	mac = malloc(sizeof(*mac), M_DEVBUF, M_WAITOK | M_ZERO);
	mac->sc = sc;
	mac->id = id;
	mac->speed = speed;
	mac->pause = OF_hasprop(fixed, "pause");
	ifp = mac->ifp = if_alloc(IFT_ETHER);
	if_setsoftc(ifp, mac);
	if_initname(ifp, device_get_name(sc->dev), id);
	if_setflags(ifp, IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST);
	if_setioctlfn(ifp, mtketh_ioctl);
	if_setstartfn(ifp, mtketh_start);
	if_setinitfn(ifp, mtketh_init);
	if_setgetcounterfn(ifp, mtketh_get_counter);
	if_setsendqlen(ifp, MTKETH_TX_RING - 1);
	if_setsendqready(ifp);
	if_setcapabilities(ifp, IFCAP_VLAN_MTU | IFCAP_VLAN_HWTAGGING |
	    IFCAP_VLAN_HWCSUM | IFCAP_VLAN_HWTSO | IFCAP_TXCSUM |
	    IFCAP_TXCSUM_IPV6 | IFCAP_RXCSUM | IFCAP_RXCSUM_IPV6 |
	    IFCAP_TSO4 | IFCAP_TSO6);
	if_setcapenable(ifp, if_getcapabilities(ifp));
	if_sethwassist(ifp, MTKETH_CSUM_FEATURES | CSUM_TSO);
	if_sethwtsomax(ifp, IP_MAXPACKET);
	if_sethwtsomaxsegcount(ifp, MTKETH_TX_MAXSEGS);
	if_sethwtsomaxsegsize(ifp, 0xffff);	/* TXD3_PLEN0 */
	media = IFM_ETHER | IFM_FDX | (speed == 2500 ? IFM_2500_X :
	    IFM_1000_SX);
	if (mac->pause)
		media |= IFM_ETH_RXPAUSE | IFM_ETH_TXPAUSE;
	ifmedia_init(&mac->media, 0, mtketh_media_change, mtketh_media_status);
	ifmedia_add(&mac->media, media, 0, NULL);
	ifmedia_set(&mac->media, media);
	if (OF_getprop(node, "mac-address", mac->eaddr, ETHER_ADDR_LEN) !=
	    ETHER_ADDR_LEN && OF_getprop(node, "local-mac-address",
	    mac->eaddr, ETHER_ADDR_LEN) != ETHER_ADDR_LEN)
		ether_gen_addr(ifp, (struct ether_addr *)mac->eaddr);
	sc->mac[id] = mac;
	return (0);
}

static int
mtketh_sgmiisys(struct mtketh_softc *sc, phandle_t node, int id,
    struct syscon **pcs)
{
	pcell_t *cells;
	int n, rv;

	n = OF_getencprop_alloc_multi(node, "mediatek,sgmiisys",
	    sizeof(*cells), (void **)&cells);
	if (n <= id) {
		OF_prop_free(cells);
		return (ENXIO);
	}
	rv = syscon_get_by_ofw_node(sc->dev, OF_node_from_xref(cells[id]),
	    pcs);
	OF_prop_free(cells);
	return (rv);
}

static int
mtketh_enable_clocks(struct mtketh_softc *sc, phandle_t node)
{
	clk_t clk;
	int error, i;

	error = clk_set_assigned(sc->dev, node);
	if (error != 0 && error != ENOENT) {
		device_printf(sc->dev, "cannot set assigned clocks\n");
		return (error);
	}
	for (i = 0; clk_get_by_ofw_index(sc->dev, node, i, &clk) == 0; i++) {
		error = clk_enable(clk);
		if (error != 0) {
			device_printf(sc->dev, "cannot enable clock %s\n",
			    clk_get_name(clk));
			return (error);
		}
	}
	return (0);
}

static void
mtketh_fe_reset(struct mtketh_softc *sc)
{
	const uint32_t rst = RSTCTRL_FE | RSTCTRL_ETH | RSTCTRL_PPE0 |
	    RSTCTRL_PPE1;

	SYSCON_WRITE_4(sc->ethsys, ETHSYS_FE_RST_CHK_IDLE_EN, 0);
	SYSCON_MODIFY_4(sc->ethsys, ETHSYS_RSTCTRL, 0, rst);
	DELAY(1000);
	SYSCON_MODIFY_4(sc->ethsys, ETHSYS_RSTCTRL, rst, 0);
	DELAY(10000);
	SYSCON_WRITE_4(sc->ethsys, ETHSYS_FE_RST_CHK_IDLE_EN, 0x3ffffff);
}

/* PSE queue thresholds for NETSYS v2, as MediaTek programs them. */
static void
mtketh_pse_config(struct mtketh_softc *sc)
{
	static const uint32_t iq[8] = { 0x001a000e, 0x01ff001a, 0x000e01ff,
	    0x000e000e, 0x000e000e, 0x000e000e, 0x000e000e, 0x000e000e };
	static const uint32_t oq[8] = { 0x000f000a, 0x001a000f, 0x000f001a,
	    0x01ff000f, 0x000f000f, 0x0006000f, 0x00060006, 0x00060006 };
	int i;

	WR4(sc, PSE_DROP_CFG, 0x300);
	WR4(sc, PSE_PPE0_DROP, 0x300);
	WR4(sc, PSE_FQFC_CFG2, 0x01fa01f4);
	for (i = 0; i < 8; i++) {
		WR4(sc, PSE_IQ_REV(i + 1), iq[i]);
		WR4(sc, PSE_OQ_TH(i + 1), oq[i]);
	}
	WR4(sc, GDM2_THRES, 4);
	WR4(sc, CDMW0_THRES, 4);
	WR4(sc, CDMW1_THRES, 4);
	WR4(sc, CDME0_THRES, 4);
	WR4(sc, CDME1_THRES, 4);
	WR4(sc, CDMM_THRES, 4);
}

/* Interrupt moderation, the same value for PDMA (RX) and QDMA (TX). */
static int
mtketh_sysctl_delay_int(SYSCTL_HANDLER_ARGS)
{
	struct mtketh_softc *sc = arg1;
	uint32_t val;
	int error;

	MTKETH_LOCK(sc);
	val = RD4(sc, PDMA_DELAY_INT);
	MTKETH_UNLOCK(sc);
	error = sysctl_handle_32(oidp, &val, 0, req);
	if (error != 0 || req->newptr == NULL)
		return (error);
	MTKETH_LOCK(sc);
	WR4(sc, PDMA_DELAY_INT, val);
	WR4(sc, QDMA_DELAY_INT, val);
	MTKETH_UNLOCK(sc);
	return (0);
}

static void
mtketh_sysctl_mib(struct mtketh_softc *sc, struct mtketh_mac *mac)
{
	struct sysctl_ctx_list *ctx = device_get_sysctl_ctx(sc->dev);
	struct sysctl_oid_list *list;
	struct sysctl_oid *node;
	struct mtketh_mib *mib = &mac->mib;
	char name[8];

	snprintf(name, sizeof(name), "mac%d", mac->id);
	node = SYSCTL_ADD_NODE(ctx,
	    SYSCTL_CHILDREN(device_get_sysctl_tree(sc->dev)), OID_AUTO, name,
	    CTLFLAG_RD | CTLFLAG_MPSAFE, NULL,
	    "GMAC counters, updated once a second");
	list = SYSCTL_CHILDREN(node);
#define	MTKETH_MIB(field, desc)						\
	SYSCTL_ADD_U64(ctx, list, OID_AUTO, #field, CTLFLAG_RD,		\
	    &mib->field, 0, desc)
	MTKETH_MIB(rx_bytes, "Bytes received");
	MTKETH_MIB(rx_frames, "Frames received");
	MTKETH_MIB(rx_overflow, "Receive overflows");
	MTKETH_MIB(rx_fcs_errors, "Receive FCS errors");
	MTKETH_MIB(rx_short, "Receive frames too short");
	MTKETH_MIB(rx_long, "Receive frames too long");
	MTKETH_MIB(rx_csum_errors, "Receive frames dropped, bad checksum");
	MTKETH_MIB(rx_pause, "Pause frames received");
	MTKETH_MIB(tx_skip, "Transmit frames skipped");
	MTKETH_MIB(tx_collisions, "Transmit collisions");
	MTKETH_MIB(tx_bytes, "Bytes transmitted");
	MTKETH_MIB(tx_frames, "Frames transmitted");
#undef MTKETH_MIB
}

static void
mtketh_sysctl(struct mtketh_softc *sc)
{
	struct sysctl_ctx_list *ctx = device_get_sysctl_ctx(sc->dev);
	struct sysctl_oid_list *list =
	    SYSCTL_CHILDREN(device_get_sysctl_tree(sc->dev));
	int i;

	for (i = 0; i < MTKETH_NMACS; i++) {
		if (sc->mac[i] != NULL)
			mtketh_sysctl_mib(sc, sc->mac[i]);
	}

	SYSCTL_ADD_U64(ctx, list, OID_AUTO, "rx_nombuf", CTLFLAG_RD,
	    &sc->rx_nombuf, 0, "Receive frames dropped for lack of mbufs");
	SYSCTL_ADD_U64(ctx, list, OID_AUTO, "rx_badport", CTLFLAG_RD,
	    &sc->rx_badport, 0, "Receive frames from an unknown port");
	SYSCTL_ADD_U64(ctx, list, OID_AUTO, "rx_badlen", CTLFLAG_RD,
	    &sc->rx_badlen, 0, "Receive frames with a bad length");
	SYSCTL_ADD_U64(ctx, list, OID_AUTO, "tx_defrag", CTLFLAG_RD,
	    &sc->tx_defrag, 0, "Transmit chains collapsed");
	SYSCTL_ADD_U64(ctx, list, OID_AUTO, "tx_minseg", CTLFLAG_RD,
	    &sc->tx_minseg, 0, "Transmit chains copied, short segments");
	SYSCTL_ADD_U64(ctx, list, OID_AUTO, "tx_tso", CTLFLAG_RD,
	    &sc->tx_tso, 0, "TSO frames sent");
	SYSCTL_ADD_U64(ctx, list, OID_AUTO, "rx_l4valid", CTLFLAG_RD,
	    &sc->rx_l4valid, 0, "Receive frames with L4 checksum valid");
	SYSCTL_ADD_U64(ctx, list, OID_AUTO, "rx_l4valid_b2", CTLFLAG_RD,
	    &sc->rx_l4valid_b2, 0, "Receive frames with rxd4 bit 2 set");
	SYSCTL_ADD_U64(ctx, list, OID_AUTO, "tx_full", CTLFLAG_RD,
	    &sc->tx_full, 0, "Transmit ring full events");
	SYSCTL_ADD_PROC(ctx, list, OID_AUTO, "delay_int",
	    CTLTYPE_U32 | CTLFLAG_RW | CTLFLAG_MPSAFE, sc, 0,
	    mtketh_sysctl_delay_int, "IU",
	    "Interrupt delay: TX en|pint|ptime in 31:16, RX in 15:0");
	SYSCTL_ADD_U32(ctx, list, OID_AUTO, "int_status2", CTLFLAG_RD,
	    &sc->int_status2, 0, "Accumulated FE INT_STATUS2 bits");
}

static int
mtketh_probe(device_t dev)
{
	if (!ofw_bus_status_okay(dev))
		return (ENXIO);
	if (ofw_bus_search_compatible(dev, compat_data)->ocd_data == 0)
		return (ENXIO);
	device_set_desc(dev, "MediaTek MT7986 Ethernet");
	return (BUS_PROBE_DEFAULT);
}

static int
mtketh_attach(device_t dev)
{
	static const char *irq_names[2] = { "fe1", "fe2" };
	static driver_intr_t *irq_handlers[2] = { mtketh_intr_tx,
	    mtketh_intr_rx };
	struct mtketh_softc *sc = device_get_softc(dev);
	struct mtketh_mac *mac;
	phandle_t node, child;
	pcell_t id;
	int error, i, idx, rid;

	sc->dev = dev;
	node = ofw_bus_get_node(dev);
	mtx_init(&sc->mtx, device_get_nameunit(dev), NULL, MTX_DEF);
	callout_init_mtx(&sc->tick, &sc->mtx, 0);

	rid = 0;
	sc->res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	if (sc->res == NULL) {
		device_printf(dev, "cannot allocate registers\n");
		return (ENXIO);
	}
	for (i = 0; i < 2; i++) {
		if (ofw_bus_find_string_index(node, "interrupt-names",
		    irq_names[i], &idx) != 0) {
			device_printf(dev, "no %s interrupt\n", irq_names[i]);
			return (ENXIO);
		}
		rid = idx;
		sc->irq_res[i] = bus_alloc_resource_any(dev, SYS_RES_IRQ, &rid,
		    RF_ACTIVE);
		if (sc->irq_res[i] == NULL) {
			device_printf(dev, "cannot allocate %s interrupt\n",
			    irq_names[i]);
			return (ENXIO);
		}
	}
	if (syscon_get_by_ofw_property(dev, node, "mediatek,ethsys",
	    &sc->ethsys) != 0) {
		device_printf(dev, "no ETHSYS syscon\n");
		return (ENXIO);
	}
	if ((error = mtketh_enable_clocks(sc, node)) != 0)
		return (error);

	/* Start from a known state, whatever the boot loader left running. */
	mtketh_fe_reset(sc);
	if ((RD4(sc, FE_GLO_MISC) & FE_GLO_MISC_PDMA_V2) != 0)
		MD4(sc, FE_GLO_MISC, FE_GLO_MISC_PDMA_V2, 0);
	mtketh_mdio_init(sc, node);
	mtketh_pse_config(sc);
	MD4(sc, CDMQ_IG_CTRL, 0, CDMQ_STAG_EN);
	for (i = 0; i < MTKETH_NMACS; i++) {
		mtketh_gdm_forward(sc, i, false);
		WR4(sc, MAC_MCR(i), MCR_FORCE_MODE);	/* link forced down */
	}

	/* Interrupts: moderated TX/RX done on fe1/fe2, all masked for now. */
	WR4(sc, FE_INT_ENABLE, 0);
	WR4(sc, QDMA_INT_MASK, 0);
	WR4(sc, PDMA_INT_MASK, 0);
	WR4(sc, PDMA_DELAY_INT, MTKETH_DELAY_INT);
	WR4(sc, QDMA_DELAY_INT, MTKETH_DELAY_INT);
	WR4(sc, PDMA_INT_GRP, INT_TX_DONE_DLY);
	WR4(sc, PDMA_INT_GRP + 4, INT_RX_DONE_DLY);
	WR4(sc, QDMA_INT_GRP, INT_TX_DONE_DLY);
	WR4(sc, QDMA_INT_GRP + 4, INT_RX_DONE_DLY);
	WR4(sc, FE_INT_GRP, 0x21021000);

	if ((error = mtketh_dma_init(sc)) != 0) {
		device_printf(dev, "cannot allocate DMA memory: %d\n", error);
		return (error);
	}

	for (child = OF_child(node); child != 0; child = OF_peer(child)) {
		if (!ofw_bus_node_is_compatible(child, "mediatek,eth-mac") ||
		    !ofw_bus_node_status_okay(child))
			continue;
		if (OF_getencprop(child, "reg", &id, sizeof(id)) !=
		    sizeof(id) || id >= MTKETH_NMACS)
			continue;
		if (mtketh_mac_attach(sc, child, id) != 0)
			continue;
		mac = sc->mac[id];
		if (mtketh_sgmiisys(sc, node, id, &mac->pcs) != 0) {
			device_printf(dev, "GMAC%d: no SGMIISYS syscon\n", id);
			if_free(mac->ifp);
			free(mac, M_DEVBUF);
			sc->mac[id] = NULL;
			continue;
		}
		mtketh_mac_config(mac);
	}
	if (sc->mac[0] == NULL && sc->mac[1] == NULL) {
		device_printf(dev, "no usable GMAC\n");
		return (ENXIO);
	}
	mtketh_vlan_strip(sc);
	MD4(sc, FE_RST_GL, 0, FE_RST_GL_PSE);
	MD4(sc, FE_RST_GL, FE_RST_GL_PSE, 0);
	mtketh_dma_start(sc);
	mtketh_sysctl(sc);

	for (i = 0; i < 2; i++) {
		error = bus_setup_intr(dev, sc->irq_res[i],
		    INTR_TYPE_NET | INTR_MPSAFE, NULL, irq_handlers[i], sc,
		    &sc->irq_cookie[i]);
		if (error != 0) {
			device_printf(dev, "cannot set up %s interrupt\n",
			    irq_names[i]);
			return (error);
		}
	}
	WR4(sc, QDMA_INT_MASK, INT_TX_DONE_DLY);
	WR4(sc, PDMA_INT_MASK, INT_RX_DONE_DLY);

	for (i = 0; i < MTKETH_NMACS; i++) {
		if ((mac = sc->mac[i]) == NULL)
			continue;
		ether_ifattach(mac->ifp, mac->eaddr);
		device_printf(dev, "GMAC%d: %d Mbit/s fixed link%s\n", i,
		    mac->speed, mac->pause ? ", pause" : "");
	}
	return (0);
}

/* Leave no DMA running into memory the next boot stage will reuse. */
static int
mtketh_shutdown(device_t dev)
{
	struct mtketh_softc *sc = device_get_softc(dev);
	int i;

	MTKETH_LOCK(sc);
	for (i = 0; i < MTKETH_NMACS; i++) {
		if (sc->mac[i] != NULL)
			mtketh_stop_locked(sc->mac[i]);
	}
	WR4(sc, QDMA_INT_MASK, 0);
	WR4(sc, PDMA_INT_MASK, 0);
	mtketh_dma_stop(sc);
	MTKETH_UNLOCK(sc);
	return (0);
}

static device_method_t mtketh_methods[] = {
	DEVMETHOD(device_probe,		mtketh_probe),
	DEVMETHOD(device_attach,	mtketh_attach),
	DEVMETHOD(device_shutdown,	mtketh_shutdown),
	DEVMETHOD_END
};

static driver_t mtketh_driver = {
	"mtketh",
	mtketh_methods,
	sizeof(struct mtketh_softc),
};

DRIVER_MODULE(mtketh, simplebus, mtketh_driver, NULL, NULL);
MODULE_DEPEND(mtketh, ether, 1, 1, 1);
