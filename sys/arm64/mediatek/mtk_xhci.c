/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

/*
 * MediaTek SSUSB host: an xHCI with a vendor IP port control (IPPC) block
 * in front of it.  Before the generic xHCI code may touch the controller,
 * its clocks and PHYs have to be on and the IPPC has to power up the host
 * IP and switch every port to host mode.  The sequence follows MediaTek's
 * Linux and U-Boot drivers.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/condvar.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/rman.h>

#include <machine/bus.h>

#include <dev/usb/usb.h>
#include <dev/usb/usbdi.h>

#include <dev/usb/usb_core.h>
#include <dev/usb/usb_busdma.h>
#include <dev/usb/usb_process.h>
#include <dev/usb/usb_device.h>

#include <dev/usb/usb_controller.h>
#include <dev/usb/usb_bus.h>
#include <dev/usb/controller/generic_xhci.h>
#include <dev/usb/controller/xhci.h>

#include <dev/clk/clk.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/phy/phy_usb.h>

/* IPPC registers. */
#define	IPPC_IP_PW_CTRL0	0x00
#define	 CTRL0_IP_SW_RST	(1u << 0)
#define	IPPC_IP_PW_CTRL1	0x04
#define	 CTRL1_IP_HOST_PDN	(1u << 0)
#define	IPPC_IP_PW_CTRL2	0x08
#define	 CTRL2_IP_DEV_PDN	(1u << 0)
#define	IPPC_IP_PW_STS1		0x10
#define	 STS1_U3_MAC_RST	(1u << 16)	/* 1: reset released */
#define	 STS1_XHCI_RST		(1u << 11)
#define	 STS1_SYS125_RST	(1u << 10)
#define	 STS1_REF_RST		(1u << 8)
#define	 STS1_SYSPLL_STABLE	(1u << 0)
#define	IPPC_IP_XHCI_CAP	0x24
#define	 CAP_U3_PORTS(x)	((x) & 0xff)
#define	 CAP_U2_PORTS(x)	(((x) >> 8) & 0xff)
#define	IPPC_U3_CTRL(p)		(0x30 + (p) * 8)
#define	IPPC_U2_CTRL(p)		(0x50 + (p) * 8)
#define	 CTRL_PORT_HOST_SEL	(1u << 2)
#define	 CTRL_PORT_PDN		(1u << 1)
#define	 CTRL_PORT_DIS		(1u << 0)

/* Scheduling fields in the reserved endpoint context dwords 5 and 6. */
#define	EP_BPKTS(x)		((x) & 0x7f)	/* packets per microframe */
#define	EP_BCSCOUNT(x)		(((x) & 0x7) << 8) /* complete-splits */
#define	EP_BBM			(1u << 11)	/* SuperSpeed burst mode */
#define	EP_BREPEAT(x)		(((x) & 0x7fff) << 16)
#define	FS_PAYLOAD_MAX		188	/* full-speed bytes per microframe */
#define	MTK_XHCI_MAX_ESIT	64	/* microframes */

#define	MTK_XHCI_MAXPHYS	8
#define	MTK_XHCI_MAXPORTS	4	/* per speed, in the IPPC */
#define	MTK_XHCI_STABLE_US	20000

#define	IPPC_RD4(sc, off)	bus_read_4((sc)->ippc, (off))
#define	IPPC_WR4(sc, off, val)	bus_write_4((sc)->ippc, (off), (val))
#define	IPPC_MD4(sc, off, clr, set)					\
	IPPC_WR4(sc, off, (IPPC_RD4(sc, off) & ~(clr)) | (set))

static const char *mtk_xhci_clk_names[] = {
	"sys_ck", "ref_ck", "mcu_ck", "dma_ck", "xhci_ck"
};

struct mtk_xhci_softc {
	struct xhci_softc	xhci;	/* must be first */
	struct resource		*ippc;
	clk_t			clks[nitems(mtk_xhci_clk_names)];
	phy_t			phys[MTK_XHCI_MAXPHYS];
	int			nphys;
	int			nu2ports;
	int			nu3ports;
};

static struct ofw_compat_data compat_data[] = {
	{ "mediatek,mt7986-xhci",	1 },
	{ "mediatek,mtk-xhci",		1 },
	{ NULL,				0 }
};

/*
 * The host schedules periodic endpoints from vendor fields in the reserved
 * dwords 5 and 6 of the endpoint context; while they are zero, interrupt
 * and isochronous endpoints get no transactions.  The values are those of
 * MediaTek's Linux scheduler for an endpoint on its own: every endpoint
 * starts in the first microframe of its interval, and bandwidth is not
 * accounted.
 */
static void
mtk_xhci_endp_quirk(device_t dev, struct usb_device *udev,
    struct xhci_endp_ctx *endp)
{
	uint32_t ctx0, ctx1, dw5, dw6;
	u_int esit, maxp, n, pkts, type;

	ctx0 = le32toh(endp->dwEpCtx0);
	ctx1 = le32toh(endp->dwEpCtx1);
	type = XHCI_EPCTX_1_EPTYPE_GET(ctx1);
	maxp = XHCI_EPCTX_1_MAXP_SIZE_GET(ctx1);
	dw5 = dw6 = 0;

	/* Types 1 and 5 are isochronous, 3 and 7 interrupt. */
	if ((type & 1) == 0)
		goto out;
	if (maxp == 0) {
		dw5 = EP_BPKTS(1);
		goto out;
	}
	switch (udev->speed) {
	case USB_SPEED_LOW:
	case USB_SPEED_FULL:
		dw5 = EP_BPKTS(1);
		/* The root hub schedules its own LS/FS ports. */
		if (udev->parent_hs_hub == NULL ||
		    udev->parent_hs_hub->parent_hub == NULL)
			break;
		/*
		 * Behind a TT: the start-split goes in microframe 0 and the
		 * complete-splits from microframe 2, plus one or two extra.
		 */
		n = howmany(maxp, FS_PAYLOAD_MAX);
		if (type != 1)		/* all but isochronous OUT */
			n += (n + 1 == 7) ? 1 : 2;
		dw5 |= EP_BCSCOUNT(MIN(n, 7));
		break;
	case USB_SPEED_HIGH:
		dw5 = EP_BPKTS(XHCI_EPCTX_1_MAXB_GET(ctx1) + 1);
		break;
	default:
		n = howmany(XHCI_EPCTX_4_MAX_ESIT_PAYLOAD_GET(
		    le32toh(endp->dwEpCtx4)), maxp);
		if (n == 0)
			n = (XHCI_EPCTX_0_MULT_GET(ctx0) + 1) *
			    (XHCI_EPCTX_1_MAXB_GET(ctx1) + 1);
		pkts = n;
		if ((type & 3) == 1) {
			/* Isochronous: spread over the interval. */
			esit = MIN(1u << XHCI_EPCTX_0_IVAL_GET(ctx0),
			    MTK_XHCI_MAX_ESIT);
			if (esit > 1 && n <= esit)
				pkts = 1;
			else if (esit > 1)
				pkts = (1u << fls(n - 1)) / esit;
			if (howmany(n, pkts) > 1)
				dw6 = EP_BREPEAT(1);
		}
		dw5 = EP_BBM | EP_BPKTS(pkts);
		break;
	}
out:
	endp->dwEpCtx5 = htole32(dw5);
	endp->dwEpCtx6 = htole32(dw6);
}

static int
mtk_xhci_ippc_init(device_t dev, struct mtk_xhci_softc *sc)
{
	uint32_t cap, mask;
	int i, us;

	/* Reset the whole IP; the device side stays powered down. */
	IPPC_MD4(sc, IPPC_IP_PW_CTRL0, 0, CTRL0_IP_SW_RST);
	DELAY(1);
	IPPC_MD4(sc, IPPC_IP_PW_CTRL0, CTRL0_IP_SW_RST, 0);
	IPPC_MD4(sc, IPPC_IP_PW_CTRL2, 0, CTRL2_IP_DEV_PDN);

	cap = IPPC_RD4(sc, IPPC_IP_XHCI_CAP);
	sc->nu3ports = MIN(CAP_U3_PORTS(cap), MTK_XHCI_MAXPORTS);
	sc->nu2ports = MIN(CAP_U2_PORTS(cap), MTK_XHCI_MAXPORTS);

	/* Power on the host IP and give it every port. */
	IPPC_MD4(sc, IPPC_IP_PW_CTRL1, CTRL1_IP_HOST_PDN, 0);
	for (i = 0; i < sc->nu3ports; i++)
		IPPC_MD4(sc, IPPC_U3_CTRL(i), CTRL_PORT_PDN | CTRL_PORT_DIS,
		    CTRL_PORT_HOST_SEL);
	for (i = 0; i < sc->nu2ports; i++)
		IPPC_MD4(sc, IPPC_U2_CTRL(i), CTRL_PORT_PDN | CTRL_PORT_DIS,
		    CTRL_PORT_HOST_SEL);

	/* Wait for the PLL and for the clock domains to leave reset. */
	mask = STS1_SYSPLL_STABLE | STS1_REF_RST | STS1_SYS125_RST |
	    STS1_XHCI_RST;
	if (sc->nu3ports > 0)
		mask |= STS1_U3_MAC_RST;
	for (us = 0; us < MTK_XHCI_STABLE_US; us += 10) {
		if ((IPPC_RD4(sc, IPPC_IP_PW_STS1) & mask) == mask)
			return (0);
		DELAY(10);
	}
	device_printf(dev, "clocks not stable, STS1 %#x\n",
	    IPPC_RD4(sc, IPPC_IP_PW_STS1));
	return (ETIMEDOUT);
}

static void
mtk_xhci_ippc_off(struct mtk_xhci_softc *sc)
{
	int i;

	for (i = 0; i < sc->nu3ports; i++)
		IPPC_MD4(sc, IPPC_U3_CTRL(i), 0, CTRL_PORT_PDN | CTRL_PORT_DIS);
	for (i = 0; i < sc->nu2ports; i++)
		IPPC_MD4(sc, IPPC_U2_CTRL(i), 0, CTRL_PORT_PDN | CTRL_PORT_DIS);
	IPPC_MD4(sc, IPPC_IP_PW_CTRL1, 0, CTRL1_IP_HOST_PDN);
}

static void
mtk_xhci_release(device_t dev)
{
	struct mtk_xhci_softc *sc = device_get_softc(dev);
	int i;

	if (sc->ippc != NULL) {
		mtk_xhci_ippc_off(sc);
		bus_release_resource(dev, SYS_RES_MEMORY,
		    rman_get_rid(sc->ippc), sc->ippc);
		sc->ippc = NULL;
	}
	for (i = sc->nphys - 1; i >= 0; i--) {
		phy_disable(sc->phys[i]);
		phy_release(sc->phys[i]);
	}
	sc->nphys = 0;
	for (i = nitems(sc->clks) - 1; i >= 0; i--) {
		if (sc->clks[i] != NULL) {
			clk_disable(sc->clks[i]);
			clk_release(sc->clks[i]);
			sc->clks[i] = NULL;
		}
	}
}

static int
mtk_xhci_probe(device_t dev)
{
	if (!ofw_bus_status_okay(dev))
		return (ENXIO);
	if (ofw_bus_search_compatible(dev, compat_data)->ocd_data == 0)
		return (ENXIO);
	device_set_desc(dev, "MediaTek USB 3.0 controller");
	return (BUS_PROBE_DEFAULT);
}

static int
mtk_xhci_attach(device_t dev)
{
	struct mtk_xhci_softc *sc = device_get_softc(dev);
	phandle_t node = ofw_bus_get_node(dev);
	phy_t phy;
	clk_t clk;
	int error, i, nphys, rid;

	/* Keep only enabled clocks and PHYs, so that release balances. */
	for (i = 0; i < nitems(mtk_xhci_clk_names); i++) {
		error = clk_get_by_ofw_name(dev, 0, mtk_xhci_clk_names[i],
		    &clk);
		if (error == 0 && (error = clk_enable(clk)) != 0)
			clk_release(clk);
		if (error != 0) {
			device_printf(dev, "cannot enable clock %s\n",
			    mtk_xhci_clk_names[i]);
			goto fail;
		}
		sc->clks[i] = clk;
	}

	if (ofw_bus_parse_xref_list_get_length(node, "phys", "#phy-cells",
	    &nphys) != 0)
		nphys = 0;
	for (i = 0; i < MIN(nphys, MTK_XHCI_MAXPHYS); i++) {
		error = phy_get_by_ofw_idx(dev, node, i, &phy);
		if (error != 0) {
			device_printf(dev, "cannot get phy %d\n", i);
			goto fail;
		}
		(void)phy_usb_set_mode(phy, PHY_USB_MODE_HOST);
		if ((error = phy_enable(phy)) != 0) {
			phy_release(phy);
			device_printf(dev, "cannot enable phy %d\n", i);
			goto fail;
		}
		sc->phys[sc->nphys++] = phy;
	}

	if (ofw_bus_find_string_index(node, "reg-names", "ippc", &rid) != 0) {
		device_printf(dev, "no ippc registers\n");
		error = ENXIO;
		goto fail;
	}
	sc->ippc = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
	    RF_ACTIVE);
	if (sc->ippc == NULL) {
		device_printf(dev, "cannot map ippc registers\n");
		error = ENXIO;
		goto fail;
	}
	if ((error = mtk_xhci_ippc_init(dev, sc)) != 0)
		goto fail;
	if (bootverbose)
		device_printf(dev, "%d USB3 and %d USB2 ports\n",
		    sc->nu3ports, sc->nu2ports);

	sc->xhci.sc_endp_quirk = mtk_xhci_endp_quirk;
	error = generic_xhci_attach(dev);
	if (error == 0)
		return (0);
fail:
	mtk_xhci_release(dev);
	return (error);
}

static int
mtk_xhci_detach(device_t dev)
{
	int error;

	if ((error = generic_xhci_detach(dev)) != 0)
		return (error);
	mtk_xhci_release(dev);
	return (0);
}

static device_method_t mtk_xhci_methods[] = {
	DEVMETHOD(device_probe,		mtk_xhci_probe),
	DEVMETHOD(device_attach,	mtk_xhci_attach),
	DEVMETHOD(device_detach,	mtk_xhci_detach),

	DEVMETHOD_END
};

DEFINE_CLASS_1(xhci, mtk_xhci_driver, mtk_xhci_methods,
    sizeof(struct mtk_xhci_softc), generic_xhci_driver);

DRIVER_MODULE(mtk_xhci, simplebus, mtk_xhci_driver, 0, 0);
MODULE_DEPEND(mtk_xhci, usb, 1, 1, 1);
