// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on code from https://github.com/snu-csl/nvmevirt
 * Copyright (C) 2026 VIA-Research, modified on 2026-04-07
 */

#ifdef CONFIG_SWARMIO_COMMIT

#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/highmem.h>
#include <linux/sched/clock.h>

#include "committer.h"
#include "common.h"
#include "dispatcher.h"
#include "nvmev/nvme_vdev.h"
#include "nvmev/pci.h"
#include "queue.h"
#include "worker.h"

extern int num_dma_descs_per_worker;
extern int dma_batch_size;

#define NR_COMMIT_QUEUE_SIZE NR_WORKER_QUEUE_SIZE

void swarmio_complete(struct swarmio_committer *committer,
		      unsigned long *last_completion_time);
void swarmio_proc_irqs(struct swarmio_committer *committer);

static bool __fill_cq_result(struct swarmio_commit_desc *desc)
{
	int i = 0;
	int sqid = desc->cqe.sq_id;
	int cqid = desc->cqid;
	int cqdb = cqid * 2 + 1;
	int new_db = vdev->dbs[cqdb];
	int old_db = vdev->old_dbs[cqdb];
	struct nvme_completion_queue *cq = vdev->cqes[cqid];
	struct nvme_completion *cqe_src, *cqe_dst;
	int cq_tail, next_cq_tail, cq_head;
	u32 dword2, dword3;

	if (new_db != old_db) {
		cq->cq_head = new_db;
		vdev->old_dbs[cqdb] = new_db;
	}

	cq_tail = cq->cq_tail;
	cq_head = cq->cq_head;

	/* check whether cq is full */
	next_cq_tail = cq_tail + 1;
	if (unlikely(next_cq_tail == cq->queue_size))
		next_cq_tail = 0;
	if (unlikely(next_cq_tail == cq_head)) {
		SWARMIO_DEBUG_RL("cq %d is full!\n", cqid);
		return false;
	}

	cqe_dst = __cq_entry_at(cq, cq_tail);
	cqe_src = &desc->cqe;

	/* write cqe */
	cqe_src->sq_head = vdev->sqes[sqid]->sq_head;
	cqe_src->status = (u16)cq->phase & 0x1;
	memcpy((u32 *)cqe_dst + 2, (u32 *)cqe_src + 2, sizeof(u32) * 2);

	/* update cq tail */
	if (++cq_tail == cq->queue_size) {
		cq_tail = 0;
		cq->phase = !cq->phase;
	}
	cq->cq_tail = cq_tail;

	cq->interrupt_ready = true;

#ifdef CONFIG_SWARMIO_PROFILE_REQ
	desc->nsecs_end_cpl = ktime_get_ns();
#endif
	return true;
}

void swarmio_complete(struct swarmio_committer *committer,
		      unsigned long *last_completion_time)
{
	unsigned short j;
	struct swarmio_commit_desc *desc;
	struct swarmio_commit_queue *commit_q = &committer->commit_queue;
	unsigned long long curr_nsecs;
	unsigned short committed_cnt, max_count;
	unsigned short new_head, tail;

	tail = atomic_read(&commit_q->tail);

	/* empty */
	if (unlikely(commit_q->head == tail))
		return;

	curr_nsecs = ktime_get_ns();

	max_count = dma_batch_size * num_dma_descs_per_worker *
		    vdev->config.num_workers_per_service_unit;

	j = commit_q->head;
	new_head = j;
	committed_cnt = 0;
	do {
		desc = commit_q->descs + j;

		if (!smp_load_acquire(&desc->is_valid)) {
			break;
		}

		if (!desc->is_completed) {
			if (desc->nsecs_target > curr_nsecs) {
				curr_nsecs = ktime_get_ns();
			}

			/* complete */
			if (desc->nsecs_target <= curr_nsecs) {
				if (desc->is_internal) {
					desc->is_completed = true;
				} else {
#ifdef CONFIG_SWARMIO_PROFILE_REQ
					desc->nsecs_start_cpl = curr_nsecs;
#endif
					desc->is_completed =
						__fill_cq_result(desc);
				}
			}
			committed_cnt++;
		}

		if (desc->is_completed && j == new_head) {
#ifdef CONFIG_SWARMIO_PROFILE_REQ
			if (!desc->is_internal) {
				__set_stats_add(&committer->stats_wait_cpl,
						desc->nsecs_start_cpl,
						desc->nsecs_copied);
				__set_stats_add(&committer->stats_fill_cpl,
						desc->nsecs_end_cpl,
						desc->nsecs_start_cpl);
			}
#endif
			desc->is_valid = false;
			if (++new_head == commit_q->size)
				new_head = 0;
		}
		if (++j == commit_q->size)
			j = 0;
	} while (j != tail && committed_cnt < max_count);

	/* advance head if it has changed */
	if (new_head != commit_q->head) {
		smp_store_release(&commit_q->head, new_head);
	}

	*last_completion_time = jiffies;
}

void swarmio_proc_irqs(struct swarmio_committer *committer)
{
	int qid, start, end, stride;
	struct nvme_completion_queue *cq;
	struct swarmio_dispatcher *dispatcher = committer->dispatcher;

	if (unlikely(!dispatcher))
		return;

	start = dispatcher->qids.start;
	end = dispatcher->qids.end;
	stride = dispatcher->qids.stride;

	if (unlikely(start == -1))
		return;

	for (qid = start; qid <= end; qid += stride) {
		cq = vdev->cqes[qid];

		if (cq == NULL || !cq->irq_enabled)
			continue;

		if (cq->interrupt_ready) {
			cq->interrupt_ready = false;
			vdev_signal_irq(cq->irq_vector);
		}
	}
}

static int swarmio_committer_thread_fn(void *data)
{
	struct swarmio_committer *committer = (struct swarmio_committer *)data;
	static unsigned long last_completion_time = 0;

	SWARMIO_DEBUG("%s started on cpu %d (node %d)\n",
		      committer->thread_name, smp_processor_id(),
		      cpu_to_node(smp_processor_id()));

	while (!kthread_should_stop()) {
		swarmio_complete(committer, &last_completion_time);
		swarmio_proc_irqs(committer);
		if (CONFIG_SWARMIO_IDLE_TIMEOUT != 0 &&
		    time_after(jiffies,
			       last_completion_time +
				       (CONFIG_SWARMIO_IDLE_TIMEOUT * HZ)))
			schedule_timeout_interruptible(1);
		else
			cond_resched();

		__thread_wait_on_flag(&vdev->stop_flag, &vdev->num_waiting,
				      &vdev->wait_queue);
	}

	return 0;
}

int swarmio_committer_init(struct nvme_vdev *vdev)
{
	unsigned int i, j;
	struct swarmio_committer *committer;
	struct swarmio_commit_queue *commit_q;
	const unsigned int num_committers = vdev->config.num_service_units;
	const unsigned int num_workers_per_service_unit =
		vdev->config.num_workers_per_service_unit;
	int ret;

	vdev->num_committers_initialized = 0;
	vdev->committers = kzalloc(
		sizeof(struct swarmio_committer) * num_committers, GFP_KERNEL);
	if (!vdev->committers)
		return -ENOMEM;

	for (i = 0; i < num_committers; i++) {
		committer = &vdev->committers[i];
		committer->dispatcher = NULL;
		committer->task_struct = NULL;

		/* assign contig. cpus for each (dispatcher, workers, committer) unit */
		j = i * (1 + num_workers_per_service_unit + 1) + 1 +
		    num_workers_per_service_unit;
		committer->cpu_id = cpumask_nth(j, vdev->config.cpu_mask);

		/* allocate and initialize commit queue */
		commit_q = &committer->commit_queue;
		commit_q->size =
			NR_COMMIT_QUEUE_SIZE * num_workers_per_service_unit;
		commit_q->descs = kzalloc(sizeof(struct swarmio_commit_desc) *
						  commit_q->size,
					  GFP_KERNEL);
		if (!commit_q->descs) {
			ret = -ENOMEM;
			goto err;
		}
		commit_q->head = 0;
		atomic_set(&commit_q->tail, 0);
#ifdef CONFIG_SWARMIO_PROFILE_REQ
		__reset_stats(&committer->stats_wait_cpl);
		__reset_stats(&committer->stats_fill_cpl);
#endif
		vdev->num_committers_initialized++;

		snprintf(committer->thread_name, sizeof(committer->thread_name),
			 "swarmio_committer_%d", i);

		committer->task_struct =
			kthread_create(swarmio_committer_thread_fn, committer,
				       "%s", committer->thread_name);
		if (IS_ERR(committer->task_struct)) {
			ret = PTR_ERR(committer->task_struct);
			committer->task_struct = NULL;
			goto err;
		}

		kthread_bind(committer->task_struct, committer->cpu_id);
		wake_up_process(committer->task_struct);
	}

	return 0;

err:
	swarmio_committer_exit(vdev);
	return ret ? ret : -EIO;
}

void swarmio_committer_exit(struct nvme_vdev *vdev)
{
	unsigned int i, j;
	struct swarmio_committer *committer;

	if (!vdev->committers)
		return;

	for (i = 0; i < vdev->num_committers_initialized; i++) {
		committer = &vdev->committers[i];

		if (!IS_ERR_OR_NULL(committer->task_struct)) {
			kthread_stop(committer->task_struct);
		}

		/* free per-worker queues */
		if (vdev->workers) {
			for (j = 0;
			     j < vdev->config.num_workers_per_service_unit;
			     j++) {
				vdev->workers
					[i * vdev->config.num_workers_per_service_unit +
					 j]
						.commit_queue = NULL;
			}
		}
		mb();
		kfree(committer->commit_queue.descs);
		committer->commit_queue.descs = NULL;
	}

	kfree(vdev->committers);
	vdev->committers = NULL;
	vdev->num_committers_initialized = 0;
}

#endif
