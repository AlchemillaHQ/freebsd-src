/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 *
 * The VLAN table and address table handling follows mtkswitch(4), Copyright
 * (c) 2016 Stanislav Galabov.
 */

/*
 * MediaTek MT7531 Gigabit Ethernet switch, on an MDIO bus.
 *
 * Seven ports: 0-4 with internal PHYs, 5 and 6 with SGMII/HSGMII
 * interfaces.  The switch registers are 32 bits wide and reached through a
 * page register at the switch's MDIO address; the PHYs are reached through
 * the switch's own indirect access register.
 *
 * The CPU port is the one whose device tree node has an "ethernet"
 * property.  It runs without the MediaTek special tag: FreeBSD has no
 * tagging layer, so the ports are separated with 802.1Q VLANs and vlan(4)
 * on the CPU's interface.  By default every port is an untagged member of
 * VLAN 1, which makes the switch a plain switch.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/gpio.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/socket.h>
#include <sys/sockio.h>

#include <net/if.h>
#include <net/if_media.h>
#include <net/if_types.h>
#include <net/if_var.h>

#include <dev/etherswitch/etherswitch.h>
#include <dev/gpio/gpiobusvar.h>
#include <dev/mii/mii.h>
#include <dev/mii/miivar.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <dev/etherswitch/mt7531/mt7531reg.h>

#include "etherswitch_if.h"
#include "mdio_if.h"
#include "miibus_if.h"

/* CPU port: forced 1000 code (2.5G comes from the HSGMII line rate). */
#define	MT7531_CPU_PMCR							\
	(PMCR_FORCE_LNK | PMCR_FORCE_SPD | PMCR_FORCE_DPX |		\
	 PMCR_FORCE_RX_FC | PMCR_FORCE_TX_FC | PMCR_IPG_SHORT |		\
	 PMCR_MAC_MODE | PMCR_TX_EN | PMCR_RX_EN | PMCR_BKOFF_EN |	\
	 PMCR_BACKPR_EN | PMCR_RX_FC | PMCR_TX_FC | PMCR_SPEED_1000 |	\
	 PMCR_DPX | PMCR_LINK)

struct mt7531_softc {
	device_t		dev;
	struct mtx		mtx;
	struct callout		tick;
	int			smi;		/* switch MDIO address */
	int			cpuport;
	int			rev;
	bool			dual_sgmii;
	etherswitch_info_t	info;
	if_t			ifp[MT7531_NPHYS];
	device_t		miibus[MT7531_NPHYS];
};

#define	MT7531_LOCK(sc)		mtx_lock(&(sc)->mtx)
#define	MT7531_UNLOCK(sc)	mtx_unlock(&(sc)->mtx)
#define	MT7531_LOCK_ASSERT(sc)	mtx_assert(&(sc)->mtx, MA_OWNED)

static struct ofw_compat_data compat_data[] = {
	{ "mediatek,mt7531",	1 },
	{ NULL,			0 }
};

/* --------------------------------------------------- Register access */

static uint32_t
mt7531_read(struct mt7531_softc *sc, uint32_t reg)
{
	device_t bus = device_get_parent(sc->dev);
	int lo, hi;

	MT7531_LOCK_ASSERT(sc);
	MDIO_WRITEREG(bus, sc->smi, 0x1f, reg >> 6);
	lo = MDIO_READREG(bus, sc->smi, (reg >> 2) & 0xf);
	hi = MDIO_READREG(bus, sc->smi, 0x10);
	return ((uint32_t)(hi & 0xffff) << 16 | (lo & 0xffff));
}

static void
mt7531_write(struct mt7531_softc *sc, uint32_t reg, uint32_t val)
{
	device_t bus = device_get_parent(sc->dev);

	MT7531_LOCK_ASSERT(sc);
	MDIO_WRITEREG(bus, sc->smi, 0x1f, reg >> 6);
	/* The low half first: the high half commits the write. */
	MDIO_WRITEREG(bus, sc->smi, (reg >> 2) & 0xf, val & 0xffff);
	MDIO_WRITEREG(bus, sc->smi, 0x10, val >> 16);
}

static void
mt7531_rmw(struct mt7531_softc *sc, uint32_t reg, uint32_t clr, uint32_t set)
{
	mt7531_write(sc, reg, (mt7531_read(sc, reg) & ~clr) | set);
}

static int
mt7531_iac(struct mt7531_softc *sc, uint32_t cmd, uint32_t *data)
{
	uint32_t val;
	int i;

	MT7531_LOCK_ASSERT(sc);
	mt7531_write(sc, MT7531_PHY_IAC, cmd | IAC_ACCESS);
	for (i = 0; i < 1000; i++) {
		val = mt7531_read(sc, MT7531_PHY_IAC);
		if ((val & IAC_ACCESS) == 0) {
			if (data != NULL)
				*data = val & IAC_DATA_MASK;
			return (0);
		}
		DELAY(100);
	}
	device_printf(sc->dev, "PHY access timeout\n");
	return (ETIMEDOUT);
}

static int
mt7531_phy_read(struct mt7531_softc *sc, int phy, int reg)
{
	uint32_t val;

	if (mt7531_iac(sc, IAC_START_C22 | IAC_CMD_C22_READ |
	    (phy & 0x1f) << IAC_ADDR_SHIFT | (reg & 0x1f) << IAC_REG_SHIFT,
	    &val) != 0)
		return (-1);
	return (val);
}

static int
mt7531_phy_write(struct mt7531_softc *sc, int phy, int reg, int val)
{
	return (mt7531_iac(sc, IAC_START_C22 | IAC_CMD_WRITE |
	    (phy & 0x1f) << IAC_ADDR_SHIFT | (reg & 0x1f) << IAC_REG_SHIFT |
	    (val & IAC_DATA_MASK), NULL));
}

static int
mt7531_phy_read_c45(struct mt7531_softc *sc, int phy, int dev, int reg)
{
	uint32_t base, val;

	base = IAC_START_C45 | (phy & 0x1f) << IAC_ADDR_SHIFT |
	    (dev & 0x1f) << IAC_REG_SHIFT;
	if (mt7531_iac(sc, base | IAC_CMD_C45_ADDR | (reg & 0xffff),
	    NULL) != 0 ||
	    mt7531_iac(sc, base | IAC_CMD_C45_READ, &val) != 0)
		return (-1);
	return (val);
}

static int
mt7531_phy_write_c45(struct mt7531_softc *sc, int phy, int dev, int reg,
    int val)
{
	uint32_t base;

	base = IAC_START_C45 | (phy & 0x1f) << IAC_ADDR_SHIFT |
	    (dev & 0x1f) << IAC_REG_SHIFT;
	if (mt7531_iac(sc, base | IAC_CMD_C45_ADDR | (reg & 0xffff), NULL) != 0)
		return (ETIMEDOUT);
	return (mt7531_iac(sc, base | IAC_CMD_WRITE | (val & 0xffff), NULL));
}

/* ----------------------------------------------------------- Set-up */

/*
 * The SGMII PCS of port 5 or 6 for 2500base-x without in-band
 * negotiation; the same block and sequence as the SoC side.
 */
static void
mt7531_pcs_config(struct mt7531_softc *sc, int port)
{
	uint32_t base = MT7531_PCS_BASE(port);

	mt7531_rmw(sc, base + PCS_QPHY_PWR_STATE, 0, PCS_PHYA_PWD);
	mt7531_rmw(sc, base + PCS_RESERVED_0, 0, PCS_SW_RESET);
	mt7531_rmw(sc, base + PCS_ANA_RGC3, RGC3_SPEED_MASK, RGC3_SPEED_3125M);
	mt7531_write(sc, base + PCS_LINK_TIMER, 10000000 / 16);	/* 10 ms */
	mt7531_rmw(sc, base + PCS_SGMII_MODE, SGMII_IF_MODE_SGMII |
	    SGMII_SPEED_DUPLEX_AN | SGMII_REMOTE_FAULT_DIS, 0);
	mt7531_rmw(sc, base + PCS_CONTROL_1, PCS_BMCR_ANENABLE, 0);
	DELAY(100);
	mt7531_write(sc, base + PCS_QPHY_PWR_STATE, 0);
	if ((mt7531_read(sc, base + PCS_RESERVED_0) & PCS_SW_RESET) != 0)
		mt7531_rmw(sc, base + PCS_RESERVED_0, PCS_SW_RESET, 0);
}

static void
mt7531_atu_flush(struct mt7531_softc *sc)
{
	int i;

	MT7531_LOCK_ASSERT(sc);
	mt7531_write(sc, MT7531_ATC, ATC_BUSY | ATC_MAT_NON_STATIC |
	    ATC_CMD_CLEAN);
	for (i = 0; i < 1000 && (mt7531_read(sc, MT7531_ATC) & ATC_BUSY); i++)
		DELAY(10);
}

static int
mt7531_vtcr(struct mt7531_softc *sc, uint32_t cmd)
{
	uint32_t val;
	int i;

	MT7531_LOCK_ASSERT(sc);
	mt7531_write(sc, MT7531_VTCR, VTCR_BUSY | cmd);
	for (i = 0; i < 1000; i++) {
		val = mt7531_read(sc, MT7531_VTCR);
		if ((val & VTCR_BUSY) == 0)
			return ((val & VTCR_IDX_INVALID) != 0 ? EINVAL : 0);
		DELAY(10);
	}
	return (ETIMEDOUT);
}

/* Every port an untagged member of VLAN 1, the PVID of every port. */
static void
mt7531_vlan_reset(struct mt7531_softc *sc)
{
	int i;

	MT7531_LOCK_ASSERT(sc);
	for (i = 0; i < MT7531_NVLANGROUPS; i++)
		mt7531_vtcr(sc, VTCR_FUNC_INVALIDATE | i);
	mt7531_write(sc, MT7531_VAWD1, VAWD1_IVL_MAC | VAWD1_VTAG_EN |
	    MT7531_ALL_PORTS << VAWD1_MEMBER_SHIFT | VAWD1_VALID);
	mt7531_write(sc, MT7531_VAWD2, 0);
	mt7531_vtcr(sc, VTCR_FUNC_WRITE | 1);
	for (i = 0; i < MT7531_NPORTS; i++)
		mt7531_rmw(sc, MT7531_PPBV1(i), PPBV_PVID_MASK, 1);
	mt7531_atu_flush(sc);
}

static int
mt7531_reset(struct mt7531_softc *sc, phandle_t node)
{
	gpio_pin_t pin;
	int error, i;

	error = gpio_pin_get_by_ofw_property(sc->dev, node, "reset-gpios",
	    &pin);
	if (error != 0) {
		device_printf(sc->dev, "no reset GPIO, keeping the switch "
		    "state\n");
		return (0);
	}
	gpio_pin_setflags(pin, GPIO_PIN_OUTPUT);
	gpio_pin_set_active(pin, false);
	DELAY(5000);
	gpio_pin_set_active(pin, true);
	gpio_pin_release(pin);
	/* The strap register reads zero until the switch is out of reset. */
	MT7531_LOCK(sc);
	for (i = 0; i < 1000; i++) {
		if (mt7531_read(sc, 0x7800) != 0)
			break;
		DELAY(1000);
	}
	MT7531_UNLOCK(sc);
	if (i == 1000) {
		device_printf(sc->dev, "switch does not leave reset\n");
		return (ETIMEDOUT);
	}
	return (0);
}

static int
mt7531_setup(struct mt7531_softc *sc)
{
	uint32_t val;
	int p, v;

	MT7531_LOCK_ASSERT(sc);
	val = mt7531_read(sc, MT7531_CREV);
	if (CREV_ID(val) != MT7531_ID) {
		device_printf(sc->dev, "unexpected chip id %#x\n", val);
		return (ENXIO);
	}
	sc->rev = CREV_REV(val);
	sc->dual_sgmii = (mt7531_read(sc, MT7531_TOP_SIG_SR) &
	    TOP_SIG_DUAL_SGMII) != 0;

	/* Every MAC forced down before the software reset. */
	for (p = 0; p < MT7531_NPORTS; p++)
		mt7531_write(sc, MT7531_PMCR(p), PMCR_LINK_DOWN);
	mt7531_write(sc, MT7531_SYS_CTRL, SYS_CTRL_SW_RST | SYS_CTRL_REG_RST);
	DELAY(100);
	mt7531_rmw(sc, MT7531_SMT0_IOLB, 0, SMT0_IOLB_MDC);

	/*
	 * The core PLL setup is for an RGMII port 5; with port 5 in SGMII
	 * the PLL keeps its reset configuration.
	 */
	if (!sc->dual_sgmii) {
		mt7531_rmw(sc, MT7531_PLLGP_EN, PLLGP_EN_COREPLL,
		    PLLGP_SW_CLKSW);
		mt7531_rmw(sc, MT7531_PLLGP_CR0, PLLGP_CR0_COREPLL_EN, 0);
		mt7531_rmw(sc, MT7531_PLLGP_EN, 0, PLLGP_SW_PLLGP);
		mt7531_rmw(sc, MT7531_PLLGP_CR0, PLLGP_CR0_POSDIV_MASK,
		    2u << PLLGP_CR0_POSDIV_SHIFT);		/* 500 MHz */
		DELAY(25);
		mt7531_rmw(sc, MT7531_PLLGP_CR0, PLLGP_CR0_SDM_PCW_MASK,
		    0x140000u << PLLGP_CR0_SDM_PCW_SHIFT);
		mt7531_rmw(sc, MT7531_PLLGP_CR0, 0, PLLGP_CR0_PCW_CHG);
		DELAY(10);
		mt7531_rmw(sc, MT7531_PLLGP_CR0, PLLGP_CR0_PCW_CHG, 0);
		mt7531_write(sc, MT7531_ANA_PLLGP_CR5, 0x00ad0000);
		mt7531_write(sc, MT7531_ANA_PLLGP_CR2, 0x04f40000);
		mt7531_rmw(sc, MT7531_PLLGP_CR0, 0, PLLGP_CR0_COREPLL_EN);
		mt7531_rmw(sc, MT7531_PLLGP_EN, 0, PLLGP_EN_COREPLL);
		DELAY(25);
	}

	/* PHY PLL on; EEE not advertised. */
	v = mt7531_phy_read_c45(sc, 0, MT7531_PHY_DEV1F, CORE_PLL_GROUP4);
	if (v >= 0)
		mt7531_phy_write_c45(sc, 0, MT7531_PHY_DEV1F, CORE_PLL_GROUP4,
		    (v | (1 << 6) | BYPASS_MODE) & ~POWER_ON_OFF);
	for (p = 0; p < MT7531_NPHYS; p++)
		mt7531_phy_write_c45(sc, p, 7, 0x3c, 0);

	/* The CPU port.  The other SGMII port (an SFP cage) stays down. */
	mt7531_pcs_config(sc, sc->cpuport);
	mt7531_write(sc, MT7531_PMCR(sc->cpuport), MT7531_CPU_PMCR);
	mt7531_write(sc, MT7531_PMCR(sc->cpuport == 6 ? 5 : 6), PMCR_LINK_DOWN);

	/*
	 * Forwarding by VLAN table on every port, egress tagging as the
	 * table says (not "consistent", which keeps the ingress tag state),
	 * flooding to every port, trapped frames to the CPU.
	 */
	for (p = 0; p < MT7531_NPORTS; p++) {
		mt7531_rmw(sc, MT7531_PCR(p), PCR_MATRIX_MASK |
		    PCR_PORT_VLAN_MASK, PCR_MATRIX(MT7531_ALL_PORTS) |
		    PCR_PORT_VLAN_SECURITY);
		mt7531_rmw(sc, MT7531_PVC(p), PVC_EG_TAG_MASK |
		    PVC_VLAN_ATTR_MASK | PVC_SPEC_TAG, PVC_EG_TAG_TABLE);
	}
	mt7531_rmw(sc, MT7531_MFC, MFC_FFP_MASK,
	    MFC_BC_FFP(MT7531_ALL_PORTS) | MFC_UNM_FFP(MT7531_ALL_PORTS) |
	    MFC_UNU_FFP(MT7531_ALL_PORTS));
	mt7531_rmw(sc, MT7531_CFC, CFC_CPU_PMAP_MASK, 1u << sc->cpuport);
	mt7531_vlan_reset(sc);

	/* PHYs on, with auto downshift and link-down power saving. */
	for (p = 0; p < MT7531_NPHYS; p++) {
		mt7531_phy_write(sc, p, MT7531_PHY_PAGE, 1);
		v = mt7531_phy_read(sc, p, MT7531_PHY_EXT_REG_14);
		mt7531_phy_write(sc, p, MT7531_PHY_EXT_REG_14,
		    v | PHY_EN_DOWNSHIFT);
		v = mt7531_phy_read(sc, p, MT7531_PHY_EXT_REG_17);
		mt7531_phy_write(sc, p, MT7531_PHY_EXT_REG_17,
		    v | PHY_LINKDOWN_POWER_SAVING);
		mt7531_phy_write(sc, p, MT7531_PHY_PAGE, 0);
		v = mt7531_phy_read(sc, p, MII_BMCR);
		mt7531_phy_write(sc, p, MII_BMCR, v & ~BMCR_PDOWN);
	}
	return (0);
}

/* ------------------------------------------------------- Port media */

static int
mt7531_ifmedia_upd(if_t ifp)
{
	struct mt7531_softc *sc = if_getsoftc(ifp);
	struct mii_data *mii;
	int port = if_getdunit(ifp);

	mii = device_get_softc(sc->miibus[port]);
	MT7531_LOCK(sc);
	mii_mediachg(mii);
	MT7531_UNLOCK(sc);
	return (0);
}

static void
mt7531_ifmedia_sts(if_t ifp, struct ifmediareq *ifmr)
{
	struct mt7531_softc *sc = if_getsoftc(ifp);
	struct mii_data *mii;
	int port = if_getdunit(ifp);

	mii = device_get_softc(sc->miibus[port]);
	MT7531_LOCK(sc);
	mii_pollstat(mii);
	ifmr->ifm_active = mii->mii_media_active;
	ifmr->ifm_status = mii->mii_media_status;
	MT7531_UNLOCK(sc);
}

static void
mt7531_tick(void *arg)
{
	struct mt7531_softc *sc = arg;
	int p;

	MT7531_LOCK_ASSERT(sc);
	for (p = 0; p < MT7531_NPHYS; p++) {
		if (sc->miibus[p] != NULL)
			mii_tick(device_get_softc(sc->miibus[p]));
	}
	callout_reset(&sc->tick, hz, mt7531_tick, sc);
}

/* ------------------------------------------------------ miibus, MDIO */

static int
mt7531_miibus_readreg(device_t dev, int phy, int reg)
{
	struct mt7531_softc *sc = device_get_softc(dev);
	int val;

	if (phy >= MT7531_NPHYS)
		return (0xffff);
	MT7531_LOCK(sc);
	val = mt7531_phy_read(sc, phy, reg);
	MT7531_UNLOCK(sc);
	return (val < 0 ? 0xffff : val);
}

static int
mt7531_miibus_writereg(device_t dev, int phy, int reg, int val)
{
	struct mt7531_softc *sc = device_get_softc(dev);
	int error;

	if (phy >= MT7531_NPHYS)
		return (ENXIO);
	MT7531_LOCK(sc);
	error = mt7531_phy_write(sc, phy, reg, val);
	MT7531_UNLOCK(sc);
	return (error);
}

/* ------------------------------------------------------ etherswitch */

static etherswitch_info_t *
mt7531_getinfo(device_t dev)
{
	struct mt7531_softc *sc = device_get_softc(dev);

	return (&sc->info);
}

static void
mt7531_es_lock(device_t dev)
{
	MT7531_LOCK((struct mt7531_softc *)device_get_softc(dev));
}

static void
mt7531_es_unlock(device_t dev)
{
	MT7531_UNLOCK((struct mt7531_softc *)device_get_softc(dev));
}

static int
mt7531_es_readreg(device_t dev, int reg)
{
	struct mt7531_softc *sc = device_get_softc(dev);

	MT7531_LOCK_ASSERT(sc);
	return (mt7531_read(sc, reg));
}

static int
mt7531_es_writereg(device_t dev, int reg, int val)
{
	struct mt7531_softc *sc = device_get_softc(dev);

	MT7531_LOCK_ASSERT(sc);
	mt7531_write(sc, reg, val);
	return (0);
}

static int
mt7531_getport(device_t dev, etherswitch_port_t *p)
{
	struct mt7531_softc *sc = device_get_softc(dev);
	struct ifmediareq *ifmr;
	struct mii_data *mii;
	uint32_t pmsr;
	int error;

	if (p->es_port < 0 || p->es_port >= MT7531_NPORTS)
		return (EINVAL);
	MT7531_LOCK(sc);
	p->es_pvid = mt7531_read(sc, MT7531_PPBV1(p->es_port)) &
	    PPBV_PVID_MASK;
	p->es_flags = p->es_port == sc->cpuport ? ETHERSWITCH_PORT_CPU : 0;
	pmsr = mt7531_read(sc, MT7531_PMSR(p->es_port));
	MT7531_UNLOCK(sc);

	if (p->es_port < MT7531_NPHYS && sc->miibus[p->es_port] != NULL) {
		mii = device_get_softc(sc->miibus[p->es_port]);
		return (ifmedia_ioctl(mii->mii_ifp, &p->es_ifr,
		    &mii->mii_media, SIOCGIFMEDIA));
	}
	/* Ports 5 and 6: report the MAC status. */
	ifmr = &p->es_ifmr;
	ifmr->ifm_count = 0;
	ifmr->ifm_mask = 0;
	ifmr->ifm_status = IFM_AVALID |
	    ((pmsr & PMSR_LINK) != 0 ? IFM_ACTIVE : 0);
	ifmr->ifm_active = IFM_ETHER | IFM_2500_X |
	    ((pmsr & PMSR_DPX) != 0 ? IFM_FDX : IFM_HDX);
	ifmr->ifm_current = ifmr->ifm_active;
	error = 0;
	return (error);
}

static int
mt7531_setport(device_t dev, etherswitch_port_t *p)
{
	struct mt7531_softc *sc = device_get_softc(dev);
	struct mii_data *mii;

	if (p->es_port < 0 || p->es_port >= MT7531_NPORTS)
		return (EINVAL);
	if ((p->es_flags & ~ETHERSWITCH_PORT_CPU) != 0)
		return (ENOTSUP);
	if (p->es_pvid != 0) {
		MT7531_LOCK(sc);
		mt7531_rmw(sc, MT7531_PPBV1(p->es_port), PPBV_PVID_MASK,
		    p->es_pvid & PPBV_PVID_MASK);
		MT7531_UNLOCK(sc);
	}
	if (p->es_port < MT7531_NPHYS && sc->miibus[p->es_port] != NULL) {
		mii = device_get_softc(sc->miibus[p->es_port]);
		return (ifmedia_ioctl(mii->mii_ifp, &p->es_ifr,
		    &mii->mii_media, SIOCSIFMEDIA));
	}
	return (0);
}

static int
mt7531_getvgroup(device_t dev, etherswitch_vlangroup_t *vg)
{
	struct mt7531_softc *sc = device_get_softc(dev);
	uint32_t vawd1, vawd2;
	int i, vid;

	if (vg->es_vlangroup < 0 || vg->es_vlangroup >= MT7531_NVLANGROUPS)
		return (EINVAL);
	/* VLAN group n is VID n. */
	vid = vg->es_vlangroup;
	vg->es_vid = vid;
	vg->es_member_ports = 0;
	vg->es_untagged_ports = 0;
	vg->es_fid = 0;
	MT7531_LOCK(sc);
	if (mt7531_vtcr(sc, VTCR_FUNC_READ | vid) != 0) {
		MT7531_UNLOCK(sc);
		return (0);
	}
	vawd1 = mt7531_read(sc, MT7531_VAWD1);
	vawd2 = mt7531_read(sc, MT7531_VAWD2);
	MT7531_UNLOCK(sc);
	if ((vawd1 & VAWD1_VALID) == 0)
		return (0);
	vg->es_vid |= ETHERSWITCH_VID_VALID;
	vg->es_member_ports = (vawd1 >> VAWD1_MEMBER_SHIFT) & VAWD1_MEMBER_MASK;
	for (i = 0; i < MT7531_NPORTS; i++) {
		if ((vawd2 & VAWD2_MASK(i)) == 0)
			vg->es_untagged_ports |= 1u << i;
	}
	vg->es_untagged_ports &= vg->es_member_ports;
	return (0);
}

static int
mt7531_setvgroup(device_t dev, etherswitch_vlangroup_t *vg)
{
	struct mt7531_softc *sc = device_get_softc(dev);
	uint32_t vawd2;
	int error, i;

	if (vg->es_vlangroup < 0 || vg->es_vlangroup >= MT7531_NVLANGROUPS ||
	    vg->es_fid != 0)
		return (EINVAL);
	/* VLAN group n is VID n. */
	if ((vg->es_vid & ETHERSWITCH_VID_MASK) != 0 &&
	    (vg->es_vid & ETHERSWITCH_VID_MASK) != vg->es_vlangroup)
		return (EINVAL);
	vawd2 = 0;
	for (i = 0; i < MT7531_NPORTS; i++) {
		if ((vg->es_member_ports & ~vg->es_untagged_ports &
		    (1u << i)) != 0)
			vawd2 |= VAWD2_TAGGED(i);
	}
	MT7531_LOCK(sc);
	if (vg->es_member_ports == 0) {
		error = mt7531_vtcr(sc,
		    VTCR_FUNC_INVALIDATE | vg->es_vlangroup);
	} else {
		mt7531_write(sc, MT7531_VAWD1, VAWD1_IVL_MAC | VAWD1_VTAG_EN |
		    (vg->es_member_ports & VAWD1_MEMBER_MASK) <<
		    VAWD1_MEMBER_SHIFT | VAWD1_VALID);
		mt7531_write(sc, MT7531_VAWD2, vawd2);
		error = mt7531_vtcr(sc, VTCR_FUNC_WRITE | vg->es_vlangroup);
	}
	mt7531_atu_flush(sc);
	MT7531_UNLOCK(sc);
	return (error);
}

static int
mt7531_getconf(device_t dev, etherswitch_conf_t *conf)
{
	conf->cmd = ETHERSWITCH_CONF_VLAN_MODE;
	conf->vlan_mode = ETHERSWITCH_VLAN_DOT1Q;
	return (0);
}

static int
mt7531_setconf(device_t dev, etherswitch_conf_t *conf)
{
	if ((conf->cmd & ETHERSWITCH_CONF_VLAN_MODE) != 0 &&
	    conf->vlan_mode != ETHERSWITCH_VLAN_DOT1Q)
		return (EINVAL);
	return (0);
}

/* ----------------------------------------------------------- Attach */

static int
mt7531_find_cpuport(phandle_t node)
{
	phandle_t ports, child;
	pcell_t reg;

	ports = ofw_bus_find_child(node, "ports");
	if (ports <= 0)
		ports = ofw_bus_find_child(node, "ethernet-ports");
	if (ports <= 0)
		return (MT7531_CPU_PORT);
	for (child = OF_child(ports); child != 0; child = OF_peer(child)) {
		if (OF_hasprop(child, "ethernet") &&
		    OF_getencprop(child, "reg", &reg, sizeof(reg)) ==
		    sizeof(reg))
			return (reg);
	}
	return (MT7531_CPU_PORT);
}

static int
mt7531_probe(device_t dev)
{
	if (!ofw_bus_status_okay(dev))
		return (ENXIO);
	if (ofw_bus_search_compatible(dev, compat_data)->ocd_data == 0)
		return (ENXIO);
	device_set_desc(dev, "MediaTek MT7531 Ethernet switch");
	return (BUS_PROBE_DEFAULT);
}

static int
mt7531_attach(device_t dev)
{
	struct mt7531_softc *sc = device_get_softc(dev);
	phandle_t node;
	pcell_t reg;
	int error, p;

	sc->dev = dev;
	node = ofw_bus_get_node(dev);
	if (OF_getencprop(node, "reg", &reg, sizeof(reg)) != sizeof(reg)) {
		device_printf(dev, "no MDIO address\n");
		return (ENXIO);
	}
	sc->smi = reg;
	sc->cpuport = mt7531_find_cpuport(node);
	if (sc->cpuport != 5 && sc->cpuport != 6) {
		device_printf(dev, "CPU port %d is not supported\n",
		    sc->cpuport);
		return (ENXIO);
	}
	mtx_init(&sc->mtx, device_get_nameunit(dev), NULL,
	    MTX_DEF | MTX_RECURSE);
	callout_init_mtx(&sc->tick, &sc->mtx, 0);

	if ((error = mt7531_reset(sc, node)) != 0)
		return (error);
	MT7531_LOCK(sc);
	error = mt7531_setup(sc);
	MT7531_UNLOCK(sc);
	if (error != 0)
		return (error);
	device_printf(dev, "rev %d, CPU port %d%s\n", sc->rev, sc->cpuport,
	    sc->dual_sgmii ? ", dual SGMII" : "");

	strlcpy(sc->info.es_name, "MediaTek MT7531", sizeof(sc->info.es_name));
	sc->info.es_nports = MT7531_NPORTS;
	sc->info.es_nvlangroups = MT7531_NVLANGROUPS;
	sc->info.es_vlan_caps = ETHERSWITCH_VLAN_DOT1Q;

	for (p = 0; p < MT7531_NPHYS; p++) {
		sc->ifp[p] = if_alloc(IFT_ETHER);
		if_setsoftc(sc->ifp[p], sc);
		if_setflagbits(sc->ifp[p], IFF_UP | IFF_BROADCAST |
		    IFF_DRV_RUNNING | IFF_SIMPLEX, 0);
		if_initname(sc->ifp[p], "mt7531port", p);
		error = mii_attach(dev, &sc->miibus[p], sc->ifp[p],
		    mt7531_ifmedia_upd, mt7531_ifmedia_sts, BMSR_DEFCAPMASK,
		    p, MII_OFFSET_ANY, 0);
		if (error != 0) {
			device_printf(dev, "port %d: no PHY\n", p);
			sc->miibus[p] = NULL;
		}
	}
	bus_identify_children(dev);
	bus_attach_children(dev);
	MT7531_LOCK(sc);
	callout_reset(&sc->tick, hz, mt7531_tick, sc);
	MT7531_UNLOCK(sc);
	return (0);
}

static device_method_t mt7531_methods[] = {
	DEVMETHOD(device_probe,		mt7531_probe),
	DEVMETHOD(device_attach,	mt7531_attach),

	DEVMETHOD(bus_add_child,	device_add_child_ordered),

	DEVMETHOD(miibus_readreg,	mt7531_miibus_readreg),
	DEVMETHOD(miibus_writereg,	mt7531_miibus_writereg),

	DEVMETHOD(etherswitch_getinfo,	mt7531_getinfo),
	DEVMETHOD(etherswitch_lock,	mt7531_es_lock),
	DEVMETHOD(etherswitch_unlock,	mt7531_es_unlock),
	DEVMETHOD(etherswitch_readreg,	mt7531_es_readreg),
	DEVMETHOD(etherswitch_writereg,	mt7531_es_writereg),
	DEVMETHOD(etherswitch_readphyreg, mt7531_miibus_readreg),
	DEVMETHOD(etherswitch_writephyreg, mt7531_miibus_writereg),
	DEVMETHOD(etherswitch_getport,	mt7531_getport),
	DEVMETHOD(etherswitch_setport,	mt7531_setport),
	DEVMETHOD(etherswitch_getvgroup, mt7531_getvgroup),
	DEVMETHOD(etherswitch_setvgroup, mt7531_setvgroup),
	DEVMETHOD(etherswitch_getconf,	mt7531_getconf),
	DEVMETHOD(etherswitch_setconf,	mt7531_setconf),
	DEVMETHOD_END
};

DEFINE_CLASS_0(mt7531sw, mt7531_driver, mt7531_methods,
    sizeof(struct mt7531_softc));

DRIVER_MODULE(mt7531sw, mtkmdio, mt7531_driver, NULL, NULL);
DRIVER_MODULE(miibus, mt7531sw, miibus_driver, NULL, NULL);
DRIVER_MODULE_ORDERED(etherswitch, mt7531sw, etherswitch_driver, NULL, NULL,
    SI_ORDER_ANY);
MODULE_DEPEND(mt7531sw, etherswitch, 1, 1, 1);
