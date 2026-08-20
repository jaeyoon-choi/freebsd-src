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
	uint32_t qid;
	uint32_t i;

	if (req_queue->hwq == NULL)
		return;

	for (qid = 0; qid < req_queue->num_q; qid++) {
		hwq = &req_queue->hwq[qid];

		/* Skip the queues a failed construct never reached. */
		if (!mtx_initialized(&hwq->recovery_lock))
			continue;

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
