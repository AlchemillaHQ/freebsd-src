/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

/*
 * MediaTek MT7986 ETHSYS and SGMIISYS: the clock gates of the network
 * subsystem.
 *
 * Each gate is a bit in one register, set when the clock runs; there are
 * no set/clear registers.  ETHSYS has the frame engine, GMAC and WO CPU
 * gates at 0x30.  SGMIISYS0/1 have the SerDes clock gates of the two SGMII
 * interfaces at 0xe4.
 *
 * The nodes are also syscons: the Ethernet driver reaches the ETHSYS
 * system and reset registers and the SGMII PCS registers through them.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>

#include <machine/bus.h>

#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <dev/clk/mediatek/mtk_clk.h>
#include <dev/syscon/syscon.h>

#include <dt-bindings/clock/mt7986-clk.h>

#include "syscon_if.h"

#define	ETHSYS_CG		0x30
#define	SGMIISYS_CG		0xe4

#define	ETHSYS_GATE(_id, _name, _parent, _reg, _bit)			\
	{ .type = MTK_CLK_GATE, .gate = {				\
	    .clkdef = { .name = (_name), .id = (_id),			\
		.parent_names = (const char *[]){ _parent },		\
		.parent_cnt = 1 },					\
	    .sta_reg = (_reg), .bit = (_bit),				\
	    .flags = MTK_GATE_INVERTED | MTK_GATE_NO_SETCLR } }

static struct mtk_clk_def mt7986_ethsys_clks[] = {
	ETHSYS_GATE(CLK_ETH_FE_EN, "eth_fe_en", "netsys_2x_sel",
	    ETHSYS_CG, 6),
	ETHSYS_GATE(CLK_ETH_GP2_EN, "eth_gp2_en", "sgm_325m_sel",
	    ETHSYS_CG, 7),
	ETHSYS_GATE(CLK_ETH_GP1_EN, "eth_gp1_en", "sgm_325m_sel",
	    ETHSYS_CG, 8),
	ETHSYS_GATE(CLK_ETH_WOCPU1_EN, "eth_wocpu1_en", "netsys_mcu_sel",
	    ETHSYS_CG, 14),
	ETHSYS_GATE(CLK_ETH_WOCPU0_EN, "eth_wocpu0_en", "netsys_mcu_sel",
	    ETHSYS_CG, 15),
};

static struct mtk_clk_def mt7986_sgmiisys0_clks[] = {
	ETHSYS_GATE(CLK_SGMII0_TX250M_EN, "sgmii0_tx250m_en", "top_xtal",
	    SGMIISYS_CG, 2),
	ETHSYS_GATE(CLK_SGMII0_RX250M_EN, "sgmii0_rx250m_en", "top_xtal",
	    SGMIISYS_CG, 3),
	ETHSYS_GATE(CLK_SGMII0_CDR_REF, "sgmii0_cdr_ref", "top_xtal",
	    SGMIISYS_CG, 4),
	ETHSYS_GATE(CLK_SGMII0_CDR_FB, "sgmii0_cdr_fb", "top_xtal",
	    SGMIISYS_CG, 5),
};

static struct mtk_clk_def mt7986_sgmiisys1_clks[] = {
	ETHSYS_GATE(CLK_SGMII1_TX250M_EN, "sgmii1_tx250m_en", "top_xtal",
	    SGMIISYS_CG, 2),
	ETHSYS_GATE(CLK_SGMII1_RX250M_EN, "sgmii1_rx250m_en", "top_xtal",
	    SGMIISYS_CG, 3),
	ETHSYS_GATE(CLK_SGMII1_CDR_REF, "sgmii1_cdr_ref", "top_xtal",
	    SGMIISYS_CG, 4),
	ETHSYS_GATE(CLK_SGMII1_CDR_FB, "sgmii1_cdr_fb", "top_xtal",
	    SGMIISYS_CG, 5),
};

static const struct mtk_clk_desc mt7986_ethsys_desc = {
	.clks = mt7986_ethsys_clks,
	.nclks = nitems(mt7986_ethsys_clks),
};

static const struct mtk_clk_desc mt7986_sgmiisys0_desc = {
	.clks = mt7986_sgmiisys0_clks,
	.nclks = nitems(mt7986_sgmiisys0_clks),
};

static const struct mtk_clk_desc mt7986_sgmiisys1_desc = {
	.clks = mt7986_sgmiisys1_clks,
	.nclks = nitems(mt7986_sgmiisys1_clks),
};

static struct ofw_compat_data compat_data[] = {
	{ "mediatek,mt7986-ethsys",	(uintptr_t)&mt7986_ethsys_desc },
	{ "mediatek,mt7986-sgmiisys_0",	(uintptr_t)&mt7986_sgmiisys0_desc },
	{ "mediatek,mt7986-sgmiisys_1",	(uintptr_t)&mt7986_sgmiisys1_desc },
	{ NULL,				0 }
};

struct mt7986_ethsys_softc {
	struct mtk_clk_softc	clk;	/* first: used by mtk_clk_attach() */
	struct syscon		*syscon;
};

/*
 * Syscon access shares the lock of the clock gates, which are in the same
 * register block.
 */
static uint32_t
mt7986_ethsys_syscon_read_4(struct syscon *syscon, bus_size_t offset)
{
	struct mt7986_ethsys_softc *sc = device_get_softc(syscon->pdev);

	mtx_assert(&sc->clk.mtx, MA_OWNED);
	return (bus_read_4(sc->clk.res, offset));
}

static int
mt7986_ethsys_syscon_write_4(struct syscon *syscon, bus_size_t offset,
    uint32_t val)
{
	struct mt7986_ethsys_softc *sc = device_get_softc(syscon->pdev);

	mtx_assert(&sc->clk.mtx, MA_OWNED);
	bus_write_4(sc->clk.res, offset, val);
	return (0);
}

static int
mt7986_ethsys_syscon_modify_4(struct syscon *syscon, bus_size_t offset,
    uint32_t clr, uint32_t set)
{
	struct mt7986_ethsys_softc *sc = device_get_softc(syscon->pdev);
	uint32_t val;

	mtx_assert(&sc->clk.mtx, MA_OWNED);
	val = bus_read_4(sc->clk.res, offset);
	val &= ~clr;
	val |= set;
	bus_write_4(sc->clk.res, offset, val);
	return (0);
}

static syscon_method_t mt7986_ethsys_syscon_methods[] = {
	SYSCONMETHOD(syscon_unlocked_read_4,	mt7986_ethsys_syscon_read_4),
	SYSCONMETHOD(syscon_unlocked_write_4,	mt7986_ethsys_syscon_write_4),
	SYSCONMETHOD(syscon_unlocked_modify_4,	mt7986_ethsys_syscon_modify_4),
	SYSCONMETHOD_END
};

DEFINE_CLASS_1(mt7986_ethsys_syscon, mt7986_ethsys_syscon_class,
    mt7986_ethsys_syscon_methods, 0, syscon_class);

static void
mt7986_ethsys_syscon_lock(device_t dev)
{
	struct mt7986_ethsys_softc *sc = device_get_softc(dev);

	mtx_lock(&sc->clk.mtx);
}

static void
mt7986_ethsys_syscon_unlock(device_t dev)
{
	struct mt7986_ethsys_softc *sc = device_get_softc(dev);

	mtx_unlock(&sc->clk.mtx);
}

static int
mt7986_ethsys_probe(device_t dev)
{
	if (!ofw_bus_status_okay(dev))
		return (ENXIO);
	if (ofw_bus_search_compatible(dev, compat_data)->ocd_data == 0)
		return (ENXIO);
	device_set_desc(dev, "MediaTek MT7986 network clock controller");
	return (BUS_PROBE_DEFAULT);
}

static int
mt7986_ethsys_attach(device_t dev)
{
	struct mt7986_ethsys_softc *sc = device_get_softc(dev);
	const struct mtk_clk_desc *desc;
	int rv;

	desc = (const struct mtk_clk_desc *)
	    ofw_bus_search_compatible(dev, compat_data)->ocd_data;
	if ((rv = mtk_clk_attach(dev, desc)) != 0)
		return (rv);
	/* The clocks stay registered: clock domains cannot be removed. */
	sc->syscon = syscon_create_ofw_node(dev, &mt7986_ethsys_syscon_class,
	    ofw_bus_get_node(dev));
	if (sc->syscon == NULL)
		device_printf(dev, "cannot register syscon\n");
	return (0);
}

static device_method_t mt7986_ethsys_methods[] = {
	DEVMETHOD(device_probe,		mt7986_ethsys_probe),
	DEVMETHOD(device_attach,	mt7986_ethsys_attach),
	DEVMETHOD(syscon_device_lock,	mt7986_ethsys_syscon_lock),
	DEVMETHOD(syscon_device_unlock,	mt7986_ethsys_syscon_unlock),
	DEVMETHOD_END
};

DEFINE_CLASS_1(mt7986_ethsys, mt7986_ethsys_driver, mt7986_ethsys_methods,
    sizeof(struct mt7986_ethsys_softc), mtk_clk_driver);

/* After TOPCKGEN, which provides the parents of these clocks. */
EARLY_DRIVER_MODULE(mt7986_ethsys, simplebus, mt7986_ethsys_driver,
    NULL, NULL, BUS_PASS_BUS + BUS_PASS_ORDER_MIDDLE + 3);
