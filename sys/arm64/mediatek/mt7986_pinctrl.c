/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/gpio.h>
#include <sys/intr.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/rman.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/fdt/fdt_pinctrl.h>
#include <dev/gpio/gpiobusvar.h>

#include "mt7986_pinctrl_data.h"
#include "pic_if.h"

#define	MT7986_GPIO_DIR		0x000
#define	MT7986_GPIO_DOUT		0x100
#define	MT7986_GPIO_DIN		0x200
#define	MT7986_GPIO_MODE		0x300
#define	MT7986_GPIO_SET		0x004
#define	MT7986_GPIO_CLR		0x008

#define	MT7986_EINT_STA		0x000
#define	MT7986_EINT_ACK		0x040
#define	MT7986_EINT_MASK		0x080
#define	MT7986_EINT_MASK_SET	0x0c0
#define	MT7986_EINT_MASK_CLR	0x100
#define	MT7986_EINT_SENS_SET	0x180
#define	MT7986_EINT_SENS_CLR	0x1c0
#define	MT7986_EINT_SOFT_SET	0x240
#define	MT7986_EINT_SOFT_CLR	0x280
#define	MT7986_EINT_POL_SET	0x340
#define	MT7986_EINT_POL_CLR	0x380
#define	MT7986_EINT_D0EN		0x400
#define	MT7986_EINT_DBNC_CLR	0x700
#define	MT7986_EINT_NWORDS	howmany(MT7986_NPINS, 32)

#define	MT7986_INTR_CAPS		(GPIO_INTR_LEVEL_LOW | \
	GPIO_INTR_LEVEL_HIGH | GPIO_INTR_EDGE_RISING | \
	GPIO_INTR_EDGE_FALLING | GPIO_INTR_EDGE_BOTH)

#define	MT7986_GPIO_CAPS		(GPIO_PIN_INPUT | GPIO_PIN_OUTPUT | \
	GPIO_PIN_OPENDRAIN | GPIO_PIN_PUSHPULL | GPIO_PIN_PULLUP | \
	GPIO_PIN_PULLDOWN | GPIO_PIN_PRESET_LOW | GPIO_PIN_PRESET_HIGH)

enum mt7986_setting {
	MT7986_MUX,
	MT7986_INPUT,
	MT7986_SCHMITT,
	MT7986_DRIVE,
	MT7986_BIAS,
	MT7986_RESISTORS,
	MT7986_DIRECTION,
	MT7986_OUTPUT,
	MT7986_NSETTINGS
};

enum mt7986_bias {
	MT7986_BIAS_OFF,
	MT7986_BIAS_UP,
	MT7986_BIAS_DOWN
};

struct mt7986_config {
	int8_t	value[MT7986_NSETTINGS];
};

struct mt7986_irqsrc {
	struct intr_irqsrc	isrc;
	u_int			pin;
	uint32_t		mode;
	bool			enabled;
	bool			level;
};

struct mt7986_pinctrl_softc {
	struct mtx		mtx;
	device_t		gpiobus;
	struct resource		*res[MT7986_NBANKS];
	int			rid[MT7986_NBANKS];
	struct resource		*eint;
	struct resource		*irq;
	int			eint_rid;
	int			irq_rid;
	void			*irq_cookie;
	struct mt7986_irqsrc	irqs[MT7986_NPINS];
	bool			open_drain[MT7986_NPINS];
	bool			open_drain_high[MT7986_NPINS];
};

static const struct {
	const char		*name;
	enum mt7986_setting	setting;
	int			value;
} mt7986_booleans[] = {
	{ "bias-disable", MT7986_BIAS, MT7986_BIAS_OFF },
	{ "input-enable", MT7986_INPUT, 1 },
	{ "input-disable", MT7986_INPUT, 0 },
	{ "input-schmitt-enable", MT7986_SCHMITT, 1 },
	{ "input-schmitt-disable", MT7986_SCHMITT, 0 },
	{ "output-enable", MT7986_DIRECTION, 1 },
	{ "output-low", MT7986_OUTPUT, 0 },
	{ "output-high", MT7986_OUTPUT, 1 },
};

static bool
mt7986_metadata(const char *name)
{

	return (strcmp(name, "name") == 0 || strcmp(name, "phandle") == 0 ||
	    strcmp(name, "linux,phandle") == 0 || strcmp(name, "status") == 0);
}

static int
mt7986_next_property(phandle_t node, const char *previous, char *name,
    size_t size)
{
	int result;

	result = OF_nextprop(node, previous[0] == '\0' ? NULL : previous,
	    name, size);
	if (result > 0 && memchr(name, '\0', size) == NULL)
		return (-1);
	return (result);
}

static int
mt7986_set_config(struct mt7986_config *conf, enum mt7986_setting setting,
    int value)
{

	if (conf->value[setting] != -1 && conf->value[setting] != value)
		return (EINVAL);
	conf->value[setting] = value;
	return (0);
}

static int
mt7986_merge_config(struct mt7986_config *dst,
    const struct mt7986_config *src)
{
	int error, i;

	for (i = 0; i < MT7986_NSETTINGS; i++) {
		if (src->value[i] == -1)
			continue;
		error = mt7986_set_config(dst, i, src->value[i]);
		if (error != 0)
			return (error);
	}
	return (0);
}

static int
mt7986_strings(phandle_t node, const char *property, char **strings,
    int *length)
{
	char *end, *p;
	int len;

	*strings = NULL;
	len = OF_getprop_alloc(node, property, (void **)strings);
	if (len <= 0)
		return (EINVAL);
	for (p = *strings; p < *strings + len; p = end + 1) {
		end = memchr(p, '\0', *strings + len - p);
		if (end == NULL || end == p)
			return (EINVAL);
	}
	*length = len;
	return (0);
}

static int
mt7986_parse_mux(phandle_t node, struct mt7986_config *conf)
{
	const struct mt7986_group *group;
	char property[64], previous[64];
	char *function, *groups, *name;
	u_int i;
	int error, flen, glen, pin, count, limit, next;

	previous[0] = '\0';
	while ((next = mt7986_next_property(node, previous, property,
	    sizeof(property))) > 0) {
		if (!mt7986_metadata(property) &&
		    strcmp(property, "function") != 0 &&
		    strcmp(property, "groups") != 0)
			return (EINVAL);
		strlcpy(previous, property, sizeof(previous));
	}
	if (next < 0)
		return (EINVAL);
	function = groups = NULL;
	error = mt7986_strings(node, "function", &function, &flen);
	if (error != 0)
		goto out;
	if (strlen(function) + 1 != (size_t)flen) {
		error = EINVAL;
		goto out;
	}
	error = mt7986_strings(node, "groups", &groups, &glen);
	if (error != 0)
		goto out;
	limit = 1;
	if (strcmp(function, "uart") == 0 || strcmp(function, "spi") == 0 ||
	    strcmp(function, "pwm") == 0)
		limit = 2;
	if (strcmp(function, "pcie") == 0 || strcmp(function, "wifi") == 0)
		limit = 3;
	count = 0;
	for (name = groups; name < groups + glen; name += strlen(name) + 1) {
		if (++count > limit) {
			error = EINVAL;
			goto out;
		}
		group = NULL;
		for (i = 0; i < nitems(mt7986_groups); i++) {
			if (strcmp(name, mt7986_groups[i].name) == 0 &&
			    strcmp(function, mt7986_groups[i].function) == 0) {
				group = &mt7986_groups[i];
				break;
			}
		}
		if (group == NULL) {
			error = EINVAL;
			goto out;
		}
		for (pin = group->first; pin <= group->last; pin++) {
			error = mt7986_set_config(&conf[pin], MT7986_MUX,
			    group->mode);
			if (error != 0)
				goto out;
		}
	}
out:
	OF_prop_free(function);
	OF_prop_free(groups);
	return (error);
}

static int
mt7986_parse_property(phandle_t node, const char *name,
    struct mt7986_config *conf, bool *advanced)
{
	pcell_t value;
	u_int i;
	int bias, error, len;
	bool adv;

	len = OF_getproplen(node, name);
	for (i = 0; i < nitems(mt7986_booleans); i++) {
		if (strcmp(name, mt7986_booleans[i].name) != 0)
			continue;
		if (len != 0)
			return (EINVAL);
		return (mt7986_set_config(conf, mt7986_booleans[i].setting,
		    mt7986_booleans[i].value));
	}
	if (strcmp(name, "drive-strength") == 0) {
		if (len != sizeof(value) ||
		    OF_getencprop(node, name, &value, sizeof(value)) != len ||
		    value < 2 || value > 16 || (value & 1) != 0)
			return (EINVAL);
		return (mt7986_set_config(conf, MT7986_DRIVE, value / 2 - 1));
	}
	adv = false;
	if (strcmp(name, "bias-pull-up") == 0)
		bias = MT7986_BIAS_UP;
	else if (strcmp(name, "bias-pull-down") == 0)
		bias = MT7986_BIAS_DOWN;
	else if (strcmp(name, "mediatek,pull-up-adv") == 0) {
		bias = MT7986_BIAS_UP;
		adv = true;
	} else if (strcmp(name, "mediatek,pull-down-adv") == 0) {
		bias = MT7986_BIAS_DOWN;
		adv = true;
	} else
		return (EOPNOTSUPP);
	if (len == 0 && !adv)
		value = 1;
	else {
		if (len != sizeof(value) ||
		    OF_getencprop(node, name, &value, sizeof(value)) != len)
			return (EINVAL);
		if (!adv) {
			if (value < 100 || value > 103)
				return (EINVAL);
			value -= 100;
		}
		if (value > 3)
			return (EINVAL);
		*advanced = true;
	}
	error = mt7986_set_config(conf, MT7986_BIAS, bias);
	if (error != 0)
		return (error);
	return (mt7986_set_config(conf, MT7986_RESISTORS, value));
}

static int
mt7986_parse_conf(phandle_t node, struct mt7986_config *conf)
{
	const struct mt7986_pad *pad;
	struct mt7986_config settings;
	char property[64], previous[64];
	char *names, *name;
	int error, len, next, pin;
	bool advanced;

	memset(&settings, -1, sizeof(settings));
	advanced = false;
	previous[0] = '\0';
	while ((next = mt7986_next_property(node, previous, property,
	    sizeof(property))) > 0) {
		if (!mt7986_metadata(property) &&
		    strcmp(property, "pins") != 0) {
			error = mt7986_parse_property(node, property, &settings,
			    &advanced);
			if (error != 0)
				return (error);
		}
		strlcpy(previous, property, sizeof(previous));
	}
	if (next < 0)
		return (EINVAL);
	if (settings.value[MT7986_OUTPUT] != -1)
		settings.value[MT7986_DIRECTION] = 1;
	names = NULL;
	error = mt7986_strings(node, "pins", &names, &len);
	if (error != 0)
		goto out;
	for (name = names; name < names + len; name += strlen(name) + 1) {
		for (pin = 0; pin < MT7986_NPINS; pin++) {
			if (strcmp(name, mt7986_pads[pin].name) == 0)
				break;
		}
		if (pin == MT7986_NPINS) {
			error = EINVAL;
			goto out;
		}
		pad = &mt7986_pads[pin];
		if (settings.value[MT7986_DRIVE] != -1 &&
		    pad->drive_width == 0) {
			error = EOPNOTSUPP;
			goto out;
		}
		if (advanced && mt7986_banks[pad->bank].pupd == 0) {
			error = EOPNOTSUPP;
			goto out;
		}
		if (settings.value[MT7986_DRIVE] >= (1 << pad->drive_width)) {
			error = ERANGE;
			goto out;
		}
		error = mt7986_merge_config(&conf[pin], &settings);
		if (error != 0)
			goto out;
	}
out:
	OF_prop_free(names);
	return (error);
}

static int
mt7986_parse_node(phandle_t node, struct mt7986_config *conf)
{

	if (OF_child(node) != 0)
		return (EINVAL);
	if (OF_hasprop(node, "function") || OF_hasprop(node, "groups"))
		return (mt7986_parse_mux(node, conf));
	if (OF_hasprop(node, "pins"))
		return (mt7986_parse_conf(node, conf));
	return (EINVAL);
}

static void
mt7986_write(struct mt7986_pinctrl_softc *sc, u_int bank, u_int reg,
    uint32_t value)
{

	bus_write_4(sc->res[bank], reg, value);
	bus_barrier(sc->res[bank], reg, 4, BUS_SPACE_BARRIER_WRITE);
}

static void
mt7986_modify(struct mt7986_pinctrl_softc *sc, u_int bank, u_int reg,
    uint32_t mask, uint32_t value)
{
	uint32_t old;

	mtx_assert(&sc->mtx, MA_OWNED);
	old = bus_read_4(sc->res[bank], reg);
	mt7986_write(sc, bank, reg, (old & ~mask) | value);
}

static void
mt7986_apply_bias(struct mt7986_pinctrl_softc *sc,
    const struct mt7986_pad *pad, const struct mt7986_config *conf)
{
	const struct mt7986_bank *bank;
	uint32_t bit;
	int bias, resistors;

	bias = conf->value[MT7986_BIAS];
	if (bias == -1)
		return;
	bank = &mt7986_banks[pad->bank];
	bit = 1U << pad->pull_bit;
	if (bank->pupd != 0) {
		/* Disconnect both resistors before changing pull direction. */
		mt7986_modify(sc, pad->bank, bank->r0, bit, 0);
		mt7986_modify(sc, pad->bank, bank->r1, bit, 0);
		if (bias == MT7986_BIAS_OFF)
			return;
		mt7986_modify(sc, pad->bank, bank->pupd, bit,
		    bias == MT7986_BIAS_DOWN ? bit : 0);
		resistors = conf->value[MT7986_RESISTORS];
		mt7986_modify(sc, pad->bank, bank->r0, bit,
		    (resistors & 1) != 0 ? bit : 0);
		mt7986_modify(sc, pad->bank, bank->r1, bit,
		    (resistors & 2) != 0 ? bit : 0);
	} else {
		mt7986_modify(sc, pad->bank, bank->pu, bit, 0);
		mt7986_modify(sc, pad->bank, bank->pd, bit, 0);
		if (bias != MT7986_BIAS_OFF)
			mt7986_modify(sc, pad->bank,
			    bias == MT7986_BIAS_UP ? bank->pu : bank->pd,
			    bit, bit);
	}
}

static void
mt7986_apply_pin(struct mt7986_pinctrl_softc *sc, u_int pin,
    const struct mt7986_config *conf)
{
	const struct mt7986_bank *bank;
	const struct mt7986_pad *pad;
	uint32_t bit, mask, reg, shift;
	int value;

	pad = &mt7986_pads[pin];
	bank = &mt7986_banks[pad->bank];
	if (conf->value[MT7986_MUX] != -1 ||
	    conf->value[MT7986_DIRECTION] != -1)
		sc->open_drain[pin] = false;
	bit = 1U << pad->input_bit;
	value = conf->value[MT7986_INPUT];
	if (value != -1)
		mt7986_modify(sc, pad->bank, bank->ies, bit, value ? bit : 0);
	value = conf->value[MT7986_SCHMITT];
	if (value != -1)
		mt7986_modify(sc, pad->bank, bank->smt, bit, value ? bit : 0);
	value = conf->value[MT7986_DRIVE];
	if (value != -1) {
		mask = ((1U << pad->drive_width) - 1) << pad->drive_bit;
		mt7986_modify(sc, pad->bank, pad->drive_reg, mask,
		    (uint32_t)value << pad->drive_bit);
	}
	mt7986_apply_bias(sc, pad, conf);
	reg = (pin / 32) * 0x10;
	bit = 1U << (pin % 32);
	value = conf->value[MT7986_OUTPUT];
	if (value != -1) {
		mt7986_write(sc, 0, MT7986_GPIO_DOUT + reg +
		    (value ? MT7986_GPIO_SET : MT7986_GPIO_CLR), bit);
	}
	if (conf->value[MT7986_DIRECTION] != -1)
		mt7986_write(sc, 0, MT7986_GPIO_DIR + reg + MT7986_GPIO_SET,
		    bit);
	value = conf->value[MT7986_MUX];
	if (value != -1) {
		shift = (pin % 8) * 4;
		mt7986_modify(sc, 0, MT7986_GPIO_MODE + (pin / 8) * 0x10,
		    7U << shift, (uint32_t)value << shift);
	}
}

static int
mt7986_pinctrl_configure(device_t dev, phandle_t xref)
{
	struct mt7986_pinctrl_softc *sc;
	struct mt7986_config *conf;
	phandle_t node, parent, child, controller;
	char name[64], property[64], previous[64];
	int error, next, pin;

	sc = device_get_softc(dev);
	controller = ofw_bus_get_node(dev);
	node = OF_node_from_xref(xref);
	if (node <= 0)
		return (EINVAL);
	parent = OF_parent(node);
	if (parent != controller && OF_parent(parent) != controller)
		return (EINVAL);
	conf = malloc(sizeof(*conf) * MT7986_NPINS, M_TEMP, M_WAITOK);
	memset(conf, -1, sizeof(*conf) * MT7986_NPINS);
	if (parent != controller || OF_hasprop(node, "function") ||
	    OF_hasprop(node, "groups") || OF_hasprop(node, "pins"))
		error = mt7986_parse_node(node, conf);
	else {
		error = 0;
		previous[0] = '\0';
		while ((next = mt7986_next_property(node, previous, property,
		    sizeof(property))) > 0) {
			if (!mt7986_metadata(property)) {
				error = EINVAL;
				break;
			}
			strlcpy(previous, property, sizeof(previous));
		}
		if (next < 0)
			error = EINVAL;
		for (child = OF_child(node); error == 0 && child != 0;
		    child = OF_peer(child)) {
			if (!ofw_bus_node_status_okay(child))
				continue;
			error = mt7986_parse_node(child, conf);
			if (error != 0)
				break;
		}
	}
	if (error == 0) {
		/* No register is changed until the entire state is valid. */
		mtx_lock_spin(&sc->mtx);
		for (pin = 0; pin < MT7986_NPINS; pin++) {
			if (sc->irqs[pin].mode == GPIO_INTR_NONE)
				continue;
			if (conf[pin].value[MT7986_MUX] > 0 ||
			    conf[pin].value[MT7986_DIRECTION] == 1 ||
			    conf[pin].value[MT7986_INPUT] == 0) {
				error = EBUSY;
				break;
			}
		}
		if (error == 0) {
			for (pin = 0; pin < MT7986_NPINS; pin++)
				mt7986_apply_pin(sc, pin, &conf[pin]);
		}
		mtx_unlock_spin(&sc->mtx);
	}
	if (error != 0) {
		memset(name, 0, sizeof(name));
		OF_getprop(node, "name", name, sizeof(name) - 1);
		device_printf(dev, "cannot configure %s: error %d\n", name,
		    error);
	}
	free(conf, M_TEMP);
	return (error);
}

static int
mt7986_gpio_mode(struct mt7986_pinctrl_softc *sc, u_int pin)
{

	mtx_assert(&sc->mtx, MA_OWNED);
	return ((bus_read_4(sc->res[0], MT7986_GPIO_MODE +
	    (pin / 8) * 0x10) >> ((pin % 8) * 4)) & 7);
}

static device_t
mt7986_gpio_get_bus(device_t dev)
{
	struct mt7986_pinctrl_softc *sc;

	sc = device_get_softc(dev);
	return (sc->gpiobus);
}

static int
mt7986_gpio_pin_max(device_t dev, int *maxpin)
{

	*maxpin = MT7986_NPINS - 1;
	return (0);
}

static int
mt7986_gpio_pin_getname(device_t dev, uint32_t pin, char *name)
{

	if (pin >= MT7986_NPINS)
		return (EINVAL);
	strlcpy(name, mt7986_pads[pin].name, GPIOMAXNAME);
	return (0);
}

static int
mt7986_gpio_pin_getcaps(device_t dev, uint32_t pin, uint32_t *caps)
{

	if (pin >= MT7986_NPINS)
		return (EINVAL);
	*caps = MT7986_GPIO_CAPS | MT7986_INTR_CAPS;
	return (0);
}

static int
mt7986_gpio_pin_getflags(device_t dev, uint32_t pin, uint32_t *flags)
{
	struct mt7986_pinctrl_softc *sc;
	const struct mt7986_bank *bank;
	const struct mt7986_pad *pad;
	uint32_t bit, pull;

	if (pin >= MT7986_NPINS)
		return (EINVAL);
	sc = device_get_softc(dev);
	pad = &mt7986_pads[pin];
	bank = &mt7986_banks[pad->bank];
	*flags = 0;
	mtx_lock_spin(&sc->mtx);
	if (mt7986_gpio_mode(sc, pin) == 0) {
		if (sc->open_drain[pin])
			*flags = GPIO_PIN_OUTPUT | GPIO_PIN_OPENDRAIN;
		else if ((bus_read_4(sc->res[0], MT7986_GPIO_DIR +
		    (pin / 32) * 0x10) & (1U << (pin % 32))) != 0)
			*flags = GPIO_PIN_OUTPUT | GPIO_PIN_PUSHPULL;
		else
			*flags = GPIO_PIN_INPUT;
	}
	bit = 1U << pad->pull_bit;
	if (bank->pupd != 0) {
		pull = bus_read_4(sc->res[pad->bank], bank->r0) |
		    bus_read_4(sc->res[pad->bank], bank->r1);
		if ((pull & bit) != 0) {
			pull = bus_read_4(sc->res[pad->bank], bank->pupd);
			*flags |= (pull & bit) != 0 ? GPIO_PIN_PULLDOWN :
			    GPIO_PIN_PULLUP;
		}
	} else {
		if ((bus_read_4(sc->res[pad->bank], bank->pu) & bit) != 0)
			*flags |= GPIO_PIN_PULLUP;
		if ((bus_read_4(sc->res[pad->bank], bank->pd) & bit) != 0)
			*flags |= GPIO_PIN_PULLDOWN;
	}
	mtx_unlock_spin(&sc->mtx);
	return (0);
}

static void
mt7986_gpio_output(struct mt7986_pinctrl_softc *sc, u_int pin,
    bool open_drain, bool high)
{
	uint32_t bit, reg;

	mtx_assert(&sc->mtx, MA_OWNED);
	reg = (pin / 32) * 0x10;
	bit = 1U << (pin % 32);
	if (open_drain && high) {
		/* Release the pad before changing its output latch. */
		mt7986_write(sc, 0, MT7986_GPIO_DIR + reg + MT7986_GPIO_CLR,
		    bit);
	}
	mt7986_write(sc, 0, MT7986_GPIO_DOUT + reg +
	    (high && !open_drain ? MT7986_GPIO_SET : MT7986_GPIO_CLR), bit);
	if (open_drain && !high)
		mt7986_write(sc, 0, MT7986_GPIO_DIR + reg + MT7986_GPIO_SET,
		    bit);
	if (open_drain)
		sc->open_drain_high[pin] = high;
}

static int
mt7986_gpio_pin_setflags(device_t dev, uint32_t pin, uint32_t flags)
{
	struct mt7986_pinctrl_softc *sc;
	struct mt7986_config conf;
	uint32_t direction, bit, reg;
	bool high, open_drain;

	if (pin >= MT7986_NPINS || (flags & ~MT7986_GPIO_CAPS) != 0)
		return (EINVAL);
	direction = flags & (GPIO_PIN_INPUT | GPIO_PIN_OUTPUT);
	if (direction != GPIO_PIN_INPUT && direction != GPIO_PIN_OUTPUT)
		return (EINVAL);
	if ((flags & (GPIO_PIN_PULLUP | GPIO_PIN_PULLDOWN)) ==
	    (GPIO_PIN_PULLUP | GPIO_PIN_PULLDOWN) ||
	    (flags & (GPIO_PIN_OPENDRAIN | GPIO_PIN_PUSHPULL)) ==
	    (GPIO_PIN_OPENDRAIN | GPIO_PIN_PUSHPULL) ||
	    (flags & (GPIO_PIN_PRESET_LOW | GPIO_PIN_PRESET_HIGH)) ==
	    (GPIO_PIN_PRESET_LOW | GPIO_PIN_PRESET_HIGH))
		return (EINVAL);
	if (direction == GPIO_PIN_INPUT && (flags & (GPIO_PIN_OPENDRAIN |
	    GPIO_PIN_PUSHPULL | GPIO_PIN_PRESET_LOW |
	    GPIO_PIN_PRESET_HIGH)) != 0)
		return (EINVAL);

	sc = device_get_softc(dev);
	memset(&conf, -1, sizeof(conf));
	conf.value[MT7986_MUX] = 0;
	conf.value[MT7986_INPUT] = 1;
	conf.value[MT7986_BIAS] = MT7986_BIAS_OFF;
	if ((flags & GPIO_PIN_PULLUP) != 0)
		conf.value[MT7986_BIAS] = MT7986_BIAS_UP;
	if ((flags & GPIO_PIN_PULLDOWN) != 0)
		conf.value[MT7986_BIAS] = MT7986_BIAS_DOWN;
	conf.value[MT7986_RESISTORS] = 1;
	reg = (pin / 32) * 0x10;
	bit = 1U << (pin % 32);
	open_drain = (flags & GPIO_PIN_OPENDRAIN) != 0;
	mtx_lock_spin(&sc->mtx);
	if (sc->irqs[pin].mode != GPIO_INTR_NONE &&
	    direction != GPIO_PIN_INPUT) {
		mtx_unlock_spin(&sc->mtx);
		return (EBUSY);
	}
	if (direction == GPIO_PIN_INPUT)
		mt7986_write(sc, 0, MT7986_GPIO_DIR + reg + MT7986_GPIO_CLR,
		    bit);
	else {
		high = sc->open_drain[pin] ? sc->open_drain_high[pin] :
		    (bus_read_4(sc->res[0], MT7986_GPIO_DOUT + reg) & bit) != 0;
		if ((flags & GPIO_PIN_PRESET_LOW) != 0)
			high = false;
		if ((flags & GPIO_PIN_PRESET_HIGH) != 0)
			high = true;
		mt7986_gpio_output(sc, pin, open_drain, high);
		if (!open_drain)
			mt7986_write(sc, 0, MT7986_GPIO_DIR + reg +
			    MT7986_GPIO_SET, bit);
	}
	/* Set direction and latch before connecting the GPIO function. */
	mt7986_apply_pin(sc, pin, &conf);
	sc->open_drain[pin] = open_drain;
	mtx_unlock_spin(&sc->mtx);
	return (0);
}

static int
mt7986_gpio_pin_get(device_t dev, uint32_t pin, uint32_t *value)
{
	struct mt7986_pinctrl_softc *sc;

	if (pin >= MT7986_NPINS)
		return (EINVAL);
	sc = device_get_softc(dev);
	mtx_lock_spin(&sc->mtx);
	*value = (bus_read_4(sc->res[0], MT7986_GPIO_DIN +
	    (pin / 32) * 0x10) >> (pin % 32)) & 1;
	mtx_unlock_spin(&sc->mtx);
	return (0);
}

static int
mt7986_gpio_pin_set(device_t dev, uint32_t pin, uint32_t value)
{
	struct mt7986_pinctrl_softc *sc;

	if (pin >= MT7986_NPINS || value > 1)
		return (EINVAL);
	sc = device_get_softc(dev);
	mtx_lock_spin(&sc->mtx);
	if (mt7986_gpio_mode(sc, pin) != 0) {
		mtx_unlock_spin(&sc->mtx);
		return (EBUSY);
	}
	mt7986_gpio_output(sc, pin, sc->open_drain[pin], value != 0);
	mtx_unlock_spin(&sc->mtx);
	return (0);
}

static int
mt7986_gpio_pin_toggle(device_t dev, uint32_t pin)
{
	struct mt7986_pinctrl_softc *sc;
	bool high;

	if (pin >= MT7986_NPINS)
		return (EINVAL);
	sc = device_get_softc(dev);
	mtx_lock_spin(&sc->mtx);
	if (mt7986_gpio_mode(sc, pin) != 0) {
		mtx_unlock_spin(&sc->mtx);
		return (EBUSY);
	}
	high = sc->open_drain[pin] ? sc->open_drain_high[pin] :
	    (bus_read_4(sc->res[0], MT7986_GPIO_DOUT + (pin / 32) * 0x10) &
	    (1U << (pin % 32))) != 0;
	mt7986_gpio_output(sc, pin, sc->open_drain[pin], !high);
	mtx_unlock_spin(&sc->mtx);
	return (0);
}

static int
mt7986_gpio_map_gpios(device_t bus, phandle_t dev, phandle_t gparent,
    int gcells, pcell_t *gpios, uint32_t *pin, uint32_t *flags)
{

	if (gparent != ofw_bus_get_node(bus) || gcells != 2 ||
	    gpios[0] >= MT7986_NPINS)
		return (EINVAL);
	*pin = gpios[0];
	*flags = gpios[1];
	return (0);
}

static phandle_t
mt7986_gpio_get_node(device_t bus, device_t child)
{

	return (ofw_bus_get_node(bus));
}

static void
mt7986_eint_write(struct mt7986_pinctrl_softc *sc, u_int reg, uint32_t value)
{

	mtx_assert(&sc->mtx, MA_OWNED);
	bus_write_4(sc->eint, reg, value);
	bus_barrier(sc->eint, reg, 4, BUS_SPACE_BARRIER_WRITE);
}

static void
mt7986_eint_pin_write(struct mt7986_pinctrl_softc *sc, u_int reg, u_int pin)
{

	mt7986_eint_write(sc, reg + (pin / 32) * 4, 1U << (pin % 32));
}

static bool
mt7986_eint_level(struct mt7986_pinctrl_softc *sc, u_int pin)
{

	mtx_assert(&sc->mtx, MA_OWNED);
	return ((bus_read_4(sc->res[0], MT7986_GPIO_DIN +
	    (pin / 32) * 0x10) & (1U << (pin % 32))) != 0);
}

/*
 * Both edges require alternating the hardware's single polarity. Sample on
 * either side of the write, retrying if the input moved. Bound the retries
 * so a noisy input cannot keep the interrupt filter in a spin loop. A
 * software interrupt requests another pass if any transition was observed
 * while rearming. This cannot count pulses shorter than the sampling window.
 */
static bool
mt7986_eint_rearm(struct mt7986_pinctrl_softc *sc, struct mt7986_irqsrc *src)
{
	bool level, next, changed;
	int i;

	changed = false;
	level = mt7986_eint_level(sc, src->pin);
	for (i = 0; i < 4; i++) {
		mt7986_eint_pin_write(sc, level ? MT7986_EINT_POL_CLR :
		    MT7986_EINT_POL_SET, src->pin);
		next = mt7986_eint_level(sc, src->pin);
		if (next == level)
			break;
		changed = true;
		level = next;
	}
	src->level = level;
	return (changed);
}

static void
mt7986_eint_unmask(struct mt7986_pinctrl_softc *sc, struct mt7986_irqsrc *src)
{
	bool level, changed;

	mtx_assert(&sc->mtx, MA_OWNED);
	mt7986_eint_pin_write(sc, MT7986_EINT_MASK_CLR, src->pin);
	if (src->mode == GPIO_INTR_EDGE_BOTH) {
		level = src->level;
		changed = mt7986_eint_rearm(sc, src);
		if (changed || level != src->level)
			mt7986_eint_pin_write(sc, MT7986_EINT_SOFT_SET,
			    src->pin);
	}
}

static int
mt7986_eint_map(struct intr_map_data *data, u_int *pin, uint32_t *mode)
{
	struct intr_map_data_fdt *fdt;
	struct intr_map_data_gpio *gpio;

	if (data == NULL)
		return (EINVAL);
	switch (data->type) {
	case INTR_MAP_DATA_FDT:
		fdt = (struct intr_map_data_fdt *)data;
		if (fdt->ncells != 2)
			return (EINVAL);
		*pin = fdt->cells[0];
		switch (fdt->cells[1]) {
		case 1:
			*mode = GPIO_INTR_EDGE_RISING;
			break;
		case 2:
			*mode = GPIO_INTR_EDGE_FALLING;
			break;
		case 3:
			*mode = GPIO_INTR_EDGE_BOTH;
			break;
		case 4:
			*mode = GPIO_INTR_LEVEL_HIGH;
			break;
		case 8:
			*mode = GPIO_INTR_LEVEL_LOW;
			break;
		default:
			return (EINVAL);
		}
		break;
	case INTR_MAP_DATA_GPIO:
		gpio = (struct intr_map_data_gpio *)data;
		*pin = gpio->gpio_pin_num;
		*mode = gpio->gpio_intr_mode;
		break;
	default:
		return (EOPNOTSUPP);
	}
	if (*pin >= MT7986_NPINS)
		return (EINVAL);
	switch (*mode) {
	case GPIO_INTR_EDGE_RISING:
	case GPIO_INTR_EDGE_FALLING:
	case GPIO_INTR_EDGE_BOTH:
	case GPIO_INTR_LEVEL_HIGH:
	case GPIO_INTR_LEVEL_LOW:
		return (0);
	default:
		return (EINVAL);
	}
}

static int
mt7986_pic_map_intr(device_t dev, struct intr_map_data *data,
    struct intr_irqsrc **isrc)
{
	struct mt7986_pinctrl_softc *sc;
	uint32_t mode;
	u_int pin;
	int error;

	error = mt7986_eint_map(data, &pin, &mode);
	if (error != 0)
		return (error);
	sc = device_get_softc(dev);
	*isrc = &sc->irqs[pin].isrc;
	return (0);
}

static int
mt7986_pic_setup_intr(device_t dev, struct intr_irqsrc *isrc,
    struct resource *res, struct intr_map_data *data)
{
	struct mt7986_pinctrl_softc *sc;
	struct mt7986_irqsrc *src;
	struct mt7986_config conf;
	uint32_t mode;
	u_int pin;
	int error;

	error = mt7986_eint_map(data, &pin, &mode);
	if (error != 0)
		return (error);
	sc = device_get_softc(dev);
	src = &sc->irqs[pin];
	if (isrc != &src->isrc)
		return (EINVAL);
	mtx_lock_spin(&sc->mtx);
	if (isrc->isrc_handlers != 0) {
		error = src->mode == mode ? 0 : EINVAL;
		mtx_unlock_spin(&sc->mtx);
		return (error);
	}
	mt7986_eint_pin_write(sc, MT7986_EINT_MASK_SET, pin);
	/* EINT uses GPIO mux 0 with the input buffer enabled. Keep the bias. */
	memset(&conf, -1, sizeof(conf));
	conf.value[MT7986_MUX] = 0;
	conf.value[MT7986_INPUT] = 1;
	mt7986_write(sc, 0, MT7986_GPIO_DIR + (pin / 32) * 0x10 +
	    MT7986_GPIO_CLR, 1U << (pin % 32));
	mt7986_apply_pin(sc, pin, &conf);
	src->mode = mode;
	mt7986_eint_pin_write(sc, (mode & GPIO_INTR_EDGE_MASK) != 0 ?
	    MT7986_EINT_SENS_CLR : MT7986_EINT_SENS_SET, pin);
	if (mode == GPIO_INTR_EDGE_BOTH)
		(void)mt7986_eint_rearm(sc, src);
	else
		mt7986_eint_pin_write(sc, (mode == GPIO_INTR_EDGE_RISING ||
		    mode == GPIO_INTR_LEVEL_HIGH) ? MT7986_EINT_POL_SET :
		    MT7986_EINT_POL_CLR, pin);
	mt7986_eint_pin_write(sc, MT7986_EINT_SOFT_CLR, pin);
	mt7986_eint_pin_write(sc, MT7986_EINT_ACK, pin);
	mtx_unlock_spin(&sc->mtx);
	return (0);
}

static int
mt7986_pic_teardown_intr(device_t dev, struct intr_irqsrc *isrc,
    struct resource *res, struct intr_map_data *data)
{
	struct mt7986_pinctrl_softc *sc;
	struct mt7986_irqsrc *src;

	sc = device_get_softc(dev);
	src = (struct mt7986_irqsrc *)isrc;
	mtx_lock_spin(&sc->mtx);
	if (isrc->isrc_handlers == 0) {
		mt7986_eint_pin_write(sc, MT7986_EINT_SOFT_CLR, src->pin);
		mt7986_eint_pin_write(sc, MT7986_EINT_ACK, src->pin);
		src->mode = GPIO_INTR_NONE;
	}
	mtx_unlock_spin(&sc->mtx);
	return (0);
}

static void
mt7986_pic_enable_intr(device_t dev, struct intr_irqsrc *isrc)
{
	struct mt7986_pinctrl_softc *sc;
	struct mt7986_irqsrc *src;

	sc = device_get_softc(dev);
	src = (struct mt7986_irqsrc *)isrc;
	mtx_lock_spin(&sc->mtx);
	src->enabled = true;
	mt7986_eint_unmask(sc, src);
	mtx_unlock_spin(&sc->mtx);
}

static void
mt7986_pic_disable_intr(device_t dev, struct intr_irqsrc *isrc)
{
	struct mt7986_pinctrl_softc *sc;
	struct mt7986_irqsrc *src;

	sc = device_get_softc(dev);
	src = (struct mt7986_irqsrc *)isrc;
	mtx_lock_spin(&sc->mtx);
	src->enabled = false;
	mt7986_eint_pin_write(sc, MT7986_EINT_MASK_SET, src->pin);
	mtx_unlock_spin(&sc->mtx);
}

static void
mt7986_pic_pre_ithread(device_t dev, struct intr_irqsrc *isrc)
{
	struct mt7986_pinctrl_softc *sc;
	struct mt7986_irqsrc *src;

	sc = device_get_softc(dev);
	src = (struct mt7986_irqsrc *)isrc;
	mtx_lock_spin(&sc->mtx);
	mt7986_eint_pin_write(sc, MT7986_EINT_MASK_SET, src->pin);
	mtx_unlock_spin(&sc->mtx);
}

static void
mt7986_pic_post_ithread(device_t dev, struct intr_irqsrc *isrc)
{
	struct mt7986_pinctrl_softc *sc;
	struct mt7986_irqsrc *src;

	sc = device_get_softc(dev);
	src = (struct mt7986_irqsrc *)isrc;
	mtx_lock_spin(&sc->mtx);
	if ((src->mode & GPIO_INTR_EDGE_MASK) == 0)
		mt7986_eint_pin_write(sc, MT7986_EINT_ACK, src->pin);
	if (src->enabled)
		mt7986_eint_unmask(sc, src);
	mtx_unlock_spin(&sc->mtx);
}

static void
mt7986_pic_post_filter(device_t dev, struct intr_irqsrc *isrc)
{
	struct mt7986_pinctrl_softc *sc;
	struct mt7986_irqsrc *src;

	sc = device_get_softc(dev);
	src = (struct mt7986_irqsrc *)isrc;
	mtx_lock_spin(&sc->mtx);
	if ((src->mode & GPIO_INTR_EDGE_MASK) == 0)
		mt7986_eint_pin_write(sc, MT7986_EINT_ACK, src->pin);
	mtx_unlock_spin(&sc->mtx);
}

static int
mt7986_eint_filter(void *arg)
{
	struct mt7986_pinctrl_softc *sc;
	struct mt7986_irqsrc *src;
	uint32_t pending;
	u_int word, pin;
	int error;

	sc = arg;
	for (word = 0; word < MT7986_EINT_NWORDS; word++) {
		mtx_lock_spin(&sc->mtx);
		pending = bus_read_4(sc->eint, MT7986_EINT_STA + word * 4) &
		    ~bus_read_4(sc->eint, MT7986_EINT_MASK + word * 4);
		mtx_unlock_spin(&sc->mtx);
		while (pending != 0) {
			pin = word * 32 + ffs(pending) - 1;
			pending &= pending - 1;
			mtx_lock_spin(&sc->mtx);
			if (pin >= MT7986_NPINS || !sc->irqs[pin].enabled) {
				mt7986_eint_pin_write(sc, MT7986_EINT_MASK_SET,
				    pin);
				mt7986_eint_pin_write(sc, MT7986_EINT_SOFT_CLR,
				    pin);
				mt7986_eint_pin_write(sc, MT7986_EINT_ACK, pin);
				mtx_unlock_spin(&sc->mtx);
				continue;
			}
			src = &sc->irqs[pin];
			mt7986_eint_pin_write(sc, MT7986_EINT_SOFT_CLR, pin);
			/* Preserve edges arriving during dispatch. */
			if ((src->mode & GPIO_INTR_EDGE_MASK) != 0)
				mt7986_eint_pin_write(sc, MT7986_EINT_ACK, pin);
			if (src->mode == GPIO_INTR_EDGE_BOTH &&
			    mt7986_eint_rearm(sc, src))
				mt7986_eint_pin_write(sc, MT7986_EINT_SOFT_SET,
				    pin);
			mtx_unlock_spin(&sc->mtx);
			error = intr_isrc_dispatch(&src->isrc,
			    curthread->td_intr_frame);
			if (error != 0) {
				mt7986_pic_disable_intr(src->isrc.isrc_dev,
				    &src->isrc);
				mtx_lock_spin(&sc->mtx);
				mt7986_eint_pin_write(sc, MT7986_EINT_ACK, pin);
				mtx_unlock_spin(&sc->mtx);
			}
		}
	}
	return (FILTER_HANDLED);
}

static int
mt7986_pinctrl_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev) ||
	    !ofw_bus_is_compatible(dev, "mediatek,mt7986a-pinctrl"))
		return (ENXIO);
	device_set_desc(dev, "MediaTek MT7986A pin controller");
	return (BUS_PROBE_DEFAULT);
}

static int
mt7986_pinctrl_attach(device_t dev)
{
	struct mt7986_pinctrl_softc *sc;
	const char *name;
	phandle_t node;
	uint32_t valid;
	int error, i, nisrc;

	sc = device_get_softc(dev);
	node = ofw_bus_get_node(dev);
	for (i = 0; i < MT7986_NBANKS; i++) {
		name = mt7986_banks[i].name;
		error = ofw_bus_find_string_index(node, "reg-names", name,
		    &sc->rid[i]);
		if (error != 0)
			goto fail;
		sc->res[i] = bus_alloc_resource_any(dev, SYS_RES_MEMORY,
		    &sc->rid[i], RF_ACTIVE);
		if (sc->res[i] == NULL || rman_get_size(sc->res[i]) < 0x1000) {
			error = ENXIO;
			goto fail;
		}
	}
	name = "eint";
	error = ofw_bus_find_string_index(node, "reg-names", name,
	    &sc->eint_rid);
	if (error != 0)
		goto fail;
	sc->eint = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &sc->eint_rid,
	    RF_ACTIVE);
	if (sc->eint == NULL || rman_get_size(sc->eint) < 0x1000) {
		error = ENXIO;
		goto fail;
	}
	name = "parent interrupt";
	sc->irq = bus_alloc_resource_any(dev, SYS_RES_IRQ, &sc->irq_rid,
	    RF_ACTIVE);
	if (sc->irq == NULL) {
		error = ENXIO;
		goto fail;
	}
	mtx_init(&sc->mtx, device_get_nameunit(dev), NULL, MTX_SPIN);
	mtx_lock_spin(&sc->mtx);
	for (i = 0; i < MT7986_EINT_NWORDS; i++) {
		valid = UINT32_MAX >> (32 - MIN(32, MT7986_NPINS - i * 32));
		mt7986_eint_write(sc, MT7986_EINT_MASK_SET + i * 4,
		    UINT32_MAX);
		mt7986_eint_write(sc, MT7986_EINT_D0EN + i * 4, valid);
		mt7986_eint_write(sc, MT7986_EINT_SOFT_CLR + i * 4,
		    UINT32_MAX);
		mt7986_eint_write(sc, MT7986_EINT_ACK + i * 4, UINT32_MAX);
		/* Debounce is left to consumers; only pins 0..15 have it. */
		mt7986_eint_write(sc, MT7986_EINT_DBNC_CLR + i * 4,
		    0x01010101);
	}
	mtx_unlock_spin(&sc->mtx);
	nisrc = 0;
	name = "GPIO bus";
	sc->gpiobus = gpiobus_add_bus(dev);
	if (sc->gpiobus == NULL) {
		error = ENXIO;
		goto fail_intr;
	}
	name = "interrupt sources";
	for (i = 0; i < MT7986_NPINS; i++) {
		sc->irqs[i].pin = i;
		error = intr_isrc_register(&sc->irqs[i].isrc, dev, 0,
		    "%s,%u", device_get_nameunit(dev), i);
		if (error != 0)
			goto fail_intr;
		nisrc++;
	}
	name = "parent interrupt handler";
	error = bus_setup_intr(dev, sc->irq, INTR_TYPE_MISC | INTR_MPSAFE,
	    mt7986_eint_filter, NULL, sc, &sc->irq_cookie);
	if (error != 0)
		goto fail_intr;
	/* INTRNG cannot deregister a PIC: publish only after fallible setup. */
	name = "interrupt controller";
	if (intr_pic_register(dev, OF_xref_from_node(node)) == NULL) {
		error = ENXIO;
		goto fail_intr;
	}
	fdt_pinctrl_register(dev, NULL);
	fdt_pinctrl_configure_tree(dev);
	bus_attach_children(dev);
	return (0);

fail_intr:
	if (sc->irq_cookie != NULL) {
		bus_teardown_intr(dev, sc->irq, sc->irq_cookie);
		sc->irq_cookie = NULL;
	}
	while (nisrc > 0)
		intr_isrc_deregister(&sc->irqs[--nisrc].isrc);
	if (sc->gpiobus != NULL) {
		gpiobus_detach_bus(dev);
		device_delete_child(dev, sc->gpiobus);
		sc->gpiobus = NULL;
	}
	mtx_destroy(&sc->mtx);
fail:
	device_printf(dev, "cannot initialize %s: error %d\n", name, error);
	if (sc->irq != NULL) {
		bus_release_resource(dev, SYS_RES_IRQ, sc->irq_rid, sc->irq);
		sc->irq = NULL;
	}
	if (sc->eint != NULL) {
		bus_release_resource(dev, SYS_RES_MEMORY, sc->eint_rid,
		    sc->eint);
		sc->eint = NULL;
	}
	for (i = 0; i < MT7986_NBANKS; i++) {
		if (sc->res[i] != NULL) {
			bus_release_resource(dev, SYS_RES_MEMORY, sc->rid[i],
			    sc->res[i]);
			sc->res[i] = NULL;
		}
	}
	return (error);
}

static int
mt7986_pinctrl_detach(device_t dev)
{

	return (EBUSY);
}

static device_method_t mt7986_pinctrl_methods[] = {
	DEVMETHOD(device_probe,		mt7986_pinctrl_probe),
	DEVMETHOD(device_attach,		mt7986_pinctrl_attach),
	DEVMETHOD(device_detach,		mt7986_pinctrl_detach),
	DEVMETHOD(fdt_pinctrl_configure,	mt7986_pinctrl_configure),
	DEVMETHOD(gpio_get_bus,		mt7986_gpio_get_bus),
	DEVMETHOD(gpio_pin_max,		mt7986_gpio_pin_max),
	DEVMETHOD(gpio_pin_getname,	mt7986_gpio_pin_getname),
	DEVMETHOD(gpio_pin_getcaps,	mt7986_gpio_pin_getcaps),
	DEVMETHOD(gpio_pin_getflags,	mt7986_gpio_pin_getflags),
	DEVMETHOD(gpio_pin_setflags,	mt7986_gpio_pin_setflags),
	DEVMETHOD(gpio_pin_get,		mt7986_gpio_pin_get),
	DEVMETHOD(gpio_pin_set,		mt7986_gpio_pin_set),
	DEVMETHOD(gpio_pin_toggle,		mt7986_gpio_pin_toggle),
	DEVMETHOD(gpio_map_gpios,		mt7986_gpio_map_gpios),
	DEVMETHOD(ofw_bus_get_node,	mt7986_gpio_get_node),
	DEVMETHOD(bus_setup_intr,	bus_generic_setup_intr),
	DEVMETHOD(bus_teardown_intr,	bus_generic_teardown_intr),
	DEVMETHOD(pic_map_intr,		mt7986_pic_map_intr),
	DEVMETHOD(pic_setup_intr,	mt7986_pic_setup_intr),
	DEVMETHOD(pic_teardown_intr,	mt7986_pic_teardown_intr),
	DEVMETHOD(pic_enable_intr,	mt7986_pic_enable_intr),
	DEVMETHOD(pic_disable_intr,	mt7986_pic_disable_intr),
	DEVMETHOD(pic_pre_ithread,	mt7986_pic_pre_ithread),
	DEVMETHOD(pic_post_ithread,	mt7986_pic_post_ithread),
	DEVMETHOD(pic_post_filter,	mt7986_pic_post_filter),
	DEVMETHOD_END
};

static driver_t mt7986_pinctrl_driver = {
	.name = "gpio",
	.methods = mt7986_pinctrl_methods,
	.size = sizeof(struct mt7986_pinctrl_softc),
};

EARLY_DRIVER_MODULE(mt7986_pinctrl, simplebus, mt7986_pinctrl_driver, 0, 0,
    BUS_PASS_INTERRUPT + BUS_PASS_ORDER_MIDDLE);
MODULE_VERSION(mt7986_pinctrl, 1);
MODULE_DEPEND(mt7986_pinctrl, gpiobus, 1, 1, 1);
