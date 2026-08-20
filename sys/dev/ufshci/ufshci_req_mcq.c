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
#include <sys/smp.h>

#include "ufshci_private.h"
#include "ufshci_reg.h"

/*
 * Each hardware queue pairs a submission queue ring with a completion
 * queue ring. Both rings share one two-page DMA allocation. The SQ
 * ring fills the first page and the CQ ring starts at the second page.
 */
int
ufshci_req_mcq_construct(struct ufshci_controller *ctrlr,
    struct ufshci_req_queue *req_queue, uint32_t num_entries,
    bool is_task_mgmt)
{
	struct ufshci_hw_queue *hwq;
	struct ufshci_tracker *tr;
	uint64_t queuemem_phys;
	uint8_t *queuemem;
	size_t alloc_size;
	uint32_t num_q;
	uint32_t qid;
	uint32_t i;
	int cpu;
	int error;

	KASSERT(!is_task_mgmt,
	    ("Task management requests support only the single doorbell"));
	KASSERT(num_entries * sizeof(struct ufshci_utp_xfer_req_desc) <=
		ctrlr->page_size,
	    ("The submission queue ring must fit in one page"));

	req_queue->ctrlr = ctrlr;
	req_queue->is_task_mgmt = is_task_mgmt;
	req_queue->num_entries = num_entries;
	/* Every hardware queue owns one tracker per ring entry. */
	req_queue->num_trackers = num_entries;
	/* Queue 0 is the admin queue. The I/O queues follow it. */
	num_q = ctrlr->num_io_queues + 1;

	req_queue->hwq = malloc(sizeof(struct ufshci_hw_queue) * num_q,
	    M_UFSHCI, M_ZERO | M_NOWAIT);
	if (req_queue->hwq == NULL)
		return (ENOMEM);

	/*
	 * The interrupt handler walks the queues by this count, so
	 * publish it only once every queue is ready to be walked.
	 */
	req_queue->num_q = num_q;

	for (qid = 0; qid < req_queue->num_q; qid++) {
		hwq = &req_queue->hwq[qid];

		hwq->id = qid;
		hwq->num_entries = req_queue->num_entries;
		hwq->num_trackers = req_queue->num_trackers;
		hwq->ctrlr = ctrlr;
		hwq->req_queue = req_queue;
		hwq->domain = req_queue->domain;
		/*
		 * Run the watchdog on a CPU that submits to this queue.
		 * The callout needs a present CPU, and CPU ids can be
		 * sparse, so walk the present ones. The admin queue
		 * stays on the first CPU.
		 */
		hwq->cpu = CPU_FIRST();
		if (qid != UFSHCI_MCQ_ADMIN_Q) {
			CPU_FOREACH(cpu) {
				if (UFSHCI_QP(ctrlr, cpu) == qid - 1) {
					hwq->cpu = cpu;
					break;
				}
			}
		}

		mtx_init(&hwq->qlock, "ufshci mcq lock", NULL, MTX_DEF);
		mtx_init(&hwq->recovery_lock, "ufshci mcq recovery lock",
		    NULL, MTX_DEF);

		callout_init_mtx(&hwq->timer, &hwq->recovery_lock, 0);
		hwq->timer_armed = false;
		hwq->recovery_state = RECOVERY_WAITING;

		/*
		 * Allocate physical memory for the SQ and CQ rings.
		 * Note: The ring base addresses are restricted to
		 * 1024-byte alignment. Page alignment keeps the CQ ring
		 * page-aligned as well.
		 */
		alloc_size = 2 * ctrlr->page_size;
		error = bus_dma_tag_create(bus_get_dma_tag(ctrlr->dev),
		    ctrlr->page_size, 0, BUS_SPACE_MAXADDR,
		    BUS_SPACE_MAXADDR, NULL, NULL, alloc_size, 1, alloc_size,
		    0, NULL, NULL, &hwq->dma_tag_queue);
		if (error != 0) {
			ufshci_printf(ctrlr,
			    "queue ring tag create failed %d\n", error);
			goto out;
		}

		error = bus_dmamem_alloc(hwq->dma_tag_queue,
		    (void **)&queuemem, BUS_DMA_COHERENT | BUS_DMA_NOWAIT,
		    &hwq->queuemem_map);
		if (error != 0) {
			ufshci_printf(ctrlr,
			    "failed to allocate queue ring memory\n");
			goto out;
		}

		error = bus_dmamap_load(hwq->dma_tag_queue,
		    hwq->queuemem_map, queuemem, alloc_size,
		    ufshci_single_map, &queuemem_phys, 0);
		if (error != 0) {
			ufshci_printf(ctrlr,
			    "failed to load queue ring memory\n");
			bus_dmamem_free(hwq->dma_tag_queue, queuemem,
			    hwq->queuemem_map);
			goto out;
		}

		hwq->utrd = (struct ufshci_utp_xfer_req_desc *)queuemem;
		hwq->cqe = (struct ufshci_completion_queue_entry *)(queuemem +
		    ctrlr->page_size);
		hwq->req_queue_addr = queuemem_phys;
		hwq->cq_queue_addr = queuemem_phys + ctrlr->page_size;

		hwq->num_cmds = 0;
		hwq->num_intr_handler_calls = 0;
		hwq->num_retries = 0;
		hwq->num_failures = 0;

		hwq->sq_head = 0;
		hwq->sq_tail = 0;
		hwq->cq_head = 0;

		/* Allocate trackers */
		hwq->act_tr = malloc_domainset(
		    sizeof(struct ufshci_tracker *) * hwq->num_trackers,
		    M_UFSHCI, DOMAINSET_PREF(hwq->domain), M_ZERO | M_WAITOK);

		TAILQ_INIT(&hwq->free_tr);
		TAILQ_INIT(&hwq->outstanding_tr);

		for (i = 0; i < hwq->num_trackers; i++) {
			tr = malloc_domainset(sizeof(struct ufshci_tracker),
			    M_UFSHCI, DOMAINSET_PREF(hwq->domain),
			    M_ZERO | M_WAITOK);

			tr->req_queue = req_queue;
			tr->hwq = hwq;
			tr->slot_num = i;
			tr->slot_state = UFSHCI_SLOT_STATE_FREE;
			TAILQ_INSERT_HEAD(&hwq->free_tr, tr, tailq);

			hwq->act_tr[i] = tr;
		}

		/* Allocate the UTP command descriptor pool. */
		error = ufshci_req_queue_cmd_desc_construct(req_queue, hwq,
		    num_entries, ctrlr);
		if (error != 0) {
			ufshci_printf(ctrlr,
			    "failed to construct cmd descriptor memory\n");
			goto out;
		}
	}

	/*
	 * No queue register is programmed here. A host controller
	 * reset clears them all, so the enable path programs every
	 * queue register on each enable.
	 */
	return (0);
out:
	ufshci_req_mcq_destroy(ctrlr, req_queue);
	return (error);
}

void
ufshci_req_mcq_destroy(struct ufshci_controller *ctrlr,
    struct ufshci_req_queue *req_queue)
{
	struct ufshci_hw_queue *hwq;
	uint32_t mcqcap;
	uint32_t qid;
	uint32_t i;
	uint8_t qcfgptr;

	if (req_queue->hwq == NULL)
		return;

	mcqcap = ufshci_mmio_read_4(ctrlr, mcqcap);
	qcfgptr = UFSHCIV(UFSHCI_MCQCAP_REG_QCFGPTR, mcqcap);

	for (qid = 0; qid < req_queue->num_q; qid++) {
		hwq = &req_queue->hwq[qid];

		/* Skip the queues a failed construct never reached. */
		if (!mtx_initialized(&hwq->recovery_lock))
			continue;

		/* Quiesce the queue registers. */
		ufshci_mmio_write_4_off(ctrlr,
		    UFSHCI_MCQ_SQATTR(qcfgptr, qid), 0);
		ufshci_mmio_write_4_off(ctrlr,
		    UFSHCI_MCQ_CQATTR(qcfgptr, qid), 0);

		mtx_lock(&hwq->recovery_lock);
		hwq->timer_armed = false;
		mtx_unlock(&hwq->recovery_lock);
		callout_drain(&hwq->timer);

		if (hwq->act_tr != NULL) {
			ufshci_req_queue_cmd_desc_destroy(req_queue, hwq);

			for (i = 0; i < hwq->num_trackers; i++)
				free(hwq->act_tr[i], M_UFSHCI);

			free(hwq->act_tr, M_UFSHCI);
			hwq->act_tr = NULL;
		}

		if (hwq->utrd != NULL) {
			bus_dmamap_unload(hwq->dma_tag_queue,
			    hwq->queuemem_map);
			bus_dmamem_free(hwq->dma_tag_queue, hwq->utrd,
			    hwq->queuemem_map);
			hwq->utrd = NULL;
			hwq->cqe = NULL;
		}

		if (hwq->dma_tag_queue) {
			bus_dma_tag_destroy(hwq->dma_tag_queue);
			hwq->dma_tag_queue = NULL;
		}

		mtx_destroy(&hwq->recovery_lock);
		mtx_destroy(&hwq->qlock);
	}


	free(req_queue->hwq, M_UFSHCI);
	req_queue->hwq = NULL;
	req_queue->num_q = 0;
}

struct ufshci_hw_queue *
ufshci_req_mcq_get_hw_queue(struct ufshci_req_queue *req_queue, uint32_t qid)
{
	KASSERT(qid < req_queue->num_q, ("Invalid queue id"));
	return (&req_queue->hwq[qid]);
}

static int
ufshci_req_mcq_enable_hwq(struct ufshci_controller *ctrlr,
    struct ufshci_hw_queue *hwq, uint8_t qcfgptr)
{
	uint32_t qid = hwq->id;
	uint32_t sqattr, cqattr, size;
	uint32_t sqhp, cqtp;
	int error = 0;

	mtx_lock(&hwq->recovery_lock);
	mtx_lock(&hwq->qlock);

	/*
	 * Disable the queues first. The controller registers a queue
	 * when its enable bit rises from zero to one, so a re-enable
	 * must pass through zero to pick up the ring addresses again.
	 */
	ufshci_mmio_write_4_off(ctrlr, UFSHCI_MCQ_SQATTR(qcfgptr, qid), 0);
	ufshci_mmio_write_4_off(ctrlr, UFSHCI_MCQ_CQATTR(qcfgptr, qid), 0);

	/* Program the ring base addresses. */
	ufshci_mmio_write_4_off(ctrlr, UFSHCI_MCQ_CQLBA(qcfgptr, qid),
	    hwq->cq_queue_addr & 0xffffffff);
	ufshci_mmio_write_4_off(ctrlr, UFSHCI_MCQ_CQUBA(qcfgptr, qid),
	    hwq->cq_queue_addr >> 32);
	ufshci_mmio_write_4_off(ctrlr, UFSHCI_MCQ_SQLBA(qcfgptr, qid),
	    hwq->req_queue_addr & 0xffffffff);
	ufshci_mmio_write_4_off(ctrlr, UFSHCI_MCQ_SQUBA(qcfgptr, qid),
	    hwq->req_queue_addr >> 32);

	/* The controller reports where its runtime registers live. */
	hwq->sqdao = ufshci_mmio_read_4_off(ctrlr,
	    UFSHCI_MCQ_SQDAO(qcfgptr, qid));
	hwq->sqisao = ufshci_mmio_read_4_off(ctrlr,
	    UFSHCI_MCQ_SQISAO(qcfgptr, qid));
	hwq->cqdao = ufshci_mmio_read_4_off(ctrlr,
	    UFSHCI_MCQ_CQDAO(qcfgptr, qid));
	hwq->cqisao = ufshci_mmio_read_4_off(ctrlr,
	    UFSHCI_MCQ_CQISAO(qcfgptr, qid));
	if (hwq->sqdao == 0 || hwq->sqisao == 0 || hwq->cqdao == 0 ||
	    hwq->cqisao == 0) {
		ufshci_printf(ctrlr,
		    "queue %u reports no runtime register offsets\n", qid);
		error = ENXIO;
		goto out;
	}

	/* Clear a stale completion interrupt and enable it. */
	ufshci_mmio_write_4_off(ctrlr, UFSHCI_MCQ_CQIS(hwq->cqisao),
	    UFSHCIM(UFSHCI_CQIS_REG_TEPS));
	ufshci_mmio_write_4_off(ctrlr, UFSHCI_MCQ_CQIE(hwq->cqisao),
	    UFSHCIM(UFSHCI_CQIE_REG_TEPE));

	/*
	 * Enable the queues. The CQ must exist before the SQ that
	 * points at it. SIZE is a 0's based value in dword units.
	 */
	size = (hwq->num_entries *
	    sizeof(struct ufshci_completion_queue_entry) /
	    sizeof(uint32_t)) - 1;
	cqattr = UFSHCIM(UFSHCI_CQATTR_REG_CQEN) |
	    UFSHCIF(UFSHCI_CQATTR_REG_SIZE, size);
	ufshci_mmio_write_4_off(ctrlr, UFSHCI_MCQ_CQATTR(qcfgptr, qid),
	    cqattr);

	size = (hwq->num_entries * sizeof(struct ufshci_utp_xfer_req_desc) /
	    sizeof(uint32_t)) - 1;
	sqattr = UFSHCIM(UFSHCI_SQATTR_REG_SQEN) |
	    UFSHCIF(UFSHCI_SQATTR_REG_CQID, qid) |
	    UFSHCIF(UFSHCI_SQATTR_REG_SIZE, size);
	ufshci_mmio_write_4_off(ctrlr, UFSHCI_MCQ_SQATTR(qcfgptr, qid),
	    sqattr);

	/* The queues must report themselves as enabled. */
	if (!(ufshci_mmio_read_4_off(ctrlr, UFSHCI_MCQ_CQATTR(qcfgptr, qid)) &
		UFSHCIM(UFSHCI_CQATTR_REG_CQEN)) ||
	    !(ufshci_mmio_read_4_off(ctrlr, UFSHCI_MCQ_SQATTR(qcfgptr, qid)) &
		UFSHCIM(UFSHCI_SQATTR_REG_SQEN))) {
		ufshci_printf(ctrlr, "queue %u did not come up\n", qid);
		error = ENXIO;
		goto out;
	}

	/*
	 * A controller may keep its ring pointers across a reset.
	 * Adopt them instead of assuming zero. Move the tails and
	 * heads together so both rings start out empty, and everything
	 * stale on them stays invisible. The pointer writes need the
	 * queues enabled, so this must come last.
	 */
	sqhp = ufshci_mmio_read_4_off(ctrlr, UFSHCI_MCQ_SQHP(hwq->sqdao));
	hwq->sq_head = hwq->sq_tail = (sqhp /
	    sizeof(struct ufshci_utp_xfer_req_desc)) % hwq->num_entries;
	ufshci_mmio_write_4_off(ctrlr, UFSHCI_MCQ_SQTP(hwq->sqdao),
	    hwq->sq_tail * sizeof(struct ufshci_utp_xfer_req_desc));

	cqtp = ufshci_mmio_read_4_off(ctrlr, UFSHCI_MCQ_CQTP(hwq->cqdao));
	hwq->cq_head = (cqtp /
	    sizeof(struct ufshci_completion_queue_entry)) % hwq->num_entries;
	ufshci_mmio_write_4_off(ctrlr, UFSHCI_MCQ_CQHP(hwq->cqdao),
	    hwq->cq_head * sizeof(struct ufshci_completion_queue_entry));

	KASSERT(!ctrlr->is_failed, ("Enabling a failed hwq\n"));
	hwq->recovery_state = RECOVERY_NONE;

out:
	mtx_unlock(&hwq->qlock);
	mtx_unlock(&hwq->recovery_lock);
	return (error);
}

int
ufshci_req_mcq_enable(struct ufshci_controller *ctrlr,
    struct ufshci_req_queue *req_queue)
{
	uint32_t config, mcqconfig, mcqcap;
	uint32_t max_active_cmds;
	uint32_t qid;
	uint8_t qcfgptr;
	int error;

	/* Select the MCQ queue type. */
	config = ufshci_mmio_read_4(ctrlr, config);
	config |= UFSHCIM(UFSHCI_CONFIG_REG_QT);
	ufshci_mmio_write_4(ctrlr, config, config);

	/*
	 * Let the device hold as many active commands as the
	 * controller supports. Both fields are 0's based values.
	 */
	max_active_cmds = UFSHCIV(UFSHCI_CAP_REG_NUTRS, ctrlr->cap) + 1;
	mcqconfig = ufshci_mmio_read_4(ctrlr, mcqconfig);
	mcqconfig &= ~UFSHCIM(UFSHCI_MCQCONFIG_REG_MAC);
	mcqconfig |= UFSHCIF(UFSHCI_MCQCONFIG_REG_MAC, max_active_cmds - 1);
	ufshci_mmio_write_4(ctrlr, mcqconfig, mcqconfig);

	mcqcap = ufshci_mmio_read_4(ctrlr, mcqcap);
	qcfgptr = UFSHCIV(UFSHCI_MCQCAP_REG_QCFGPTR, mcqcap);

	/*
	 * The controller reports where its queue banks live. Bound the
	 * last bank against the mapped register window before writing
	 * through it.
	 */
	if (UFSHCI_MCQ_QCFG(qcfgptr, req_queue->num_q - 1) +
		UFSHCI_MCQ_QCFG_SIZE >
	    rman_get_size(ctrlr->resource)) {
		ufshci_printf(ctrlr,
		    "queue config pointer %u does not fit the register "
		    "window\n", qcfgptr);
		return (ENXIO);
	}

	for (qid = 0; qid < req_queue->num_q; qid++) {
		error = ufshci_req_mcq_enable_hwq(ctrlr,
		    &req_queue->hwq[qid], qcfgptr);
		if (error != 0) {
			/*
			 * Put the queues that already came up back into
			 * recovery. A live queue would take requests
			 * the controller can no longer complete.
			 */
			while (qid-- > 0) {
				mtx_lock(&req_queue->hwq[qid].recovery_lock);
				req_queue->hwq[qid].recovery_state =
				    RECOVERY_WAITING;
				mtx_unlock(&req_queue->hwq[qid].recovery_lock);
			}
			return (error);
		}
	}

	/*
	 * The disable path claimed the requests that were on the
	 * rings a reset wiped. Return them as aborted only after
	 * every queue is back. A completion here can resubmit onto
	 * any queue right away. An aborted admin request retries onto
	 * its own ring, and a CAM retry can pick any I/O queue.
	 */
	for (qid = 0; qid < req_queue->num_q; qid++)
		ufshci_req_queue_complete_aborted_hwq(&req_queue->hwq[qid]);

	return (0);
}

int
ufshci_req_mcq_reserve_slot(struct ufshci_hw_queue *hwq,
    struct ufshci_tracker **tr)
{
	struct ufshci_controller *ctrlr = hwq->ctrlr;
	uint32_t sq_head;

	mtx_assert(&hwq->qlock, MA_OWNED);

	/*
	 * The ring is full when the next tail would catch up with the
	 * head. Refresh the head from the controller before giving up.
	 */
	if ((hwq->sq_tail + 1) % hwq->num_entries == hwq->sq_head) {
		sq_head = (ufshci_mmio_read_4_off(ctrlr,
			       UFSHCI_MCQ_SQHP(hwq->sqdao)) /
			      sizeof(struct ufshci_utp_xfer_req_desc)) %
		    hwq->num_entries;
		hwq->sq_head = sq_head;
		if ((hwq->sq_tail + 1) % hwq->num_entries == sq_head)
			return (EBUSY);
	}

	*tr = TAILQ_FIRST(&hwq->free_tr);
	if (*tr == NULL)
		return (EBUSY);
	(*tr)->hwq = hwq;

	return (0);
}

void
ufshci_req_mcq_ring_doorbell(struct ufshci_controller *ctrlr,
    struct ufshci_tracker *tr)
{
	struct ufshci_hw_queue *hwq = tr->hwq;

	mtx_assert(&hwq->qlock, MA_OWNED);

	/* The tail pointer register takes a byte offset. */
	hwq->sq_tail = (hwq->sq_tail + 1) % hwq->num_entries;
	ufshci_mmio_write_4_off(ctrlr, UFSHCI_MCQ_SQTP(hwq->sqdao),
	    hwq->sq_tail * sizeof(struct ufshci_utp_xfer_req_desc));

	hwq->num_cmds++;
}

/*
 * Map a completion queue entry back to its tracker through the
 * physical address of the UTP command descriptor.
 */
static struct ufshci_tracker *
ufshci_req_mcq_cqe_to_tracker(struct ufshci_hw_queue *hwq,
    struct ufshci_completion_queue_entry *cqe)
{
	bus_addr_t ucd_addr;
	uint32_t i;

	ucd_addr = cqe->utp_cmd_desc_base_addr &
	    UFSHCI_CQE_UCD_BASE_ADDR_MASK;

	for (i = 0; i < hwq->num_trackers; i++) {
		if (hwq->ucd_bus_addr[i] == ucd_addr)
			return (hwq->act_tr[i]);
	}

	return (NULL);
}

bool
ufshci_req_mcq_process_cpl(struct ufshci_hw_queue *hwq)
{
	struct ufshci_controller *ctrlr = hwq->ctrlr;
	struct ufshci_completion_queue_entry *cqe;
	struct ufshci_tracker *tr;
	uint32_t cq_tail;
	bool completed;
	bool done = false;

	mtx_assert(&hwq->recovery_lock, MA_OWNED);

	hwq->num_intr_handler_calls++;

	/*
	 * A queue that never came up has no register offsets. Offset
	 * zero names the capability register, so touching it here
	 * would write to a read-only register.
	 */
	if (hwq->cqisao == 0 || hwq->sqisao == 0)
		return (false);

	/*
	 * Clear the interrupt status first. A completion that arrives
	 * during the scan raises it again, so no event is lost.
	 */
	ufshci_mmio_write_4_off(ctrlr, UFSHCI_MCQ_CQIS(hwq->cqisao),
	    UFSHCIM(UFSHCI_CQIS_REG_TEPS));

	bus_dmamap_sync(hwq->dma_tag_queue, hwq->queuemem_map,
	    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);

	/*
	 * The tail pointer register carries a byte offset. Keep it
	 * inside the ring. A value the loop below can never reach
	 * would spin it forever.
	 */
	cq_tail = (ufshci_mmio_read_4_off(ctrlr,
		       UFSHCI_MCQ_CQTP(hwq->cqdao)) /
		      sizeof(struct ufshci_completion_queue_entry)) %
	    hwq->num_entries;

	while (hwq->cq_head != cq_tail) {
		cqe = &hwq->cqe[hwq->cq_head];
		tr = ufshci_req_mcq_cqe_to_tracker(hwq, cqe);
		hwq->cq_head = (hwq->cq_head + 1) % hwq->num_entries;

		if (tr == NULL) {
			ufshci_printf(ctrlr,
			    "queue %u completed an unknown descriptor "
			    "address\n", hwq->id);
			continue;
		}

		/*
		 * The failure path claims a scheduled slot under the
		 * queue lock and completes it manually. Skip such a
		 * slot here so it does not complete twice.
		 */
		mtx_lock(&hwq->qlock);
		completed = tr->slot_state == UFSHCI_SLOT_STATE_SCHEDULED;
		mtx_unlock(&hwq->qlock);

		if (completed) {
			tr->ocs = cqe->overall_command_status;
			ufshci_req_queue_complete_tracker(tr);
			done = true;
		}

		/* Pick up the entries pushed during the scan. */
		if (hwq->cq_head == cq_tail)
			cq_tail = (ufshci_mmio_read_4_off(ctrlr,
				       UFSHCI_MCQ_CQTP(hwq->cqdao)) /
				      sizeof(struct
					  ufshci_completion_queue_entry)) %
			    hwq->num_entries;
	}

	/* Hand the consumed entries back to the controller. */
	ufshci_mmio_write_4_off(ctrlr, UFSHCI_MCQ_CQHP(hwq->cqdao),
	    hwq->cq_head * sizeof(struct ufshci_completion_queue_entry));

	return (done);
}

void
ufshci_req_mcq_clear_cpl_ntf(struct ufshci_controller *ctrlr,
    struct ufshci_tracker *tr)
{
	/*
	 * NOP
	 * MCQ has no completion notification register. The CQ head
	 * pointer update in the completion scan takes its place.
	 */
}

int
ufshci_req_mcq_get_inflight_io(struct ufshci_controller *ctrlr)
{
	/* TODO: Implement inflight io */

	return (0);
}

void
ufshci_req_mcq_disable(struct ufshci_controller *ctrlr,
    struct ufshci_req_queue *req_queue)
{
	struct ufshci_hw_queue *hwq;
	struct ufshci_tracker *tr, *tr_temp;
	uint32_t qid;

	for (qid = 0; qid < req_queue->num_q; qid++) {
		hwq = &req_queue->hwq[qid];

		mtx_lock(&hwq->recovery_lock);
		mtx_lock(&hwq->qlock);

		hwq->recovery_state = RECOVERY_WAITING;
		TAILQ_FOREACH_SAFE(tr, &hwq->outstanding_tr, tailq,
		    tr_temp) {
			tr->deadline = SBT_MAX;

			/*
			 * A failed controller never enables the queues
			 * again, so the failure path owns the requests.
			 * Claiming them here would strand them.
			 */
			if (ctrlr->is_failed)
				continue;

			/*
			 * Claim the tracker. The rings restart from a
			 * clean state, so its completion never comes.
			 * The enable path returns it as aborted, and
			 * the claim keeps the completion scan away.
			 */
			if (tr->slot_state == UFSHCI_SLOT_STATE_SCHEDULED)
				tr->slot_state =
				    UFSHCI_SLOT_STATE_NEED_ERROR_HANDLING;
		}

		mtx_unlock(&hwq->qlock);
		mtx_unlock(&hwq->recovery_lock);
	}
}
