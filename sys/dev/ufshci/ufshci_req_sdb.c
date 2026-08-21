/*-
 * Copyright (c) 2025, Samsung Electronics Co., Ltd.
 * Written by Jaeyoon Choi
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/domainset.h>
#include <sys/module.h>

#include "sys/kassert.h"
#include "ufshci_private.h"
#include "ufshci_reg.h"

int
ufshci_req_sdb_construct(struct ufshci_controller *ctrlr,
    struct ufshci_req_queue *req_queue, uint32_t num_entries, bool is_task_mgmt)
{
	struct ufshci_hw_queue *hwq;
	size_t desc_size, alloc_size;
	uint64_t queuemem_phys;
	uint8_t *queuemem;
	struct ufshci_tracker *tr;
	const size_t lock_name_len = 32;
	char qlock_name[lock_name_len], recovery_lock_name[lock_name_len];
	char *base;
	int i, error;

	req_queue->ctrlr = ctrlr;
	req_queue->is_task_mgmt = is_task_mgmt;
	req_queue->num_entries = num_entries;
	/*
	 * In Single Doorbell mode, the number of queue entries and the number
	 * of trackers are the same.
	 */
	req_queue->num_trackers = num_entries;
	req_queue->num_q = 1;

	/* Single Doorbell mode uses only one queue. (UFSHCI_SDB_Q = 0) */
	req_queue->hwq = malloc(sizeof(struct ufshci_hw_queue), M_UFSHCI,
	    M_ZERO | M_NOWAIT);
	if (req_queue->hwq == NULL)
		return (ENOMEM);
	hwq = &req_queue->hwq[UFSHCI_SDB_Q];
	hwq->num_entries = req_queue->num_entries;
	hwq->num_trackers = req_queue->num_trackers;
	hwq->ctrlr = ctrlr;
	hwq->req_queue = req_queue;

	base = is_task_mgmt ? "ufshci utmrq" : "ufshci utrq";
	snprintf(qlock_name, sizeof(qlock_name), "%s #%d lock", base,
	    UFSHCI_SDB_Q);
	snprintf(recovery_lock_name, sizeof(recovery_lock_name),
	    "%s #%d recovery lock", base, UFSHCI_SDB_Q);

	mtx_init(&hwq->qlock, qlock_name, NULL, MTX_DEF);
	mtx_init(&hwq->recovery_lock, recovery_lock_name, NULL, MTX_DEF);

	callout_init_mtx(&hwq->timer, &hwq->recovery_lock, 0);
	hwq->timer_armed = false;
	hwq->recovery_state = RECOVERY_WAITING;

	/*
	 * Allocate physical memory for request queue (UTP Transfer Request
	 * Descriptor (UTRD) or UTP Task Management Request Descriptor (UTMRD))
	 * Note: UTRD/UTMRD format is restricted to 1024-byte alignment.
	 */
	desc_size = is_task_mgmt ?
	    sizeof(struct ufshci_utp_task_mgmt_req_desc) :
	    sizeof(struct ufshci_utp_xfer_req_desc);
	alloc_size = num_entries * desc_size;
	error = bus_dma_tag_create(bus_get_dma_tag(ctrlr->dev), 1024,
	    ctrlr->page_size, BUS_SPACE_MAXADDR, BUS_SPACE_MAXADDR, NULL, NULL,
	    alloc_size, 1, alloc_size, 0, NULL, NULL, &hwq->dma_tag_queue);
	if (error != 0) {
		ufshci_printf(ctrlr, "request queue tag create failed %d\n",
		    error);
		goto out;
	}

	error = bus_dmamem_alloc(hwq->dma_tag_queue, (void **)&queuemem,
	    BUS_DMA_COHERENT | BUS_DMA_NOWAIT, &hwq->queuemem_map);
	if (error != 0) {
		ufshci_printf(ctrlr,
		    "failed to allocate request queue memory\n");
		goto out;
	}

	error = bus_dmamap_load(hwq->dma_tag_queue, hwq->queuemem_map, queuemem,
	    alloc_size, ufshci_single_map, &queuemem_phys, 0);
	if (error != 0) {
		ufshci_printf(ctrlr, "failed to load request queue memory\n");
		bus_dmamem_free(hwq->dma_tag_queue, queuemem,
		    hwq->queuemem_map);
		goto out;
	}

	hwq->num_cmds = 0;
	hwq->num_intr_handler_calls = 0;
	hwq->num_retries = 0;
	hwq->num_failures = 0;
	hwq->req_queue_addr = queuemem_phys;

	/* Allocate trackers */
	hwq->act_tr = malloc_domainset(sizeof(struct ufshci_tracker *) *
		req_queue->num_entries,
	    M_UFSHCI, DOMAINSET_PREF(req_queue->domain), M_ZERO | M_WAITOK);

	TAILQ_INIT(&hwq->free_tr);
	TAILQ_INIT(&hwq->outstanding_tr);

	for (i = 0; i < req_queue->num_trackers; i++) {
		tr = malloc_domainset(sizeof(struct ufshci_tracker), M_UFSHCI,
		    DOMAINSET_PREF(req_queue->domain), M_ZERO | M_WAITOK);

		tr->req_queue = req_queue;
		tr->slot_num = i;
		tr->slot_state = UFSHCI_SLOT_STATE_FREE;
		TAILQ_INSERT_HEAD(&hwq->free_tr, tr, tailq);

		hwq->act_tr[i] = tr;
	}

	if (is_task_mgmt) {
		/* UTP Task Management Request (UTMR) */
		uint32_t utmrlba, utmrlbau;

		hwq->utmrd = (struct ufshci_utp_task_mgmt_req_desc *)queuemem;

		utmrlba = hwq->req_queue_addr & 0xffffffff;
		utmrlbau = hwq->req_queue_addr >> 32;
		ufshci_mmio_write_4(ctrlr, utmrlba, utmrlba);
		ufshci_mmio_write_4(ctrlr, utmrlbau, utmrlbau);
	} else {
		/* UTP Transfer Request (UTR) */
		uint32_t utrlba, utrlbau;

		hwq->utrd = (struct ufshci_utp_xfer_req_desc *)queuemem;

		/*
		 * Allocate physical memory for the command descriptor.
		 * UTP Transfer Request (UTR) requires memory for a separate
		 * command in addition to the queue.
		 */
		error = ufshci_req_queue_cmd_desc_construct(req_queue, hwq,
		    num_entries, ctrlr);
		if (error != 0) {
			ufshci_printf(ctrlr,
			    "failed to construct cmd descriptor memory\n");
			goto out;
		}

		utrlba = hwq->req_queue_addr & 0xffffffff;
		utrlbau = hwq->req_queue_addr >> 32;
		ufshci_mmio_write_4(ctrlr, utrlba, utrlba);
		ufshci_mmio_write_4(ctrlr, utrlbau, utrlbau);
	}

	return (0);
out:
	ufshci_req_sdb_destroy(ctrlr, req_queue);
	return (error);
}

void
ufshci_req_sdb_destroy(struct ufshci_controller *ctrlr,
    struct ufshci_req_queue *req_queue)
{
	struct ufshci_hw_queue *hwq;
	int i;

	if (req_queue->hwq == NULL)
		return;

	hwq = &req_queue->hwq[UFSHCI_SDB_Q];

	mtx_lock(&hwq->recovery_lock);
	hwq->timer_armed = false;
	mtx_unlock(&hwq->recovery_lock);
	callout_drain(&hwq->timer);

	if (hwq->act_tr != NULL) {
		if (!req_queue->is_task_mgmt) {
			ufshci_req_queue_cmd_desc_destroy(req_queue, hwq);
		}

		for (i = 0; i < req_queue->num_trackers; i++)
			free(hwq->act_tr[i], M_UFSHCI);

		free(hwq->act_tr, M_UFSHCI);
		hwq->act_tr = NULL;
	}

	if (hwq->utrd != NULL) {
		bus_dmamap_unload(hwq->dma_tag_queue, hwq->queuemem_map);
		bus_dmamem_free(hwq->dma_tag_queue, hwq->utrd,
		    hwq->queuemem_map);
		hwq->utrd = NULL;
	}

	if (hwq->dma_tag_queue) {
		bus_dma_tag_destroy(hwq->dma_tag_queue);
		hwq->dma_tag_queue = NULL;
	}

	mtx_destroy(&hwq->recovery_lock);
	mtx_destroy(&hwq->qlock);

	free(req_queue->hwq, M_UFSHCI);
	req_queue->hwq = NULL;
}

struct ufshci_hw_queue *
ufshci_req_sdb_get_hw_queue(struct ufshci_req_queue *req_queue, uint32_t qid)
{
	KASSERT(qid == UFSHCI_SDB_Q,
	    ("Single doorbell mode has only one queue"));
	return &req_queue->hwq[qid];
}

void
ufshci_req_sdb_disable(struct ufshci_controller *ctrlr,
    struct ufshci_req_queue *req_queue)
{
	struct ufshci_hw_queue *hwq = &req_queue->hwq[UFSHCI_SDB_Q];
	struct ufshci_tracker *tr, *tr_temp;

	mtx_lock(&hwq->recovery_lock);
	mtx_lock(&hwq->qlock);

	if (mtx_initialized(&hwq->recovery_lock))
		mtx_assert(&hwq->recovery_lock, MA_OWNED);
	if (mtx_initialized(&hwq->qlock))
		mtx_assert(&hwq->qlock, MA_OWNED);

	hwq->recovery_state = RECOVERY_WAITING;
	TAILQ_FOREACH_SAFE(tr, &hwq->outstanding_tr, tailq, tr_temp) {
		tr->deadline = SBT_MAX;

		/*
		 * A failed controller never enables the queue again, so
		 * the failure path owns the requests.
		 */
		if (ctrlr->is_failed)
			continue;

		/*
		 * Claim the tracker. A reset clears the doorbell
		 * register, which makes every slot look complete to the
		 * completion scan. The scan skips a claimed slot, and
		 * the enable path returns the request as aborted.
		 */
		if (tr->slot_state == UFSHCI_SLOT_STATE_SCHEDULED)
			tr->slot_state = UFSHCI_SLOT_STATE_NEED_ERROR_HANDLING;
	}

	mtx_unlock(&hwq->qlock);
	mtx_unlock(&hwq->recovery_lock);
}

int
ufshci_req_sdb_enable(struct ufshci_controller *ctrlr,
    struct ufshci_req_queue *req_queue)
{
	struct ufshci_hw_queue *hwq = &req_queue->hwq[UFSHCI_SDB_Q];
	int error = 0;

	mtx_lock(&hwq->recovery_lock);
	mtx_lock(&hwq->qlock);

	if (req_queue->is_task_mgmt) {
		uint32_t hcs, utmrldbr, utmrlrsr;
		uint32_t utmrlba, utmrlbau;

		/*
		 * Some controllers require re-enabling. When a controller is
		 * re-enabled, the utmrlba registers are initialized, and these
		 * must be reconfigured upon re-enabling.
		 */
		utmrlba = hwq->req_queue_addr & 0xffffffff;
		utmrlbau = hwq->req_queue_addr >> 32;
		ufshci_mmio_write_4(ctrlr, utmrlba, utmrlba);
		ufshci_mmio_write_4(ctrlr, utmrlbau, utmrlbau);

		hcs = ufshci_mmio_read_4(ctrlr, hcs);
		if (!(hcs & UFSHCIM(UFSHCI_HCS_REG_UTMRLRDY))) {
			ufshci_printf(ctrlr,
			    "UTP task management request list is not ready\n");
			error = ENXIO;
			goto out;
		}

		utmrldbr = ufshci_mmio_read_4(ctrlr, utmrldbr);
		if (utmrldbr != 0) {
			ufshci_printf(ctrlr,
			    "UTP task management request list door bell is not ready\n");
			error = ENXIO;
			goto out;
		}

		utmrlrsr = UFSHCIM(UFSHCI_UTMRLRSR_REG_UTMRLRSR);
		ufshci_mmio_write_4(ctrlr, utmrlrsr, utmrlrsr);
	} else {
		uint32_t hcs, utrldbr, utrlcnr, utrlrsr;
		uint32_t utrlba, utrlbau;

		/*
		 * Some controllers require re-enabling. When a controller is
		 * re-enabled, the utrlba registers are initialized, and these
		 * must be reconfigured upon re-enabling.
		 */
		utrlba = hwq->req_queue_addr & 0xffffffff;
		utrlbau = hwq->req_queue_addr >> 32;
		ufshci_mmio_write_4(ctrlr, utrlba, utrlba);
		ufshci_mmio_write_4(ctrlr, utrlbau, utrlbau);

		hcs = ufshci_mmio_read_4(ctrlr, hcs);
		if (!(hcs & UFSHCIM(UFSHCI_HCS_REG_UTRLRDY))) {
			ufshci_printf(ctrlr,
			    "UTP transfer request list is not ready\n");
			error = ENXIO;
			goto out;
		}

		utrldbr = ufshci_mmio_read_4(ctrlr, utrldbr);
		if (utrldbr != 0) {
			ufshci_printf(ctrlr,
			    "UTP transfer request list door bell is not ready\n");
			ufshci_printf(ctrlr,
			    "Clear the UTP transfer request list door bell\n");
			ufshci_mmio_write_4(ctrlr, utrldbr, utrldbr);
		}

		utrlcnr = ufshci_mmio_read_4(ctrlr, utrlcnr);
		if (utrlcnr != 0) {
			ufshci_printf(ctrlr,
			    "UTP transfer request list notification is not ready\n");
			ufshci_printf(ctrlr,
			    "Clear the UTP transfer request list notification\n");
			ufshci_mmio_write_4(ctrlr, utrlcnr, utrlcnr);
		}

		utrlrsr = UFSHCIM(UFSHCI_UTRLRSR_REG_UTRLRSR);
		ufshci_mmio_write_4(ctrlr, utrlrsr, utrlrsr);
	}

	if (mtx_initialized(&hwq->recovery_lock))
		mtx_assert(&hwq->recovery_lock, MA_OWNED);
	if (mtx_initialized(&hwq->qlock))
		mtx_assert(&hwq->qlock, MA_OWNED);
	KASSERT(!req_queue->ctrlr->is_failed, ("Enabling a failed hwq\n"));

	hwq->recovery_state = RECOVERY_NONE;

out:
	mtx_unlock(&hwq->qlock);
	mtx_unlock(&hwq->recovery_lock);

	/*
	 * Return the requests the disable path claimed. The queue is
	 * back, so an aborted request can retry right away.
	 */
	if (error == 0)
		ufshci_req_queue_complete_aborted_hwq(hwq);

	return (error);
}

int
ufshci_req_sdb_reserve_slot(struct ufshci_hw_queue *hwq,
    struct ufshci_tracker **tr, bool admin)
{
	uint32_t count;
	uint8_t i;

	/*
	 * Hold the last slot for admin requests. A reset sends its
	 * bring-up commands through this queue, and the I/O it just
	 * released must not take every slot before it gets there.
	 */
	count = hwq->num_entries;
	if (!admin && !hwq->req_queue->is_task_mgmt)
		count--;

	for (i = 0; i < count; i++) {
		if (hwq->act_tr[i]->slot_state == UFSHCI_SLOT_STATE_FREE) {
			*tr = hwq->act_tr[i];
			(*tr)->hwq = hwq;
			return (0);
		}
	}
	return (EBUSY);
}

void
ufshci_req_sdb_utmr_clear_cpl_ntf(struct ufshci_controller *ctrlr,
    struct ufshci_tracker *tr)
{
	/*
	 * NOP
	 * UTP Task Management does not have a Completion Notification
	 * Register.
	 */
}

void
ufshci_req_sdb_utr_clear_cpl_ntf(struct ufshci_controller *ctrlr,
    struct ufshci_tracker *tr)
{
	uint32_t utrlcnr;

	utrlcnr = 1 << tr->slot_num;
	ufshci_mmio_write_4(ctrlr, utrlcnr, utrlcnr);
}

void
ufshci_req_sdb_utmr_ring_doorbell(struct ufshci_controller *ctrlr,
    struct ufshci_tracker *tr)
{
	uint32_t utmrldbr = 0;

	utmrldbr |= 1 << tr->slot_num;
	ufshci_mmio_write_4(ctrlr, utmrldbr, utmrldbr);

	tr->req_queue->hwq[UFSHCI_SDB_Q].num_cmds++;
}

void
ufshci_req_sdb_utr_ring_doorbell(struct ufshci_controller *ctrlr,
    struct ufshci_tracker *tr)
{
	uint32_t utrldbr = 0;

	utrldbr |= 1 << tr->slot_num;
	ufshci_mmio_write_4(ctrlr, utrldbr, utrldbr);

	tr->req_queue->hwq[UFSHCI_SDB_Q].num_cmds++;
}

bool
ufshci_req_sdb_utmr_is_doorbell_cleared(struct ufshci_controller *ctrlr,
    uint8_t slot)
{
	uint32_t utmrldbr;

	utmrldbr = ufshci_mmio_read_4(ctrlr, utmrldbr);
	return (!(utmrldbr & (1 << slot)));
}

bool
ufshci_req_sdb_utr_is_doorbell_cleared(struct ufshci_controller *ctrlr,
    uint8_t slot)
{
	uint32_t utrldbr;

	utrldbr = ufshci_mmio_read_4(ctrlr, utrldbr);
	return (!(utrldbr & (1 << slot)));
}

bool
ufshci_req_sdb_process_cpl(struct ufshci_hw_queue *hwq)
{
	struct ufshci_req_queue *req_queue = hwq->req_queue;
	struct ufshci_tracker *tr;
	uint8_t slot;
	bool done = false;

	mtx_assert(&hwq->recovery_lock, MA_OWNED);

	hwq->num_intr_handler_calls++;

	bus_dmamap_sync(hwq->dma_tag_queue, hwq->queuemem_map,
	    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);

	for (slot = 0; slot < req_queue->num_entries; slot++) {
		bool completed;

		tr = hwq->act_tr[slot];

		KASSERT(tr, ("there is no tracker assigned to the slot"));
		/*
		 * When the response is delivered from the device, the doorbell
		 * is cleared. Check it under qlock so that a slot whose
		 * doorbell write is still in flight in the submit path is not
		 * mistaken for a completed one.
		 */
		mtx_lock(&hwq->qlock);
		completed = tr->slot_state == UFSHCI_SLOT_STATE_SCHEDULED &&
		    req_queue->qops.is_doorbell_cleared(req_queue->ctrlr,
			slot);
		/*
		 * Claim the slot while the lock is held. The reset path
		 * walks the same slots, so without a claim both paths
		 * could complete this request.
		 */
		if (completed)
			tr->slot_state = UFSHCI_SLOT_STATE_COMPLETING;
		mtx_unlock(&hwq->qlock);
		if (completed) {
			if (req_queue->is_task_mgmt)
				tr->ocs =
				    hwq->utmrd[slot].overall_command_status;
			else
				tr->ocs =
				    hwq->utrd[slot].overall_command_status;
			ufshci_req_queue_complete_tracker(tr);
			done = true;
		}
	}

	return (done);
}

int
ufshci_req_sdb_get_inflight_io(struct ufshci_controller *ctrlr)
{
	/* TODO: Implement inflight io*/

	return (0);
}
