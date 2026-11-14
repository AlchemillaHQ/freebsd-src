/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

/*
 * MediaTek MT7986 (NETSYS v2) frame engine registers.  Offsets are from
 * the frame engine base (the "reg" of the ethernet node) unless noted.
 */

#ifndef _ARM64_MEDIATEK_IF_MTKETHREG_H_
#define	_ARM64_MEDIATEK_IF_MTKETHREG_H_

/* Frame engine, PSE, GDM and CDM. */
#define	FE_RST_GL		0x0004
#define	 FE_RST_GL_PSE		(1u << 0)
#define	FE_INT_STATUS2		0x0008
#define	FE_INT_ENABLE		0x000c
#define	FE_INT_GRP		0x0020
#define	PSE_FQFC_CFG2		0x0104
#define	PSE_DROP_CFG		0x0108
#define	PSE_PPE0_DROP		0x0110
#define	FE_GLO_MISC		0x0124
#define	 FE_GLO_MISC_PDMA_V2	(1u << 4)
#define	PSE_IQ_REV(n)		(0x0140 + ((n) - 1) * 4)	/* n = 1..8 */
#define	PSE_OQ_TH(n)		(0x0160 + ((n) - 1) * 4)	/* n = 1..8 */
#define	CDMP_EG_CTRL		0x0404
#define	 CDMP_EG_VLAN_STRIP	(1u << 0)	/* RX tags into rxd2/rxd3 */
#define	CDMQ_IG_CTRL		0x1400
#define	 CDMQ_STAG_EN		(1u << 0)
#define	GDM2_THRES		0x1530
#define	CDMW0_THRES		0x164c
#define	CDMW1_THRES		0x1650
#define	CDME0_THRES		0x1654
#define	CDME1_THRES		0x1658
#define	CDMM_THRES		0x165c

#define	GDM_FWD_CFG(id)		(0x0500 + (id) * 0x1000)
#define	 GDM_ICS_EN		(1u << 22)
#define	 GDM_TCS_EN		(1u << 21)
#define	 GDM_UCS_EN		(1u << 20)
#define	 GDM_STRP_CRC		(1u << 16)
#define	 GDM_DST_MASK		0xffff		/* my-MAC/bcast/mcast/unknown */
#define	 GDM_DST_PDMA		0x0000		/* PSE port 0 */
#define	 GDM_DST_DROP		0x7777		/* PSE port 7 */
#define	GDM_MAC_LSB(id)		(0x0508 + (id) * 0x1000)
#define	GDM_MAC_MSB(id)		(0x050c + (id) * 0x1000)
#define	GDM_MIB(id)		(0x1c00 + (id) * 0x40)	/* read to clear */
#define	 MIB_RX_BYTES		0x00	/* 64 bits, low word first */
#define	 MIB_RX_FRAMES		0x08
#define	 MIB_RX_OVERFLOW	0x10
#define	 MIB_RX_FCS		0x14
#define	 MIB_RX_SHORT		0x18
#define	 MIB_RX_LONG		0x1c
#define	 MIB_RX_CSUM		0x20	/* bad checksum, frame dropped */
#define	 MIB_RX_PAUSE		0x24
#define	 MIB_TX_SKIP		0x28
#define	 MIB_TX_COLL		0x2c
#define	 MIB_TX_BYTES		0x30	/* 64 bits, low word first */
#define	 MIB_TX_FRAMES		0x38

/* GMACs and the MDIO controller. */
#define	MAC_PPSC		0x10000
#define	 PPSC_MDC_CFG_SHIFT	24
#define	 PPSC_MDC_CFG_MASK	(0x3fu << PPSC_MDC_CFG_SHIFT)
#define	 PPSC_MDC_TURBO		(1u << 20)
#define	MAC_PIAC		0x10004
#define	 PIAC_ACCESS		(1u << 31)
#define	 PIAC_REG_SHIFT		25
#define	 PIAC_ADDR_SHIFT	20
#define	 PIAC_CMD_WRITE		(1u << 18)
#define	 PIAC_CMD_READ		(2u << 18)
#define	 PIAC_START_C22		(1u << 16)
#define	 PIAC_DATA_MASK		0xffff
#define	MAC_MCR(id)		(0x10100 + (id) * 0x100)
#define	 MCR_MAX_RX_2048	(3u << 24)
#define	 MCR_IPG_CFG		((1u << 18) | (1u << 16))
#define	 MCR_FORCE_MODE		(1u << 15)
#define	 MCR_TX_EN		(1u << 14)
#define	 MCR_RX_EN		(1u << 13)
#define	 MCR_RX_FIFO_CLR_DIS	(1u << 12)
#define	 MCR_BACKOFF_EN		(1u << 9)
#define	 MCR_BACKPR_EN		(1u << 8)
#define	 MCR_FORCE_RX_FC	(1u << 5)
#define	 MCR_FORCE_TX_FC	(1u << 4)
#define	 MCR_SPEED_1000		(1u << 3)
#define	 MCR_SPEED_100		(1u << 2)
#define	 MCR_FORCE_DPX		(1u << 1)
#define	 MCR_FORCE_LINK		(1u << 0)
#define	MAC_MSR(id)		(0x10108 + (id) * 0x100)
#define	 MSR_LINK		(1u << 0)

/* Delay interrupt configuration, the same layout for PDMA and QDMA. */
#define	 DLY_TX_EN		(1u << 31)
#define	 DLY_TX_PINT_SHIFT	24
#define	 DLY_TX_PTIME_SHIFT	16
#define	 DLY_RX_EN		(1u << 15)
#define	 DLY_RX_PINT_SHIFT	8
#define	 DLY_RX_PTIME_SHIFT	0
#define	 INT_TX_DONE_DLY	(1u << 28)
#define	 INT_RX_DONE_DLY	(1u << 30)

/* DMA global configuration bits, shared by PDMA and QDMA. */
#define	 GLO_TX_DMA_EN		(1u << 0)
#define	 GLO_TX_DMA_BUSY	(1u << 1)
#define	 GLO_RX_DMA_EN		(1u << 2)
#define	 GLO_RX_DMA_BUSY	(1u << 3)
#define	 GLO_TX_BT_32DW		(3u << 4)
#define	 GLO_TX_WB_DDONE	(1u << 6)
#define	 GLO_MULTI_EN		(1u << 10)	/* PDMA */
#define	 GLO_NDP_CO_PRO		(1u << 10)	/* QDMA */
#define	 GLO_LEAKY_BUCKET_EN	(1u << 11)	/* QDMA */
#define	 GLO_RX_BT_32DW		(3u << 11)	/* PDMA */
#define	 GLO_MULTI_CNT		(4u << 12)	/* QDMA */
#define	 GLO_RESV_BUF		(0x40u << 16)	/* QDMA */
#define	 GLO_WCOMP_EN		(1u << 24)	/* QDMA */
#define	 GLO_DMAD_WR_WDONE	(1u << 26)	/* QDMA */
#define	 GLO_CHK_DDONE_EN	(1u << 28)	/* QDMA */
#define	 GLO_RX_2B_OFFSET	(1u << 31)

/* PDMA, receive ring 0 (the legacy, 16-byte descriptor layout). */
#define	PDMA_RX_BASE		0x4100
#define	PDMA_RX_MAX_CNT		0x4104
#define	PDMA_RX_CRX_IDX		0x4108
#define	PDMA_GLO_CFG		0x4204
#define	PDMA_RST_IDX		0x4208
#define	 PDMA_RST_DRX_IDX0	(1u << 16)
#define	PDMA_DELAY_INT		0x420c
#define	PDMA_INT_STATUS		0x4220
#define	PDMA_INT_MASK		0x4228
#define	PDMA_INT_GRP		0x4250

/* QDMA, transmit. */
#define	QTX_CFG(q)		(0x4400 + (q) * 0x10)
#define	QTX_SCH(q)		(0x4404 + (q) * 0x10)
#define	 QTX_CFG_RES_THRES	((4u << 8) | 4u)
#define	 QTX_SCH_DEFAULT	(0x30000000u | (1u << 27) | (1u << 20) | \
				    (4u << 16))	/* min rate 10 Mbit/s */
#define	QDMA_NQUEUES		16
#define	QDMA_GLO_CFG		0x4604
#define	QDMA_DELAY_INT		0x460c
#define	QDMA_FC_THRES		0x4610
#define	 QDMA_FC_THRES_DEFAULT	((1u << 20) | (7u << 16) | 0x4444)
#define	QDMA_INT_STATUS		0x4618
#define	QDMA_INT_MASK		0x461c
#define	QDMA_INT_GRP		0x4620
#define	QDMA_HRED		0x4644
#define	QTX_CTX_PTR		0x4700
#define	QTX_DTX_PTR		0x4704
#define	QTX_CRX_PTR		0x4710
#define	QTX_DRX_PTR		0x4714
#define	QDMA_FQ_HEAD		0x4720
#define	QDMA_FQ_TAIL		0x4724
#define	QDMA_FQ_CNT		0x4728
#define	QDMA_FQ_BLEN		0x472c
#define	QDMA_TX_SCH_RATE	0x4798
#define	 QDMA_TX_SCH_MAX_WFQ	((1u << 15) | (1u << 31))

/* Transmit descriptor (QDMA, NETSYS v2): 8 words. */
struct mtketh_txd {
	uint32_t	txd1;		/* buffer address */
	uint32_t	txd2;		/* next descriptor */
	uint32_t	txd3;
	uint32_t	txd4;
	uint32_t	txd5;
	uint32_t	txd6;
	uint32_t	txd7;
	uint32_t	txd8;
} __packed;
#define	 TXD3_OWNER_CPU		(1u << 31)
#define	 TXD3_LS0		(1u << 30)
#define	 TXD3_PLEN0(x)		(((x) & 0xffff) << 8)
#define	 TXD4_SWC		(1u << 30)
#define	 TXD4_QID(x)		(((x) & 0x3f) << 16)
#define	 TXD4_FPORT(x)		(((x) & 0xf) << 8)	/* 1 GDM1, 2 GDM2 */
#define	 TXD5_TSO		(1u << 31)	/* MSS in the TCP checksum */
#define	 TXD5_CSUM		(7u << 28)
#define	 TXD6_INS_VLAN		(1u << 16)

/* Receive descriptor (PDMA, legacy layout): 4 words. */
struct mtketh_rxd {
	uint32_t	rxd1;		/* buffer address */
	uint32_t	rxd2;
	uint32_t	rxd3;
	uint32_t	rxd4;
} __packed;
#define	 RXD2_DONE		(1u << 31)
#define	 RXD2_LS0		(1u << 30)
#define	 RXD2_PLEN0(x)		(((x) & 0x3fff) << 16)
#define	 RXD2_GET_PLEN0(x)	(((x) >> 16) & 0x3fff)
#define	 RXD2_VTAG		(1u << 15)	/* VLAN tag stripped */
#define	 RXD3_TCI(x)		((x) & 0xffff)
#define	 RXD4_SPORT(x)		(((x) >> 19) & 0x7)	/* 1 GDM1, 2 GDM2 */
#define	 RXD4_SPECIAL_TAG	(1u << 22)
#define	 RXD4_L4_VALID		(1u << 24)

/* ETHSYS (syscon "mediatek,ethsys"). */
#define	ETHSYS_SYSCFG0		0x14
#define	 SYSCFG0_GE_MODE_SHIFT(id)	(12 + 2 * (id))
#define	 SYSCFG0_GE_MODE_MASK(id)	(3u << SYSCFG0_GE_MODE_SHIFT(id))
#define	 SYSCFG0_SGMII_SEL(id)	(1u << (9 - (id)))
#define	ETHSYS_FE_RST_CHK_IDLE_EN 0x28
#define	ETHSYS_RSTCTRL		0x34
#define	 RSTCTRL_FE		(1u << 6)
#define	 RSTCTRL_ETH		(1u << 23)
#define	 RSTCTRL_PPE0		(1u << 30)
#define	 RSTCTRL_PPE1		(1u << 31)

/* SGMIISYS PCS (syscon "mediatek,sgmiisys"). */
#define	PCS_CONTROL_1		0x00
#define	 PCS_BMCR_ANENABLE	(1u << 12)
#define	 PCS_BMSR_LINK		(1u << 18)
#define	PCS_LINK_TIMER		0x18
#define	PCS_SGMII_MODE		0x20
#define	 SGMII_IF_MODE_SGMII	(1u << 0)
#define	 SGMII_SPEED_DUPLEX_AN	(1u << 1)
#define	 SGMII_REMOTE_FAULT_DIS	(1u << 8)
#define	PCS_RESERVED_0		0x34
#define	 PCS_SW_RESET		(1u << 0)
#define	PCS_QPHY_PWR_STATE	0xe8
#define	 PCS_PHYA_PWD		(1u << 4)
#define	PCS_ANA_RGC3		0x128
#define	 RGC3_SPEED_MASK	(3u << 2)
#define	 RGC3_SPEED_3125M	(1u << 2)		/* 2500base-x */
#define	 RGC3_SPEED_1250M	(0u << 2)		/* 1000base-x */

#endif /* _ARM64_MEDIATEK_IF_MTKETHREG_H_ */
