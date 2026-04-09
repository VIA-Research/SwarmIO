// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on code from https://github.com/snu-csl/nvmevirt
 * Copyright (C) 2026 VIA-Research, modified on 2026-04-07
 */

#ifndef _SWARMIO_COMMITTER_H
#define _SWARMIO_COMMITTER_H

#include "common.h"

#ifdef CONFIG_SWARMIO_COMMIT
struct swarmio_commit_desc {
	union {
		struct nvme_completion cqe;
		struct {
			void *write_buffer;
			size_t buffs_to_release;
		} internal;
	};

#ifdef CONFIG_SWARMIO_PROFILE_REQ
	unsigned long long nsecs_target;
	unsigned long long nsecs_copied;
	unsigned long long nsecs_start_cpl;
	unsigned long long nsecs_end_cpl;
#else
	unsigned long long nsecs_target;
#endif
	int cqid;

	bool is_valid;
	bool is_completed;
	bool is_internal;
} __attribute__((aligned(32)));

struct swarmio_commit_queue {
	struct swarmio_commit_desc *descs;

	atomic_t tail;

	unsigned short head;
	unsigned short size;
};

struct swarmio_committer {
	struct swarmio_commit_queue commit_queue;

	struct swarmio_dispatcher *dispatcher;

	struct task_struct *task_struct;
	char thread_name[32];

	unsigned int cpu_id;

#ifdef CONFIG_SWARMIO_PROFILE_REQ
	struct swarmio_stats stats_wait_cpl;
	struct swarmio_stats stats_fill_cpl;
#endif
};

int swarmio_committer_init(struct nvme_vdev *vdev);
void swarmio_committer_exit(struct nvme_vdev *vdev);
#endif

#endif /* _SWARMIO_COMMITTER_H */
