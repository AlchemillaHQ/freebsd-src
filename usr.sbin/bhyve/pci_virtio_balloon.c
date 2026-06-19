/*-
 * Copyright (c) 2026 The FreeBSD Foundation
 *
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by Alchemilla Ventures Private Limited
 * <hello@alchemilla.io> under sponsorship from the FreeBSD Foundation.
 */

#include <sys/param.h>
#ifndef WITHOUT_CAPSICUM
#include <sys/capsicum.h>
#endif
#include <sys/linker_set.h>
#include <sys/mman.h>
#include <sys/uio.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bhyverun.h"
#include "config.h"
#include "debug.h"
#include "ipc.h"
#include "pci_emul.h"
#include "virtio.h"
#ifdef BHYVE_SNAPSHOT
#include "snapshot.h"
#endif

#include <dev/virtio/balloon/virtio_balloon.h>

#define VTBALLOON_RINGSZ 64

static int pci_vtballoon_debug = 0;
#define DPRINTF(params)          \
	if (pci_vtballoon_debug) \
	PRINTLN params
#define WPRINTF(params) PRINTLN params

static struct pci_vtballoon_softc *g_vtballoon;

struct pci_vtballoon_softc {
	struct virtio_softc vbs_vs;
	struct vqueue_info vbs_vq[2];
	pthread_mutex_t vbs_mtx;
	uint32_t vbs_num_pages;
	uint32_t vbs_actual;
};

#define VBS_INFLATE_VQ 0
#define VBS_DEFLATE_VQ 1

static void pci_vtballoon_reset(void *);
static void pci_vtballoon_notify(void *, struct vqueue_info *);
static int pci_vtballoon_cfgread(void *, int, int, uint32_t *);
static int pci_vtballoon_cfgwrite(void *, int, int, uint32_t);
static void pci_vtballoon_apply_features(void *, uint64_t);
#ifdef BHYVE_SNAPSHOT
static int pci_vtballoon_snapshot(void *, struct vm_snapshot_meta *);
#endif

static nvlist_t *vm_balloon_command(struct vmctx *, const nvlist_t *);

static struct virtio_consts vtballoon_vi_consts = {
	.vc_name = "vtballoon",
	.vc_nvq = 2,
	.vc_cfgsize = sizeof(struct virtio_balloon_config),
	.vc_reset = pci_vtballoon_reset,
	.vc_qnotify = pci_vtballoon_notify,
	.vc_cfgread = pci_vtballoon_cfgread,
	.vc_cfgwrite = pci_vtballoon_cfgwrite,
	.vc_apply_features = pci_vtballoon_apply_features,
	.vc_hv_caps = VIRTIO_BALLOON_F_MUST_TELL_HOST,
#ifdef BHYVE_SNAPSHOT
	.vc_snapshot = pci_vtballoon_snapshot,
#endif
};

static void
pci_vtballoon_reset(void *vsc)
{
	struct pci_vtballoon_softc *sc = vsc;

	DPRINTF(("vtballoon: device reset requested"));
	vi_reset_dev(&sc->vbs_vs);
	sc->vbs_num_pages = 0;
	sc->vbs_actual = 0;
}

static void
pci_vtballoon_notify(void *vsc, struct vqueue_info *vq)
{
	struct iovec iov;
	struct pci_vtballoon_softc *sc = vsc;
	struct vi_req req;
	uint32_t *pfns;
	void *hva;
	uint64_t gpa;
	int n, i, npfns;

	DPRINTF(("vtballoon: notify vq=%d", vq->vq_num));

	if (vq->vq_num == VBS_INFLATE_VQ) {
		while (vq_has_descs(vq)) {
			n = vq_getchain(vq, &iov, 1, &req);
			assert(n == 1);

			npfns = iov.iov_len / sizeof(uint32_t);
			pfns = (uint32_t *)iov.iov_base;

			DPRINTF(
			    ("vtballoon: inflate request for %d pages", npfns));

			for (i = 0; i < npfns; i++) {
				gpa = (uint64_t)pfns[i] <<
				    VIRTIO_BALLOON_PFN_SHIFT;
				DPRINTF(
				    ("vtballoon:   PFN %d: 0x%x (gpa 0x%lx)",
				    i, pfns[i], gpa));

				/*
				 * madvise(MADV_DONTNEED) deactivates the
				 * backing pages, making them candidates for
				 * the host page daemon to reclaim under
				 * memory pressure.  This only works when
				 * guest memory is not wired (no -S flag);
				 * with wired memory vm_object_madvise
				 * skips the pages.
				 */
				hva = paddr_guest2host(
				    sc->vbs_vs.vs_pi->pi_vmctx, gpa,
				    PAGE_SIZE);
				if (hva != NULL)
					madvise(hva, PAGE_SIZE,
					    MADV_DONTNEED);
				else
					WPRINTF(("vtballoon: invalid GPA "
					    "for PFN 0x%x", pfns[i]));
			}

			pthread_mutex_lock(&sc->vbs_mtx);
			sc->vbs_actual += npfns;
			pthread_mutex_unlock(&sc->vbs_mtx);

			vq_relchain(vq, req.idx, 0);
		}
		vq_endchains(vq, 1);
	} else if (vq->vq_num == VBS_DEFLATE_VQ) {
		while (vq_has_descs(vq)) {
			n = vq_getchain(vq, &iov, 1, &req);
			assert(n == 1);

			npfns = iov.iov_len / sizeof(uint32_t);
			pfns = (uint32_t *)iov.iov_base;

			DPRINTF(
			    ("vtballoon: deflate request for %d pages", npfns));

			for (i = 0; i < npfns; i++) {
				DPRINTF(
				    ("vtballoon:   PFN %d: 0x%x",
				    i, pfns[i]));
			}

			pthread_mutex_lock(&sc->vbs_mtx);
			if (sc->vbs_actual >= (uint32_t)npfns)
				sc->vbs_actual -= npfns;
			else
				sc->vbs_actual = 0;
			pthread_mutex_unlock(&sc->vbs_mtx);

			vq_relchain(vq, req.idx, 0);
		}
		vq_endchains(vq, 1);
	} else {
		DPRINTF(("vtballoon: notify on unknown queue %d", vq->vq_num));
	}
}

static int
pci_vtballoon_cfgread(void *vsc, int offset, int size, uint32_t *val)
{
	struct pci_vtballoon_softc *sc = vsc;
	const uint8_t *src;

	*val = 0;

	if (offset >= 0 && offset + size <=
	    (int)sizeof(struct virtio_balloon_config)) {
		if (offset < 4)
			src = (const uint8_t *)&sc->vbs_num_pages + offset;
		else
			src = (const uint8_t *)&sc->vbs_actual +
			    (offset - 4);
		memcpy(val, src, size);
	}

	DPRINTF(("vtballoon: cfgread off=%d size=%d -> 0x%x (num=%u act=%u)",
	    offset, size, *val, sc->vbs_num_pages, sc->vbs_actual));

	return (0);
}

static int
pci_vtballoon_cfgwrite(void *vsc, int offset, int size, uint32_t val)
{
	struct pci_vtballoon_softc *sc = vsc;
	uint8_t *dst;

	DPRINTF(("vtballoon: cfgwrite off=%d size=%d val=0x%x",
	    offset, size, val));

	if (offset >= 4 && offset + size <=
	    (int)sizeof(struct virtio_balloon_config)) {
		dst = (uint8_t *)&sc->vbs_actual + (offset - 4);
		memcpy(dst, &val, size);
		DPRINTF(("vtballoon: actual set to %u", sc->vbs_actual));
	} else if (offset < 4) {
		DPRINTF(("vtballoon: guest wrote num_pages (read-only)"));
	}

	return (0);
}

static void
pci_vtballoon_apply_features(void *vsc __unused, uint64_t features)
{
	DPRINTF(("vtballoon: negotiated features 0x%lx", features));
}

#ifdef BHYVE_SNAPSHOT
static int
pci_vtballoon_snapshot(void *vsc, struct vm_snapshot_meta *meta)
{
	int ret;
	struct pci_vtballoon_softc *sc = vsc;

	DPRINTF(("vtballoon: device snapshot requested"));

	SNAPSHOT_VAR_OR_LEAVE(sc->vbs_num_pages, meta, ret, done);
	SNAPSHOT_VAR_OR_LEAVE(sc->vbs_actual, meta, ret, done);

done:
	return (ret);
}
#endif

static int
pci_vtballoon_init(struct pci_devinst *pi, nvlist_t *nvl)
{
	struct pci_vtballoon_softc *sc;
	const char *value;

	DPRINTF(("vtballoon: init called"));

	sc = calloc(1, sizeof(struct pci_vtballoon_softc));
	g_vtballoon = sc;

	pthread_mutex_init(&sc->vbs_mtx, NULL);

	vi_softc_linkup(&sc->vbs_vs, &vtballoon_vi_consts, sc, pi, sc->vbs_vq);
	sc->vbs_vs.vs_mtx = &sc->vbs_mtx;

	sc->vbs_vq[VBS_INFLATE_VQ].vq_qsize = VTBALLOON_RINGSZ;
	sc->vbs_vq[VBS_DEFLATE_VQ].vq_qsize = VTBALLOON_RINGSZ;

	value = get_config_value_node(nvl, "num_pages");
	if (value != NULL)
		sc->vbs_num_pages = (uint32_t)strtoul(value, NULL, 0);

	pci_set_cfgdata16(pi, PCIR_DEVICE, VIRTIO_DEV_BALLOON);
	pci_set_cfgdata16(pi, PCIR_VENDOR, VIRTIO_VENDOR);
	pci_set_cfgdata8(pi, PCIR_CLASS, PCIC_MEMORY);
	pci_set_cfgdata16(pi, PCIR_SUBDEV_0, VIRTIO_ID_BALLOON);
	pci_set_cfgdata16(pi, PCIR_SUBVEND_0, VIRTIO_VENDOR);

	if (vi_intr_init(&sc->vbs_vs, 1, fbsdrun_virtio_msix())) {
		EPRINTLN("vtballoon: vi_intr_init failed");
		return (1);
	}
	vi_set_io_bar(&sc->vbs_vs, 0);

	DPRINTF(("vtballoon: init done, num_pages=%u", sc->vbs_num_pages));

	return (0);
}

static nvlist_t *
vm_balloon_command(struct vmctx *ctx __unused, const nvlist_t *nvl)
{
	struct pci_vtballoon_softc *sc;
	nvlist_t *reply;
	uint64_t num_pages;

	reply = nvlist_create(0);

	if (!nvlist_exists_number(nvl, "num_pages")) {
		nvlist_add_string(reply, "error", "missing num_pages");
		return (reply);
	}

	num_pages = nvlist_get_number(nvl, "num_pages");
	sc = g_vtballoon;
	if (sc == NULL) {
		nvlist_add_string(reply, "error", "no balloon device found");
		return (reply);
	}

	pthread_mutex_lock(&sc->vbs_mtx);
	sc->vbs_num_pages = (uint32_t)num_pages;
	pthread_mutex_unlock(&sc->vbs_mtx);

	vi_interrupt(&sc->vbs_vs, VIRTIO_PCI_ISR_CONFIG,
	    sc->vbs_vs.vs_msix_cfg_idx);

	DPRINTF(("vtballoon: command set num_pages=%u actual=%u",
	    sc->vbs_num_pages, sc->vbs_actual));

	nvlist_add_number(reply, "num_pages", num_pages);
	nvlist_add_number(reply, "actual", sc->vbs_actual);
	return (reply);
}
IPC_COMMAND(balloon, vm_balloon_command);

static const struct pci_devemu pci_de_vtballoon = {
	.pe_emu = "virtio-balloon",
	.pe_init = pci_vtballoon_init,
	.pe_barwrite = vi_pci_write,
	.pe_barread = vi_pci_read,
#ifdef BHYVE_SNAPSHOT
	.pe_snapshot = vi_pci_snapshot,
#endif
};
PCI_EMUL_SET(pci_de_vtballoon);
