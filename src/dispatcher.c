// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on code from https://github.com/snu-csl/nvmevirt
 * Copyright (C) 2026 VIA-Research, modified on 2026-04-07
 */

#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/sched/clock.h>

#include "common.h"
#include "committer.h"
#include "dispatcher.h"
#include "nvmev/admin.h"
#include "nvmev/nvme_vdev.h"
#include "nvmev/pci.h"
#include "queue.h"
#include "worker.h"
#include "dsa/dsa.h"

#define sq_dma(entry_id) (sq->dma_addr + sizeof(struct nvme_command) * entry_id)

#ifdef CONFIG_SWARMIO_DMA_DISPATCHER
extern bool disp_using_dma;
#else
static bool disp_using_dma = false;
#endif
extern int num_dma_descs_per_disp;
extern int dma_timeout;
extern int num_dsa_devs;

static inline void __swarmio_disable_disp_dma(void)
{
	SWARMIO_ERROR("disabling dispatcher DMA after DSA failure\n");
	disp_using_dma = false;
}

/* BAR updates are handled only by dispatcher 0. */
bool swarmio_proc_bars(struct swarmio_dispatcher *dispatcher)
{
	if (dispatcher->id != 0) {
		return false;
	}

	volatile struct __nvme_bar *old_bar = vdev->old_bar;
	volatile struct nvme_ctrl_regs *bar = vdev->bar;
	struct nvme_admin_queue *queue = vdev->admin_q;
	unsigned int num_pages, i;

	if (old_bar->aqa != bar->u_aqa) {
		old_bar->aqa = bar->u_aqa;

		vdev->dbs[0] = vdev->old_dbs[0] = 0;
		vdev->dbs[1] = vdev->old_dbs[1] = 0;

		goto out;
	}
	if (old_bar->asq != bar->u_asq) {
		old_bar->asq = bar->u_asq;

		vdev->dbs[0] = vdev->old_dbs[0] = 0;

		goto out;
	}
	if (old_bar->acq != bar->u_acq) {
		old_bar->acq = bar->u_acq;

		vdev->dbs[1] = vdev->old_dbs[1] = 0;

		goto out;
	}
	if (old_bar->cc != bar->u_cc) {
		if (bar->cc.en == 1) {
			if (vdev->admin_q) {
				;
			} else {
				queue = kzalloc(sizeof(*queue), GFP_KERNEL);
				if (!queue) {
					SWARMIO_ERROR(
						"Failed to allocate admin queue\n");
					bar->csts.rdy = 0;
					goto out;
				}

				queue->cq_head = 0;
				queue->phase = 1;
				queue->sq_depth = bar->aqa.asqs + 1;
				queue->cq_depth = bar->aqa.acqs + 1;

				num_pages = DIV_ROUND_UP(
					queue->sq_depth *
						sizeof(struct nvme_command),
					PAGE_SIZE);
				queue->nvme_sq =
					kcalloc(num_pages,
						sizeof(struct nvme_command *),
						GFP_KERNEL);
				if (!queue->nvme_sq) {
					SWARMIO_ERROR(
						"Failed to allocate admin SQ page table\n");
					goto err_enable_admin_queue;
				}

				for (i = 0; i < num_pages; i++) {
					queue->nvme_sq[i] = page_address(
						pfn_to_page(vdev->bar->u_asq >>
							    PAGE_SHIFT) +
						i);
				}

				num_pages = DIV_ROUND_UP(
					queue->cq_depth *
						sizeof(struct nvme_completion),
					PAGE_SIZE);
				queue->nvme_cq = kcalloc(
					num_pages,
					sizeof(struct nvme_completion *),
					GFP_KERNEL);
				if (!queue->nvme_cq) {
					SWARMIO_ERROR(
						"Failed to allocate admin CQ page table\n");
					goto err_enable_admin_queue;
				}

				for (i = 0; i < num_pages; i++) {
					queue->nvme_cq[i] = page_address(
						pfn_to_page(vdev->bar->u_acq >>
							    PAGE_SHIFT) +
						i);
				}
				WRITE_ONCE(vdev->admin_q, queue);
				SWARMIO_INFO("controller enabled\n");
			}
			bar->csts.rdy = 1;
		} else if (bar->cc.en == 0) {
			if (queue) {
				unsigned int num_service_units =
					vdev->config.num_service_units;
				unsigned int num_workers =
					vdev->config.num_workers;
#ifdef CONFIG_SWARMIO_COMMIT
				unsigned int num_waiting =
					num_service_units * 2 + num_workers - 1;
#else
				unsigned int num_waiting =
					num_service_units + num_workers - 1;
#endif
				struct swarmio_dispatcher *disp;
				unsigned int qid;

				WRITE_ONCE(vdev->stop_flag, 1);
				while (atomic_read(&vdev->num_waiting) !=
				       num_waiting) {
					cond_resched();
				}

				for (qid = 1; qid <= vdev->num_sq; qid++) {
					struct nvme_submission_queue *sq =
						vdev->sqes[qid];
					vdev->sqes[qid] = NULL;
					vdev_delete_sq(sq);
				}

				for (qid = 1; qid <= vdev->num_cq; qid++) {
					struct nvme_completion_queue *cq =
						vdev->cqes[qid];
					vdev->cqes[qid] = NULL;
					vdev_delete_cq(cq);
				}

				for (i = 0; i < num_service_units; i++) {
					disp = &vdev->dispatchers[i];
					disp->qids.start = disp->qids.end = -1;
				}

				mb();

				WRITE_ONCE(vdev->stop_flag, 0);
				wake_up_all(&vdev->wait_queue);

				if (queue->nvme_sq) {
					kfree(queue->nvme_sq);
					queue->nvme_sq = NULL;
				}
				if (queue->nvme_cq) {
					kfree(queue->nvme_cq);
					queue->nvme_cq = NULL;
				}
				kfree(queue);
				vdev->admin_q = NULL;
				SWARMIO_INFO("controller disabled\n");
			}
			bar->csts.rdy = 0;
		}

		if (bar->cc.shn == 1) {
			bar->csts.shst = 2;

			vdev->dbs[0] = vdev->old_dbs[0] = 0;
			vdev->dbs[1] = vdev->old_dbs[1] = 0;

			if (vdev->admin_q)
				vdev->admin_q->cq_head = 0;
		}

		old_bar->cc = bar->u_cc;

		goto out;
	}

	return false;

out:
	smp_mb();
	return true;

err_enable_admin_queue:
	kfree(queue->nvme_cq);
	kfree(queue->nvme_sq);
	kfree(queue);
	bar->csts.rdy = 0;
	goto out;
}

static bool swarmio_proc_admin_queues(struct swarmio_dispatcher *dispatcher)
{
	if (dispatcher->id != 0) {
		return false;
	}
	bool updated = false;
	int new_db;

	// Admin SQ
	new_db = vdev->dbs[0];
	if (new_db != vdev->old_dbs[0]) {
		struct nvme_admin_queue *queue = vdev->admin_q;
		int num_proc = new_db - vdev->old_dbs[0];
		int curr = vdev->old_dbs[0];
		int seq;

		if (num_proc < 0)
			num_proc += queue->sq_depth;

		for (seq = 0; seq < num_proc; seq++) {
			vdev_proc_admin_sq_entry(curr++);

			if (curr == queue->sq_depth) {
				curr = 0;
			}
		}

		vdev_signal_irq(0);
		vdev->old_dbs[0] = new_db;
		updated = true;
	}

	// Admin CQ
	new_db = vdev->dbs[1];
	if (new_db != vdev->old_dbs[1]) {
		vdev->old_dbs[1] = new_db;
		updated = true;
	}

	return updated;
}

static bool
swarmio_proc_submission_queues(struct swarmio_dispatcher *dispatcher)
{
	bool updated = false;
	int qid, dbs_idx, new_db, old_db;
	int start = dispatcher->qids.start;
	int end = dispatcher->qids.end;
	int stride = dispatcher->qids.stride;
	if (start == -1)
		return false;

	for (qid = start; qid <= end; qid += stride) {
#ifdef CONFIG_SWARMIO_DISP_PREFETCH_SQE
		int next_qid = qid + stride;
		if (next_qid > end)
			next_qid = start;

		if (vdev->sqes[next_qid]) {
			swarmio_prefetch_sq(next_qid, dispatcher);
		}
#endif

		if (vdev->sqes[qid] == NULL)
			continue;
		dbs_idx = qid * 2;
		new_db = vdev->dbs[dbs_idx];
		old_db = vdev->old_dbs[dbs_idx];
		if (new_db != old_db) {
			vdev->old_dbs[dbs_idx] = swarmio_dispatch_sq(
				qid, new_db, old_db, dispatcher);
			updated = true;
		}
	}

	return updated;
}

// Returns true if an event is processed
static bool vdev_proc_dbs(struct swarmio_dispatcher *dispatcher)
{
	bool updated = false;

	updated |= swarmio_proc_admin_queues(dispatcher);
	updated |= swarmio_proc_submission_queues(dispatcher);

	return updated;
}

static int swarmio_dispatcher_thread_fn(void *data)
{
	struct swarmio_dispatcher *dispatcher =
		(struct swarmio_dispatcher *)data;
	static unsigned long last_dispatched_time = 0;
	static unsigned int i;

	SWARMIO_DEBUG("%s started on cpu %d (node %d)\n",
		      dispatcher->thread_name, dispatcher->cpu_id,
		      cpu_to_node(dispatcher->cpu_id));

	for (i = 0; i < dispatcher->num_workers;) {
		if (dispatcher->workers[i] != NULL) {
			i++;
		} else {
			schedule_timeout_interruptible(1);
		}
	}

	while (!kthread_should_stop()) {
		if (swarmio_proc_bars(dispatcher))
			last_dispatched_time = jiffies;
		if (vdev_proc_dbs(dispatcher))
			last_dispatched_time = jiffies;

		if (CONFIG_SWARMIO_IDLE_TIMEOUT != 0 &&
		    time_after(jiffies,
			       last_dispatched_time +
				       (CONFIG_SWARMIO_IDLE_TIMEOUT * HZ)))
			schedule_timeout_interruptible(1);
		else
			cond_resched();

		if (dispatcher->id != 0) {
			__thread_wait_on_flag(&vdev->stop_flag,
					      &vdev->num_waiting,
					      &vdev->wait_queue);
		}
	}

	return 0;
}

int swarmio_dispatcher_init(struct nvme_vdev *vdev)
{
	unsigned int i, j;
	unsigned int num_workers_per_service_unit =
		vdev->config.num_workers_per_service_unit;
	unsigned int num_service_units = vdev->config.num_service_units;
	struct swarmio_dispatcher *dispatcher;
	struct swarmio_worker *worker;
	int dev_id;
	int wq_id;
	int ret;

	vdev->num_service_units_initialized = 0;
	vdev->dispatchers =
		kzalloc(sizeof(struct swarmio_dispatcher) * num_service_units,
			GFP_KERNEL);
	if (!vdev->dispatchers)
		return -ENOMEM;

	for (i = 0; i < num_service_units; i++) {
		dispatcher = &vdev->dispatchers[i];

		dispatcher->id = i;
		dispatcher->num_workers = num_workers_per_service_unit;
		dispatcher->qids.start = -1;
		dispatcher->qids.end = -1;
		dispatcher->qids.stride = 0;
#ifdef CONFIG_SWARMIO_PROFILE_REQ
		__reset_stats(&dispatcher->stats_target);
		__reset_stats(&dispatcher->stats_dispatch);
#endif

		/* assign contig. cpus for each (dispatcher, workers) unit */
#ifdef CONFIG_SWARMIO_COMMIT
		j = i * (1 + num_workers_per_service_unit + 1);
#else
		j = i * (1 + num_workers_per_service_unit);
#endif
		dispatcher->cpu_id = cpumask_nth(j, vdev->config.cpu_mask);

		dispatcher->timing_workspace =
			kzalloc(vdev->config.num_sched_insts * 2 *
					sizeof(unsigned long long),
				GFP_KERNEL);
		if (!dispatcher->timing_workspace) {
			ret = -ENOMEM;
			goto err;
		}
		dispatcher->workers =
			kzalloc(sizeof(struct swarmio_worker *) *
					num_workers_per_service_unit,
				GFP_KERNEL);
		if (!dispatcher->workers) {
			ret = -ENOMEM;
			goto err;
		}
		for (j = 0; j < num_workers_per_service_unit; j++) {
			dispatcher->workers[j] =
				&vdev->workers[i * num_workers_per_service_unit +
					       j];
		}

		snprintf(dispatcher->thread_name,
			 sizeof(dispatcher->thread_name),
			 "swarmio_dispatcher_%d", i);

		/* allocate and initialize dma context */
		if (disp_using_dma) {
			dsa_assign_wq_by_group_disagg(i, 0, &dev_id, &wq_id);
			dispatcher->dma_ctx = kzalloc(
				sizeof(struct dsa_dma_ctx_single), GFP_KERNEL);
			if (!dispatcher->dma_ctx) {
				ret = -ENOMEM;
				goto err;
			}
			ret = dsa_dma_ctx_single_init(dispatcher->dma_ctx,
						      dev_id, wq_id,
						      num_dma_descs_per_disp,
						      dma_timeout);
			if (ret) {
				SWARMIO_ERROR("%s: dma ctx init failed!\n",
					      dispatcher->thread_name);
				goto err;
			}
			SWARMIO_DEBUG("%s assigned %s\n",
				      dispatcher->thread_name,
				      dispatcher->dma_ctx->wq->name);
		}

		mb();

		// allow others to reference dispatcher
#ifdef CONFIG_SWARMIO_COMMIT
		vdev->committers[i].dispatcher = dispatcher;
#endif
		for (j = 0; j < num_workers_per_service_unit; j++) {
			dispatcher->workers[j]->dispatcher = dispatcher;
		}

		dispatcher->task_struct =
			kthread_create(swarmio_dispatcher_thread_fn, dispatcher,
				       "%s", dispatcher->thread_name);
		if (IS_ERR(dispatcher->task_struct)) {
			ret = PTR_ERR(dispatcher->task_struct);
			dispatcher->task_struct = NULL;
			goto err;
		}
		kthread_bind(dispatcher->task_struct, dispatcher->cpu_id);
		wake_up_process(dispatcher->task_struct);
		vdev->num_service_units_initialized++;
	}

	return 0;

err:
	swarmio_dispatcher_exit(vdev);
	return ret ? ret : -EIO;
}

void swarmio_dispatcher_exit(struct nvme_vdev *vdev)
{
	unsigned int i, j;
	unsigned int num_workers_per_service_unit =
		vdev->config.num_workers_per_service_unit;
	struct swarmio_dispatcher *dispatcher;

	if (!vdev->dispatchers)
		return;

	for (i = 0; i < vdev->num_service_units_initialized; i++) {
		dispatcher = &vdev->dispatchers[i];

		if (!IS_ERR_OR_NULL(dispatcher->task_struct)) {
			kthread_stop(dispatcher->task_struct);
		}
		if (dispatcher->dma_ctx) {
			dsa_dma_ctx_single_remove(dispatcher->dma_ctx);
		}
		kfree(dispatcher->dma_ctx);
		kfree(dispatcher->timing_workspace);
		for (j = 0; j < num_workers_per_service_unit; j++) {
			if (dispatcher->workers && dispatcher->workers[j])
				dispatcher->workers[j]->dispatcher = NULL;
		}
		kfree(dispatcher->workers);
	}

	kfree(vdev->dispatchers);
	vdev->dispatchers = NULL;
	vdev->num_service_units_initialized = 0;
}

static bool __dispatch_sq_entry(int sqid, int sq_entry, size_t *io_size,
				struct swarmio_dispatcher *dispatcher)
{
	int i;
	struct nvme_submission_queue *sq = vdev->sqes[sqid];
#ifndef CONFIG_SWARMIO_AGGREGATED_TIMING_UPDATE
	unsigned long long nsecs_start = ktime_get_ns();
#endif
	struct swarmio_worker *worker;
	struct swarmio_worker_local_queue *local_queue;
	struct swarmio_io_work *w;
	struct nvme_command *cmd, *src;
	unsigned short new_local_queue_tail;
	unsigned int nsid;
	struct swarmio_nvme_ns *ns;
	struct nvme_sched_info sched_info;

	worker = dispatcher->workers[__get_worker(sqid, dispatcher)];
	if (unlikely(!worker))
		return false;

	local_queue = &worker->local_queue;

	new_local_queue_tail = local_queue->tail + 1;
	if (new_local_queue_tail == local_queue->size)
		new_local_queue_tail = 0;

	/* work queue is full */
	if (new_local_queue_tail == smp_load_acquire(&local_queue->head)) {
		SWARMIO_DEBUG_RL("%s work q is full!\n", worker->thread_name);
		return false;
	}

	/* copy cmd */
	w = local_queue->works + local_queue->tail;
#ifdef CONFIG_SWARMIO_DISP_COALESCE_SQE
	src = sq->cmd_buffer + sq_entry;
#else
	if (disp_using_dma) {
		src = sq->cmd_buffer + sq_entry;

		dma_addr_t cmd_buffer_dma =
			sq->cmd_buffer_dma +
			sq_entry * sizeof(struct nvme_command);
		int ret;
		ret = dsa_dma_issue_sync_single(
			dispatcher->dma_ctx, cmd_buffer_dma, sq_dma(sq_entry),
			sizeof(struct nvme_command), NULL, NULL);
		if (unlikely(ret)) {
			__swarmio_disable_disp_dma();
			src = __sq_entry_at(sq, sq_entry);
		}
	} else {
		src = __sq_entry_at(sq, sq_entry);
	}
#endif
	memcpy(&w->cmd, src, sizeof(struct nvme_command));
	cmd = &w->cmd;

#ifndef CONFIG_SWARMIO_AGGREGATED_TIMING_UPDATE
	nsid = cmd->common.nsid - 1;
	ns = &vdev->ns[nsid];
	sched_info = (struct nvme_sched_info){
		.cmd = cmd,
		.status = NVME_SC_SUCCESS,
		.nsecs_target = nsecs_start,
		.nsecs_start = nsecs_start,
	};

	/* run perf model */
	if (!ns->sched_nvme_cmd(ns, &sched_info))
		return false;
#endif

	*io_size = __cmd_io_size(&cmd->rw);

	/////////////////////////////////
	/* populate work desc */
	w->cqe.sq_id = sqid;
	w->cqe.command_id = cmd->rw.command_id;
	w->cqid = sq->cqid;
#ifdef CONFIG_SWARMIO_PROFILE_REQ
#ifdef CONFIG_SWARMIO_AGGREGATED_TIMING_UPDATE
	w->nsecs_start = sq->nsecs_starts[sq_entry];
#else
	w->nsecs_start = nsecs_start;
#endif
#endif
#ifdef CONFIG_SWARMIO_AGGREGATED_TIMING_UPDATE
	w->nsecs_target = sq->nsecs_targets[sq_entry];
#else
	w->nsecs_target = sched_info.nsecs_target;
#endif
	w->is_internal = false;
	w->is_issued = false;
	w->is_copied = false;
	w->is_completed = false;
	/////////////////////////////////

	/* advance sq head */
	if (++sq_entry == sq->queue_size) {
		sq_entry = 0;
	}
	sq->sq_head = sq_entry;

#ifdef CONFIG_SWARMIO_PROFILE_REQ
	unsigned long long curr_nsecs = ktime_get_ns();
	w->nsecs_enqueued = curr_nsecs;
	__set_stats_add(&dispatcher->stats_target, w->nsecs_target,
			w->nsecs_start);
	__set_stats_add(&dispatcher->stats_dispatch, curr_nsecs,
			w->nsecs_start);
#endif

	/* enqueue into work queue */
	smp_store_release(&local_queue->tail, new_local_queue_tail);

	return true;
}

#ifdef CONFIG_SWARMIO_DISP_PREFETCH_SQE
void swarmio_prefetch_sq(int qid, struct swarmio_dispatcher *dispatcher)
{
	int dbs_idx = qid * 2;
	int new_db = vdev->dbs[dbs_idx];
	struct nvme_submission_queue *sq = vdev->sqes[qid];
	int pref_size;
	dma_addr_t cmd_buffer_dma;

	if (unlikely(!sq))
		return;
	if (sq->is_pref_issued)
		return;

	pref_size = new_db - sq->fetched_pos;
	if (!disp_using_dma || !pref_size)
		return;

	if (unlikely(pref_size < 0)) {
		pref_size = sq->queue_size - sq->fetched_pos;
	}

	pref_size = min(pref_size, NR_MAX_FETCH_SQE);
	SWARMIO_DEBUG_RL("sq%d prefetch size: %d\n", qid, pref_size);

	cmd_buffer_dma = sq->cmd_buffer_dma +
			 sq->fetched_pos * sizeof(struct nvme_command);
	if (unlikely(dsa_dma_issue_async_single(
		    dispatcher->dma_ctx, cmd_buffer_dma,
		    sq_dma(sq->fetched_pos),
		    sizeof(struct nvme_command) * pref_size, &sq->dma_desc_id,
		    NULL, NULL))) {
		__swarmio_disable_disp_dma();
		return;
	}
	sq->is_pref_issued = true;
	sq->pref_size = pref_size;

	return;
}
#endif

int swarmio_dispatch_sq(int sqid, int new_db, int old_db,
			struct swarmio_dispatcher *dispatcher)
{
	struct nvme_submission_queue *sq = vdev->sqes[sqid];
#if defined(CONFIG_SWARMIO_DISP_COALESCE_SQE)
	int num_fetch = new_db - sq->fetched_pos;
	int ret;
#endif
	int num_proc;
	int seq;
	int sq_entry = old_db;
	int latest_db;
	dma_addr_t cmd_buffer_dma;

	if (unlikely(!sq))
		return old_db;

#ifdef CONFIG_SWARMIO_DISP_COALESCE_SQE
	if (num_fetch < 0)
		num_fetch = sq->queue_size - sq->fetched_pos;

	if (num_fetch == 0 && old_db == sq->fetched_pos)
		return old_db;

	num_fetch = min(num_fetch, NR_MAX_FETCH_SQE);

	if (num_fetch > 0) {
		cmd_buffer_dma = sq->cmd_buffer_dma +
				 sq->fetched_pos * sizeof(struct nvme_command);
		if (disp_using_dma) {
#if defined(CONFIG_SWARMIO_DISP_PREFETCH_SQE)
			if (sq->is_pref_issued) {
				ret = dsa_dma_wait_one_single(
					dispatcher->dma_ctx, sq->dma_desc_id,
					NULL);
				num_fetch = sq->pref_size;
				sq->is_pref_issued = false;
				sq->pref_size = 0;
			} else {
#endif
				ret = dsa_dma_issue_sync_single(
					dispatcher->dma_ctx, cmd_buffer_dma,
					sq_dma(sq->fetched_pos),
					sizeof(struct nvme_command) * num_fetch,
					NULL, NULL);
#if defined(CONFIG_SWARMIO_DISP_PREFETCH_SQE)
			}
#endif
			if (ret) {
				__swarmio_disable_disp_dma();
				memcpy(sq->cmd_buffer + sq->fetched_pos,
				       __sq_entry_at(sq, sq->fetched_pos),
				       sizeof(struct nvme_command) * num_fetch);
			}
		} else {
			memcpy(sq->cmd_buffer + sq->fetched_pos,
			       __sq_entry_at(sq, sq->fetched_pos),
			       sizeof(struct nvme_command) * num_fetch);
		}

#ifdef CONFIG_SWARMIO_AGGREGATED_TIMING_UPDATE
		{
			struct nvme_command *batch_cmds =
				sq->cmd_buffer + sq->fetched_pos;
			unsigned long long now = ktime_get_ns();
			struct swarmio_nvme_ns *ns =
				&vdev->ns[batch_cmds[0].common.nsid - 1];

#ifdef CONFIG_SWARMIO_PROFILE_REQ
			ns->sched_nvme_cmd_aggregated(
				ns, batch_cmds, now,
				&sq->nsecs_starts[sq->fetched_pos],
				&sq->nsecs_targets[sq->fetched_pos], num_fetch,
				dispatcher);
#else
			ns->sched_nvme_cmd_aggregated(
				ns, batch_cmds, now,
				&sq->nsecs_targets[sq->fetched_pos], num_fetch,
				dispatcher);
#endif
		}
#endif
		sq->fetched_pos =
			(sq->fetched_pos + num_fetch) % sq->queue_size;
	}

	num_proc = sq->fetched_pos - old_db;
	if (num_proc < 0)
		num_proc = sq->queue_size - old_db;

#else
	num_proc = new_db - old_db;

	if (num_proc < 0)
		num_proc = sq->queue_size - old_db;

	num_proc = min(num_proc, NR_MAX_FETCH_SQE);
#endif
	for (seq = 0; seq < num_proc; seq++) {
		size_t io_size;
		if (!__dispatch_sq_entry(sqid, sq_entry, &io_size, dispatcher))
			break;

		if (++sq_entry == sq->queue_size) {
			sq_entry = 0;
		}
	}

	latest_db = (old_db + seq) % sq->queue_size;

	return latest_db;
}
