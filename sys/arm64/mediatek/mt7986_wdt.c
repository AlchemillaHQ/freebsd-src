/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/eventhandler.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/watchdog.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <dev/hwreset/hwreset.h>
#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <dt-bindings/reset/mt7986-resets.h>

#include "hwreset_if.h"

#define	MT7986_WDT_MODE		0x00
#define	 MT7986_MODE_KEY		0x22000000
#define	 MT7986_MODE_CNT_SEL	(1u << 8)
#define	 MT7986_MODE_DUAL	(1u << 6)
#define	 MT7986_MODE_IRQ		(1u << 3)
#define	 MT7986_MODE_EXTEN	(1u << 2)
#define	 MT7986_MODE_ENABLE	(1u << 0)
#define	MT7986_WDT_LENGTH	0x04
#define	 MT7986_LENGTH_KEY	0x08
#define	 MT7986_LENGTH_SHIFT	5
#define	 MT7986_LENGTH_MAX	0x7ff
#define	MT7986_WDT_RESTART	0x08
#define	 MT7986_RESTART_KEY	0x1971
#define	MT7986_WDT_SWSYSRST	0x18
#define	 MT7986_SWSYSRST_KEY	0x88000000
#define	MT7986_WDT_KEY_MASK	0xff000000

/* 512 cycles of the nominal 32.768 kHz clock per length unit. */
#define	MT7986_WDT_UNIT		(SBT_1S / 64)
#define	MT7986_WDT_MAX		(MT7986_LENGTH_MAX * MT7986_WDT_UNIT)

struct mt7986_wdt_softc {
	struct resource		*res;
	struct mtx		mtx;
	eventhandler_tag	watchdog_tag;
	bool			disable_extrst;
	bool			reset_by_toprgu;
	bool			shutting_down;
};

static void
mt7986_wdt_write(struct mt7986_wdt_softc *sc, bus_size_t reg, uint32_t value)
{

	mtx_assert(&sc->mtx, MA_OWNED);
	bus_write_4(sc->res, reg, value);
	bus_barrier(sc->res, reg, 4, BUS_SPACE_BARRIER_WRITE);
}

static void
mt7986_wdt_stop(struct mt7986_wdt_softc *sc)
{
	uint32_t mode;

	mtx_assert(&sc->mtx, MA_OWNED);
	mode = bus_read_4(sc->res, MT7986_WDT_MODE);
	mode &= ~(MT7986_WDT_KEY_MASK | MT7986_MODE_ENABLE);
	mt7986_wdt_write(sc, MT7986_WDT_MODE, mode | MT7986_MODE_KEY);
}

static void
mt7986_wdt_event(void *arg, sbintime_t timeout, sbintime_t *error_sbt,
    int *error)
{
	struct mt7986_wdt_softc *sc = arg;
	uint32_t length, mode;

	mtx_lock_spin(&sc->mtx);
	/* Also disarm an old timeout when the new request is unsupported. */
	mt7986_wdt_stop(sc);
	if (timeout <= 0 || timeout > MT7986_WDT_MAX || sc->shutting_down)
		goto out;

	/* Round up, never truncate a request or wrap the eleven-bit field. */
	length = howmany(timeout, MT7986_WDT_UNIT);
	mt7986_wdt_write(sc, MT7986_WDT_LENGTH,
	    (length << MT7986_LENGTH_SHIFT) | MT7986_LENGTH_KEY);
	mt7986_wdt_write(sc, MT7986_WDT_RESTART, MT7986_RESTART_KEY);
	mode = bus_read_4(sc->res, MT7986_WDT_MODE);
	mode &= ~(MT7986_WDT_KEY_MASK | MT7986_MODE_DUAL | MT7986_MODE_IRQ);
	if (sc->disable_extrst)
		mode &= ~MT7986_MODE_EXTEN;
	else
		mode |= MT7986_MODE_EXTEN;
	if (sc->reset_by_toprgu)
		mode |= MT7986_MODE_CNT_SEL;
	mt7986_wdt_write(sc, MT7986_WDT_MODE,
	    mode | MT7986_MODE_ENABLE | MT7986_MODE_KEY);
	*error_sbt = length * MT7986_WDT_UNIT - timeout;
	*error = 0;
out:
	mtx_unlock_spin(&sc->mtx);
}

static bool
mt7986_wdt_reset_valid(intptr_t id)
{

	switch (id) {
	case MT7986_TOPRGU_APMIXEDSYS_SW_RST:
	case MT7986_TOPRGU_SGMII0_SW_RST:
	case MT7986_TOPRGU_SGMII1_SW_RST:
	case MT7986_TOPRGU_INFRA_SW_RST:
	case MT7986_TOPRGU_U2PHY_SW_RST:
	case MT7986_TOPRGU_PCIE_SW_RST:
	case MT7986_TOPRGU_SSUSB_SW_RST:
	case MT7986_TOPRGU_ETHDMA_SW_RST:
	case MT7986_TOPRGU_CONSYS_SW_RST:
		return (true);
	default:
		return (false);
	}
}

static int
mt7986_wdt_reset_map(device_t dev, phandle_t xref, int ncells,
    pcell_t *cells, intptr_t *id)
{

	if (ncells != 1 || !mt7986_wdt_reset_valid(cells[0]))
		return (EINVAL);
	*id = cells[0];
	return (0);
}

static int
mt7986_wdt_reset_assert(device_t dev, intptr_t id, bool assert)
{
	struct mt7986_wdt_softc *sc = device_get_softc(dev);
	uint32_t mask, value;

	if (!mt7986_wdt_reset_valid(id))
		return (EINVAL);
	mask = 1u << id;
	mtx_lock_spin(&sc->mtx);
	value = bus_read_4(sc->res, MT7986_WDT_SWSYSRST);
	value &= ~(MT7986_WDT_KEY_MASK | mask);
	if (assert)
		value |= mask;
	mt7986_wdt_write(sc, MT7986_WDT_SWSYSRST,
	    value | MT7986_SWSYSRST_KEY);
	mtx_unlock_spin(&sc->mtx);
	return (0);
}

static int
mt7986_wdt_reset_is_asserted(device_t dev, intptr_t id, bool *asserted)
{
	struct mt7986_wdt_softc *sc = device_get_softc(dev);

	if (!mt7986_wdt_reset_valid(id))
		return (EINVAL);
	mtx_lock_spin(&sc->mtx);
	*asserted = (bus_read_4(sc->res, MT7986_WDT_SWSYSRST) &
	    (1u << id)) != 0;
	mtx_unlock_spin(&sc->mtx);
	return (0);
}

static int
mt7986_wdt_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev) ||
	    !ofw_bus_is_compatible(dev, "mediatek,mt7986-wdt"))
		return (ENXIO);
	device_set_desc(dev, "MediaTek MT7986 watchdog and reset controller");
	return (BUS_PROBE_DEFAULT);
}

static int
mt7986_wdt_attach(device_t dev)
{
	struct mt7986_wdt_softc *sc = device_get_softc(dev);
	phandle_t node;
	int rid;

	rid = 0;
	sc->res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	if (sc->res == NULL)
		return (ENXIO);
	if (rman_get_size(sc->res) < MT7986_WDT_SWSYSRST + 4) {
		bus_release_resource(dev, SYS_RES_MEMORY, rid, sc->res);
		return (ENXIO);
	}
	node = ofw_bus_get_node(dev);
	sc->disable_extrst = OF_hasprop(node, "mediatek,disable-extrst");
	sc->reset_by_toprgu = OF_hasprop(node, "mediatek,reset-by-toprgu");
	mtx_init(&sc->mtx, device_get_nameunit(dev), NULL, MTX_SPIN);
	/* Userland must explicitly arm the watchdog through watchdog(4). */
	mtx_lock_spin(&sc->mtx);
	mt7986_wdt_stop(sc);
	mtx_unlock_spin(&sc->mtx);
	sc->watchdog_tag = EVENTHANDLER_REGISTER(watchdog_sbt_list,
	    mt7986_wdt_event, sc, 0);
	if (sc->watchdog_tag == NULL) {
		mtx_destroy(&sc->mtx);
		bus_release_resource(dev, SYS_RES_MEMORY, rid, sc->res);
		return (ENOMEM);
	}
	hwreset_register_ofw_provider(dev);
	return (0);
}

static int
mt7986_wdt_detach(device_t dev)
{

	/* Reset consumers can retain handles to this provider. */
	return (EBUSY);
}

static int
mt7986_wdt_shutdown(device_t dev)
{
	struct mt7986_wdt_softc *sc = device_get_softc(dev);

	mtx_lock_spin(&sc->mtx);
	sc->shutting_down = true;
	mt7986_wdt_stop(sc);
	mtx_unlock_spin(&sc->mtx);
	return (0);
}

static device_method_t mt7986_wdt_methods[] = {
	DEVMETHOD(device_probe,		mt7986_wdt_probe),
	DEVMETHOD(device_attach,		mt7986_wdt_attach),
	DEVMETHOD(device_detach,		mt7986_wdt_detach),
	DEVMETHOD(device_shutdown,	mt7986_wdt_shutdown),
	DEVMETHOD(hwreset_map,		mt7986_wdt_reset_map),
	DEVMETHOD(hwreset_assert,		mt7986_wdt_reset_assert),
	DEVMETHOD(hwreset_is_asserted,	mt7986_wdt_reset_is_asserted),
	DEVMETHOD_END
};

static driver_t mt7986_wdt_driver = {
	.name = "mt7986_wdt",
	.methods = mt7986_wdt_methods,
	.size = sizeof(struct mt7986_wdt_softc)
};

EARLY_DRIVER_MODULE(mt7986_wdt, simplebus, mt7986_wdt_driver, NULL, NULL,
    BUS_PASS_BUS + BUS_PASS_ORDER_LAST);
MODULE_VERSION(mt7986_wdt, 1);
