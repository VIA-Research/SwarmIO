// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on code from https://github.com/snu-csl/nvmevirt
 * Copyright (C) 2026 VIA-Research, modified on 2026-04-07
 */

#ifndef _SWARMIO_WORKER_H
#define _SWARMIO_WORKER_H

#include "common.h"

struct swarmio_dispatcher;
struct swarmio_commit_queue;
struct nvme_vdev;

#ifdef CONFIG_SWARMIO_WORKER_BATCH_IO
typedef struct dsa_dma_ctx_batch dma_ctx_t;
#else
typedef struct dsa_dma_ctx_single dma_ctx_t;
#endif

struct swarmio_io_work {
	struct nvme_command cmd;

	union {
		struct nvme_completion cqe;
		struct {
			void *write_buffer;
			size_t buffs_to_release;
		} internal;
	};

#ifdef CONFIG_SWARMIO_PROFILE_REQ
	unsigned long long nsecs_start;
#endif
	unsigned long long nsecs_target;
	int cqid;

	bool is_internal;
	bool is_issued;
	bool is_copied;
	bool is_completed;

#ifdef CONFIG_SWARMIO_PROFILE_REQ
	unsigned long long nsecs_enqueued;
	unsigned long long nsecs_issued;
	unsigned long long nsecs_copied;
#ifndef CONFIG_SWARMIO_COMMIT
	unsigned long long nsecs_start_cpl;
	unsigned long long nsecs_end_cpl;
#endif
#endif
};

struct swarmio_worker_local_queue {
	struct swarmio_io_work *works;

	unsigned short head;
	unsigned short tail;
	unsigned short size;
};

struct swarmio_worker {
	struct swarmio_worker_local_queue local_queue;
#ifdef CONFIG_SWARMIO_COMMIT
	struct swarmio_commit_queue *commit_queue;
#endif
	dma_ctx_t *dma_ctx;
	u64 *paddr_list;

	struct swarmio_dispatcher *dispatcher;

	unsigned int id;
	unsigned int disp_id;

	struct task_struct *task_struct;
	char thread_name[32];

	unsigned int cpu_id;

#ifdef CONFIG_SWARMIO_PROFILE_REQ
	struct swarmio_stats stats_wait_issue;
	struct swarmio_stats stats_copy;
	struct swarmio_stats stats_wait_cpl;
	struct swarmio_stats stats_fill_cpl;
	struct swarmio_stats stats_error;
#endif
};

int swarmio_worker_init(struct nvme_vdev *vdev);
void swarmio_worker_exit(struct nvme_vdev *vdev);

#endif /* _SWARMIO_WORKER_H */
