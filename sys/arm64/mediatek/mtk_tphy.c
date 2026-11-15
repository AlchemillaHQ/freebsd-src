/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

/*
 * MediaTek T-PHY, version 2: the USB 2.0 and USB 3.0 PHY ports of the
 * SSUSB xHCI.  Each port is a child node with its own register window.
 * Whether a port is driven as USB2 or USB3 comes from the consumer's phy
 * cell (PHY_TYPE_USB2 or PHY_TYPE_USB3), as both USB2 ports would
 * otherwise map to the same id.  The register settings follow MediaTek's
 * U-Boot driver, which uses a fixed HS slew rate instead of calibrating.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/rman.h>

#include <machine/bus.h>

#include <dev/clk/clk.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/ofw/ofw_subr.h>
#include <dev/ofw/openfirm.h>
#include <dev/phy/phy_usb.h>

#include <contrib/device-tree/include/dt-bindings/phy/phy.h>

#include "phydev_if.h"
#include "phynode_if.h"
#include "phynode_usb_if.h"

#define	TPHY_MAX_PORTS		4

/* Banks inside a port's register window (version 2). */
#define	U2_COM			0x300
#define	U3_SPLLC		0x000
#define	U3_PHYD			0x200
#define	U3_PHYA			0x400

/* USB2 port, COM bank. */
#define	U2_ACR0			(U2_COM + 0x000)
#define	 ACR0_USB20_INTR_EN	(1u << 5)
#define	U2_ACR4			(U2_COM + 0x020)
#define	 ACR4_GPIO_CTL		(1u << 9)
#define	 ACR4_GPIO_MODE		(1u << 8)
#define	U2_ACR5			(U2_COM + 0x014)
#define	 ACR5_HSTX_SRCTRL_MASK	(7u << 12)
#define	 ACR5_HSTX_SRCTRL(x)	((x) << 12)
#define	 ACR5_HS_100U_U3_EN	(1u << 11)
#define	U2_ACR6			(U2_COM + 0x018)
#define	 ACR6_BC11_SW_EN	(1u << 23)
#define	 ACR6_OTG_VBUSCMP_EN	(1u << 20)
#define	 ACR6_SQTH_MASK		(0xfu << 0)
#define	 ACR6_SQTH(x)		((x) << 0)
#define	U2_DTM0			(U2_COM + 0x068)
#define	 DTM0_FORCE_UART_EN	(1u << 26)
#define	 DTM0_FORCE_DATAIN	(1u << 23)
#define	 DTM0_FORCE_DM_PULLDOWN	(1u << 21)
#define	 DTM0_FORCE_DP_PULLDOWN	(1u << 20)
#define	 DTM0_FORCE_XCVRSEL	(1u << 19)
#define	 DTM0_FORCE_SUSPENDM	(1u << 18)
#define	 DTM0_FORCE_TERMSEL	(1u << 17)
#define	 DTM0_DATAIN_MASK	(0xfu << 10)
#define	 DTM0_DMPULLDOWN	(1u << 7)
#define	 DTM0_DPPULLDOWN	(1u << 6)
#define	 DTM0_XCVRSEL_MASK	(3u << 4)
#define	 DTM0_XCVRSEL(x)	((x) << 4)
#define	 DTM0_TERMSEL		(1u << 2)
#define	 DTM0_FORCE_MASK	(DTM0_FORCE_DATAIN |			\
				    DTM0_FORCE_DM_PULLDOWN |		\
				    DTM0_FORCE_DP_PULLDOWN |		\
				    DTM0_FORCE_XCVRSEL |		\
				    DTM0_FORCE_TERMSEL |		\
				    DTM0_DMPULLDOWN | DTM0_DPPULLDOWN |	\
				    DTM0_TERMSEL)
#define	U2_DTM1			(U2_COM + 0x06c)
#define	 DTM1_UART_EN		(1u << 16)
#define	 DTM1_VBUSVALID		(1u << 5)
#define	 DTM1_SESSEND		(1u << 4)
#define	 DTM1_AVALID		(1u << 2)

/* USB3 port. */
#define	U3_XTALCTL3		(U3_SPLLC + 0x018)
#define	 XTALCTL3_RX_PWD	(1u << 9)
#define	 XTALCTL3_FRC_RX_PWD	(1u << 8)
#define	U3_PHYA_REG6		(U3_PHYA + 0x018)
#define	 REG6_TX_EIDLE_CM_MASK	(0xfu << 28)
#define	 REG6_TX_EIDLE_CM(x)	((uint32_t)(x) << 28)
#define	U3_PHYA_REG9		(U3_PHYA + 0x024)
#define	 REG9_RX_DAC_MUX_MASK	(0x1fu << 1)
#define	 REG9_RX_DAC_MUX(x)	((x) << 1)
#define	U3_PHYA_DA_REG0		(U3_PHYA + 0x100)
#define	 DA0_XTAL_EXT_EN_MASK	(3u << 10)
#define	 DA0_XTAL_EXT_EN(x)	((x) << 10)
#define	U3_PHYD_LFPS1		(U3_PHYD + 0x00c)
#define	 LFPS1_FWAKE_TH_MASK	(0x3fu << 16)
#define	 LFPS1_FWAKE_TH(x)	((x) << 16)
#define	U3_PHYD_CDR1		(U3_PHYD + 0x05c)
#define	 CDR1_BIR_LTD1_MASK	(0x1fu << 24)
#define	 CDR1_BIR_LTD1(x)	((x) << 24)
#define	 CDR1_BIR_LTD0_MASK	(0x1fu << 8)
#define	 CDR1_BIR_LTD0(x)	((x) << 8)
#define	U3_PHYD_RXDET1		(U3_PHYD + 0x128)
#define	 RXDET1_STB2_SET_MASK	(0x1ffu << 9)
#define	 RXDET1_STB2_SET(x)	((x) << 9)
#define	U3_PHYD_RXDET2		(U3_PHYD + 0x12c)
#define	 RXDET2_STB2_SET_P3_MASK 0x1ffu

#define	RD4(port, off)		bus_read_4((port)->res, (off))
#define	WR4(port, off, val)	bus_write_4((port)->res, (off), (val))
#define	MD4(port, off, clr, set)					\
	WR4(port, off, (RD4(port, off) & ~(clr)) | (set))

struct mtk_tphy_port {
	phandle_t		node;
	struct resource		*res;
	clk_t			ref;
	clk_t			da_ref;
	int			type;	/* PHY_TYPE_USB2/USB3, set by map */
	bool			inited;
};

struct mtk_tphy_softc {
	device_t		dev;
	int			nports;
	struct mtk_tphy_port	ports[TPHY_MAX_PORTS];
};

static struct ofw_compat_data compat_data[] = {
	{ "mediatek,mt7986-tphy",	1 },
	{ "mediatek,generic-tphy-v2",	1 },
	{ NULL,				0 }
};

static void
mtk_tphy_u2_init(struct mtk_tphy_port *port)
{
	/* USB function, PLL on. */
	MD4(port, U2_DTM0, DTM0_FORCE_UART_EN | DTM0_FORCE_SUSPENDM,
	    DTM0_XCVRSEL(1));
	MD4(port, U2_DTM1, DTM1_UART_EN, 0);
	MD4(port, U2_ACR0, 0, ACR0_USB20_INTR_EN);
	/* No 100 uA current to the SuperSpeed side. */
	MD4(port, U2_ACR5, ACR5_HS_100U_U3_EN, 0);
	MD4(port, U2_ACR4, ACR4_GPIO_CTL | ACR4_GPIO_MODE, 0);
	/* No BC1.1 path on DP/DM; squelch threshold. */
	MD4(port, U2_ACR6, ACR6_BC11_SW_EN | ACR6_SQTH_MASK, ACR6_SQTH(2));
	/* Fixed HS slew rate, as U-Boot programs it. */
	MD4(port, U2_ACR5, ACR5_HSTX_SRCTRL_MASK, ACR5_HSTX_SRCTRL(4));
}

static void
mtk_tphy_u2_power(struct mtk_tphy_port *port, bool on)
{
	if (on) {
		MD4(port, U2_DTM0, DTM0_XCVRSEL_MASK | DTM0_DATAIN_MASK |
		    DTM0_FORCE_MASK, 0);
		MD4(port, U2_ACR6, 0, ACR6_OTG_VBUSCMP_EN);
		MD4(port, U2_DTM1, DTM1_SESSEND, DTM1_VBUSVALID | DTM1_AVALID);
	} else {
		MD4(port, U2_DTM0, DTM0_XCVRSEL_MASK | DTM0_DATAIN_MASK, 0);
		MD4(port, U2_ACR6, ACR6_OTG_VBUSCMP_EN, 0);
		MD4(port, U2_DTM1, DTM1_VBUSVALID | DTM1_AVALID, DTM1_SESSEND);
	}
}

static void
mtk_tphy_u3_init(struct mtk_tphy_port *port)
{
	/* Gate the PCIe analog XTAL clock and XSQ. */
	MD4(port, U3_XTALCTL3, 0, XTALCTL3_RX_PWD | XTALCTL3_FRC_RX_PWD);
	MD4(port, U3_PHYA_DA_REG0, DA0_XTAL_EXT_EN_MASK, DA0_XTAL_EXT_EN(2));
	MD4(port, U3_PHYA_REG9, REG9_RX_DAC_MUX_MASK, REG9_RX_DAC_MUX(4));
	MD4(port, U3_PHYA_REG6, REG6_TX_EIDLE_CM_MASK, REG6_TX_EIDLE_CM(0xe));
	MD4(port, U3_PHYD_CDR1, CDR1_BIR_LTD0_MASK | CDR1_BIR_LTD1_MASK,
	    CDR1_BIR_LTD0(0xc) | CDR1_BIR_LTD1(3));
	MD4(port, U3_PHYD_LFPS1, LFPS1_FWAKE_TH_MASK, LFPS1_FWAKE_TH(0x34));
	MD4(port, U3_PHYD_RXDET1, RXDET1_STB2_SET_MASK, RXDET1_STB2_SET(0x10));
	MD4(port, U3_PHYD_RXDET2, RXDET2_STB2_SET_P3_MASK, 0x10);
}

static int
mtk_tphy_enable(struct phynode *phynode, bool enable)
{
	struct mtk_tphy_softc *sc;
	struct mtk_tphy_port *port;
	int error;

	sc = device_get_softc(phynode_get_device(phynode));
	port = &sc->ports[phynode_get_id(phynode)];
	if (port->type != PHY_TYPE_USB2 && port->type != PHY_TYPE_USB3)
		return (ENXIO);
	if (enable && !port->inited) {
		if (port->ref != NULL && (error = clk_enable(port->ref)) != 0)
			return (error);
		if (port->da_ref != NULL &&
		    (error = clk_enable(port->da_ref)) != 0) {
			if (port->ref != NULL)
				clk_disable(port->ref);
			return (error);
		}
		if (port->type == PHY_TYPE_USB2)
			mtk_tphy_u2_init(port);
		else
			mtk_tphy_u3_init(port);
		port->inited = true;
	}
	/* The USB3 port has nothing to switch. */
	if (port->type == PHY_TYPE_USB2 && port->inited)
		mtk_tphy_u2_power(port, enable);
	return (0);
}

static int
mtk_tphy_get_mode(struct phynode *phynode, int *mode)
{
	*mode = PHY_USB_MODE_HOST;
	return (0);
}

static int
mtk_tphy_set_mode(struct phynode *phynode, int mode)
{
	return (mode == PHY_USB_MODE_HOST ? 0 : EOPNOTSUPP);
}

static phynode_method_t mtk_tphy_phynode_methods[] = {
	PHYNODEMETHOD(phynode_enable,		mtk_tphy_enable),
	PHYNODEMETHOD(phynode_usb_get_mode,	mtk_tphy_get_mode),
	PHYNODEMETHOD(phynode_usb_set_mode,	mtk_tphy_set_mode),
	PHYNODEMETHOD_END
};
DEFINE_CLASS_1(mtk_tphy_phynode, mtk_tphy_phynode_class,
    mtk_tphy_phynode_methods, sizeof(struct phynode_usb_sc),
    phynode_usb_class);

static int
mtk_tphy_map(device_t dev, phandle_t xref, int ncells, pcell_t *cells,
    intptr_t *id)
{
	struct mtk_tphy_softc *sc = device_get_softc(dev);
	phandle_t node;
	int i;

	if (ncells != 1)
		return (ERANGE);
	if (cells[0] != PHY_TYPE_USB2 && cells[0] != PHY_TYPE_USB3)
		return (EINVAL);
	node = OF_node_from_xref(xref);
	for (i = 0; i < sc->nports; i++) {
		if (sc->ports[i].node == node) {
			sc->ports[i].type = cells[0];
			*id = i;
			return (0);
		}
	}
	return (ENXIO);
}

static int
mtk_tphy_probe(device_t dev)
{
	if (!ofw_bus_status_okay(dev))
		return (ENXIO);
	if (ofw_bus_search_compatible(dev, compat_data)->ocd_data == 0)
		return (ENXIO);
	device_set_desc(dev, "MediaTek USB T-PHY");
	return (BUS_PROBE_DEFAULT);
}

static int
mtk_tphy_attach(device_t dev)
{
	struct mtk_tphy_softc *sc = device_get_softc(dev);
	struct phynode_init_def def;
	struct mtk_tphy_port *port;
	struct phynode *phynode;
	bus_addr_t paddr;
	bus_size_t psize;
	phandle_t child;
	int rid;

	sc->dev = dev;
	for (child = OF_child(ofw_bus_get_node(dev)); child != 0;
	    child = OF_peer(child)) {
		if (!ofw_bus_node_status_okay(child))
			continue;
		if (sc->nports == TPHY_MAX_PORTS) {
			device_printf(dev, "too many ports\n");
			break;
		}
		port = &sc->ports[sc->nports];
		port->node = child;
		/* The ports are only described relative to our "ranges". */
		if (ofw_reg_to_paddr(child, 0, &paddr, &psize, NULL) != 0) {
			device_printf(dev, "port %d: no registers\n",
			    sc->nports);
			return (ENXIO);
		}
		rid = sc->nports;
		port->res = bus_alloc_resource(dev, SYS_RES_MEMORY, &rid,
		    paddr, paddr + psize - 1, psize, RF_ACTIVE);
		if (port->res == NULL) {
			device_printf(dev, "port %d: cannot map registers\n",
			    sc->nports);
			return (ENXIO);
		}
		/* Both clocks are optional; the USB3 port has only "ref". */
		(void)clk_get_by_ofw_name(dev, child, "ref", &port->ref);
		(void)clk_get_by_ofw_name(dev, child, "da_ref", &port->da_ref);

		memset(&def, 0, sizeof(def));
		def.id = sc->nports;
		def.ofw_node = child;
		phynode = phynode_create(dev, &mtk_tphy_phynode_class, &def);
		if (phynode == NULL || phynode_register(phynode) == NULL) {
			device_printf(dev, "port %d: cannot register\n",
			    sc->nports);
			return (ENXIO);
		}
		sc->nports++;
	}
	if (sc->nports == 0)
		return (ENXIO);
	return (0);
}

static device_method_t mtk_tphy_methods[] = {
	DEVMETHOD(device_probe,		mtk_tphy_probe),
	DEVMETHOD(device_attach,	mtk_tphy_attach),

	DEVMETHOD(phydev_map,		mtk_tphy_map),

	DEVMETHOD_END
};

static DEFINE_CLASS_0(mtk_tphy, mtk_tphy_driver, mtk_tphy_methods,
    sizeof(struct mtk_tphy_softc));

EARLY_DRIVER_MODULE(mtk_tphy, simplebus, mtk_tphy_driver, NULL, NULL,
    BUS_PASS_SUPPORTDEV + BUS_PASS_ORDER_MIDDLE);
