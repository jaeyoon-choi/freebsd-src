/*-
 * Copyright (c) 2026, Samsung Electronics Co., Ltd.
 * Written by Jaeyoon Choi
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/proc.h>
#include <sys/smp.h>

#include <vm/vm.h>
#include <vm/pmap.h>

#include <contrib/dev/acpica/include/acpi.h>

#include <dev/acpica/acpivar.h>

#include "ufshci_private.h"

#define UFSHCI_QCOM_QMP_PHY_V6_WINDOW_SIZE 0x2000
#define UFSHCI_QCOM_QMP_PHY_V6_HOST_DELTA 0x4000
#define UFSHCI_QCOM_X1E80100_GCC_WINDOW_SIZE 0x100000
#define UFSHCI_QCOM_X1E80100_GCC_PADDR 0x100000
#define UFSHCI_QCOM_X1E80100_TCSR_WINDOW_SIZE 0x30000
#define UFSHCI_QCOM_X1E80100_TCSR_PADDR 0x1fc0000

static int ufshci_acpi_probe(device_t);
static int ufshci_acpi_attach(device_t);
static int ufshci_acpi_detach(device_t);
static int ufshci_acpi_suspend(device_t);
static int ufshci_acpi_resume(device_t);
static int ufshci_acpi_map_qcom_direct(struct ufshci_controller *ctrlr,
    const char *what, const char *tunable, bus_addr_t default_paddr,
    vm_size_t size, bus_space_tag_t *tag, bus_space_handle_t *handle,
    bus_addr_t *paddr, bool *is_direct_map);
static void ufshci_acpi_map_qcom_aux_direct(struct ufshci_controller *ctrlr);
static void ufshci_acpi_map_qcom_phy_direct(
    struct ufshci_controller *ctrlr);
static void ufshci_acpi_release_resources(struct ufshci_controller *ctrlr);

static device_method_t ufshci_acpi_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe, ufshci_acpi_probe),
	DEVMETHOD(device_attach, ufshci_acpi_attach),
	DEVMETHOD(device_detach, ufshci_acpi_detach),
	DEVMETHOD(device_suspend, ufshci_acpi_suspend),
	DEVMETHOD(device_resume, ufshci_acpi_resume), { 0, 0 }
};

static driver_t ufshci_acpi_driver = {
	"ufshci",
	ufshci_acpi_methods,
	sizeof(struct ufshci_controller),
};

DRIVER_MODULE(ufshci, acpi, ufshci_acpi_driver, 0, 0);
MODULE_DEPEND(ufshci, acpi, 1, 1, 1);

static struct ufshci_acpi_device {
	const char *hid;
	const char *desc;
	uint32_t ref_clk;
	uint32_t quirks;
} ufshci_acpi_devices[] = {
	{ "QCOM24A5", "Qualcomm Snapdragon X Elite UFS Host Controller",
	    UFSHCI_REF_CLK_19_2MHz,
	    UFSHCI_QUIRK_REINIT_AFTER_MAX_GEAR_SWITCH |
			UFSHCI_QUIRK_BROKEN_LSDBS_MCQS_CAP |
			UFSHCI_QUIRK_QCOM_CORE_CLK_300MHZ |
			UFSHCI_QUIRK_QCOM_DEV_REF_CLK_CTRL |
			UFSHCI_QUIRK_HS_G5_RATE_A |
			UFSHCI_QUIRK_DISABLE_HOST_TX_LCC |
			UFSHCI_QUIRK_INITIAL_ADAPT_FOR_HS_G4 },
	{ 0x00000000, NULL, 0, 0 }
};

static char *ufshci_acpi_ids[] = { "QCOM24A5", NULL };

static const struct ufshci_acpi_device *
ufshci_acpi_find_device(device_t dev)
{
	char *hid;
	int i;
	int rv;

	rv = ACPI_ID_PROBE(device_get_parent(dev), dev, ufshci_acpi_ids, &hid);
	if (rv > 0)
		return (NULL);

	for (i = 0; ufshci_acpi_devices[i].hid != NULL; i++) {
		if (strcmp(ufshci_acpi_devices[i].hid, hid) != 0)
			continue;
		return (&ufshci_acpi_devices[i]);
	}

	return (NULL);
}

static int
ufshci_acpi_probe(device_t dev)
{
	struct ufshci_controller *ctrlr = device_get_softc(dev);
	const struct ufshci_acpi_device *acpi_dev;

	acpi_dev = ufshci_acpi_find_device(dev);
	if (acpi_dev == NULL)
		return (ENXIO);

	if (acpi_dev->hid) {
		ctrlr->quirks = acpi_dev->quirks;
		ctrlr->ref_clk = acpi_dev->ref_clk;
	}

	if (acpi_dev->desc) {
		device_set_desc(dev, acpi_dev->desc);
		return (BUS_PROBE_DEFAULT);
	}

	return (ENXIO);
}

static int
ufshci_acpi_allocate_memory(struct ufshci_controller *ctrlr)
{
	struct resource *resource;
	rman_res_t size;
	int enable_bringup;
	int rid;

	ctrlr->resource_id = 0;
	ctrlr->resource = bus_alloc_resource_any(ctrlr->dev, SYS_RES_MEMORY,
	    &ctrlr->resource_id, RF_ACTIVE);

	if (ctrlr->resource == NULL) {
		ufshci_printf(ctrlr, "unable to allocate acpi resource\n");
		return (ENOMEM);
	}

	ctrlr->bus_tag = rman_get_bustag(ctrlr->resource);
	ctrlr->bus_handle = rman_get_bushandle(ctrlr->resource);
	ctrlr->regs = (struct ufshci_registers *)ctrlr->bus_handle;
	ctrlr->qcom_phy_bus_tag = NULL;
	ctrlr->qcom_phy_bus_handle = 0;
	ctrlr->qcom_phy_resource_id = -1;
	ctrlr->qcom_phy_resource = NULL;
	ctrlr->qcom_phy_paddr = 0;
	ctrlr->qcom_phy_is_direct_map = false;
	ctrlr->qcom_gcc_bus_tag = NULL;
	ctrlr->qcom_gcc_bus_handle = 0;
	ctrlr->qcom_gcc_paddr = 0;
	ctrlr->qcom_gcc_is_direct_map = false;
	ctrlr->qcom_tcsr_bus_tag = NULL;
	ctrlr->qcom_tcsr_bus_handle = 0;
	ctrlr->qcom_tcsr_paddr = 0;
	ctrlr->qcom_tcsr_is_direct_map = false;
	ctrlr->qcom_acpi_bringup_enabled = false;
	ctrlr->qcom_acpi_bringup_ready = false;
	ctrlr->qcom_acpi_pwrseq_done = false;

	if (!(ctrlr->quirks & UFSHCI_QUIRK_QCOM_CORE_CLK_300MHZ))
		return (0);

	enable_bringup = 0;
	if (TUNABLE_INT_FETCH("hw.ufshci.qcom.acpi_bringup",
	    &enable_bringup) && enable_bringup != 0)
		ctrlr->qcom_acpi_bringup_enabled = true;

	/*
	 * X1E/SM8550-class firmware may expose the QMP UFS PHY as a
	 * separate memory resource next to the host controller window.
	 * Probe the remaining memory resources and keep the 0x2000-sized one
	 * as the PHY window for direct MMIO bring-up.
	 */
	for (rid = 1; rid <= 3; rid++) {
		ctrlr->qcom_phy_resource_id = rid;
		resource = bus_alloc_resource_any(ctrlr->dev, SYS_RES_MEMORY,
		    &ctrlr->qcom_phy_resource_id, RF_ACTIVE);
		if (resource == NULL)
			continue;

		size = rman_get_size(resource);
		if (size != UFSHCI_QCOM_QMP_PHY_V6_WINDOW_SIZE) {
			bus_release_resource(ctrlr->dev, SYS_RES_MEMORY,
			    ctrlr->qcom_phy_resource_id, resource);
			continue;
		}

		ctrlr->qcom_phy_resource = resource;
		ctrlr->qcom_phy_bus_tag = rman_get_bustag(resource);
		ctrlr->qcom_phy_bus_handle = rman_get_bushandle(resource);
		ufshci_printf(ctrlr,
		    "using QCOM QMP UFS PHY MMIO resource rid %d\n",
		    ctrlr->qcom_phy_resource_id);
		break;
	}

	ufshci_acpi_map_qcom_aux_direct(ctrlr);

	if (ctrlr->qcom_phy_resource == NULL)
		ufshci_acpi_map_qcom_phy_direct(ctrlr);

	return (0);
}

static int
ufshci_acpi_map_qcom_direct(struct ufshci_controller *ctrlr, const char *what,
    const char *tunable, bus_addr_t default_paddr, vm_size_t size,
    bus_space_tag_t *tag, bus_space_handle_t *handle, bus_addr_t *paddr,
    bool *is_direct_map)
{
	uint64_t tunable_paddr;
	void *va;

	if (*is_direct_map)
		return (0);

	tunable_paddr = 0;
	if (TUNABLE_UINT64_FETCH(tunable, &tunable_paddr))
		*paddr = (bus_addr_t)tunable_paddr;
	else
		*paddr = default_paddr;

	va = pmap_mapdev((vm_paddr_t)*paddr, size);
	if (va == NULL) {
		ufshci_printf(ctrlr,
		    "QCOM %s direct map failed at %#jx\n", what,
		    (uintmax_t)*paddr);
		*paddr = 0;
		return (ENXIO);
	}

	*tag = ctrlr->bus_tag;
	*handle = (bus_space_handle_t)va;
	*is_direct_map = true;
	ufshci_printf(ctrlr, "using QCOM %s direct map at %#jx\n", what,
	    (uintmax_t)*paddr);
	return (0);
}

static void
ufshci_acpi_map_qcom_aux_direct(struct ufshci_controller *ctrlr)
{
	int error;

	if (!ctrlr->qcom_acpi_bringup_enabled)
		return;

	error = ufshci_acpi_map_qcom_direct(ctrlr, "GCC",
	    "hw.ufshci.qcom.gcc_paddr", UFSHCI_QCOM_X1E80100_GCC_PADDR,
	    UFSHCI_QCOM_X1E80100_GCC_WINDOW_SIZE, &ctrlr->qcom_gcc_bus_tag,
	    &ctrlr->qcom_gcc_bus_handle, &ctrlr->qcom_gcc_paddr,
	    &ctrlr->qcom_gcc_is_direct_map);
	if (error != 0)
		return;

	error = ufshci_acpi_map_qcom_direct(ctrlr, "TCSR",
	    "hw.ufshci.qcom.tcsr_paddr", UFSHCI_QCOM_X1E80100_TCSR_PADDR,
	    UFSHCI_QCOM_X1E80100_TCSR_WINDOW_SIZE, &ctrlr->qcom_tcsr_bus_tag,
	    &ctrlr->qcom_tcsr_bus_handle, &ctrlr->qcom_tcsr_paddr,
	    &ctrlr->qcom_tcsr_is_direct_map);
	if (error != 0)
		return;

	ctrlr->qcom_acpi_bringup_ready = true;
}

static void
ufshci_acpi_map_qcom_phy_direct(struct ufshci_controller *ctrlr)
{
	int enable_autodirect;
	uint64_t phy_paddr_tunable;
	rman_res_t host_paddr;
	void *va;

	enable_autodirect = 0;
	phy_paddr_tunable = 0;
	if (TUNABLE_UINT64_FETCH("hw.ufshci.qcom.phy_paddr",
	    &phy_paddr_tunable)) {
		ctrlr->qcom_phy_paddr = (bus_addr_t)phy_paddr_tunable;
	} else if ((TUNABLE_INT_FETCH("hw.ufshci.qcom.phy_autodirect",
	    &enable_autodirect) && enable_autodirect != 0) ||
	    ctrlr->qcom_acpi_bringup_ready) {
		host_paddr = rman_get_start(ctrlr->resource);
		if (host_paddr < UFSHCI_QCOM_QMP_PHY_V6_HOST_DELTA) {
			ufshci_printf(ctrlr,
			    "QCOM QMP UFS PHY direct map skipped: "
			    "host BAR %#jx is smaller than delta %#x\n",
			    (uintmax_t)host_paddr,
			    UFSHCI_QCOM_QMP_PHY_V6_HOST_DELTA);
			return;
		}
		ctrlr->qcom_phy_paddr = host_paddr -
		    UFSHCI_QCOM_QMP_PHY_V6_HOST_DELTA;
	} else {
		ufshci_printf(ctrlr,
		    "QCOM QMP UFS PHY direct map disabled; "
		    "set hw.ufshci.qcom.phy_paddr, "
		    "hw.ufshci.qcom.phy_autodirect=1, or "
		    "hw.ufshci.qcom.acpi_bringup=1 to test it\n");
		return;
	}

	va = pmap_mapdev((vm_paddr_t)ctrlr->qcom_phy_paddr,
	    UFSHCI_QCOM_QMP_PHY_V6_WINDOW_SIZE);
	if (va == NULL) {
		ufshci_printf(ctrlr,
		    "QCOM QMP UFS PHY direct map failed at %#jx\n",
		    (uintmax_t)ctrlr->qcom_phy_paddr);
		ctrlr->qcom_phy_paddr = 0;
		return;
	}

	ctrlr->qcom_phy_bus_tag = ctrlr->bus_tag;
	ctrlr->qcom_phy_bus_handle = (bus_space_handle_t)va;
	ctrlr->qcom_phy_is_direct_map = true;
	ufshci_printf(ctrlr,
	    "using QCOM QMP UFS PHY direct map at %#jx\n",
	    (uintmax_t)ctrlr->qcom_phy_paddr);
}

static void
ufshci_acpi_release_resources(struct ufshci_controller *ctrlr)
{
	if (ctrlr->qcom_phy_is_direct_map) {
		pmap_unmapdev((void *)ctrlr->qcom_phy_bus_handle,
		    UFSHCI_QCOM_QMP_PHY_V6_WINDOW_SIZE);
		ctrlr->qcom_phy_is_direct_map = false;
	}

	if (ctrlr->qcom_phy_resource != NULL) {
		bus_release_resource(ctrlr->dev, SYS_RES_MEMORY,
		    ctrlr->qcom_phy_resource_id, ctrlr->qcom_phy_resource);
		ctrlr->qcom_phy_resource = NULL;
	}

	ctrlr->qcom_phy_bus_tag = NULL;
	ctrlr->qcom_phy_bus_handle = 0;
	ctrlr->qcom_phy_resource_id = -1;
	ctrlr->qcom_phy_paddr = 0;

	if (ctrlr->qcom_tcsr_is_direct_map) {
		pmap_unmapdev((void *)ctrlr->qcom_tcsr_bus_handle,
		    UFSHCI_QCOM_X1E80100_TCSR_WINDOW_SIZE);
		ctrlr->qcom_tcsr_is_direct_map = false;
	}
	ctrlr->qcom_tcsr_bus_tag = NULL;
	ctrlr->qcom_tcsr_bus_handle = 0;
	ctrlr->qcom_tcsr_paddr = 0;

	if (ctrlr->qcom_gcc_is_direct_map) {
		pmap_unmapdev((void *)ctrlr->qcom_gcc_bus_handle,
		    UFSHCI_QCOM_X1E80100_GCC_WINDOW_SIZE);
		ctrlr->qcom_gcc_is_direct_map = false;
	}
	ctrlr->qcom_gcc_bus_tag = NULL;
	ctrlr->qcom_gcc_bus_handle = 0;
	ctrlr->qcom_gcc_paddr = 0;
	ctrlr->qcom_acpi_bringup_ready = false;
	ctrlr->qcom_acpi_bringup_enabled = false;
	ctrlr->qcom_acpi_pwrseq_done = false;

	if (ctrlr->resource != NULL) {
		bus_release_resource(ctrlr->dev, SYS_RES_MEMORY,
		    ctrlr->resource_id, ctrlr->resource);
		ctrlr->resource = NULL;
	}

	if (ctrlr->tag != NULL) {
		bus_teardown_intr(ctrlr->dev, ctrlr->res, ctrlr->tag);
		ctrlr->tag = NULL;
	}

	if (ctrlr->res != NULL) {
		bus_release_resource(ctrlr->dev, SYS_RES_IRQ,
		    rman_get_rid(ctrlr->res), ctrlr->res);
		ctrlr->res = NULL;
	}
}

static int
ufshci_acpi_setup_shared(struct ufshci_controller *ctrlr)
{
	int error;

	ctrlr->num_io_queues = 1;
	ctrlr->rid = 0;
	ctrlr->res = bus_alloc_resource_any(ctrlr->dev, SYS_RES_IRQ,
	    &ctrlr->rid, RF_SHAREABLE | RF_ACTIVE);
	if (ctrlr->res == NULL) {
		ufshci_printf(ctrlr, "unable to allocate shared interrupt\n");
		return (ENOMEM);
	}

	error = bus_setup_intr(ctrlr->dev, ctrlr->res,
	    INTR_TYPE_MISC | INTR_MPSAFE, NULL, ufshci_ctrlr_shared_handler,
	    ctrlr, &ctrlr->tag);
	if (error) {
		ufshci_printf(ctrlr, "unable to setup shared interrupt\n");
		return (error);
	}

	return (0);
}

static int
ufshci_acpi_setup_interrupts(struct ufshci_controller *ctrlr)
{
	int num_io_queues, per_cpu_io_queues, min_cpus_per_ioq;

	/*
	 * TODO: Need to implement MCQ(Multi Circular Queue)
	 * Example: num_io_queues = mp_ncpus;
	 */
	num_io_queues = 1;
	TUNABLE_INT_FETCH("hw.ufshci.num_io_queues", &num_io_queues);
	if (num_io_queues < 1 || num_io_queues > mp_ncpus)
		num_io_queues = mp_ncpus;

	per_cpu_io_queues = 1;
	TUNABLE_INT_FETCH("hw.ufshci.per_cpu_io_queues", &per_cpu_io_queues);
	if (per_cpu_io_queues == 0)
		num_io_queues = 1;

	min_cpus_per_ioq = smp_threads_per_core;
	TUNABLE_INT_FETCH("hw.ufshci.min_cpus_per_ioq", &min_cpus_per_ioq);
	if (min_cpus_per_ioq > 1) {
		num_io_queues = min(num_io_queues,
		    max(1, mp_ncpus / min_cpus_per_ioq));
	}

	if (num_io_queues > vm_ndomains)
		num_io_queues -= num_io_queues % vm_ndomains;

	ctrlr->num_io_queues = num_io_queues;
	return (ufshci_acpi_setup_shared(ctrlr));
}

static int
ufshci_acpi_attach(device_t dev)
{
	struct ufshci_controller *ctrlr = device_get_softc(dev);
	int status;

	ctrlr->dev = dev;
	status = ufshci_acpi_allocate_memory(ctrlr);
	if (status != 0)
		goto bad;

	if ((ctrlr->quirks & UFSHCI_QUIRK_QCOM_CORE_CLK_300MHZ) != 0 &&
	    acpi_spmc_dump_qcom_device(dev) != 0)
		ufshci_printf(ctrlr,
		    "QCOM PEP power recipe was not found in ACPI\n");

	status = ufshci_acpi_setup_interrupts(ctrlr);
	if (status != 0)
		goto bad;

	status = ufshci_attach(dev);
	if (status != 0)
		goto bad;

	return (0);
bad:
	ufshci_acpi_release_resources(ctrlr);
	return (status);
}

static int
ufshci_acpi_detach(device_t dev)
{
	struct ufshci_controller *ctrlr = device_get_softc(dev);
	int error;

	error = ufshci_detach(dev);
	ufshci_acpi_release_resources(ctrlr);
	return (error);
}

static int
ufshci_acpi_suspend(device_t dev)
{
	struct ufshci_controller *ctrlr = device_get_softc(dev);
	int error;

	error = bus_generic_suspend(dev);
	if (error)
		return (error);

	/* Currently, PCI-based ufshci only supports POWER_STYPE_STANDBY */
	error = ufshci_ctrlr_suspend(ctrlr, POWER_STYPE_STANDBY);
	if (error == 0)
		ctrlr->qcom_acpi_pwrseq_done = false;
	return (error);
}

static int
ufshci_acpi_resume(device_t dev)
{
	struct ufshci_controller *ctrlr = device_get_softc(dev);
	int error;

	error = ufshci_ctrlr_resume(ctrlr, POWER_STYPE_AWAKE);
	if (error)
		return (error);

	error = bus_generic_resume(dev);
	return (error);
}
