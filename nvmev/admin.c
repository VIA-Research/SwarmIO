// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on code from https://github.com/snu-csl/nvmevirt
 * Copyright (C) 2026 VIA-Research, modified on 2026-04-07
 */

#include "common.h"
#include "dispatcher.h"
#include "admin.h"
#include "nvme_vdev.h"
#include "pci.h"
#include "queue.h"

#ifndef NVME_STATUS_DNR
#define NVME_STATUS_DNR 0x4000
#endif

#define prp_address_offset(prp, offset)                          \
	(page_address(pfn_to_page(prp >> PAGE_SHIFT) + offset) + \
	 (prp & ~PAGE_MASK))
#define prp_address(prp) prp_address_offset(prp, 0)

static void __make_cq_entry_results(int eid, u16 ret, u32 result0, u32 result1)
{
	struct nvme_admin_queue *queue = vdev->admin_q;
	struct nvme_common_command *cmd =
		&__admin_sq_entry_at(queue, eid)->common;
	int cq_head = queue->cq_head;

	*__admin_cq_entry_at(queue, cq_head) = (struct nvme_completion){
		.command_id = cmd->command_id,
		.sq_id = 0,
		.sq_head = eid,
		.result = {
			.u64 = cpu_to_le64((u64)result0 | ((u64)result1 << 32)),
		},
		.status = queue->phase | (ret << 1),
	};

	if (++cq_head == queue->cq_depth) {
		cq_head = 0;
		queue->phase = !queue->phase;
	}
	queue->cq_head = cq_head;
}

static void __make_cq_entry(int eid, u16 ret)
{
	__make_cq_entry_results(eid, ret, 0, 0);
}

void vdev_delete_sq(struct nvme_submission_queue *sq)
{
	if (!sq)
		return;

	if (sq->cmd_buffer)
		memunmap(sq->cmd_buffer);
	sq->cmd_buffer = NULL;

#if defined(CONFIG_SWARMIO_DISP_COALESCE_SQE)
#if defined(CONFIG_SWARMIO_AGGREGATED_TIMING_UPDATE) && \
	defined(CONFIG_SWARMIO_PROFILE_REQ)
	kfree(sq->nsecs_starts);
	sq->nsecs_starts = NULL;
#endif
	kfree(sq->nsecs_targets);
	sq->nsecs_targets = NULL;
#endif

	kfree(sq->sq);
	sq->sq = NULL;

	if (sq->mapped)
		memunmap(sq->mapped);
	sq->mapped = NULL;

	kfree(sq);
}

void vdev_delete_cq(struct nvme_completion_queue *cq)
{
	if (!cq)
		return;

	kfree(cq->cq);
	cq->cq = NULL;

	if (cq->mapped)
		memunmap(cq->mapped);
	cq->mapped = NULL;

	kfree(cq);
}

static void admin_create_cq(int eid)
{
	struct nvme_admin_queue *queue = vdev->admin_q;
	struct nvme_completion_queue *cq;
	struct nvme_create_cq *cmd =
		&__admin_sq_entry_at(queue, eid)->create_cq;
	unsigned int num_pages, i;
	int dbs_idx;

	cq = kzalloc(sizeof(struct nvme_completion_queue), GFP_KERNEL);

	cq->qid = cmd->cqid;

	cq->irq_enabled = cmd->cq_flags & NVME_CQ_IRQ_ENABLED ? true : false;
	if (cq->irq_enabled) {
		cq->irq_vector = cmd->irq_vector;
	}
	cq->interrupt_ready = false;

	cq->queue_size = cmd->qsize + 1;
	cq->phase = 1;

	cq->cq_head = 0;
#if !defined(CONFIG_SWARMIO_DISP_ALLOC_WORKER_RR) || \
	defined(CONFIG_SWARMIO_COMMIT)
	cq->cq_tail = 0;
#else
	atomic_set(&cq->cq_tail, 0);
#endif
	spin_lock_init(&cq->entry_lock);
	mutex_init(&cq->irq_lock);

	/* TODO Physically non-contiguous prp list */
	cq->phys_contig = cmd->cq_flags & NVME_QUEUE_PHYS_CONTIG ? true : false;
	WARN_ON(!cq->phys_contig);

	num_pages = DIV_ROUND_UP(
		cq->queue_size * sizeof(struct nvme_completion), PAGE_SIZE);
	cq->cq = kzalloc(sizeof(struct nvme_completion *) * num_pages,
			 GFP_KERNEL);

	if (pfn_valid(cmd->prp1 >> PAGE_SHIFT)) {
		cq->mapped = NULL;
		for (i = 0; i < num_pages; i++)
			cq->cq[i] = prp_address_offset(cmd->prp1, i);
	} else {
		cq->mapped =
			memremap(cmd->prp1, num_pages * PAGE_SIZE, MEMREMAP_WT);
		for (i = 0; i < num_pages; i++)
			cq->cq[i] =
				(void *)((uint64_t)cq->mapped + i * PAGE_SIZE);
	}

	vdev->cqes[cq->qid] = cq;

	dbs_idx = cq->qid * 2 + 1;
	vdev->dbs[dbs_idx] = vdev->old_dbs[dbs_idx] = 0;

	__make_cq_entry(eid, NVME_SC_SUCCESS);
}

static void admin_delete_cq(int eid)
{
	struct nvme_admin_queue *queue = vdev->admin_q;
	struct nvme_completion_queue *cq;
	unsigned int qid;

	qid = __admin_sq_entry_at(queue, eid)->delete_queue.qid;

	cq = vdev->cqes[qid];
	vdev->cqes[qid] = NULL;
	vdev_delete_cq(cq);

	__make_cq_entry(eid, NVME_SC_SUCCESS);
}

static void admin_create_sq(int eid)
{
	struct nvme_admin_queue *queue = vdev->admin_q;
	struct nvme_create_sq *cmd =
		&__admin_sq_entry_at(queue, eid)->create_sq;
	struct nvme_submission_queue *sq;
	unsigned int num_pages, i;
	int dbs_idx;
	unsigned long max_qs_bytes =
		sizeof(struct nvme_command) * NR_MAX_QUEUE_SIZE;
	unsigned long cmd_buffer_start;

	sq = kzalloc(sizeof(struct nvme_submission_queue), GFP_KERNEL);

	sq->qid = cmd->sqid;
	sq->cqid = cmd->cqid;

	sq->queue_size = cmd->qsize + 1;
	sq->sq_head = 0;

#ifdef CONFIG_SWARMIO_DISP_PREFETCH_SQE
	sq->is_pref_issued = false;
	sq->dma_desc_id = -1;
	sq->pref_size = 0;
#endif

#if defined(CONFIG_SWARMIO_DISP_COALESCE_SQE)
	sq->fetched_pos = 0;
#if defined(CONFIG_SWARMIO_AGGREGATED_TIMING_UPDATE) && \
	defined(CONFIG_SWARMIO_PROFILE_REQ)
	sq->nsecs_starts = kzalloc(sizeof(unsigned long long) * sq->queue_size,
				   GFP_KERNEL);
#endif
	sq->nsecs_targets = kzalloc(sizeof(unsigned long long) * sq->queue_size,
				    GFP_KERNEL);
#endif

	cmd_buffer_start =
		vdev->config.cmd_buffer_start + max_qs_bytes * (sq->qid - 1);
	sq->cmd_buffer = (struct nvme_command *)memremap(
		cmd_buffer_start, max_qs_bytes, MEMREMAP_WB);
	sq->cmd_buffer_dma = cmd_buffer_start;
	if (unlikely(!sq->cmd_buffer)) {
		SWARMIO_ERROR("Failed to memremap cmd buffer for SQ %d\n",
			      sq->qid);
		__make_cq_entry(eid, NVME_SC_INTERNAL | NVME_STATUS_DNR);
	}

	/* TODO Physically non-contiguous prp list */
	WARN_ON(!(cmd->sq_flags & NVME_QUEUE_PHYS_CONTIG));

	num_pages = DIV_ROUND_UP(sq->queue_size * sizeof(struct nvme_command),
				 PAGE_SIZE);
	sq->sq = kzalloc(sizeof(struct nvme_command *) * num_pages, GFP_KERNEL);

	sq->dma_addr = cmd->prp1;
	if (pfn_valid(cmd->prp1 >> PAGE_SHIFT)) {
		sq->mapped = NULL;
		for (i = 0; i < num_pages; i++)
			sq->sq[i] = prp_address_offset(cmd->prp1, i);
	} else {
		sq->mapped =
			memremap(cmd->prp1, num_pages * PAGE_SIZE, MEMREMAP_WT);
		for (i = 0; i < num_pages; i++)
			sq->sq[i] =
				(void *)((uint64_t)sq->mapped + i * PAGE_SIZE);
	}

	vdev->sqes[sq->qid] = sq;

	dbs_idx = sq->qid * 2;
	vdev->dbs[dbs_idx] = 0;
	vdev->old_dbs[dbs_idx] = 0;

	__make_cq_entry(eid, NVME_SC_SUCCESS);
}

static void admin_delete_sq(int eid)
{
	struct nvme_admin_queue *queue = vdev->admin_q;
	struct nvme_delete_queue *cmd =
		&__admin_sq_entry_at(queue, eid)->delete_queue;
	struct nvme_submission_queue *sq;
	unsigned int qid;

	qid = cmd->qid;

	sq = vdev->sqes[qid];
	vdev->sqes[qid] = NULL;
	vdev_delete_sq(sq);

	__make_cq_entry(eid, NVME_SC_SUCCESS);
}

/***
 * Log pages
 */
static void admin_get_log_page(int eid)
{
	struct nvme_admin_queue *queue = vdev->admin_q;
	struct nvme_get_log_page_command *cmd =
		&__admin_sq_entry_at(queue, eid)->get_log_page;
	void *page;
	uint32_t len = ((((uint32_t)cmd->numdu << 16) | cmd->numdl) + 1) << 2;

	page = prp_address(cmd->dptr.prp1);

	switch (cmd->lid) {
	case NVME_LOG_SMART: {
		static const struct nvme_smart_log smart_log = {
			.critical_warning = 0,
			.spare_thresh = 20,
			.host_reads[0] = cpu_to_le64(0),
			.host_writes[0] = cpu_to_le64(0),
			.num_err_log_entries[0] = cpu_to_le64(0),
			.temperature[0] = 0 & 0xff,
			.temperature[1] = (0 >> 8) & 0xff,
		};

		__memcpy(page, &smart_log, len);
		break;
	}
	case NVME_LOG_CMD_EFFECTS: {
		static const struct nvme_effects_log effects_log = {
			.acs = {
				[nvme_admin_get_log_page] = cpu_to_le32(NVME_CMD_EFFECTS_CSUPP),
				[nvme_admin_identify] = cpu_to_le32(NVME_CMD_EFFECTS_CSUPP),
				// [nvme_admin_abort_cmd] = cpu_to_le32(NVME_CMD_EFFECTS_CSUPP),
				[nvme_admin_set_features] = cpu_to_le32(NVME_CMD_EFFECTS_CSUPP),
				[nvme_admin_get_features] = cpu_to_le32(NVME_CMD_EFFECTS_CSUPP),
				[nvme_admin_async_event] = cpu_to_le32(NVME_CMD_EFFECTS_CSUPP),
				// [nvme_admin_keep_alive] = cpu_to_le32(NVME_CMD_EFFECTS_CSUPP),
			},
			.iocs = { 0, },
			.resv = { 0, },
		};

		__memcpy(page, &effects_log, len);
		break;
	}
	default:
		/* Note (NVMeVirt): Returning zeroed data for an unsupported log page */
		SWARMIO_ERROR("Unimplemented log page identifier: 0x%hhx\n",
			      cmd->lid);
		__memset(page, 0, len);
		break;
	}

	__make_cq_entry(eid, NVME_SC_SUCCESS);
}

/***
 * Identify functions
 */
static void admin_identify_namespace(int eid)
{
	struct nvme_admin_queue *queue = vdev->admin_q;
	struct nvme_id_ns *ns;
	struct nvme_identify *cmd = &__admin_sq_entry_at(queue, eid)->identify;
	size_t nsid = cmd->nsid - 1;

	ns = prp_address(cmd->dptr.prp1);
	memset(ns, 0x0, PAGE_SIZE);

	ns->lbaf[0].ms = 0;
	ns->lbaf[0].ds = 9;
	ns->lbaf[0].rp = NVME_LBAF_RP_GOOD;

	ns->lbaf[1].ms = 8;
	ns->lbaf[1].ds = 9;
	ns->lbaf[1].rp = NVME_LBAF_RP_GOOD;

	ns->lbaf[2].ms = 16;
	ns->lbaf[2].ds = 9;
	ns->lbaf[2].rp = NVME_LBAF_RP_GOOD;

	ns->lbaf[3].ms = 0;
	ns->lbaf[3].ds = 12;
	ns->lbaf[3].rp = NVME_LBAF_RP_BEST;

	ns->lbaf[4].ms = 8;
	ns->lbaf[4].ds = 12;
	ns->lbaf[4].rp = NVME_LBAF_RP_BEST;

	ns->lbaf[5].ms = 64;
	ns->lbaf[5].ds = 12;
	ns->lbaf[5].rp = NVME_LBAF_RP_BEST;

	ns->lbaf[6].ms = 128;
	ns->lbaf[6].ds = 12;
	ns->lbaf[6].rp = NVME_LBAF_RP_BEST;

	if (LBA_BITS == 9) {
		ns->flbas = 0;
	} else if (LBA_BITS == 12) {
		ns->flbas = 3;
	} else {
		BUG();
	}

	ns->nlbaf = 6;
	ns->dps = 0;

	ns->nsze = (vdev->ns[nsid].size >> ns->lbaf[ns->flbas].ds);
	ns->ncap = ns->nsze;
	ns->nuse = ns->nsze;

	__make_cq_entry(eid, NVME_SC_SUCCESS);
}

static void admin_identify_namespaces(int eid)
{
	struct nvme_admin_queue *queue = vdev->admin_q;
	struct nvme_identify *cmd = &__admin_sq_entry_at(queue, eid)->identify;
	unsigned int *ns;
	int i;

	ns = prp_address(cmd->dptr.prp1);
	memset(ns, 0x00, PAGE_SIZE * 2);

	for (i = 1; i <= vdev->num_ns; i++) {
		if (i > cmd->nsid) {
			*ns = i;
			ns++;
		}
	}

	__make_cq_entry(eid, NVME_SC_SUCCESS);
}

static void admin_identify_namespace_desc(int eid)
{
	struct nvme_admin_queue *queue = vdev->admin_q;
	struct nvme_identify *cmd = &__admin_sq_entry_at(queue, eid)->identify;
	struct nvme_ns_id_desc *ns_desc;
	__u8 *nid;
	int nsid = cmd->nsid - 1;

	ns_desc = prp_address(cmd->dptr.prp1);
	memset(ns_desc, 0x00, NVME_IDENTIFY_DATA_SIZE);
	nid = (__u8 *)(ns_desc + 1);

	ns_desc->nidt = NVME_NIDT_CSI;
	ns_desc->nidl = 1;

	nid[0] = vdev->ns[nsid].csi; // Zoned Namespace Command Set

	__make_cq_entry(eid, NVME_SC_SUCCESS);
}

static void admin_identify_ctrl(int eid)
{
	struct nvme_admin_queue *queue = vdev->admin_q;
	struct nvme_identify *cmd = &__admin_sq_entry_at(queue, eid)->identify;
	struct nvme_id_ctrl *ctrl;

	ctrl = prp_address(cmd->dptr.prp1);
	memset(ctrl, 0x00, sizeof(*ctrl));

	ctrl->nn = vdev->num_ns;
	ctrl->oncs = 0; //optional command
	ctrl->acl = 3; //minimum 4 required, 0's based value
	ctrl->vwc = 0;
	snprintf(ctrl->sn, sizeof(ctrl->sn), "SN_%02d", 1);
	snprintf(ctrl->mn, sizeof(ctrl->mn), "MN_%02d", 1);
	snprintf(ctrl->fr, sizeof(ctrl->fr), "FR_%03d", 2);
	ctrl->mdts = vdev->mdts;
	ctrl->sqes = 0x66;
	ctrl->cqes = 0x44;

	__make_cq_entry(eid, NVME_SC_SUCCESS);
}

static void admin_identify(int eid)
{
	struct nvme_admin_queue *queue = vdev->admin_q;
	int cns = __admin_sq_entry_at(queue, eid)->identify.cns;

	switch (cns) {
	case 0x00:
		admin_identify_namespace(eid);
		break;
	case 0x01:
		admin_identify_ctrl(eid);
		break;
	case 0x02:
		admin_identify_namespaces(eid);
		break;
	case 0x03:
		admin_identify_namespace_desc(eid);
		break;
	default:
		__make_cq_entry(eid, NVME_SC_INVALID_OPCODE);
		SWARMIO_ERROR("unsupported CNS value: %d\n", cns);
	}
}

/***
 * Set/get features
 */
static void admin_set_features(int eid)
{
	struct nvme_admin_queue *queue = vdev->admin_q;
	struct nvme_features *cmd = &__admin_sq_entry_at(queue, eid)->features;
	__le32 result0 = 0;
	__le32 result1 = 0;

	switch (cmd->fid) {
	case NVME_FEAT_ARBITRATION:
	case NVME_FEAT_POWER_MGMT:
	case NVME_FEAT_LBA_RANGE:
	case NVME_FEAT_TEMP_THRESH:
	case NVME_FEAT_ERR_RECOVERY:
	case NVME_FEAT_VOLATILE_WC:
		break;
	case NVME_FEAT_NUM_QUEUES: {
		int num_queue;
		unsigned int i;
#ifdef CONFIG_SWARMIO_DISP_ASSIGN_QID_CONT
		unsigned int len;
#endif
		int j = 1;
		int qid, dbs_idx;
		unsigned int num_service_units = vdev->config.num_service_units;
		unsigned int num_workers = vdev->config.num_workers;
#ifdef CONFIG_SWARMIO_COMMIT
		unsigned int num_waiting =
			num_service_units * 2 + num_workers - 1;
#else
		unsigned int num_waiting = num_service_units + num_workers - 1;
#endif
		struct swarmio_dispatcher *dispatcher;

		WRITE_ONCE(vdev->stop_flag, 1);
		while (atomic_read(&vdev->num_waiting) != num_waiting) {
			cond_resched();
		}

		// # of sq in 0-base
		num_queue = (__admin_sq_entry_at(queue, eid)->features.dword11 &
			     0xFFFF) +
			    1;
		vdev->num_sq = min(num_queue, NR_MAX_IO_QUEUE);

		// # of cq in 0-base
		num_queue =
			((__admin_sq_entry_at(queue, eid)->features.dword11 >>
			  16) &
			 0xFFFF) +
			1;
		vdev->num_cq = min(num_queue, NR_MAX_IO_QUEUE);

		result0 = ((vdev->num_cq - 1) << 16 | (vdev->num_sq - 1));

		for (qid = 1; qid <= vdev->num_sq; qid++) {
			dbs_idx = qid * 2;
			vdev->dbs[dbs_idx] = 0;
			vdev->old_dbs[dbs_idx] = 0;
			vdev->dbs[dbs_idx + 1] = 0;
			vdev->old_dbs[dbs_idx + 1] = 0;
		}

		for (i = 0; i < num_service_units; i++) {
			dispatcher = &vdev->dispatchers[i];

#ifdef CONFIG_SWARMIO_DISP_ASSIGN_QID_CONT
			len = (vdev->num_sq + num_service_units - 1 - i) /
			      num_service_units;

			if (len > 0) {
				dispatcher->qids.start = j;
				dispatcher->qids.end = j + len - 1;
				dispatcher->qids.stride = 1;
			} else {
				dispatcher->qids.start = dispatcher->qids.end =
					-1;
			}
			j += len;
#else
			if ((i + 1) <= vdev->num_sq) {
				dispatcher->qids.start = i + 1;
				dispatcher->qids.end = vdev->num_sq;
				dispatcher->qids.stride = num_service_units;

			} else {
				dispatcher->qids.start = dispatcher->qids.end =
					-1;
			}
#endif
			if (dispatcher->qids.start != -1)
				SWARMIO_INFO(
					"%s assigned sq [%02d-%02d, stride: %02d]\n",
					dispatcher->thread_name,
					dispatcher->qids.start,
					dispatcher->qids.end,
					dispatcher->qids.stride);
		}

		mb();

		WRITE_ONCE(vdev->stop_flag, 0);
		wake_up_all(&vdev->wait_queue);
	}
	case NVME_FEAT_IRQ_COALESCE:
	case NVME_FEAT_IRQ_CONFIG:
	case NVME_FEAT_WRITE_ATOMIC:
	case NVME_FEAT_ASYNC_EVENT:
	case NVME_FEAT_AUTO_PST:
	case NVME_FEAT_SW_PROGRESS:
	case NVME_FEAT_HOST_ID:
	case NVME_FEAT_RESV_MASK:
	case NVME_FEAT_RESV_PERSIST:
	default:
		break;
	}

	__make_cq_entry_results(eid, NVME_SC_SUCCESS, result0, result1);
}

static void admin_get_features(int eid)
{
	struct nvme_admin_queue *queue = vdev->admin_q;
	struct nvme_features *cmd = &__admin_sq_entry_at(queue, eid)->features;
	__le32 result0 = 0;
	__le32 result1 = 0;

	switch (cmd->fid) {
	case NVME_FEAT_ARBITRATION:
	case NVME_FEAT_POWER_MGMT:
	case NVME_FEAT_LBA_RANGE:
	case NVME_FEAT_TEMP_THRESH:
	case NVME_FEAT_ERR_RECOVERY:
	case NVME_FEAT_VOLATILE_WC:
		break;
	case NVME_FEAT_NUM_QUEUES:
		result0 = ((vdev->num_cq - 1) << 16 | (vdev->num_sq - 1));
		break;
	case NVME_FEAT_IRQ_COALESCE:
	case NVME_FEAT_IRQ_CONFIG:
	case NVME_FEAT_WRITE_ATOMIC:
	case NVME_FEAT_ASYNC_EVENT:
	case NVME_FEAT_AUTO_PST:
	case NVME_FEAT_SW_PROGRESS:
	case NVME_FEAT_HOST_ID:
	case NVME_FEAT_RESV_MASK:
	case NVME_FEAT_RESV_PERSIST:
	default:
		break;
	}

	__make_cq_entry_results(eid, NVME_SC_SUCCESS, result0, result1);
}

/***
 * Misc
 */
static void admin_async_event(int eid)
{
	__make_cq_entry(eid, NVME_SC_SUCCESS);
	// __make_cq_entry(eid, NVME_SC_ASYNC_LIMIT);
}

void vdev_proc_admin_sq_entry(int entry_id)
{
	struct nvme_admin_queue *queue = vdev->admin_q;
	struct nvme_command *sqe = __admin_sq_entry_at(queue, entry_id);

	switch (sqe->common.opcode) {
	case nvme_admin_delete_sq:
		admin_delete_sq(entry_id);
		break;
	case nvme_admin_create_sq:
		admin_create_sq(entry_id);
		break;
	case nvme_admin_get_log_page:
		admin_get_log_page(entry_id);
		break;
	case nvme_admin_delete_cq:
		admin_delete_cq(entry_id);
		break;
	case nvme_admin_create_cq:
		admin_create_cq(entry_id);
		break;
	case nvme_admin_identify:
		admin_identify(entry_id);
		break;
	case nvme_admin_abort_cmd:
		break;
	case nvme_admin_set_features:
		admin_set_features(entry_id);
		break;
	case nvme_admin_get_features:
		admin_get_features(entry_id);
		break;
	case nvme_admin_async_event:
		admin_async_event(entry_id);
		break;
	case nvme_admin_activate_fw:
	case nvme_admin_download_fw:
	case nvme_admin_format_nvm:
	case nvme_admin_security_send:
	case nvme_admin_security_recv:
	default:
		__make_cq_entry(entry_id, NVME_SC_INVALID_OPCODE);
		SWARMIO_ERROR("Unhandled admin requests: %d",
			      sqe->common.opcode);
		break;
	}
}
