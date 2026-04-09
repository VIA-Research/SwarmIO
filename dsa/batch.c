// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 VIA-Research
 */

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/idxd.h>
#include <linux/sched/clock.h>

#include <idxd.h>

#include "dsa.h"

extern struct idxd_device *dsa_idxd_list[DSA_MAX_DEVS];

static int dsa_alloc_lv2_desc_list(struct dsa_dma_ctx_batch *ctx, int len)
{
	struct device *dev;
	struct idxd_device *idxd;

	if (!ctx)
		return -EINVAL;
	idxd = ctx->wq->idxd;
	dev = &idxd->pdev->dev;

	ctx->lv2_hw_desc_list =
		dma_alloc_coherent(dev, sizeof(struct dsa_hw_desc) * len,
				   &ctx->lv2_hw_desc_list_dma, GFP_KERNEL);
	if (!ctx->lv2_hw_desc_list)
		return -ENOMEM;

	ctx->lv2_compl_list = dma_alloc_coherent(
		dev, sizeof(struct dsa_completion_record) * len,
		&ctx->lv2_compl_list_dma, GFP_KERNEL);
	if (!ctx->lv2_compl_list)
		goto err_alloc_compls;

	return 0;

err_alloc_compls:
	dma_free_coherent(dev, sizeof(struct dsa_hw_desc) * len,
			  ctx->lv2_hw_desc_list, ctx->lv2_hw_desc_list_dma);

	return -ENOMEM;
}

static void dsa_free_lv2_desc_list(struct idxd_device *idxd,
				   struct dsa_dma_ctx_batch *ctx)
{
	int len;
	struct device *dev;

	if (!idxd || !ctx)
		return;
	dev = &idxd->pdev->dev;
	len = ctx->batch_size * ctx->num_descs;

	if (ctx->lv2_hw_desc_list) {
		dma_free_coherent(dev, sizeof(struct dsa_hw_desc) * len,
				  ctx->lv2_hw_desc_list,
				  ctx->lv2_hw_desc_list_dma);
		ctx->lv2_hw_desc_list = NULL;
	}

	if (ctx->lv2_compl_list) {
		dma_free_coherent(dev,
				  sizeof(struct dsa_completion_record) * len,
				  ctx->lv2_compl_list, ctx->lv2_compl_list_dma);
		ctx->lv2_compl_list = NULL;
	}
}

static int dsa_alloc_and_prep_batch_desc(struct dsa_batch_desc *batch_desc,
					 struct idxd_wq *wq,
					 dma_addr_t lv2_hw_descs_dma,
					 dma_addr_t lv2_compls_dma,
					 int batch_size)
{
	int i;
	struct idxd_desc *desc;

	if (!batch_desc || !wq)
		return -EINVAL;

	// populate lv2 descs
	for (i = 0; i < batch_size; i++) {
		__dsa_prep_hw_desc(
			&batch_desc->lv2_hw_descs[i], DSA_OPCODE_MEMMOVE, 0, 0,
			0,
			lv2_compls_dma +
				sizeof(struct dsa_completion_record) * i,
			IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR);
		batch_desc->lv2_compls[i].status = DSA_COMP_NONE;
	}

	// alloc populate batch desc
	desc = idxd_alloc_desc(wq, IDXD_OP_BLOCK);
	if (IS_ERR(desc)) {
		SWARMIO_ERROR("allocate desc for wq %d failed\n", wq->id);
		return -ENODEV;
	}
	__dsa_prep_hw_desc(desc->hw, DSA_OPCODE_BATCH, lv2_hw_descs_dma, 0,
			   batch_size, desc->compl_dma,
			   IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR);
	desc->completion->status = DSA_COMP_NONE;

	batch_desc->idxd_desc = desc;

	return 0;
}

int dsa_dma_ctx_batch_init(struct dsa_dma_ctx_batch *ctx, int dev_id, int wq_id,
			   int batch_size, int num_descs, int timeout)
{
	int i;
	int ret = 0;
	int offset = 0;
	struct idxd_device *idxd = dsa_idxd_list[dev_id];
	struct idxd_wq *wq;

	if (!ctx || !idxd)
		return -ENODEV;

	if (wq_id >= idxd->max_wqs)
		return -EINVAL;
	wq = idxd->wqs[wq_id];

	INIT_LIST_HEAD(&ctx->issued_descs);
	bitmap_zero(ctx->issued_mask, DSA_MAX_NUM_DESCS_PER_THREAD);

	ctx->batch_size = batch_size;
	ctx->curr_desc = 0;
	ctx->curr_num_filled = 0;
	ctx->num_descs = num_descs;
	ctx->start_issue_time = 0;
	ctx->last_wait_time = 0;
	ctx->batch_issue_timeout_ns =
		DSA_BATCH_MAX_SUBMIT_TIMEOUT_US / 2 * 1000;
	ctx->batch_sync_timeout_ns = DSA_BATCH_MAX_SYNC_TIMEOUT_US / 2 * 1000;
	ctx->dev_id = dev_id;
	ctx->wq_id = wq_id;
	ctx->wq = wq;
	ctx->timeout = timeout;

	/* alloc lv2_desc_list */
	ret = dsa_alloc_lv2_desc_list(ctx, num_descs * batch_size);
	if (ret < 0) {
		SWARMIO_ERROR("failed to allocate lv2 desc list\n");
		return ret;
	}

	/* alloc private data_list (e.g., callback params) */
	ctx->data_list =
		kzalloc(sizeof(void *) * num_descs * batch_size, GFP_KERNEL);
	if (!ctx->data_list) {
		ret = -ENOMEM;
		goto err_alloc_data_list;
	}

	ctx->batch_descs =
		kzalloc(sizeof(struct dsa_batch_desc) * num_descs, GFP_KERNEL);
	if (!ctx->batch_descs) {
		ret = -ENOMEM;
		SWARMIO_ERROR("failed to allocate batch descs\n");
		goto err_alloc_batch_descs;
	}

	for (i = 0; i < ctx->num_descs; i++) {
		/* alloc and prep batch desc */
		offset = batch_size * i;
		ctx->batch_descs[i].lv2_hw_descs =
			ctx->lv2_hw_desc_list + offset;
		ctx->batch_descs[i].lv2_compls = ctx->lv2_compl_list + offset;
		ctx->batch_descs[i].data = ctx->data_list + offset;
		ctx->batch_descs[i].num_filled = 0;
		ctx->batch_descs[i].id = i;

		ret = dsa_alloc_and_prep_batch_desc(
			&ctx->batch_descs[i], wq,
			ctx->lv2_hw_desc_list_dma +
				sizeof(struct dsa_hw_desc) * offset,
			ctx->lv2_compl_list_dma +
				sizeof(struct dsa_completion_record) * offset,
			batch_size);
		if (ret < 0) {
			SWARMIO_ERROR("failed to prep batch desc for %s\n",
				      wq->name);
			goto err_prep_batch_descs;
		}
	}

	return 0;

err_prep_batch_descs:
	i--;
	while (i >= 0) {
		if (ctx->batch_descs[i].idxd_desc) {
			idxd_free_desc(wq, ctx->batch_descs[i].idxd_desc);
			ctx->batch_descs[i].idxd_desc = NULL;
		}
		i--;
	}
	kfree(ctx->batch_descs);
	ctx->batch_descs = NULL;
err_alloc_batch_descs:
	kfree(ctx->data_list);
	ctx->data_list = NULL;
err_alloc_data_list:
	dsa_free_lv2_desc_list(idxd, ctx);

	return ret;
}

void dsa_dma_ctx_batch_remove(struct dsa_dma_ctx_batch *ctx)
{
	int i;
	struct idxd_device *idxd;
	struct idxd_wq *wq;

	if (!ctx)
		return;

	wq = ctx->wq;
	idxd = wq->idxd;

	for (i = 0; i < ctx->num_descs; i++) {
		if (ctx->batch_descs[i].idxd_desc)
			idxd_free_desc(wq, ctx->batch_descs[i].idxd_desc);
	}
	kfree(ctx->batch_descs);
	kfree(ctx->data_list);
	dsa_free_lv2_desc_list(idxd, ctx);
}

int dsa_dma_issue_async_batch(struct dsa_dma_ctx_batch *ctx, dma_addr_t dma_dst,
			      dma_addr_t dma_src, size_t size, void *data,
			      void (*callback)(void *))
{
	int ret = 0;
	int i, j;
	struct dsa_batch_desc *batch_desc;
	struct dsa_hw_desc *lv2_hw_desc;
	u64 now;
	s64 delta_ns;

	i = ctx->curr_desc;
	if (unlikely(i >= ctx->num_descs)) {
		SWARMIO_DEBUG_RL("all batch descs are already in flight\n");
		ret = dsa_dma_wait_oldest_batch(ctx, callback);
		if (ret)
			return ret;
		i = find_first_zero_bit(ctx->issued_mask, ctx->num_descs);
		if (unlikely(i >= ctx->num_descs)) {
			SWARMIO_ERROR("no free batch desc after completion\n");
			return -EBUSY;
		}
		ctx->curr_desc = i;
	}
	batch_desc = &ctx->batch_descs[i];

	j = batch_desc->num_filled;
	if (unlikely(j >= ctx->batch_size)) {
		SWARMIO_ERROR("batch desc %d is already full\n", i);
		return -EBUSY;
	}
	lv2_hw_desc = &batch_desc->lv2_hw_descs[j];

	/* populate lv2 desc */
	lv2_hw_desc->opcode = DSA_OPCODE_MEMMOVE;
	lv2_hw_desc->src_addr = dma_src;
	lv2_hw_desc->dst_addr = dma_dst;
	lv2_hw_desc->xfer_size = size;
	batch_desc->lv2_compls[j].status = DSA_COMP_NONE;

	batch_desc->data[j] = data;
	batch_desc->num_filled++;
	ctx->curr_num_filled = batch_desc->num_filled;
	now = local_clock();
	if (ctx->curr_num_filled == 1) {
		if (ctx->start_issue_time != 0) {
			delta_ns = (s64)(now - ctx->start_issue_time);
			ctx->batch_issue_timeout_ns =
				clamp(delta_ns, 0LL,
				      DSA_BATCH_MAX_SUBMIT_TIMEOUT_US * 1000);
		}
		ctx->start_issue_time = now;

		SWARMIO_DEBUG_RL("issue_timeout: %llu\n",
				 ctx->batch_issue_timeout_ns);
	}

	if ((j + 1) < ctx->batch_size)
		return 0;

	/* this batch is full */
	batch_desc->idxd_desc->hw->desc_count = ctx->batch_size;
	batch_desc->idxd_desc->completion->status = DSA_COMP_NONE;

	/* issue batch desc */
	ret = idxd_submit_desc(ctx->wq, batch_desc->idxd_desc);
	if (ret < 0) {
		SWARMIO_ERROR("failed to issue batch desc (size: %d): %d",
			      ctx->batch_size, ret);
		return ret;
	}
	list_add_tail(&batch_desc->node, &ctx->issued_descs);
	set_bit(i, ctx->issued_mask);

	ctx->curr_desc = find_first_zero_bit(ctx->issued_mask, ctx->num_descs);

	return 0;
}

int dsa_dma_issue_async_remaining_batch(struct dsa_dma_ctx_batch *ctx)
{
	int ret = 0;
	int i;
	int num_remaining;
	struct dsa_batch_desc *batch_desc;
	struct dsa_hw_desc *lv2_hw_desc;

	i = ctx->curr_desc;
	if (i >= ctx->num_descs) /* all batches already issued */
		return 0;

	batch_desc = &ctx->batch_descs[i];
	num_remaining = batch_desc->num_filled;
	if (num_remaining == 0) /* no remaining lv2 desc in next batch */
		return 0;

	if (num_remaining ==
	    1) { /* need to pad NOOP if only 1 desc is filled */
		SWARMIO_DEBUG("padding NOOP to batch desc %d\n", i);
		lv2_hw_desc = &batch_desc->lv2_hw_descs[1];

		/* populate lv2 desc */
		lv2_hw_desc->opcode = DSA_OPCODE_NOOP;
		lv2_hw_desc->src_addr = 0;
		lv2_hw_desc->dst_addr = 0;
		lv2_hw_desc->xfer_size = 0;
		batch_desc->lv2_compls[1].status = DSA_COMP_SUCCESS;
		batch_desc->data[1] = NULL;

		batch_desc->num_filled++;
		num_remaining = 2;
	}

	/* adjust lv2 desc count in batch desc */
	batch_desc->idxd_desc->hw->desc_count = num_remaining;
	batch_desc->idxd_desc->completion->status = DSA_COMP_NONE;

	/* issue batch desc */
	ret = idxd_submit_desc(ctx->wq, batch_desc->idxd_desc);
	if (ret < 0) {
		SWARMIO_ERROR("failed to issue batch desc (size: %d): %d",
			      num_remaining, ret);
		return ret;
	}
	list_add_tail(&batch_desc->node, &ctx->issued_descs);
	set_bit(i, ctx->issued_mask);

	ctx->curr_desc = find_first_zero_bit(ctx->issued_mask, ctx->num_descs);
	ctx->curr_num_filled = 0;

	return 0;
}

int dsa_dma_wait_oldest_batch(struct dsa_dma_ctx_batch *ctx,
			      void (*callback)(void *))
{
	int j;
	struct dsa_batch_desc *batch_desc;
	u64 now;
	s64 delta_ns;

	if (list_empty(&ctx->issued_descs))
		return 0;

	batch_desc = list_first_entry(&ctx->issued_descs, struct dsa_batch_desc,
				      node);

	/* wait for oldest issued batch */
	__dsa_sync_wait(batch_desc->idxd_desc, ctx->timeout);
	clear_bit(batch_desc->id, ctx->issued_mask);
	list_del(&batch_desc->node);
	if (batch_desc->idxd_desc->completion->status != DSA_COMP_SUCCESS) {
		SWARMIO_ERROR_RL("DMA polling failed!: %d\n",
				 batch_desc->idxd_desc->completion->status);
		for (j = 0; j < batch_desc->num_filled; j++) {
			if (batch_desc->lv2_compls[j].status !=
			    DSA_COMP_SUCCESS)
				SWARMIO_ERROR_RL(
					"lv2_hw_descs[%d]: opcode: %d, status: %d\n",
					j, batch_desc->lv2_hw_descs[j].opcode,
					batch_desc->lv2_compls[j].status);
		}
		batch_desc->num_filled = 0;
		return -EIO;
	}

	/* invoke callback (e.g., writing cqe) */
	if (callback) {
		for (j = 0; j < batch_desc->num_filled; j++) {
			callback(batch_desc->data[j]);
			batch_desc->data[j] = NULL;
		}
	}
	batch_desc->num_filled = 0;

	now = local_clock();
	if (ctx->last_wait_time != 0) {
		delta_ns = (s64)(now - ctx->last_wait_time);
		ctx->batch_sync_timeout_ns =
			clamp(delta_ns * ctx->num_descs, 0LL,
			      DSA_BATCH_MAX_SYNC_TIMEOUT_US * 1000);
		SWARMIO_DEBUG_RL("batch_timeout: %llu\n",
				 ctx->batch_sync_timeout_ns);
	}

	ctx->last_wait_time = now;

	return 0;
}

int dsa_dma_wait_all_batch(struct dsa_dma_ctx_batch *ctx,
			   void (*callback)(void *))
{
	int i, j;
	struct dsa_batch_desc *batch_desc;
	u64 now;
	s64 delta_ns;

	/* wait for issued batches */
	for_each_set_bit(i, ctx->issued_mask, ctx->num_descs) {
		batch_desc = &ctx->batch_descs[i];

		/* poll completion for batch desc */
		__dsa_sync_wait(batch_desc->idxd_desc, ctx->timeout);
		clear_bit(i, ctx->issued_mask);
		list_del(&batch_desc->node);
		if (batch_desc->idxd_desc->completion->status !=
		    DSA_COMP_SUCCESS) {
			SWARMIO_ERROR_RL(
				"DMA polling failed!: %d\n",
				batch_desc->idxd_desc->completion->status);
			for (j = 0; j < batch_desc->num_filled; j++) {
				if (batch_desc->lv2_compls[j].status !=
				    DSA_COMP_SUCCESS)
					SWARMIO_ERROR_RL(
						"lv2_hw_descs[%d]: opcode: %d, status: %d\n",
						j,
						batch_desc->lv2_hw_descs[j]
							.opcode,
						batch_desc->lv2_compls[j]
							.status);
			}
			batch_desc->num_filled = 0;
			return -EIO;
		}

		/* invoke callback (e.g., writing cqe) */
		if (callback) {
			for (j = 0; j < batch_desc->num_filled; j++) {
				callback(batch_desc->data[j]);
				batch_desc->data[j] = NULL;
			}
		}
		batch_desc->num_filled = 0;
	}

	now = local_clock();
	if (ctx->last_wait_time != 0) {
		delta_ns = (s64)(now - ctx->last_wait_time);
		ctx->batch_sync_timeout_ns =
			clamp(delta_ns * ctx->num_descs, 0LL,
			      DSA_BATCH_MAX_SYNC_TIMEOUT_US * 1000);
	}

	ctx->last_wait_time = now;

	return 0;
}

inline bool __dsa_dma_is_issue_timeout_batch(struct dsa_dma_ctx_batch *ctx)
{
	return (ctx->curr_num_filled > 0 &&
		local_clock() >
			ctx->start_issue_time + ctx->batch_issue_timeout_ns);
}

inline bool __dsa_dma_is_wait_timeout_batch(struct dsa_dma_ctx_batch *ctx)
{
	return (local_clock() >
		ctx->last_wait_time + ctx->batch_sync_timeout_ns);
}

inline bool __dsa_dma_all_busy_batch(struct dsa_dma_ctx_batch *ctx)
{
	return (ctx->curr_desc >= ctx->num_descs);
}
