/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

/*
 * MediaTek clock gate with write-1-to-set and write-1-to-clear registers
 * and a status register, or (MTK_GATE_NO_SETCLR) a bit in one register.
 * A set status bit normally means "gated".
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>

#include <dev/clk/clk.h>
#include <dev/clk/mediatek/mtk_clk.h>

#include "clkdev_if.h"

struct mtk_clk_gate_sc {
	uint32_t	set_reg;
	uint32_t	clr_reg;
	uint32_t	sta_reg;
	uint32_t	mask;
	bool		inverted;
	bool		no_setclr;
};

#define	RD4(_clk, _off, _val)						\
	CLKDEV_READ_4(clknode_get_device(_clk), _off, _val)
#define	WR4(_clk, _off, _val)						\
	CLKDEV_WRITE_4(clknode_get_device(_clk), _off, _val)
#define	MD4(_clk, _off, _clr, _set)					\
	CLKDEV_MODIFY_4(clknode_get_device(_clk), _off, _clr, _set)
#define	DEVICE_LOCK(_clk)						\
	CLKDEV_DEVICE_LOCK(clknode_get_device(_clk))
#define	DEVICE_UNLOCK(_clk)						\
	CLKDEV_DEVICE_UNLOCK(clknode_get_device(_clk))

static int
mtk_clk_gate_init(struct clknode *clk, device_t dev)
{
	clknode_init_parent_idx(clk, 0);
	return (0);
}

static int
mtk_clk_gate_set_gate(struct clknode *clk, bool enable)
{
	struct mtk_clk_gate_sc *sc = clknode_get_softc(clk);

	DEVICE_LOCK(clk);
	/* Writing the set register sets the status bit. */
	if (sc->no_setclr)
		MD4(clk, sc->sta_reg, enable != sc->inverted ? sc->mask : 0,
		    enable != sc->inverted ? 0 : sc->mask);
	else if (enable != sc->inverted)
		WR4(clk, sc->clr_reg, sc->mask);
	else
		WR4(clk, sc->set_reg, sc->mask);
	DEVICE_UNLOCK(clk);
	return (0);
}

static int
mtk_clk_gate_get_gate(struct clknode *clk, bool *enabled)
{
	struct mtk_clk_gate_sc *sc = clknode_get_softc(clk);
	uint32_t val;

	DEVICE_LOCK(clk);
	RD4(clk, sc->sta_reg, &val);
	DEVICE_UNLOCK(clk);
	*enabled = ((val & sc->mask) == 0) != sc->inverted;
	return (0);
}

static clknode_method_t mtk_clk_gate_methods[] = {
	CLKNODEMETHOD(clknode_init,		mtk_clk_gate_init),
	CLKNODEMETHOD(clknode_set_gate,		mtk_clk_gate_set_gate),
	CLKNODEMETHOD(clknode_get_gate,		mtk_clk_gate_get_gate),
	CLKNODEMETHOD_END
};

DEFINE_CLASS_1(mtk_clk_gate_clknode, mtk_clk_gate_clknode_class,
    mtk_clk_gate_methods, sizeof(struct mtk_clk_gate_sc), clknode_class);

int
mtk_clk_gate_register(struct clkdom *clkdom,
    const struct mtk_clk_gate_def *def)
{
	struct clknode *clk;
	struct mtk_clk_gate_sc *sc;

	clk = clknode_create(clkdom, &mtk_clk_gate_clknode_class,
	    &def->clkdef);
	if (clk == NULL)
		return (ENXIO);

	sc = clknode_get_softc(clk);
	sc->set_reg = def->set_reg;
	sc->clr_reg = def->clr_reg;
	sc->sta_reg = def->sta_reg;
	sc->mask = 1u << def->bit;
	sc->inverted = (def->flags & MTK_GATE_INVERTED) != 0;
	sc->no_setclr = (def->flags & MTK_GATE_NO_SETCLR) != 0;

	if (clknode_register(clkdom, clk) == NULL)
		return (ENXIO);
	return (0);
}
