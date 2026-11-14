/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Hayzam Sherif <hayzam@alchemilla.io>
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/callout.h>
#include <sys/endian.h>
#include <sys/kernel.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>
#include <sys/sysctl.h>
#include <sys/taskqueue.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <dev/clk/clk.h>
#include <dev/fdt/fdt_pinctrl.h>
#include <dev/hwreset/hwreset.h>
#include <dev/mmc/bridge.h>
#include <dev/mmc/mmcbrvar.h>
#include <dev/mmc/mmcreg.h>
#include <dev/mmc/mmc_fdt_helpers.h>
#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/regulator/regulator.h>

#include "mmcbr_if.h"
#include "mmc_pwrseq_if.h"
#include "mt7986_msdc.h"

enum msdc_state {
	MSDC_IDLE,
	MSDC_START,
	MSDC_COMMAND,
	MSDC_DATA,
	MSDC_DMA_IDLE,
	MSDC_BUSY
};

/* Two 28-byte GPDs (the second terminates the chain), then aligned BDs. */
struct msdc_dma_descriptors {
	uint32_t gpd[2][7];
	uint32_t padding[2];
	uint32_t bd[MSDC_MAX_SEGS][4];
};
CTASSERT(offsetof(struct msdc_dma_descriptors, bd) == 64);

struct mt7986_msdc_softc {
	device_t dev;
	device_t child;
	struct resource *res[3];
	void *ih;
	struct mtx mtx;
	struct callout timer;
	struct mmc_host host;
	struct mmc_helper helper;
	clk_t clocks[5];
	unsigned nclocks;
	hwreset_t reset;
	bus_dma_tag_t dma_tag;
	bus_dmamap_t dma_map;
	struct msdc_dma_descriptors *descriptors;
	bus_addr_t dma_address;
	bus_dma_tag_t data_tag;
	bus_dmamap_t data_map;
	size_t data_length;
	int map_error;
	unsigned data_nsegments;
	bool dma_loaded;
	bool data_map_created;
	bool data_loaded;
	bool dma_owned;
	bool powered;
	bool vmmc_on;
	bool vqmmc_on;
	bool gpio_setup;
	bool hw_ready;
	bool detaching;
	bool host_busy;
	bool failed;
	bool pio;
	bool tuning;
	bool tuned;
	uint32_t vqmmc_uv;
	uint32_t tuned_iocon, tuned_top_control, tuned_top_cmd;
	uint32_t tune_rise, tune_fall;	/* Passing delays, one bit each. */
	unsigned tune_delay, tune_window;
	bool hs400_mode;		/* Tuned for HS400. */
	bool ds_tuned;
	uint32_t hs400_ds;		/* DT "hs400-ds-delay" */
	uint32_t tuned_ds, ds_pass;
	unsigned ds_delay, ds_window;
	unsigned retune_request;	/* Also writable through sysctl. */
	uint64_t retunes;
	struct mmc_request *req;
	struct mmc_command *cmd;
	enum msdc_state state;
	sbintime_t deadline;
	uint32_t events;
	uint32_t source_hz;
	uint32_t clock_hz;
	uint32_t requested_hz;
	uint64_t requests, commands, interrupts, completions;
	uint64_t dma_reads, dma_writes, dma_bytes;
	uint64_t dma_segments;
	unsigned dma_max_segments;
	unsigned dma_max_bytes;
	uint64_t errors, sw_timeouts;
};

static struct resource_spec msdc_spec[] = {
	{ SYS_RES_MEMORY, 0, RF_ACTIVE },
	{ SYS_RES_MEMORY, 1, RF_ACTIVE },
	{ SYS_RES_IRQ, 0, RF_ACTIVE },
	{ -1, 0, 0 }
};

static const char *const msdc_clock_names[] = {
	"source", "hclk", "source_cg", "bus_clk", "sys_cg"
};

static struct mmc_request *msdc_advance(struct mt7986_msdc_softc *);
static void msdc_timeout(void *);
static int msdc_dma_prepare(struct mt7986_msdc_softc *, struct mmc_data *);
static void msdc_hs400_strobe_tune(struct mt7986_msdc_softc *);

static uint32_t
msdc_read(struct mt7986_msdc_softc *sc, bus_size_t reg)
{

	return (bus_read_4(sc->res[0], reg));
}

static void
msdc_write(struct mt7986_msdc_softc *sc, bus_size_t reg, uint32_t value)
{

	bus_write_4(sc->res[0], reg, value);
	bus_barrier(sc->res[0], reg, 4, BUS_SPACE_BARRIER_WRITE);
}

static void
msdc_modify(struct mt7986_msdc_softc *sc, bus_size_t reg, uint32_t clear,
    uint32_t set)
{

	msdc_write(sc, reg, (msdc_read(sc, reg) & ~clear) | set);
}

static int
msdc_poll(struct mt7986_msdc_softc *sc, bus_size_t reg, uint32_t mask,
    uint32_t expected)
{
	unsigned i;

	/* Only short hardware reset/clock handshakes use polling. */
	for (i = 0; i < 10000; i++) {
		if ((msdc_read(sc, reg) & mask) == expected)
			return (0);
		DELAY(1);
	}
	return (ETIMEDOUT);
}

static int
msdc_reset_controller(struct mt7986_msdc_softc *sc)
{

	msdc_write(sc, MSDC_INTEN, 0);
	msdc_modify(sc, MSDC_CFG, 0, CFG_RST);
	if (msdc_poll(sc, MSDC_CFG, CFG_RST, 0) != 0)
		return (ETIMEDOUT);
	msdc_modify(sc, MSDC_FIFOCS, 0, FIFO_CLEAR);
	if (msdc_poll(sc, MSDC_FIFOCS, FIFO_CLEAR, 0) != 0)
		return (ETIMEDOUT);
	msdc_write(sc, MSDC_INT, msdc_read(sc, MSDC_INT));
	return (0);
}

/*
 * A transfer the card never completes can leave the engine active after
 * DMA_STOP. A controller reset clears it. Fails only if the engine is
 * still active afterwards, when it may still access the request's memory.
 */
static int
msdc_stop_dma(struct mt7986_msdc_softc *sc)
{
	uint32_t cfg, ctrl;

	if ((msdc_read(sc, DMA_CFG) & DMA_ACTIVE) == 0)
		return (0);
	msdc_modify(sc, DMA_CTRL, 0, DMA_STOP);
	if (msdc_poll(sc, DMA_CFG, DMA_ACTIVE, 0) == 0)
		return (0);
	cfg = msdc_read(sc, DMA_CFG);
	ctrl = msdc_read(sc, DMA_CTRL);
	(void)msdc_reset_controller(sc);
	if (msdc_poll(sc, DMA_CFG, DMA_ACTIVE, 0) != 0)
		return (ETIMEDOUT);
	device_printf(sc->dev, "DMA stopped by controller reset "
	    "(dma %#x ctrl %#x)\n", cfg, ctrl);
	return (0);
}

static void
msdc_dma_finish(struct mt7986_msdc_softc *sc, bool success)
{
	struct mmc_data *data;

	if (msdc_stop_dma(sc) != 0)
		panic("%s: DMA did not stop after a controller reset",
		    device_get_nameunit(sc->dev));
	if (sc->dma_owned) {
		data = sc->cmd->data;
		bus_dmamap_sync(sc->dma_tag, sc->dma_map,
		    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
		bus_dmamap_sync(sc->data_tag, sc->data_map,
		    (data->flags & MMC_DATA_READ) ? BUS_DMASYNC_POSTREAD :
		    BUS_DMASYNC_POSTWRITE);
		sc->dma_owned = false;
		if (success) {
			data->xfer_len = data->len;
			sc->dma_bytes += data->len;
		}
	}
	/* A command can fail after mapping but before starting the engine. */
	if (sc->data_loaded) {
		bus_dmamap_unload(sc->data_tag, sc->data_map);
		sc->data_loaded = false;
	}
}

/*
 * Fed by descriptor DMA, the eMMC bus-test write never leaves the
 * controller and the card is left waiting for it. It is at most eight
 * bytes, so it goes through the TX FIFO by PIO instead.
 */
static bool
msdc_pio_write(struct mmc_command *cmd)
{

	return (cmd->opcode == MMC_BUSTEST_W && cmd->data != NULL &&
	    (cmd->data->flags & MMC_DATA_WRITE) != 0 &&
	    cmd->data->len <= 8 && cmd->data->len % 4 == 0);
}

static int
msdc_encode(struct mmc_command *cmd, uint32_t *raw, uint32_t *blocks)
{
	struct mmc_data *data = cmd->data;
	size_t block_size;
	uint32_t response, rw;

	if (cmd->opcode > 63)
		return (MMC_ERR_INVALID);
	response = 0;
	if (cmd->flags & MMC_RSP_PRESENT) {
		if (cmd->flags & MMC_RSP_136)
			response = 2;
		else if (cmd->flags & MMC_RSP_BUSY)
			response = 7;
		else if (cmd->flags & MMC_RSP_CRC)
			response = 1;
		else
			response = 3;
	}
	*raw = cmd->opcode | (response << 7);
	*blocks = 0;
	if (cmd->opcode == MMC_STOP_TRANSMISSION)
		*raw |= CMD_STOP;
	if (data == NULL)
		return (MMC_ERR_NONE);
	rw = data->flags & (MMC_DATA_READ | MMC_DATA_WRITE);
	if (data->data == NULL || data->len == 0 || data->len > MSDC_MAX_DATA ||
	    (data->flags & MMC_DATA_STREAM) != 0 ||
	    (rw != MMC_DATA_READ && rw != MMC_DATA_WRITE))
		return (MMC_ERR_INVALID);
	block_size = (data->flags & MMC_DATA_BLOCK_SIZE) ? data->block_size :
	    MIN(data->len, MMC_SECTOR_SIZE);
	if (block_size == 0 || block_size > 0xfff ||
	    data->len % block_size != 0)
		return (MMC_ERR_INVALID);
	*blocks = data->len / block_size;
	*raw |= block_size << 16;
	*raw |= ((*blocks > 1 || (data->flags & MMC_DATA_MULTI) != 0 ||
	    cmd->opcode == MMC_READ_MULTIPLE_BLOCK ||
	    cmd->opcode == MMC_WRITE_MULTIPLE_BLOCK) ? 2u : 1u) << 11;
	if (data->flags & MMC_DATA_WRITE)
		*raw |= CMD_WRITE;
	return (MMC_ERR_NONE);
}

static void
msdc_arm_timer(struct mt7986_msdc_softc *sc, bool busy)
{
	sbintime_t delay;

	delay = MAX(sc->deadline - getsbinuptime(), 1);
	if (busy)
		delay = MIN(delay, SBT_1MS);
	callout_reset_sbt(&sc->timer, delay, 0, msdc_timeout, sc, C_PREL(1));
}

static struct mmc_request *
msdc_complete(struct mt7986_msdc_softc *sc)
{
	struct mmc_request *req;

	mtx_assert(&sc->mtx, MA_OWNED);
	KASSERT(!sc->dma_owned, ("MSDC completion with DMA ownership"));
	KASSERT(!sc->data_loaded, ("MSDC completion with a mapped request"));
	msdc_write(sc, MSDC_INTEN, 0);
	callout_stop(&sc->timer);
	req = sc->req;
	/* mmcsd checks the main command, including completion of its stop. */
	if (req->stop != NULL && req->stop->error != MMC_ERR_NONE &&
	    req->cmd->error == MMC_ERR_NONE) {
		req->cmd->error = req->stop->error;
		if (req->cmd->data != NULL)
			req->cmd->data->xfer_len = 0;
	}
	sc->req = NULL;
	sc->cmd = NULL;
	sc->state = MSDC_IDLE;
	sc->pio = false;
	sc->completions++;
	return (req);
}

static struct mmc_request *
msdc_command_done(struct mt7986_msdc_softc *sc)
{

	msdc_write(sc, MSDC_INTEN, 0);
	if (sc->cmd == sc->req->cmd && sc->req->stop != NULL && !sc->failed &&
	    (sc->cmd->error == MMC_ERR_NONE || sc->cmd->data != NULL)) {
		sc->cmd = sc->req->stop;
		sc->state = MSDC_START;
		/* A write stop can also wait for the card's programming. */
		sc->deadline = getsbinuptime() +
		    (sc->req->cmd->data != NULL &&
		    (sc->req->cmd->data->flags & MMC_DATA_WRITE) != 0 ?
		    MSDC_WRITE_TIMEOUT : MSDC_TIMEOUT);
		return (msdc_advance(sc));
	}
	return (msdc_complete(sc));
}

static struct mmc_request *
msdc_fail(struct mt7986_msdc_softc *sc, int error)
{

	/*
	 * Capture the failing transfer before stopping DMA or resetting it.
	 * Probing (commands nothing answers), the bus-test write and tuning
	 * fail routinely; their dumps are kept for verbose boots.
	 */
	if (bootverbose || !(sc->tuning || sc->pio ||
	    (error == MMC_ERR_TIMEOUT && sc->state == MSDC_COMMAND &&
	    sc->cmd->data == NULL)))
		device_printf(sc->dev,
		    "CMD%u error %d state %u arg %#x len %zu flags %#x resp %#x "
		    "events %#x irq %#x mask %#x dma %#x ctrl %#x "
		    "sts %#x ps %#x fifo %#x blocks %u cfg %#x sdc %#x "
		    "pio %d sa %#x ca %#x datcrc %#x emmc %#x\n",
		    sc->cmd->opcode, error, sc->state, sc->cmd->arg,
		    sc->cmd->data != NULL ? sc->cmd->data->len : 0,
		    sc->cmd->flags, sc->cmd->resp[0], sc->events,
		    msdc_read(sc, MSDC_INT), msdc_read(sc, MSDC_INTEN),
		    msdc_read(sc, DMA_CFG), msdc_read(sc, DMA_CTRL),
		    msdc_read(sc, SDC_STS), msdc_read(sc, MSDC_PS),
		    msdc_read(sc, MSDC_FIFOCS), msdc_read(sc, SDC_BLK_NUM),
		    msdc_read(sc, MSDC_CFG), msdc_read(sc, SDC_CFG), sc->pio,
		    msdc_read(sc, DMA_SA), msdc_read(sc, DMA_CA),
		    msdc_read(sc, SDC_DATCRC_STS), msdc_read(sc, EMMC_STS));
	sc->cmd->error = error;
	/* Tuning probes fail by design; they are not counted as errors. */
	if (!sc->tuning)
		sc->errors++;
	/* A CRC error at the tuned speeds asks the MMC layer to re-tune. */
	if (error == MMC_ERR_BADCRC && sc->tuned && !sc->tuning &&
	    sc->clock_hz > MSDC_DEFAULT_DELAY_FREQ)
		sc->retune_request = 1;
	msdc_write(sc, MSDC_INTEN, 0);
	msdc_dma_finish(sc, false);
	if (msdc_reset_controller(sc) != 0) {
		sc->failed = true;
		device_printf(sc->dev, "controller reset timed out\n");
	}
	return (msdc_command_done(sc));
}

static struct mmc_request *
msdc_advance(struct mt7986_msdc_softc *sc)
{
	struct mmc_command *cmd = sc->cmd;
	uint32_t raw, blocks, busy;

	mtx_assert(&sc->mtx, MA_OWNED);
	switch (sc->state) {
	case MSDC_START:
		busy = STS_CMD_BUSY;
		if (cmd->opcode != MMC_STOP_TRANSMISSION &&
		    cmd->opcode != MMC_SEND_STATUS)
			busy |= STS_BUSY;
		if ((msdc_read(sc, SDC_STS) & busy) != 0) {
			msdc_arm_timer(sc, true);
			return (NULL);
		}
		if (msdc_encode(cmd, &raw, &blocks) != MMC_ERR_NONE)
			return (msdc_fail(sc, MMC_ERR_INVALID));
		sc->events = 0;
		msdc_write(sc, MSDC_INTEN, 0);
		msdc_write(sc, MSDC_INT, msdc_read(sc, MSDC_INT));
		sc->pio = msdc_pio_write(cmd);
		msdc_modify(sc, MSDC_CFG, CFG_PIO, sc->pio ? CFG_PIO : 0);
		msdc_write(sc, SDC_BLK_NUM, blocks);
		msdc_write(sc, SDC_ARG, cmd->arg);
		sc->state = MSDC_COMMAND;
		msdc_write(sc, MSDC_INTEN, INT_CMD_MASK |
		    (cmd->data != NULL ? INT_DATA_MASK : 0));
		msdc_arm_timer(sc, false);
		msdc_write(sc, SDC_CMD, raw);
		sc->commands++;
		return (NULL);
	case MSDC_DMA_IDLE:
		if (msdc_read(sc, DMA_CFG) & DMA_ACTIVE) {
			msdc_arm_timer(sc, true);
			return (NULL);
		}
		msdc_dma_finish(sc, true);
		if (sc->req->stop != NULL)
			return (msdc_command_done(sc));
		if ((cmd->data->flags & MMC_DATA_WRITE) == 0)
			return (msdc_command_done(sc));
		sc->state = MSDC_BUSY;
		/* FALLTHROUGH */
	case MSDC_BUSY:
		if ((msdc_read(sc, MSDC_PS) & PS_DAT0) == 0) {
			msdc_arm_timer(sc, true);
			return (NULL);
		}
		return (msdc_command_done(sc));
	default:
		return (NULL);
	}
}

static void
msdc_dma_start(struct mt7986_msdc_softc *sc)
{
	struct mmc_data *data = sc->cmd->data;

	KASSERT(!sc->dma_owned && (msdc_read(sc, DMA_CFG) & DMA_ACTIVE) == 0,
	    ("MSDC starting an active DMA engine"));
	KASSERT(sc->data_loaded, ("MSDC starting without mapped data"));
	if (data->flags & MMC_DATA_WRITE)
		sc->dma_writes++;
	else
		sc->dma_reads++;
	bus_dmamap_sync(sc->data_tag, sc->data_map,
	    (data->flags & MMC_DATA_READ) ? BUS_DMASYNC_PREREAD :
	    BUS_DMASYNC_PREWRITE);
	/* Hardware consumes the chain and writes back GPD ownership. */
	bus_dmamap_sync(sc->dma_tag, sc->dma_map,
	    BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);
	sc->dma_owned = true;
	msdc_write(sc, DMA_HIGH, 0);
	msdc_write(sc, DMA_SA, sc->dma_address);
	msdc_modify(sc, DMA_CFG, DMA_CHECKSUM_12B, DMA_CHECKSUM);
	sc->dma_segments += sc->data_nsegments;
	sc->dma_max_segments = MAX(sc->dma_max_segments, sc->data_nsegments);
	sc->dma_max_bytes = MAX(sc->dma_max_bytes, data->len);
	sc->state = MSDC_DATA;
	/* Flash programming can keep DAT0 busy well beyond ten seconds. */
	if (data->flags & MMC_DATA_WRITE) {
		sc->deadline = getsbinuptime() + MSDC_WRITE_TIMEOUT;
		msdc_arm_timer(sc, false);
	}
	/* The command and its response precede DMA_START. */
	msdc_write(sc, DMA_CTRL, DMA_BURST64 | DMA_DESCRIPTOR | DMA_START);
}

static void
msdc_pio_start(struct mt7986_msdc_softc *sc)
{
	struct mmc_data *data = sc->cmd->data;
	const uint8_t *p = data->data;

	sc->state = MSDC_DATA;
	sc->deadline = getsbinuptime() + MSDC_BUSTEST_TIMEOUT;
	msdc_arm_timer(sc, false);
	for (size_t i = 0; i < data->len; i += 4)
		msdc_write(sc, MSDC_TXDATA, le32dec(p + i));
}

static void
msdc_intr(void *arg)
{
	struct mt7986_msdc_softc *sc = arg;
	struct mmc_request *done = NULL;
	struct mmc_command *cmd;
	uint32_t events;
	unsigned i;

	mtx_lock(&sc->mtx);
	events = msdc_read(sc, MSDC_INT) & msdc_read(sc, MSDC_INTEN);
	if (events == 0)
		goto out;
	sc->interrupts++;
	msdc_write(sc, MSDC_INT, events);
	if (sc->req == NULL) {
		msdc_write(sc, MSDC_INTEN, 0);
		goto out;
	}
	cmd = sc->cmd;
	if (events & (INT_CMDTMO | INT_DATTMO))
		done = msdc_fail(sc, MMC_ERR_TIMEOUT);
	else if (events & (INT_RSPCRC | INT_DATCRC))
		done = msdc_fail(sc, MMC_ERR_BADCRC);
	else if (events & (INT_BD_CSUM | INT_GPD_CSUM | INT_DMA_PROTECT |
	    INT_AXI_ERR))
		done = msdc_fail(sc, MMC_ERR_FAILED);
	else {
		sc->events |= events & (INT_XFER | INT_DMA_DONE);
		if (sc->state == MSDC_COMMAND && (events & INT_CMDRDY)) {
			if (cmd->flags & MMC_RSP_136) {
				for (i = 0; i < 4; i++)
					cmd->resp[i] = msdc_read(sc,
					    SDC_RESP(3 - i));
			} else if (cmd->flags & MMC_RSP_PRESENT)
				cmd->resp[0] = msdc_read(sc, SDC_RESP(0));
			msdc_write(sc, MSDC_INTEN,
			    cmd->data != NULL ? INT_DATA_MASK : 0);
			if (cmd->data != NULL && sc->pio)
				msdc_pio_start(sc);
			else if (cmd->data != NULL)
				msdc_dma_start(sc);
			else if (cmd->flags & MMC_RSP_BUSY) {
				sc->state = MSDC_BUSY;
				done = msdc_advance(sc);
			} else
				done = msdc_command_done(sc);
		}
		if (sc->state == MSDC_DATA && sc->pio) {
			if (sc->events & INT_XFER) {
				cmd->data->xfer_len = cmd->data->len;
				sc->state = MSDC_BUSY;
				done = msdc_advance(sc);
			}
		} else if (sc->state == MSDC_DATA) {
			if (sc->events == (INT_XFER | INT_DMA_DONE)) {
				sc->state = MSDC_DMA_IDLE;
				done = msdc_advance(sc);
			}
		}
	}
out:
	mtx_unlock(&sc->mtx);
	if (done != NULL)
		done->done(done);
}

static void
msdc_timeout(void *arg)
{
	struct mt7986_msdc_softc *sc = arg;
	struct mmc_request *done;

	mtx_assert(&sc->mtx, MA_OWNED);
	if (callout_pending(&sc->timer) || !callout_active(&sc->timer))
		return;
	callout_deactivate(&sc->timer);
	if (sc->req == NULL)
		return;
	if (getsbinuptime() >= sc->deadline) {
		sc->sw_timeouts++;
		device_printf(sc->dev, "CMD%u timed out in state %u\n",
		    sc->cmd->opcode, sc->state);
		done = msdc_fail(sc, MMC_ERR_TIMEOUT);
	} else
		done = msdc_advance(sc);
	if (done != NULL) {
		mtx_unlock(&sc->mtx);
		done->done(done);
		mtx_lock(&sc->mtx);
	}
}

static int
mt7986_msdc_request(device_t dev, device_t child, struct mmc_request *req)
{
	struct mt7986_msdc_softc *sc = device_get_softc(dev);
	struct mmc_request *done = NULL;
	uint32_t raw, blocks;
	int error;

	mtx_lock(&sc->mtx);
	if (sc->req != NULL) {
		mtx_unlock(&sc->mtx);
		return (EBUSY);
	}
	sc->requests++;
	req->cmd->error = MMC_ERR_NONE;
	if (req->cmd->data != NULL)
		req->cmd->data->xfer_len = 0;
	if (req->stop != NULL)
		req->stop->error = MMC_ERR_NONE;
	error = msdc_encode(req->cmd, &raw, &blocks);
	if (error == MMC_ERR_NONE && req->stop != NULL) {
		error = msdc_encode(req->stop, &raw, &blocks);
		if (req->stop->data != NULL)
			error = MMC_ERR_INVALID;
	}
	if (sc->failed || sc->detaching || sc->clock_hz == 0)
		error = MMC_ERR_FAILED;
	if (error == MMC_ERR_NONE && req->cmd->data != NULL &&
	    !msdc_pio_write(req->cmd) &&
	    msdc_dma_prepare(sc, req->cmd->data) != 0)
		error = MMC_ERR_NO_MEMORY;
	if (error != MMC_ERR_NONE) {
		req->cmd->error = error;
		sc->errors++;
		sc->completions++;
		done = req;
	} else {
		sc->req = req;
		sc->cmd = req->cmd;
		sc->state = MSDC_START;
		/* A tuning read at a bad delay fails fast or not at all. */
		sc->deadline = getsbinuptime() +
		    (sc->tuning ? MSDC_TUNING_TIMEOUT : MSDC_TIMEOUT);
		done = msdc_advance(sc);
	}
	mtx_unlock(&sc->mtx);
	if (done != NULL)
		done->done(done);
	return (0);
}

static int
msdc_clock_values(uint32_t source, uint32_t target, uint32_t *config,
    uint32_t *actual)
{
	uint64_t divisor;

	if (source == 0 || target == 0)
		return (EINVAL);
	if (target >= source) {
		*config = 1u << 20;
		*actual = source;
	} else if (target >= source / 2) {
		*config = 0;
		*actual = source / 2;
	} else {
		divisor = howmany((uint64_t)source, (uint64_t)target * 4);
		if (divisor > 0xfff)
			return (ERANGE);
		*config = divisor << 8;
		*actual = source / (4 * divisor);
	}
	return (0);
}

/*
 * DDR modes (DDR52 and HS400): divider field 0 gives source / 4, and a
 * field of n gives source / (8 * n).
 */
static int
msdc_ddr_clock_values(uint32_t source, uint32_t target, bool hs400,
    uint32_t *config, uint32_t *actual)
{
	uint64_t divisor;

	if (source == 0 || target == 0)
		return (EINVAL);
	*config = hs400 ? CFG_MODE_HS400 : CFG_MODE_DDR;
	if (target >= source / 4) {
		*actual = source / 4;
		return (0);
	}
	divisor = howmany((uint64_t)source, (uint64_t)target * 8);
	if (divisor > 0xfff)
		return (ERANGE);
	*config |= divisor << 8;
	*actual = source / (8 * divisor);
	return (0);
}

static int
msdc_set_clock(struct mt7986_msdc_softc *sc, uint32_t target)
{
	enum mmc_bus_timing timing = sc->host.ios.timing;
	uint32_t config, actual, timeout;
	int error;

	if (target == 0) {
		msdc_modify(sc, MSDC_CFG, CFG_CKPDN, 0);
		sc->clock_hz = sc->requested_hz = 0;
		return (0);
	}
	if (timing == bus_timing_mmc_ddr52 || timing == bus_timing_mmc_hs400)
		error = msdc_ddr_clock_values(sc->source_hz, target,
		    timing == bus_timing_mmc_hs400, &config, &actual);
	else
		error = msdc_clock_values(sc->source_hz, target, &config,
		    &actual);
	if (error != 0)
		return (error);
	msdc_modify(sc, MSDC_CFG, CFG_CKPDN | CFG_DIV_MASK |
	    CFG_MODE_MASK | CFG_HS400, config);
	error = msdc_poll(sc, MSDC_CFG, CFG_CKSTB, CFG_CKSTB);
	if (error != 0) {
		sc->clock_hz = sc->requested_hz = 0;
		return (error);
	}
	msdc_modify(sc, MSDC_CFG, 0, CFG_CKPDN);
	/* Two seconds, rounded up in units of 2^20 card-clock cycles. */
	timeout = howmany((uint64_t)actual * 2, 1u << 20);
	timeout = MIN(MAX(timeout, 1) - 1, 255);
	msdc_modify(sc, SDC_CFG, SDC_DTOC_MASK, timeout << 24);
	sc->clock_hz = actual;
	sc->requested_hz = target;
	return (0);
}

static int
msdc_power(struct mt7986_msdc_softc *sc, bool on)
{
	int error, first_error;

	if (on && sc->powered)
		return (0);
	if (on) {
		if (!sc->vmmc_on) {
			error = regulator_enable(sc->helper.vmmc_supply);
			if (error != 0)
				return (error);
			sc->vmmc_on = true;
		}
		if (!sc->vqmmc_on) {
			error = regulator_enable(sc->helper.vqmmc_supply);
			if (error != 0) {
				if (regulator_disable(
				    sc->helper.vmmc_supply) == 0)
					sc->vmmc_on = false;
				return (error);
			}
			sc->vqmmc_on = true;
		}
	}
	if (sc->helper.mmc_pwrseq != NULL)
		MMC_PWRSEQ_SET_POWER(sc->helper.mmc_pwrseq, on);
	sc->powered = on;
	first_error = 0;
	if (!on && sc->vqmmc_on) {
		error = regulator_disable(sc->helper.vqmmc_supply);
		if (error == 0)
			sc->vqmmc_on = false;
		else
			first_error = error;
	}
	if (!on && sc->vmmc_on) {
		error = regulator_disable(sc->helper.vmmc_supply);
		if (error == 0)
			sc->vmmc_on = false;
		else if (first_error == 0)
			first_error = error;
	}
	return (first_error);
}

static void
msdc_set_delay(struct mt7986_msdc_softc *sc, unsigned delay, bool falling)
{
	uint32_t value;

	msdc_modify(sc, MSDC_IOCON, IOCON_EDGES, falling ? IOCON_EDGES : 0);
	value = bus_read_4(sc->res[1], TOP_CMD) & ~TOP_CMD_RXDLY_MASK;
	bus_write_4(sc->res[1], TOP_CMD, value | delay << 5);
	value = bus_read_4(sc->res[1], TOP_CONTROL) & ~TOP_DAT_RXDLY_MASK;
	bus_write_4(sc->res[1], TOP_CONTROL, value | delay << 7);
	bus_barrier(sc->res[1], 0, 8, BUS_SPACE_BARRIER_WRITE);
}

/* Tuned sampling applies only above the clocks of the untuned timings. */
static void
msdc_apply_delays(struct mt7986_msdc_softc *sc)
{

	if (sc->tuned && sc->clock_hz > MSDC_DEFAULT_DELAY_FREQ) {
		msdc_write(sc, MSDC_IOCON, sc->tuned_iocon);
		bus_write_4(sc->res[1], TOP_CONTROL, sc->tuned_top_control);
		bus_write_4(sc->res[1], TOP_CMD, sc->tuned_top_cmd);
		if (sc->host.ios.timing == bus_timing_mmc_hs400)
			bus_write_4(sc->res[1], TOP_DS_TUNE,
			    sc->ds_tuned ? sc->tuned_ds : sc->hs400_ds);
	} else {
		msdc_write(sc, MSDC_IOCON, 0);
		bus_write_4(sc->res[1], TOP_CONTROL,
		    TOP_RX_ENHANCE | TOP_DAT_RXDLY_SEL);
		bus_write_4(sc->res[1], TOP_CMD, TOP_CMD_RXDLY_SEL);
	}
	bus_barrier(sc->res[1], 0, 8, BUS_SPACE_BARRIER_WRITE);
}

static int
mt7986_msdc_update_ios(device_t dev, device_t child)
{
	struct mt7986_msdc_softc *sc = device_get_softc(dev);
	struct mmc_ios *ios = &sc->host.ios;
	uint32_t width;
	int error;

	switch (ios->timing) {
	case bus_timing_normal:
	case bus_timing_hs:
		break;
	case bus_timing_mmc_hs200:
		if ((sc->host.caps & MMC_CAP_MMC_HS200_180) == 0)
			return (EOPNOTSUPP);
		break;
	case bus_timing_mmc_ddr52:	/* Only on the way back from HS400. */
	case bus_timing_mmc_hs400:
		if ((sc->host.caps & MMC_CAP_MMC_HS400_180) == 0)
			return (EOPNOTSUPP);
		break;
	default:
		return (EOPNOTSUPP);
	}
	switch (ios->bus_width) {
	case bus_width_1:
		width = 0;
		break;
	case bus_width_4:
		width = 1;
		break;
	case bus_width_8:
		width = 2;
		break;
	default:
		return (EINVAL);
	}
	if (ios->clock > (uint32_t)sc->host.f_max)
		return (EINVAL);
	/* The MMC bus serializes IOS changes with host ownership. */
	if (sc->req != NULL)
		return (EBUSY);
	if (ios->power_mode == power_off) {
		error = msdc_set_clock(sc, 0);
		if (error == 0)
			error = msdc_power(sc, false);
		return (error);
	}
	error = msdc_power(sc, true);
	if (error != 0)
		return (error);
	msdc_modify(sc, SDC_CFG, SDC_BUSWIDTH_MASK, width << 16);
	error = msdc_set_clock(sc, ios->clock);
	if (error != 0)
		return (error);
	msdc_apply_delays(sc);
	/*
	 * The MMC layer has no step after its switch to HS400, so the strobe
	 * is tuned the first time HS400 runs above the untuned clocks.
	 */
	if (ios->timing == bus_timing_mmc_hs400 && sc->tuned && !sc->ds_tuned &&
	    !sc->tuning && sc->clock_hz > MSDC_DEFAULT_DELAY_FREQ)
		msdc_hs400_strobe_tune(sc);
	return (0);
}

static int
mt7986_msdc_switch_vccq(device_t dev, device_t child)
{
	struct mt7986_msdc_softc *sc = device_get_softc(dev);

	/*
	 * The rails are fixed, so nothing is switched.  SD cards keep 3.3 V
	 * signaling, also with a 1.8 V VQMMC as on the BPI-R3.  1.8 V I/O
	 * is reported only with HS200 on a 1.8 V VQMMC.
	 */
	switch (sc->host.ios.vccq) {
	case vccq_330:
		return (0);
	case vccq_180:
		return ((sc->host.caps & MMC_CAP_SIGNALING_180) != 0 ? 0 :
		    EOPNOTSUPP);
	default:
		return (EOPNOTSUPP);
	}
}

/* JEDEC tuning block patterns, read with CMD21 in HS200 mode. */
static const uint8_t msdc_tuning_4bit[64] = {
	0xff, 0x0f, 0xff, 0x00, 0xff, 0xcc, 0xc3, 0xcc,
	0xc3, 0x3c, 0xcc, 0xff, 0xfe, 0xff, 0xfe, 0xef,
	0xff, 0xdf, 0xff, 0xdd, 0xff, 0xfb, 0xff, 0xfb,
	0xbf, 0xff, 0x7f, 0xff, 0x77, 0xf7, 0xbd, 0xef,
	0xff, 0xf0, 0xff, 0xf0, 0x0f, 0xfc, 0xcc, 0x3c,
	0xcc, 0x33, 0xcc, 0xcf, 0xff, 0xef, 0xff, 0xee,
	0xff, 0xfd, 0xff, 0xfd, 0xdf, 0xff, 0xbf, 0xff,
	0xbb, 0xff, 0xf7, 0xff, 0xf7, 0x7f, 0x7b, 0xde,
};

static const uint8_t msdc_tuning_8bit[128] = {
	0xff, 0xff, 0x00, 0xff, 0xff, 0xff, 0x00, 0x00,
	0xff, 0xff, 0xcc, 0xcc, 0xcc, 0x33, 0xcc, 0xcc,
	0xcc, 0x33, 0x33, 0xcc, 0xcc, 0xcc, 0xff, 0xff,
	0xff, 0xee, 0xff, 0xff, 0xff, 0xee, 0xee, 0xff,
	0xff, 0xff, 0xdd, 0xff, 0xff, 0xff, 0xdd, 0xdd,
	0xff, 0xff, 0xff, 0xbb, 0xff, 0xff, 0xff, 0xbb,
	0xbb, 0xff, 0xff, 0xff, 0x77, 0xff, 0xff, 0xff,
	0x77, 0x77, 0xff, 0x77, 0xbb, 0xdd, 0xee, 0xff,
	0xff, 0xff, 0xff, 0x00, 0xff, 0xff, 0xff, 0x00,
	0x00, 0xff, 0xff, 0xcc, 0xcc, 0xcc, 0x33, 0xcc,
	0xcc, 0xcc, 0x33, 0x33, 0xcc, 0xcc, 0xcc, 0xff,
	0xff, 0xff, 0xee, 0xff, 0xff, 0xff, 0xee, 0xee,
	0xff, 0xff, 0xff, 0xdd, 0xff, 0xff, 0xff, 0xdd,
	0xdd, 0xff, 0xff, 0xff, 0xbb, 0xff, 0xff, 0xff,
	0xbb, 0xbb, 0xff, 0xff, 0xff, 0x77, 0xff, 0xff,
	0xff, 0x77, 0x77, 0xff, 0x77, 0xbb, 0xdd, 0xee,
};

static void
msdc_tuning_wakeup(struct mmc_request *req)
{
	struct mt7986_msdc_softc *sc = req->done_data;

	mtx_lock(&sc->mtx);
	req->flags |= MMC_REQ_DONE;
	mtx_unlock(&sc->mtx);
	wakeup(req);
}

/* One single-block read issued by the driver itself, waiting for it. */
static int
msdc_tuning_cmd(struct mt7986_msdc_softc *sc, uint32_t opcode, uint8_t *buf,
    size_t len)
{
	struct mmc_command cmd;
	struct mmc_data data;
	struct mmc_request req;

	memset(&cmd, 0, sizeof(cmd));
	memset(&data, 0, sizeof(data));
	memset(&req, 0, sizeof(req));
	memset(buf, 0, len);
	cmd.opcode = opcode;
	cmd.flags = MMC_RSP_R1 | MMC_CMD_ADTC;
	cmd.data = &data;
	cmd.mrq = &req;
	data.data = buf;
	data.len = len;
	data.flags = MMC_DATA_READ;
	data.mrq = &req;
	req.cmd = &cmd;
	req.done = msdc_tuning_wakeup;
	req.done_data = sc;
	if (mt7986_msdc_request(sc->dev, sc->dev, &req) != 0)
		return (MMC_ERR_FAILED);
	mtx_lock(&sc->mtx);
	while ((req.flags & MMC_REQ_DONE) == 0)
		mtx_sleep(&req, &sc->mtx, 0, "msdctune", 0);
	mtx_unlock(&sc->mtx);
	return (cmd.error);
}

/* One CMD21 read; it passes only without errors and with the pattern. */
static bool
msdc_tuning_read(struct mt7986_msdc_softc *sc, uint8_t *buf,
    const uint8_t *pattern, size_t len)
{

	return (msdc_tuning_cmd(sc, MMC_SEND_TUNING_BLOCK_HS200, buf, len) ==
	    MMC_ERR_NONE && memcmp(buf, pattern, len) == 0);
}

static uint32_t
msdc_tuning_sweep(struct mt7986_msdc_softc *sc, bool falling, uint8_t *buf,
    const uint8_t *pattern, size_t len)
{
	uint32_t pass = 0;

	for (unsigned delay = 0; delay < MSDC_DELAYS; delay++) {
		msdc_set_delay(sc, delay, falling);
		if (msdc_tuning_read(sc, buf, pattern, len))
			pass |= 1u << delay;
	}
	return (pass);
}

/* The longest run of passing delays. */
static unsigned
msdc_tuning_window(uint32_t pass, unsigned *start)
{
	unsigned best = 0, run = 0;

	*start = 0;
	for (unsigned delay = 0; delay <= MSDC_DELAYS; delay++) {
		if (delay < MSDC_DELAYS && (pass & (1u << delay)) != 0) {
			run++;
			continue;
		}
		if (run > best) {
			best = run;
			*start = delay - run;
		}
		run = 0;
	}
	return (best);
}

/*
 * HS200 tuning: the command and data sampling delays move together over
 * the 32 delay cells. The falling edge is tried only when the rising edge
 * leaves a narrow window. The middle of the longest window is used and
 * must pass repeated reads.
 */
static int
mt7986_msdc_tune(device_t dev, device_t child, bool hs400)
{
	struct mt7986_msdc_softc *sc = device_get_softc(dev);
	const uint8_t *pattern;
	uint8_t *buf;
	uint32_t dtoc;
	unsigned rise_start, rise_len, fall_start, fall_len, start, width;
	size_t len;
	bool falling;
	int error;

	if (sc->host.ios.timing != bus_timing_mmc_hs200)
		return (0);
	switch (sc->host.ios.bus_width) {
	case bus_width_4:
		pattern = msdc_tuning_4bit;
		len = sizeof(msdc_tuning_4bit);
		break;
	case bus_width_8:
		pattern = msdc_tuning_8bit;
		len = sizeof(msdc_tuning_8bit);
		break;
	default:
		return (EINVAL);
	}
	buf = malloc(len, M_DEVBUF, M_WAITOK);
	sc->hs400_mode = hs400;
	if (hs400) {
		/*
		 * HS400 reads are clocked in by the data strobe: start from
		 * the board's DT delay, tuned after the switch.  Two
		 * outstanding writes, as MediaTek configures for HS400.
		 */
		bus_write_4(sc->res[1], TOP_DS_TUNE, sc->hs400_ds);
		msdc_modify(sc, EMMC50_CFG3, CFG3_OUTS_WR_MASK, 2);
		sc->ds_tuned = false;
	}
	mtx_lock(&sc->mtx);
	sc->tuning = true;
	sc->tuned = false;
	/* The shortest hardware data timeout, 2^20 card clocks. */
	dtoc = msdc_read(sc, SDC_CFG) & SDC_DTOC_MASK;
	msdc_modify(sc, SDC_CFG, SDC_DTOC_MASK, 0);
	mtx_unlock(&sc->mtx);

	sc->tune_rise = msdc_tuning_sweep(sc, false, buf, pattern, len);
	rise_len = msdc_tuning_window(sc->tune_rise, &rise_start);
	sc->tune_fall = 0;
	fall_start = fall_len = 0;
	if (rise_len < MSDC_DELAYS / 2) {
		sc->tune_fall = msdc_tuning_sweep(sc, true, buf, pattern, len);
		fall_len = msdc_tuning_window(sc->tune_fall, &fall_start);
	}
	falling = fall_len > rise_len;
	start = falling ? fall_start : rise_start;
	width = falling ? fall_len : rise_len;
	sc->tune_window = width;
	sc->tune_delay = start + width / 2;
	error = width == 0 ? EIO : 0;
	if (error == 0) {
		msdc_set_delay(sc, sc->tune_delay, falling);
		for (unsigned i = 0; i < 8 && error == 0; i++)
			if (!msdc_tuning_read(sc, buf, pattern, len))
				error = EIO;
	}
	if (error == 0 && hs400) {
		/* Only the command delay applies in HS400. */
		msdc_modify(sc, MSDC_IOCON, IOCON_DSPL | IOCON_W_DSPL, 0);
		bus_write_4(sc->res[1], TOP_CONTROL,
		    bus_read_4(sc->res[1], TOP_CONTROL) & ~TOP_DAT_RXDLY_MASK);
	}
	if (error == 0) {
		sc->tuned_iocon = msdc_read(sc, MSDC_IOCON);
		sc->tuned_top_control = bus_read_4(sc->res[1], TOP_CONTROL);
		sc->tuned_top_cmd = bus_read_4(sc->res[1], TOP_CMD);
	}

	mtx_lock(&sc->mtx);
	sc->tuning = false;
	sc->tuned = error == 0;
	msdc_modify(sc, SDC_CFG, SDC_DTOC_MASK, dtoc);
	mtx_unlock(&sc->mtx);
	if (error != 0)
		msdc_apply_delays(sc);
	device_printf(dev, "HS200 tuning%s %s: %s edge, delay %u, window %u "
	    "(rising %#x, falling %#x)\n", hs400 ? " for HS400" : "",
	    error == 0 ? "done" : "failed", falling ? "falling" : "rising",
	    sc->tune_delay, width, sc->tune_rise, sc->tune_fall);
	free(buf, M_DEVBUF);
	return (error);
}

static uint32_t
msdc_hash(const uint8_t *buf, size_t len)
{
	uint32_t hash = 2166136261u;	/* FNV-1a */

	for (size_t i = 0; i < len; i++)
		hash = (hash ^ buf[i]) * 16777619u;
	return (hash);
}

/*
 * HS400 strobe tuning: sweep the strobe delay (DLY1) with EXT_CSD reads.
 * A read passes without errors and with the contents most reads return:
 * misreads differ from each other, while EXT_CSD repeats across the
 * window.  The DT delay stays unless two or more reads agree.
 */
static void
msdc_hs400_strobe_tune(struct mt7986_msdc_softc *sc)
{
	uint8_t *buf;
	uint32_t base, dtoc, hashes[MSDC_DELAYS], good, common;
	unsigned start, count, most;

	buf = malloc(MMC_EXTCSD_SIZE, M_DEVBUF, M_WAITOK);
	mtx_lock(&sc->mtx);
	sc->tuning = true;
	dtoc = msdc_read(sc, SDC_CFG) & SDC_DTOC_MASK;
	msdc_modify(sc, SDC_CFG, SDC_DTOC_MASK, 0);
	mtx_unlock(&sc->mtx);

	base = (sc->hs400_ds & ~DS_DLY1_MASK) | DS_DLY_SEL;
	good = 0;
	for (unsigned delay = 0; delay < MSDC_DELAYS; delay++) {
		bus_write_4(sc->res[1], TOP_DS_TUNE, base | delay << 10);
		if (msdc_tuning_cmd(sc, MMC_SEND_EXT_CSD, buf,
		    MMC_EXTCSD_SIZE) != MMC_ERR_NONE)
			continue;
		hashes[delay] = msdc_hash(buf, MMC_EXTCSD_SIZE);
		good |= 1u << delay;
	}
	common = 0;
	most = 0;
	for (unsigned i = 0; i < MSDC_DELAYS; i++) {
		if ((good & (1u << i)) == 0)
			continue;
		count = 0;
		for (unsigned j = 0; j < MSDC_DELAYS; j++)
			if ((good & (1u << j)) != 0 && hashes[j] == hashes[i])
				count++;
		if (count > most) {
			most = count;
			common = hashes[i];
		}
	}
	sc->ds_pass = 0;
	for (unsigned i = 0; most >= 2 && i < MSDC_DELAYS; i++)
		if ((good & (1u << i)) != 0 && hashes[i] == common)
			sc->ds_pass |= 1u << i;
	sc->ds_window = msdc_tuning_window(sc->ds_pass, &start);
	sc->ds_delay = start + sc->ds_window / 2;
	sc->tuned_ds = sc->ds_window != 0 ? base | sc->ds_delay << 10 :
	    sc->hs400_ds;
	bus_write_4(sc->res[1], TOP_DS_TUNE, sc->tuned_ds);

	mtx_lock(&sc->mtx);
	sc->tuning = false;
	sc->ds_tuned = true;
	msdc_modify(sc, SDC_CFG, SDC_DTOC_MASK, dtoc);
	mtx_unlock(&sc->mtx);
	device_printf(sc->dev, "HS400 strobe tuning %s: delay %u, window %u "
	    "(%#x), DS_TUNE %#x\n", sc->ds_window != 0 ? "done" :
	    "failed, DT delay kept", sc->ds_delay, sc->ds_window, sc->ds_pass,
	    sc->tuned_ds);
	free(buf, M_DEVBUF);
}

static int
mt7986_msdc_retune(device_t dev, device_t child, bool reset)
{
	struct mt7986_msdc_softc *sc = device_get_softc(dev);

	mtx_lock(&sc->mtx);
	sc->retune_request = 0;
	sc->retunes++;
	mtx_unlock(&sc->mtx);
	return (mt7986_msdc_tune(dev, child, sc->hs400_mode));
}

static int
mt7986_msdc_get_ro(device_t dev, device_t child)
{
	struct mt7986_msdc_softc *sc = device_get_softc(dev);

	return (mmc_fdt_gpio_get_readonly(&sc->helper));
}

static int
mt7986_msdc_acquire_host(device_t dev, device_t child)
{
	struct mt7986_msdc_softc *sc = device_get_softc(dev);

	mtx_lock(&sc->mtx);
	while (sc->host_busy)
		msleep(sc, &sc->mtx, 0, "msdchost", 0);
	sc->host_busy = true;
	mtx_unlock(&sc->mtx);
	return (0);
}

static int
mt7986_msdc_release_host(device_t dev, device_t child)
{
	struct mt7986_msdc_softc *sc = device_get_softc(dev);

	mtx_lock(&sc->mtx);
	sc->host_busy = false;
	wakeup(sc);
	mtx_unlock(&sc->mtx);
	return (0);
}

static int
mt7986_msdc_read_ivar(device_t dev, device_t child, int which, uintptr_t *value)
{
	struct mt7986_msdc_softc *sc = device_get_softc(dev);

	switch (which) {
	case MMCBR_IVAR_BUS_MODE:
		*value = sc->host.ios.bus_mode;
		break;
	case MMCBR_IVAR_BUS_WIDTH:
		*value = sc->host.ios.bus_width;
		break;
	case MMCBR_IVAR_CHIP_SELECT:
		*value = sc->host.ios.chip_select;
		break;
	case MMCBR_IVAR_CLOCK:
		*value = sc->host.ios.clock;
		break;
	case MMCBR_IVAR_F_MIN:
		*value = sc->host.f_min;
		break;
	case MMCBR_IVAR_F_MAX:
		*value = sc->host.f_max;
		break;
	case MMCBR_IVAR_HOST_OCR:
		*value = sc->host.host_ocr;
		break;
	case MMCBR_IVAR_MODE:
		*value = sc->host.mode;
		break;
	case MMCBR_IVAR_OCR:
		*value = sc->host.ocr;
		break;
	case MMCBR_IVAR_POWER_MODE:
		*value = sc->host.ios.power_mode;
		break;
	case MMCBR_IVAR_VDD:
		*value = sc->host.ios.vdd;
		break;
	case MMCBR_IVAR_VCCQ:
		*value = sc->host.ios.vccq;
		break;
	case MMCBR_IVAR_CAPS:
		*value = sc->host.caps;
		break;
	case MMCBR_IVAR_TIMING:
		*value = sc->host.ios.timing;
		break;
	case MMCBR_IVAR_MAX_DATA:
		*value = MSDC_MAX_DATA / MMC_SECTOR_SIZE;
		break;
	case MMCBR_IVAR_MAX_BUSY_TIMEOUT:
		*value = 10000;
		break;
	/* Only a reset re-tunes HS400 (through HS200), so always ask for it. */
	case MMCBR_IVAR_RETUNE_REQ:
		*value = sc->retune_request != 0 ? retune_req_reset :
		    retune_req_none;
		break;
	default:
		return (EINVAL);
	}
	return (0);
}

static int
mt7986_msdc_write_ivar(device_t dev, device_t child, int which, uintptr_t value)
{
	struct mt7986_msdc_softc *sc = device_get_softc(dev);

	switch (which) {
	case MMCBR_IVAR_BUS_MODE:
		sc->host.ios.bus_mode = value;
		break;
	case MMCBR_IVAR_BUS_WIDTH:
		sc->host.ios.bus_width = value;
		break;
	case MMCBR_IVAR_CHIP_SELECT:
		sc->host.ios.chip_select = value;
		break;
	case MMCBR_IVAR_CLOCK:
		sc->host.ios.clock = value;
		break;
	case MMCBR_IVAR_MODE:
		sc->host.mode = value;
		break;
	case MMCBR_IVAR_OCR:
		sc->host.ocr = value;
		break;
	case MMCBR_IVAR_POWER_MODE:
		sc->host.ios.power_mode = value;
		break;
	case MMCBR_IVAR_VDD:
		sc->host.ios.vdd = value;
		break;
	case MMCBR_IVAR_VCCQ:
		sc->host.ios.vccq = value;
		break;
	case MMCBR_IVAR_TIMING:
		sc->host.ios.timing = value;
		break;
	default:
		return (EINVAL);
	}
	return (0);
}

static void
msdc_dma_address(void *arg, bus_dma_segment_t *segments, int nsegments,
    int error)
{
	struct mt7986_msdc_softc *sc = arg;

	sc->map_error = error;
	if (error == 0) {
		sc->dma_loaded = true;
		if (nsegments != 1 || segments[0].ds_addr > UINT32_MAX -
		    sizeof(*sc->descriptors) + 1 ||
		    segments[0].ds_len != sizeof(*sc->descriptors))
			sc->map_error = EINVAL;
		else
			sc->dma_address = segments[0].ds_addr;
	}
}

static void
msdc_descriptor_checksum(uint32_t *descriptor)
{
	const uint8_t *bytes = (const uint8_t *)descriptor;
	uint8_t sum = 0;

	/* The checksum byte is zero when a descriptor is constructed. */
	for (unsigned i = 0; i < 16; i++)
		sum += bytes[i];
	descriptor[0] |= htole32((uint32_t)(uint8_t)(0xff - sum) << 8);
}

static void
msdc_data_address(void *arg, bus_dma_segment_t *segments, int count, int error)
{
	struct mt7986_msdc_softc *sc = arg;
	struct msdc_dma_descriptors *d = sc->descriptors;
	bus_addr_t next, address;
	size_t total = 0;
	uint32_t flags;

	sc->map_error = error;
	if (error != 0)
		return;
	sc->data_loaded = true;
	if (count < 1 || count > MSDC_MAX_SEGS) {
		sc->map_error = EFBIG;
		return;
	}
	memset(d, 0, sizeof(*d));
	for (int i = 0; i < count; i++) {
		address = segments[i].ds_addr;
		if (segments[i].ds_len == 0 ||
		    segments[i].ds_len > sc->data_length - total ||
		    address > MSDC_DMA_MAX_ADDRESS ||
		    segments[i].ds_len - 1 > MSDC_DMA_MAX_ADDRESS - address) {
			sc->map_error = EFBIG;
			return;
		}
		total += segments[i].ds_len;
		next = i + 1 == count ? 0 : sc->dma_address +
		    offsetof(struct msdc_dma_descriptors, bd) +
		    (i + 1) * sizeof(d->bd[0]);
		flags = (uint32_t)(address >> 32) << 28;
		if (i + 1 == count)
			flags |= MSDC_DESC_EOL;
		d->bd[i][0] = htole32(flags);
		d->bd[i][1] = htole32((uint32_t)next);
		d->bd[i][2] = htole32((uint32_t)address);
		d->bd[i][3] = htole32(segments[i].ds_len);
		msdc_descriptor_checksum(d->bd[i]);
	}
	if (total != sc->data_length) {
		sc->map_error = EINVAL;
		return;
	}
	d->gpd[0][0] = htole32(MSDC_DESC_HWO | MSDC_DESC_BDP);
	d->gpd[0][1] = htole32(sc->dma_address + sizeof(d->gpd[0]));
	d->gpd[0][2] = htole32(sc->dma_address +
	    offsetof(struct msdc_dma_descriptors, bd));
	msdc_descriptor_checksum(d->gpd[0]);
	sc->data_nsegments = count;
}

static int
msdc_dma_prepare(struct mt7986_msdc_softc *sc, struct mmc_data *data)
{
	int error;

	KASSERT(!sc->data_loaded && !sc->dma_owned,
	    ("MSDC mapping while a previous request owns DMA"));
	sc->map_error = 0;
	sc->data_length = data->len;
	sc->data_nsegments = 0;
	/* NOWAIT forbids a deferred callback after the request was rejected. */
	error = bus_dmamap_load(sc->data_tag, sc->data_map, data->data,
	    data->len, msdc_data_address, sc, BUS_DMA_NOWAIT);
	if (error == 0)
		error = sc->map_error;
	if (error != 0 && sc->data_loaded) {
		bus_dmamap_unload(sc->data_tag, sc->data_map);
		sc->data_loaded = false;
	}
	return (error);
}

static int
msdc_dma_alloc(struct mt7986_msdc_softc *sc)
{
	int error;

	error = bus_dma_tag_create(bus_get_dma_tag(sc->dev), 64, 0,
	    BUS_SPACE_MAXADDR_32BIT, BUS_SPACE_MAXADDR, NULL, NULL,
	    sizeof(*sc->descriptors), 1, sizeof(*sc->descriptors), 0,
	    NULL, NULL, &sc->dma_tag);
	if (error != 0)
		return (error);
	error = bus_dmamem_alloc(sc->dma_tag, (void **)&sc->descriptors,
	    BUS_DMA_WAITOK | BUS_DMA_ZERO | BUS_DMA_COHERENT, &sc->dma_map);
	if (error != 0)
		return (error);
	error = bus_dmamap_load(sc->dma_tag, sc->dma_map, sc->descriptors,
	    sizeof(*sc->descriptors), msdc_dma_address, sc, BUS_DMA_NOWAIT);
	if (error != 0)
		return (error);
	if (sc->map_error != 0)
		return (sc->map_error);
	error = bus_dma_tag_create(bus_get_dma_tag(sc->dev), 4, 0,
	    MSDC_DMA_MAX_ADDRESS, BUS_SPACE_MAXADDR, NULL, NULL,
	    MSDC_MAX_DATA, MSDC_MAX_SEGS, MSDC_MAX_DATA, BUS_DMA_ALLOCNOW,
	    NULL, NULL, &sc->data_tag);
	if (error != 0)
		return (error);
	error = bus_dmamap_create(sc->data_tag, 0, &sc->data_map);
	if (error == 0)
		sc->data_map_created = true;
	return (error);
}

static int
msdc_hw_init(struct mt7986_msdc_softc *sc)
{
	int error;

	error = msdc_stop_dma(sc);
	if (error != 0)
		return (error);
	msdc_modify(sc, MSDC_CFG, CFG_PIO, CFG_MODE | CFG_CKPDN);
	error = msdc_reset_controller(sc);
	if (error != 0)
		return (error);
	msdc_modify(sc, MSDC_PS, PS_CDEN, 0);
	msdc_write(sc, MSDC_IOCON, 0);
	/* MT7986 asynchronous FIFO and stopped-clock settings. */
	msdc_write(sc, PATCH0, 0x403c0046);
	msdc_write(sc, PATCH1, 0xffff4309);
	msdc_modify(sc, PATCH2, (3u << 2) | (1u << 15) | (1u << 28) |
	    PATCH2_DMA_36BIT, (3u << 2) | PATCH2_DMA_36BIT);
	msdc_modify(sc, EMMC50_CFG0, 0, 1u << 4);
	msdc_modify(sc, SDC_FIFO_CFG, (1u << 24) | (1u << 25), 0);
	bus_write_4(sc->res[1], TOP_CONTROL,
	    TOP_RX_ENHANCE | TOP_DAT_RXDLY_SEL);
	bus_write_4(sc->res[1], TOP_CMD, TOP_CMD_RXDLY_SEL);
	bus_barrier(sc->res[1], 0, 8, BUS_SPACE_BARRIER_WRITE);
	msdc_modify(sc, SDC_CFG, SDC_SDIOIDE | SDC_DTOC_MASK,
	    SDC_SDIO | (3u << 24));
	sc->hw_ready = true;
	return (0);
}

static void
msdc_card_event(device_t dev, bool present)
{
	struct mt7986_msdc_softc *sc = device_get_softc(dev);

	bus_topo_lock();
	if (!sc->detaching) {
		if (present && sc->child == NULL) {
			sc->child = device_add_child(dev, "mmc",
			    DEVICE_UNIT_ANY);
			if (sc->child != NULL)
				bus_attach_children(dev);
		} else if (!present && sc->child != NULL) {
			if (device_delete_child(dev, sc->child) == 0)
				sc->child = NULL;
		}
	}
	bus_topo_unlock();
}

static void
msdc_release_resources(struct mt7986_msdc_softc *sc)
{
	unsigned i;

	if (sc->gpio_setup)
		mmc_fdt_gpio_teardown(&sc->helper);
	callout_drain(&sc->timer);
	if (sc->hw_ready) {
		msdc_write(sc, MSDC_INTEN, 0);
		msdc_stop_dma(sc);
	}
	if (sc->ih != NULL)
		bus_teardown_intr(sc->dev, sc->res[2], sc->ih);
	KASSERT(!sc->data_loaded && !sc->dma_owned,
	    ("MSDC detach with an active DMA request"));
	if (sc->data_map_created)
		bus_dmamap_destroy(sc->data_tag, sc->data_map);
	if (sc->data_tag != NULL)
		bus_dma_tag_destroy(sc->data_tag);
	if (sc->dma_loaded)
		bus_dmamap_unload(sc->dma_tag, sc->dma_map);
	if (sc->descriptors != NULL)
		bus_dmamem_free(sc->dma_tag, sc->descriptors, sc->dma_map);
	if (sc->dma_tag != NULL)
		bus_dma_tag_destroy(sc->dma_tag);
	msdc_power(sc, false);
	if (sc->helper.vqmmc_supply != NULL)
		regulator_release(sc->helper.vqmmc_supply);
	if (sc->helper.vmmc_supply != NULL)
		regulator_release(sc->helper.vmmc_supply);
	if (sc->reset != NULL)
		hwreset_release(sc->reset);
	for (i = sc->nclocks; i > 0; i--) {
		clk_disable(sc->clocks[i - 1]);
		clk_release(sc->clocks[i - 1]);
	}
	bus_release_resources(sc->dev, msdc_spec, sc->res);
	mtx_destroy(&sc->mtx);
}

static int
mt7986_msdc_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev) ||
	    !ofw_bus_is_compatible(dev, "mediatek,mt7986-mmc"))
		return (ENXIO);
	device_set_desc(dev, "MediaTek MT7986 MSDC SD/MMC controller");
	return (BUS_PROBE_DEFAULT);
}

static int
msdc_fixed_voltage(phandle_t node, const char *property, uint32_t *voltage)
{
	phandle_t xref, supply;
	uint32_t minimum, maximum;

	if (OF_getencprop(node, property, &xref, sizeof(xref)) != sizeof(xref))
		return (ENXIO);
	supply = OF_node_from_xref(xref);
	if (!ofw_bus_node_is_compatible(supply, "regulator-fixed") ||
	    OF_getencprop(supply, "regulator-min-microvolt", &minimum,
	    sizeof(minimum)) != sizeof(minimum) ||
	    OF_getencprop(supply, "regulator-max-microvolt", &maximum,
	    sizeof(maximum)) != sizeof(maximum) || minimum != maximum)
		return (EOPNOTSUPP);
	*voltage = minimum;
	return (0);
}

static int
mt7986_msdc_attach(device_t dev)
{
	struct mt7986_msdc_softc *sc = device_get_softc(dev);
	struct sysctl_ctx_list *ctx;
	struct sysctl_oid_list *tree;
	phandle_t node = ofw_bus_get_node(dev);
	uint64_t rate;
	uint32_t width, voltage, keep;
	unsigned i;
	int error, index;

	sc->dev = dev;
	mtx_init(&sc->mtx, device_get_nameunit(dev), NULL, MTX_DEF);
	callout_init_mtx(&sc->timer, &sc->mtx, 0);
	error = bus_alloc_resources(dev, msdc_spec, sc->res);
	if (error != 0)
		goto fail;
	if (rman_get_size(sc->res[0]) < SDC_FIFO_CFG + 4 ||
	    rman_get_size(sc->res[1]) < TOP_CMD + 4) {
		error = ENXIO;
		goto fail;
	}
	error = clk_set_assigned(dev, node);
	if (error != 0)
		goto fail;
	for (i = 0; i < nitems(msdc_clock_names); i++) {
		if (i >= 3 && ofw_bus_find_string_index(node, "clock-names",
		    msdc_clock_names[i], &index) != 0)
			continue;
		error = clk_get_by_ofw_name(dev, node, msdc_clock_names[i],
		    &sc->clocks[sc->nclocks]);
		if (error != 0)
			goto fail;
		error = clk_enable(sc->clocks[sc->nclocks]);
		if (error != 0) {
			clk_release(sc->clocks[sc->nclocks]);
			goto fail;
		}
		sc->nclocks++;
	}
	error = clk_get_freq(sc->clocks[0], &rate);
	if (error != 0 || rate == 0 || rate > UINT32_MAX) {
		error = ENXIO;
		goto fail;
	}
	sc->source_hz = rate;
	if (OF_hasprop(node, "resets")) {
		error = hwreset_get_by_ofw_name(dev, node, "hrst", &sc->reset);
		if (error != 0)
			goto fail;
		error = hwreset_assert(sc->reset);
		if (error != 0)
			goto fail;
		DELAY(10);
		error = hwreset_deassert(sc->reset);
		if (error != 0)
			goto fail;
	}
	error = fdt_pinctrl_configure_by_name(dev, "default");
	if (error != 0)
		goto fail;
	width = 1;
	if (OF_hasprop(node, "bus-width") &&
	    OF_getencprop(node, "bus-width", &width, sizeof(width)) !=
	    sizeof(width)) {
		error = EINVAL;
		goto fail;
	}
	if (width != 1 && width != 4 && width != 8) {
		error = EINVAL;
		goto fail;
	}
	sc->host.f_min = 400000;
	sc->host.f_max = MSDC_MAX_FREQ;
	sc->host.host_ocr = MMC_OCR_320_330 | MMC_OCR_330_340;
	error = mmc_fdt_parse(dev, node, &sc->helper, &sc->host);
	if (error != 0)
		goto fail;
	if (sc->helper.vmmc_supply == NULL || sc->helper.vqmmc_supply == NULL ||
	    (OF_hasprop(node, "mmc-pwrseq") && sc->helper.mmc_pwrseq == NULL)) {
		error = ENXIO;
		goto fail;
	}
	/* Only fixed rails: nothing is switched. */
	if (msdc_fixed_voltage(node, "vmmc-supply", &voltage) != 0 ||
	    voltage != 3300000 ||
	    msdc_fixed_voltage(node, "vqmmc-supply", &voltage) != 0 ||
	    (voltage != 3300000 && voltage != 1800000)) {
		device_printf(dev, "unsupported supply configuration\n");
		error = EOPNOTSUPP;
		goto fail;
	}
	sc->vqmmc_uv = voltage;
	keep = MMC_CAP_4_BIT_DATA | MMC_CAP_8_BIT_DATA | MMC_CAP_HSPEED;
	/*
	 * HS200 needs 1.8 V I/O, which a fixed 1.8 V VQMMC (the eMMC wiring)
	 * provides without switching. It is kept when the DT asks for it.
	 */
	if (sc->vqmmc_uv == 1800000)
		keep |= MMC_CAP_MMC_HS200_180;
	/* HS400 builds on HS200 and needs the board's strobe delay. */
	if (sc->vqmmc_uv == 1800000 && OF_getencprop(node, "hs400-ds-delay",
	    &sc->hs400_ds, sizeof(sc->hs400_ds)) == sizeof(sc->hs400_ds))
		keep |= MMC_CAP_MMC_HS400_180;
	sc->host.caps &= keep;
	if ((sc->host.caps & MMC_CAP_MMC_HS200_180) == 0)
		sc->host.caps &= ~MMC_CAP_MMC_HS400_180;
	sc->host.caps |= MMC_CAP_SIGNALING_330 | MMC_CAP_WAIT_WHILE_BUSY;
	if ((sc->host.caps & MMC_CAP_MMC_HS200_180) != 0)
		sc->host.caps |= MMC_CAP_SIGNALING_180;
	sc->host.f_max = MIN(sc->host.f_max, MSDC_MAX_FREQ);
	if (sc->host.f_max < sc->host.f_min) {
		error = EINVAL;
		goto fail;
	}
	error = msdc_hw_init(sc);
	if (error != 0)
		goto fail;
	error = msdc_dma_alloc(sc);
	if (error != 0)
		goto fail;
	error = bus_setup_intr(dev, sc->res[2], INTR_TYPE_BIO | INTR_MPSAFE,
	    NULL, msdc_intr, sc, &sc->ih);
	if (error != 0)
		goto fail;
	error = mmc_fdt_gpio_setup(dev, node, &sc->helper, msdc_card_event);
	if (error != 0)
		goto fail;
	sc->gpio_setup = true;
	ctx = device_get_sysctl_ctx(dev);
	tree = SYSCTL_CHILDREN(device_get_sysctl_tree(dev));
#define MSDC_COUNTER(name, description) \
	SYSCTL_ADD_U64(ctx, tree, OID_AUTO, #name, CTLFLAG_RD, \
	    &sc->name, 0, description)
	MSDC_COUNTER(requests, "Requests submitted");
	MSDC_COUNTER(commands, "Commands issued");
	MSDC_COUNTER(interrupts, "Controller interrupts handled");
	MSDC_COUNTER(completions, "Requests completed");
	MSDC_COUNTER(dma_reads, "DMA reads started");
	MSDC_COUNTER(dma_writes, "DMA writes started");
	MSDC_COUNTER(dma_bytes, "Bytes transferred successfully by DMA");
	MSDC_COUNTER(dma_segments, "Data segments submitted to descriptor DMA");
	SYSCTL_ADD_UINT(ctx, tree, OID_AUTO, "dma_max_segments", CTLFLAG_RD,
	    &sc->dma_max_segments, 0, "Most data segments in one request");
	SYSCTL_ADD_UINT(ctx, tree, OID_AUTO, "dma_max_bytes", CTLFLAG_RD,
	    &sc->dma_max_bytes, 0, "Largest DMA request in bytes");
	MSDC_COUNTER(errors, "Command or data errors");
	MSDC_COUNTER(sw_timeouts, "Software request timeouts");
	MSDC_COUNTER(retunes, "Re-tunings run");
#undef MSDC_COUNTER
	SYSCTL_ADD_UINT(ctx, tree, OID_AUTO, "clock_hz", CTLFLAG_RD,
	    &sc->clock_hz, 0, "Actual card clock");
	SYSCTL_ADD_UINT(ctx, tree, OID_AUTO, "source_hz", CTLFLAG_RD,
	    &sc->source_hz, 0, "Source clock from clock framework");
	SYSCTL_ADD_UINT(ctx, tree, OID_AUTO, "tune_delay", CTLFLAG_RD,
	    &sc->tune_delay, 0, "HS200 sampling delay cell");
	SYSCTL_ADD_UINT(ctx, tree, OID_AUTO, "tune_window", CTLFLAG_RD,
	    &sc->tune_window, 0, "HS200 passing delay cells around it");
	SYSCTL_ADD_UINT(ctx, tree, OID_AUTO, "tune_rising", CTLFLAG_RD,
	    &sc->tune_rise, 0, "HS200 passing delays, rising edge");
	SYSCTL_ADD_UINT(ctx, tree, OID_AUTO, "tune_falling", CTLFLAG_RD,
	    &sc->tune_fall, 0, "HS200 passing delays, falling edge");
	SYSCTL_ADD_UINT(ctx, tree, OID_AUTO, "ds_delay", CTLFLAG_RD,
	    &sc->ds_delay, 0, "HS400 data strobe delay cell");
	SYSCTL_ADD_UINT(ctx, tree, OID_AUTO, "ds_window", CTLFLAG_RD,
	    &sc->ds_window, 0, "HS400 passing strobe delay cells");
	SYSCTL_ADD_UINT(ctx, tree, OID_AUTO, "ds_passing", CTLFLAG_RD,
	    &sc->ds_pass, 0, "HS400 passing strobe delays");
	SYSCTL_ADD_UINT(ctx, tree, OID_AUTO, "retune", CTLFLAG_RW,
	    &sc->retune_request, 0, "Re-tune before the next request");
	device_printf(dev,
	    "%u Hz source, descriptor DMA, %u-byte request limit\n",
	    sc->source_hz, MSDC_MAX_DATA);
	if (sc->helper.cd_pin == NULL ||
	    mmc_fdt_gpio_get_present(&sc->helper)) {
		sc->child = device_add_child(dev, "mmc", DEVICE_UNIT_ANY);
		if (sc->child == NULL) {
			error = ENXIO;
			goto fail;
		}
		bus_attach_children(dev);
	}
	return (0);
fail:
	sc->detaching = true;
	device_printf(dev, "attach failed: %d\n", error);
	msdc_release_resources(sc);
	return (error);
}

static int
mt7986_msdc_detach(device_t dev)
{
	struct mt7986_msdc_softc *sc = device_get_softc(dev);
	int error;

	mtx_lock(&sc->mtx);
	if (sc->req != NULL) {
		mtx_unlock(&sc->mtx);
		return (EBUSY);
	}
	mtx_unlock(&sc->mtx);
	error = device_delete_children(dev);
	if (error != 0)
		return (error);
	mtx_lock(&sc->mtx);
	KASSERT(sc->req == NULL, ("MSDC request after child detach"));
	sc->detaching = true;
	mtx_unlock(&sc->mtx);
	msdc_release_resources(sc);
	return (0);
}

static int
mt7986_msdc_shutdown(device_t dev)
{
	struct mt7986_msdc_softc *sc = device_get_softc(dev);

	mtx_lock(&sc->mtx);
	if (sc->req != NULL) {
		mtx_unlock(&sc->mtx);
		return (EBUSY);
	}
	sc->detaching = true;
	callout_stop(&sc->timer);
	msdc_write(sc, MSDC_INTEN, 0);
	msdc_stop_dma(sc);
	mtx_unlock(&sc->mtx);
	return (0);
}

static device_method_t mt7986_msdc_methods[] = {
	DEVMETHOD(device_probe,		mt7986_msdc_probe),
	DEVMETHOD(device_attach,		mt7986_msdc_attach),
	DEVMETHOD(device_detach,		mt7986_msdc_detach),
	DEVMETHOD(device_shutdown,	mt7986_msdc_shutdown),
	DEVMETHOD(bus_read_ivar,		mt7986_msdc_read_ivar),
	DEVMETHOD(bus_write_ivar,		mt7986_msdc_write_ivar),
	DEVMETHOD(mmcbr_request,		mt7986_msdc_request),
	DEVMETHOD(mmcbr_update_ios,	mt7986_msdc_update_ios),
	DEVMETHOD(mmcbr_switch_vccq,	mt7986_msdc_switch_vccq),
	DEVMETHOD(mmcbr_tune,		mt7986_msdc_tune),
	DEVMETHOD(mmcbr_retune,		mt7986_msdc_retune),
	DEVMETHOD(mmcbr_get_ro,		mt7986_msdc_get_ro),
	DEVMETHOD(mmcbr_acquire_host,	mt7986_msdc_acquire_host),
	DEVMETHOD(mmcbr_release_host,	mt7986_msdc_release_host),
	DEVMETHOD_END
};

static driver_t mt7986_msdc_driver = {
	.name = "mt7986_msdc",
	.methods = mt7986_msdc_methods,
	.size = sizeof(struct mt7986_msdc_softc)
};

DRIVER_MODULE(mt7986_msdc, simplebus, mt7986_msdc_driver, NULL, NULL);
MMC_DECLARE_BRIDGE(mt7986_msdc);
MODULE_VERSION(mt7986_msdc, 1);
