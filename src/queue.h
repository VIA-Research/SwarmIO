// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on code from https://github.com/snu-csl/nvmevirt
 * Copyright (C) 2026 VIA-Research, modified on 2026-04-07
 */

#ifndef _SWARMIO_QUEUE_H
#define _SWARMIO_QUEUE_H

#include "common.h"

struct nvme_submission_queue {
	int qid;
	int cqid;

	int queue_size;
	int sq_head;

	struct nvme_command __iomem **sq;
	void *mapped;
	dma_addr_t dma_addr;

#ifdef CONFIG_SWARMIO_DISP_PREFETCH_SQE
	int pref_size;
	int dma_desc_id;
	bool is_pref_issued;
#endif

#if defined(CONFIG_SWARMIO_DISP_COALESCE_SQE)
	int fetched_pos;
#if defined(CONFIG_SWARMIO_AGGREGATED_TIMING_UPDATE) && \
	defined(CONFIG_SWARMIO_PROFILE_REQ)
	unsigned long long *nsecs_starts;
#endif
	unsigned long long *nsecs_targets;
#endif

	struct nvme_command *cmd_buffer;
	dma_addr_t cmd_buffer_dma;
};

struct nvme_completion_queue {
	int qid;
	int irq_vector;
	bool irq_enabled;
	bool interrupt_ready;
	bool phys_contig;

	spinlock_t entry_lock;
	struct mutex irq_lock;

	int queue_size;

	int phase;
	int cq_head;
#if !defined(CONFIG_SWARMIO_DISP_ALLOC_WORKER_RR) || \
	defined(CONFIG_SWARMIO_COMMIT)
	int cq_tail;
#else
	atomic_t cq_tail;
#endif

	struct nvme_completion __iomem **cq;
	void *mapped;
};

struct nvme_admin_queue {
	int phase;

	int sq_depth;
	int cq_depth;

	int cq_head;

	struct nvme_command __iomem **nvme_sq;
	struct nvme_completion __iomem **nvme_cq;
};

#define NR_SQE_PER_PAGE (PAGE_SIZE / sizeof(struct nvme_command))
#define NR_CQE_PER_PAGE (PAGE_SIZE / sizeof(struct nvme_completion))

#define SQ_ENTRY_TO_PAGE_NUM(entry_id) (entry_id / NR_SQE_PER_PAGE)
#define CQ_ENTRY_TO_PAGE_NUM(entry_id) (entry_id / NR_CQE_PER_PAGE)

#define SQ_ENTRY_TO_PAGE_OFFSET(entry_id) (entry_id % NR_SQE_PER_PAGE)
#define CQ_ENTRY_TO_PAGE_OFFSET(entry_id) (entry_id % NR_CQE_PER_PAGE)

static inline struct nvme_command *
__sq_entry_at(struct nvme_submission_queue *sq, int entry_id)
{
	return &sq->sq[SQ_ENTRY_TO_PAGE_NUM(entry_id)]
		      [SQ_ENTRY_TO_PAGE_OFFSET(entry_id)];
}

static inline struct nvme_completion *
__cq_entry_at(struct nvme_completion_queue *cq, int entry_id)
{
	return &cq->cq[CQ_ENTRY_TO_PAGE_NUM(entry_id)]
		      [CQ_ENTRY_TO_PAGE_OFFSET(entry_id)];
}

static inline struct nvme_command *
__admin_sq_entry_at(struct nvme_admin_queue *queue, int entry_id)
{
	return &queue->nvme_sq[SQ_ENTRY_TO_PAGE_NUM(entry_id)]
			      [SQ_ENTRY_TO_PAGE_OFFSET(entry_id)];
}

static inline struct nvme_completion *
__admin_cq_entry_at(struct nvme_admin_queue *queue, int entry_id)
{
	return &queue->nvme_cq[CQ_ENTRY_TO_PAGE_NUM(entry_id)]
			      [CQ_ENTRY_TO_PAGE_OFFSET(entry_id)];
}

#endif /* _SWARMIO_QUEUE_H */
