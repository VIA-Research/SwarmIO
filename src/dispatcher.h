// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on code from https://github.com/snu-csl/nvmevirt
 * Copyright (C) 2026 VIA-Research, modified on 2026-04-07
 */

#ifndef _SWARMIO_DISPATCHER_H
#define _SWARMIO_DISPATCHER_H

#include "common.h"

struct swarmio_worker;
struct nvme_vdev;

struct swarmio_dispatcher {
	struct swarmio_worker **workers;
	struct dsa_dma_ctx_single *dma_ctx;

	struct {
		int start;
		int end;
		int stride;
	} qids;

#ifdef CONFIG_SWARMIO_DISP_ALLOC_WORKER_RR
	int worker_turn;
#endif

	unsigned int id;
	unsigned int num_workers;

	struct task_struct *task_struct;
	char thread_name[32];

	unsigned int cpu_id;

#ifdef CONFIG_SWARMIO_PROFILE_REQ
	struct swarmio_stats stats_target;
	struct swarmio_stats stats_dispatch;
#endif

	unsigned long long *timing_workspace;
};

#ifndef CONFIG_SWARMIO_DISP_ALLOC_WORKER_RR
static inline unsigned int __get_worker(int sqid,
					struct swarmio_dispatcher *dispatcher)
{
	return (((sqid - dispatcher->qids.start) / dispatcher->qids.stride) %
		dispatcher->num_workers);
}
#else
static inline unsigned int __get_worker(int sqid,
					struct swarmio_dispatcher *dispatcher)
{
	return dispatcher->worker_turn++ % dispatcher->num_workers;
}
#endif

int swarmio_dispatcher_init(struct nvme_vdev *vdev);
void swarmio_dispatcher_exit(struct nvme_vdev *vdev);
bool swarmio_proc_bars(struct swarmio_dispatcher *dispatcher);
#ifdef CONFIG_SWARMIO_DISP_PREFETCH_SQE
void swarmio_prefetch_sq(int qid, struct swarmio_dispatcher *dispatcher);
#endif
int swarmio_dispatch_sq(int qid, int new_db, int old_db,
			struct swarmio_dispatcher *dispatcher);

#endif /* _SWARMIO_DISPATCHER_H */
