// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on code from https://github.com/snu-csl/nvmevirt
 * Copyright (C) 2026 VIA-Research, modified on 2026-04-07
 */

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
#include "dsa/dsa.h"

#if defined(CONFIG_SWARMIO_WORKER_SKIP_IO) || \
	!defined(CONFIG_SWARMIO_DMA_WORKER)
static bool worker_using_dma = false;
#else
extern bool worker_using_dma;
#endif
extern int num_dma_descs_per_worker;
extern int dma_timeout;
extern int num_dsa_devs;
extern int dma_batch_size;

#define PTRS_PER_PRP_LIST (1UL << (PAGE_SHIFT - 3))

#ifndef CONFIG_SWARMIO_COMMIT
void swarmio_proc_irqs(struct swarmio_worker *worker);
#endif
void swarmio_proc_io(struct swarmio_worker *worker,
		     unsigned long *last_io_time);

static inline int __dsa_dma_issue(dma_ctx_t *ctx, dma_addr_t dma_dst,
				  dma_addr_t dma_src, size_t size, void *data,
				  void (*callback)(void *))
{
#ifdef CONFIG_SWARMIO_WORKER_BATCH_IO
	return dsa_dma_issue_async_batch(ctx, dma_dst, dma_src, size, data,
					 callback);
#else
	return dsa_dma_issue_sync_single(ctx, dma_dst, dma_src, size, data,
					 callback);
#endif
}

static inline void __swarmio_disable_worker_dma(void)
{
	SWARMIO_ERROR("disabling worker DMA after DSA failure\n");
	worker_using_dma = false;
}

#ifndef CONFIG_SWARMIO_COMMIT
static bool __fill_cq_result(struct swarmio_io_work *w)
{
	int i = 0;
	int sqid = w->cqe.sq_id;
	int cqid = w->cqid;
	int cqdb = cqid * 2 + 1;
#ifndef CONFIG_SWARMIO_DISP_ALLOC_WORKER_RR
	int new_db = vdev->dbs[cqdb];
	int old_db = vdev->old_dbs[cqdb];
#else
	int pos;
#endif
	struct nvme_completion_queue *cq = vdev->cqes[cqid];
	struct nvme_completion *cqe_src, *cqe_dst;
	int cq_tail, next_cq_tail, cq_head, phase;
	u32 dword2, dword3;

#ifndef CONFIG_SWARMIO_DISP_ALLOC_WORKER_RR
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

	phase = cq->phase;

	cqe_dst = __cq_entry_at(cq, cq_tail);
#else
	do {
		cq_tail = atomic_read(&cq->cq_tail);
		pos = cq_tail % cq->queue_size;

		next_cq_tail = cq_tail + 1;

		if (unlikely((next_cq_tail) % cq->queue_size ==
			     READ_ONCE(vdev->dbs[cqdb]))) {
			SWARMIO_DEBUG_RL("cq %d is full (tail: %d)!\n", cqid,
					 cq_tail % cq->queue_size);
			return false;
		}
	} while (atomic_cmpxchg(&cq->cq_tail, cq_tail, next_cq_tail) !=
		 cq_tail);
	phase = !((cq_tail / cq->queue_size) & 1);

	cqe_dst = __cq_entry_at(cq, pos);
#endif
	cqe_src = &w->cqe;

	/* write cqe */
	cqe_src->sq_head = vdev->sqes[sqid]->sq_head;
	cqe_src->status = (u16)phase & 0x1;
	memcpy((u32 *)cqe_dst + 2, (u32 *)cqe_src + 2, sizeof(u32) * 2);

#ifndef CONFIG_SWARMIO_DISP_ALLOC_WORKER_RR
	/* update cq tail */
	if (++cq_tail == cq->queue_size) {
		cq_tail = 0;
		cq->phase = !phase;
	}
	cq->cq_tail = cq_tail;

	cq->interrupt_ready = true;
#else
	smp_store_release(&cq->interrupt_ready, true);
#endif

#ifdef CONFIG_SWARMIO_PROFILE_REQ
	const unsigned long long curr_nsecs = ktime_get_ns();
	w->nsecs_end_cpl = curr_nsecs;
#endif
	return true;
}

static inline void __swarmio_complete(struct swarmio_io_work *w)
{
	unsigned long long curr_nsecs = ktime_get_ns();

#ifndef CONFIG_SWARMIO_WORKER_SKIP_IO
	if (w->nsecs_target <= curr_nsecs) {
#else
	{
#endif
		if (w->is_internal) {
			w->is_completed = true;
		} else {
#ifdef CONFIG_SWARMIO_PROFILE_REQ
			w->nsecs_start_cpl = curr_nsecs;
#endif
			w->is_completed = __fill_cq_result(w);
		}
	}
}
#endif

static inline void __swarmio_mark_copied(struct swarmio_io_work *w)
{
	w->is_copied = true;
#ifdef CONFIG_SWARMIO_PROFILE_REQ
	const unsigned long long curr_nsecs = ktime_get_ns();
#endif
#ifdef CONFIG_SWARMIO_PROFILE_REQ
	w->nsecs_copied = curr_nsecs;
#endif
}

static inline void __swarmio_dma_cb(void *data)
{
	struct swarmio_io_work *w;

	if (!data)
		return;
	w = (struct swarmio_io_work *)data;

	__swarmio_mark_copied(w);
#ifndef CONFIG_SWARMIO_COMMIT
	__swarmio_complete(w);
#endif
}

static void __swarmio_xfer_data(struct nvme_rw_command *cmd)
{
	size_t offset, length, remaining;
	int prp_offs = 0;
	int prp2_offs = 0;
	u64 paddr;
	u64 *paddr_list = NULL;
	size_t nsid = cmd->nsid - 1; // 0-based
	bool is_paddr_memremap = false;

	offset = __cmd_io_offset(cmd);
	length = __cmd_io_size(cmd);
	remaining = length;

	while (remaining) {
		size_t io_size;
		void *vaddr;
		size_t mem_offs = 0;
		bool is_vaddr_memremap = false;

		if (prp_offs == 0) {
			paddr = cmd->dptr.prp1;
		} else if (prp_offs == 1) {
			paddr = cmd->dptr.prp2;
			if (remaining > PAGE_SIZE) {
				if (pfn_valid(paddr >> PAGE_SHIFT)) {
					paddr_list =
						kmap_atomic_pfn(PRP_PFN(paddr));
				} else {
					paddr_list = memremap(paddr, PAGE_SIZE,
							      MEMREMAP_WT);
					is_paddr_memremap = true;
				}
				paddr_list =
					(u64 *)((uintptr_t)paddr_list +
						(paddr & PAGE_OFFSET_MASK));
				paddr = paddr_list[prp2_offs++];
			}
		} else {
			BUG_ON(prp2_offs == (PTRS_PER_PRP_LIST - 1) &&
			       remaining > PAGE_SIZE);
			paddr = paddr_list[prp2_offs++];
		}

		if (pfn_valid(paddr >> PAGE_SHIFT)) {
			vaddr = kmap_atomic_pfn(PRP_PFN(paddr));
		} else {
			vaddr = memremap(paddr, PAGE_SIZE, MEMREMAP_WT);
			is_vaddr_memremap = true;
		}

		io_size = min_t(size_t, remaining, PAGE_SIZE);

		if (paddr & PAGE_OFFSET_MASK) {
			mem_offs = paddr & PAGE_OFFSET_MASK;
			if (io_size + mem_offs > PAGE_SIZE)
				io_size = PAGE_SIZE - mem_offs;
		}

		if (cmd->opcode == nvme_cmd_write ||
		    cmd->opcode == nvme_cmd_zone_append) {
			memcpy(vdev->ns[nsid].mapped + offset, vaddr + mem_offs,
			       io_size);
		} else if (cmd->opcode == nvme_cmd_read) {
			memcpy_toio(vaddr + mem_offs,
				    vdev->ns[nsid].mapped + offset, io_size);
		}

		if (vaddr != NULL && !is_vaddr_memremap) {
			kunmap_atomic(vaddr);
			vaddr = NULL;
		} else if (vaddr != NULL && is_vaddr_memremap) {
			memunmap(vaddr);
			vaddr = NULL;
			is_vaddr_memremap = false;
		}

		remaining -= io_size;
		offset += io_size;
		prp_offs++;
	}

	if (paddr_list) {
		if (!is_paddr_memremap)
			kunmap_atomic(paddr_list);
		else if (is_paddr_memremap)
			memunmap(paddr_list);
	}
}

static inline int __swarmio_submit_dma(dma_ctx_t *dma_ctx, u8 opcode, u64 paddr,
				       size_t storage_offset, size_t io_size,
				       void *callback_data)
{
	if (opcode == nvme_cmd_write || opcode == nvme_cmd_zone_append) {
		return __dsa_dma_issue(
			dma_ctx, vdev->config.storage_start + storage_offset,
			paddr, io_size, callback_data, __swarmio_dma_cb);
	} else if (opcode == nvme_cmd_read) {
		return __dsa_dma_issue(
			dma_ctx, paddr,
			vdev->config.storage_start + storage_offset, io_size,
			callback_data, __swarmio_dma_cb);
	}

	return 0;
}

static int __swarmio_xfer_data_using_dma(struct nvme_rw_command *cmd,
					 dma_ctx_t *dma_ctx,
					 struct swarmio_io_work *w,
					 u64 *paddr_list)
{
	size_t offset, length, remaining;
	int prp_offs = 0;
	int prp2_offs = 0;
	int num_prps = 0;
	u64 paddr;
	u64 *tmp_paddr_list = NULL;
	size_t io_size;
	size_t mem_offs = 0;
	bool is_memremap;

	offset = __cmd_io_offset(cmd);
	length = __cmd_io_size(cmd);
	remaining = length;

	/* get the PRP list */
	while (remaining) {
		if (prp_offs == 0) {
			paddr = cmd->dptr.prp1;
		} else if (prp_offs == 1) {
			paddr = cmd->dptr.prp2;
			if (remaining > PAGE_SIZE) {
				if (pfn_valid(paddr >> PAGE_SHIFT)) {
					tmp_paddr_list =
						kmap_atomic_pfn(PRP_PFN(paddr));
					is_memremap = false;
				} else {
					tmp_paddr_list = memremap(
						paddr, PAGE_SIZE, MEMREMAP_WT);
					is_memremap = true;
				}
				tmp_paddr_list =
					(u64 *)((uintptr_t)tmp_paddr_list +
						(paddr & PAGE_OFFSET_MASK));

				paddr = tmp_paddr_list[prp2_offs++];
			}
		} else {
			BUG_ON(prp2_offs == (PTRS_PER_PRP_LIST - 1) &&
			       remaining > PAGE_SIZE);
			paddr = tmp_paddr_list[prp2_offs++];
		}

		io_size = min_t(size_t, remaining, PAGE_SIZE);

		mem_offs = paddr & PAGE_OFFSET_MASK;
		if (mem_offs) {
			if (io_size + mem_offs > PAGE_SIZE)
				io_size = PAGE_SIZE - mem_offs;
		}

		remaining -= io_size;
		paddr_list[prp_offs++] = paddr;
	}
	num_prps = prp_offs;

	if (unlikely(tmp_paddr_list)) {
		if (is_memremap)
			memunmap((void *)((uintptr_t)tmp_paddr_list &
					  PAGE_MASK));
		else
			kunmap_atomic((void *)((uintptr_t)tmp_paddr_list &
					       PAGE_MASK));
	}

	remaining = length;
	prp_offs = 0;

	/* Loop for data transfer */
	while (remaining) {
		size_t page_size = 0;
		mem_offs = 0;
		io_size = 0;

		paddr = paddr_list[prp_offs];
		page_size = min_t(size_t, remaining, PAGE_SIZE);

		mem_offs = paddr & PAGE_OFFSET_MASK;
		if (unlikely(mem_offs)) {
			if (page_size + mem_offs > PAGE_SIZE)
				page_size = PAGE_SIZE - mem_offs;
		}

		for (prp_offs++; prp_offs < num_prps; prp_offs++) {
			if (paddr_list[prp_offs] ==
			    paddr_list[prp_offs - 1] + PAGE_SIZE)
				page_size += PAGE_SIZE;
			else
				break;
		}

		io_size = min_t(size_t, remaining, page_size);

#ifdef CONFIG_SWARMIO_PROFILE_REQ
		w->nsecs_issued = ktime_get_ns();
#endif
		if (__swarmio_submit_dma(dma_ctx, cmd->opcode, paddr, offset,
					 io_size,
					 io_size < remaining ? NULL : w))
			return -EIO;

		remaining -= io_size;
		offset += io_size;
	}

	return 0;
}

void swarmio_proc_io(struct swarmio_worker *worker, unsigned long *last_io_time)
{
	unsigned short i, max_count, issued_count = 0, completed_count = 0;
	dma_ctx_t *ctx = worker->dma_ctx;
	struct swarmio_worker_local_queue *local_queue = &worker->local_queue;
	struct swarmio_io_work *w;
	unsigned short local_queue_tail = smp_load_acquire(&local_queue->tail);
	unsigned short new_local_queue_head;

	/* empty */
	if (local_queue->head == local_queue_tail)
		return;

#ifdef CONFIG_SWARMIO_COMMIT
	struct swarmio_commit_queue *commit_q = worker->commit_queue;
	struct swarmio_commit_desc *commit_desc;

	if (unlikely(!commit_q))
		return;
#endif
	max_count = dma_batch_size * num_dma_descs_per_worker;

	/* loop1: dma issue */
	i = local_queue->head;
	do {
		w = local_queue->works + i;

		/* issue i/o */
		if (!w->is_issued) {
			w->is_issued = true;
			if (!w->is_internal) {
				if (worker_using_dma) {
#ifndef CONFIG_SWARMIO_WORKER_SKIP_IO
					int ret;

					ret = __swarmio_xfer_data_using_dma(
						&w->cmd.rw, ctx, w,
						worker->paddr_list);
					if (unlikely(ret)) {
						__swarmio_disable_worker_dma();
						__swarmio_xfer_data(&w->cmd.rw);
						__swarmio_mark_copied(w);
					}
#else
#ifdef CONFIG_SWARMIO_PROFILE_REQ
					w->nsecs_issued = ktime_get_ns();
#endif
					__swarmio_mark_copied(w);
#endif
				} else {
#ifdef CONFIG_SWARMIO_PROFILE_REQ
					w->nsecs_issued = ktime_get_ns();
#endif
#ifndef CONFIG_SWARMIO_WORKER_SKIP_IO
					__swarmio_xfer_data(&w->cmd.rw);
#endif
					__swarmio_mark_copied(w);
				}
			}
			issued_count++;
		}

		if (++i == local_queue->size)
			i = 0;
	} while (i != local_queue_tail && issued_count < max_count);

	/* loop2: completion */
	i = local_queue->head;
	new_local_queue_head = i;
	do {
		w = local_queue->works + i;

		/* complete */
		if (w->is_copied && !w->is_completed) {
#ifdef CONFIG_SWARMIO_COMMIT
			int old_commit_q_tail;
			int new_commit_q_tail;
			int tmp_commit_q_tail;
			bool reserved = false;

			old_commit_q_tail = atomic_read(&commit_q->tail);

			do {
				new_commit_q_tail = (old_commit_q_tail + 1) %
						    commit_q->size;

				if (unlikely(new_commit_q_tail ==
					     smp_load_acquire(
						     &commit_q->head))) {
					SWARMIO_DEBUG_RL(
						"worker %s: commit queue is full!\n",
						worker->thread_name);

					reserved = false;
					break;
				}

				tmp_commit_q_tail = atomic_cmpxchg(
					&commit_q->tail, old_commit_q_tail,
					new_commit_q_tail);
				if (tmp_commit_q_tail == old_commit_q_tail) {
					reserved = true;
					break;
				}
				old_commit_q_tail = tmp_commit_q_tail;

				cpu_relax();
			} while (true);

			if (!reserved) {
				break;
			}

			commit_desc = &commit_q->descs[old_commit_q_tail];

			/* populate commit desc */
			commit_desc->is_completed = false;
			if (w->is_internal) {
				commit_desc->is_internal = true;
			} else {
				commit_desc->is_internal = false;
				commit_desc->cqe.sq_id = w->cqe.sq_id;
				commit_desc->cqe.command_id =
					w->cmd.rw.command_id;
				commit_desc->cqid = w->cqid;
			}
			commit_desc->nsecs_target = w->nsecs_target;
#ifdef CONFIG_SWARMIO_PROFILE_REQ
			commit_desc->nsecs_copied = w->nsecs_copied;
#endif
			smp_store_release(&commit_desc->is_valid, true);

			w->is_completed = true;
#else
			__swarmio_complete(w);
#endif
			completed_count++;
		}

		if (w->is_completed && i == new_local_queue_head) {
			if (++new_local_queue_head == local_queue->size)
				new_local_queue_head = 0;
#ifdef CONFIG_SWARMIO_PROFILE_REQ
			__set_stats_add(&worker->stats_wait_issue,
					w->nsecs_issued, w->nsecs_enqueued);
			__set_stats_add(&worker->stats_copy, w->nsecs_copied,
					w->nsecs_issued);
#ifndef CONFIG_SWARMIO_COMMIT
			__set_stats_add(&worker->stats_wait_cpl,
					w->nsecs_start_cpl, w->nsecs_copied);
			__set_stats_add(&worker->stats_fill_cpl,
					w->nsecs_end_cpl, w->nsecs_start_cpl);
			if (w->nsecs_target > w->nsecs_start) {
				worker->stats_error.sum +=
					((w->nsecs_end_cpl - w->nsecs_target) *
					 1000ULL) /
					(w->nsecs_target - w->nsecs_start);
				worker->stats_error.count++;
			}
#endif
#endif
		}

		if (++i == local_queue->size)
			i = 0;
	} while (i != local_queue_tail && completed_count < max_count);

	/* advance head if it has changed */
	if (new_local_queue_head != local_queue->head) {
		smp_store_release(&local_queue->head, new_local_queue_head);
	}

#ifdef CONFIG_SWARMIO_WORKER_BATCH_IO
	if (worker_using_dma) {
		if (__dsa_dma_is_issue_timeout_batch(ctx)) {
			int ret;

			ret = dsa_dma_issue_async_remaining_batch(ctx);
			if (unlikely(ret))
				__swarmio_disable_worker_dma();
		}

		if (__dsa_dma_is_wait_timeout_batch(ctx)) {
			int ret;

			ret = dsa_dma_wait_oldest_batch(ctx, __swarmio_dma_cb);
			if (unlikely(ret))
				__swarmio_disable_worker_dma();
		}
	}
#endif

	*last_io_time = jiffies;
}

#ifndef CONFIG_SWARMIO_COMMIT
void swarmio_proc_irqs(struct swarmio_worker *worker)
{
	int qid, start, end, stride;
	struct nvme_completion_queue *cq;
	struct swarmio_dispatcher *dispatcher = worker->dispatcher;

	if (unlikely(!dispatcher))
		return;

	start = dispatcher->qids.start;
	end = dispatcher->qids.end;
	stride = dispatcher->qids.stride;

	if (unlikely(start == -1))
		return;

	for (qid = start; qid <= end; qid += stride) {
		if (worker->id !=
		    (((qid - start) / stride) % dispatcher->num_workers))
			continue;

		cq = vdev->cqes[qid];

		if (cq == NULL || !cq->irq_enabled)
			continue;

#ifdef CONFIG_SWARMIO_DISP_ALLOC_WORKER_RR
		if (smp_load_acquire(&cq->interrupt_ready)) {
			WRITE_ONCE(cq->interrupt_ready, false);
			vdev_signal_irq(cq->irq_vector);
		}
#else
		if (cq->interrupt_ready) {
			cq->interrupt_ready = false;
			vdev_signal_irq(cq->irq_vector);
		}
#endif
	}
}
#endif

static int swarmio_worker_thread_fn(void *data)
{
	struct swarmio_worker *worker = (struct swarmio_worker *)data;
	static unsigned long last_io_time = 0;

	SWARMIO_DEBUG("%s started on cpu %d (node %d)\n", worker->thread_name,
		      smp_processor_id(), cpu_to_node(smp_processor_id()));

	while (!kthread_should_stop()) {
		swarmio_proc_io(worker, &last_io_time);
#ifndef CONFIG_SWARMIO_COMMIT
		swarmio_proc_irqs(worker);
#endif
		if (CONFIG_SWARMIO_IDLE_TIMEOUT != 0 &&
		    time_after(jiffies,
			       last_io_time +
				       (CONFIG_SWARMIO_IDLE_TIMEOUT * HZ)))
			schedule_timeout_interruptible(1);
		else
			cond_resched();

		__thread_wait_on_flag(&vdev->stop_flag, &vdev->num_waiting,
				      &vdev->wait_queue);
	}

	return 0;
}

int swarmio_worker_init(struct nvme_vdev *vdev)
{
	unsigned int i, j;
	int dev_id, wq_id;
	struct swarmio_worker *worker;
	struct swarmio_worker_local_queue *local_queue;
	size_t local_queue_size;
	int ret;

	unsigned int num_workers = vdev->config.num_workers;
	unsigned int num_workers_per_service_unit =
		vdev->config.num_workers_per_service_unit;

	local_queue_size =
		min_t(size_t, NR_WORKER_QUEUE_SIZE,
		      KMALLOC_MAX_SIZE / sizeof(struct swarmio_io_work));
	if (local_queue_size != NR_WORKER_QUEUE_SIZE)
		SWARMIO_WARN("worker queue size clipped from %u to %zu\n",
			     NR_WORKER_QUEUE_SIZE, local_queue_size);

	vdev->num_workers_initialized = 0;
	vdev->workers = kzalloc(sizeof(struct swarmio_worker) * num_workers,
				GFP_KERNEL);
	if (!vdev->workers)
		return -ENOMEM;

	for (i = 0; i < num_workers; i++) {
		worker = &vdev->workers[i];
		worker->id = i % num_workers_per_service_unit;
		worker->disp_id = i / num_workers_per_service_unit;
		worker->dispatcher = NULL;
#ifdef CONFIG_SWARMIO_COMMIT
		worker->commit_queue =
			&vdev->committers[worker->disp_id].commit_queue;
#endif
#ifdef CONFIG_SWARMIO_PROFILE_REQ
		__reset_stats(&worker->stats_wait_issue);
		__reset_stats(&worker->stats_copy);
		__reset_stats(&worker->stats_wait_cpl);
		__reset_stats(&worker->stats_fill_cpl);
		__reset_stats(&worker->stats_error);
#endif
		/* assign contig. cpus for each (dispatcher, workers) unit */
#ifdef CONFIG_SWARMIO_COMMIT
		j = worker->disp_id * (1 + num_workers_per_service_unit + 1) +
		    1 + worker->id;
#else
		j = worker->disp_id * (1 + num_workers_per_service_unit) + 1 +
		    worker->id;
#endif
		worker->cpu_id = cpumask_nth(j, vdev->config.cpu_mask);

		/* allocate and initialize work queue */
		local_queue = &worker->local_queue;
		local_queue->size = local_queue_size;
		local_queue->works = kzalloc(sizeof(struct swarmio_io_work) *
						     local_queue->size,
					     GFP_KERNEL);
		if (!local_queue->works) {
			ret = -ENOMEM;
			goto err;
		}
		local_queue->head = 0;
		local_queue->tail = 0;
		worker->paddr_list =
			kzalloc(sizeof(u64) * PTRS_PER_PRP_LIST, GFP_KERNEL);
		if (!worker->paddr_list) {
			ret = -ENOMEM;
			goto err;
		}

		snprintf(worker->thread_name, sizeof(worker->thread_name),
			 "swarmio_worker_%d-%d", worker->disp_id, worker->id);

		/* allocate and initialize dma context */
		if (worker_using_dma) {
			dsa_assign_wq_by_group_disagg(worker->disp_id,
						      worker->id + 1, &dev_id,
						      &wq_id);
			worker->dma_ctx =
				kzalloc(sizeof(dma_ctx_t), GFP_KERNEL);
			if (!worker->dma_ctx) {
				ret = -ENOMEM;
				goto err;
			}
#ifdef CONFIG_SWARMIO_WORKER_BATCH_IO
			ret = dsa_dma_ctx_batch_init(worker->dma_ctx, dev_id,
						     wq_id, dma_batch_size,
						     num_dma_descs_per_worker,
						     dma_timeout);
#else
			ret = dsa_dma_ctx_single_init(worker->dma_ctx, dev_id,
						      wq_id,
						      num_dma_descs_per_worker,
						      dma_timeout);
#endif
			if (ret) {
				SWARMIO_ERROR("%s: dma ctx init failed!\n",
					      worker->thread_name);
				goto err;
			}
			SWARMIO_DEBUG("%s assigned %s\n", worker->thread_name,
				      worker->dma_ctx->wq->name);
		}

		worker->task_struct = kthread_create(swarmio_worker_thread_fn,
						     worker, "%s",
						     worker->thread_name);
		if (IS_ERR(worker->task_struct)) {
			ret = PTR_ERR(worker->task_struct);
			worker->task_struct = NULL;
			goto err;
		}

		kthread_bind(worker->task_struct, worker->cpu_id);
		wake_up_process(worker->task_struct);
		vdev->num_workers_initialized++;
	}

	return 0;

err:
	swarmio_worker_exit(vdev);
	return ret ? ret : -EIO;
}

void swarmio_worker_exit(struct nvme_vdev *vdev)
{
	unsigned int i;
	struct swarmio_worker *worker;

	if (!vdev->workers)
		return;

	for (i = 0; i < vdev->num_workers_initialized; i++) {
		worker = &vdev->workers[i];

		if (!IS_ERR_OR_NULL(worker->task_struct)) {
			kthread_stop(worker->task_struct);
		}

		if (worker->dma_ctx) {
#ifdef CONFIG_SWARMIO_WORKER_BATCH_IO
			dsa_dma_ctx_batch_remove(worker->dma_ctx);
#else
			dsa_dma_ctx_single_remove(worker->dma_ctx);
#endif
		}

		if (worker->dispatcher)
			worker->dispatcher->workers[worker->id] = NULL;

		mb();

		kfree(worker->dma_ctx);
		kfree(worker->paddr_list);
		kfree(worker->local_queue.works);
	}

	kfree(vdev->workers);
	vdev->workers = NULL;
	vdev->num_workers_initialized = 0;
}
