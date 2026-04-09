// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 VIA-Research
 */

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/idxd.h>

#include <idxd.h>

#include "dsa.h"

extern struct idxd_device *dsa_idxd_list[DSA_MAX_DEVS];

int dsa_dma_ctx_single_init(struct dsa_dma_ctx_single *ctx, int dev_id,
			    int wq_id, int num_descs, int timeout)
{
	int i;
	int ret = 0;
	struct idxd_device *idxd = dsa_idxd_list[dev_id];
	struct idxd_wq *wq;

	if (!ctx || !idxd)
		return -ENODEV;

	if (wq_id >= idxd->max_wqs)
		return -EINVAL;
	wq = idxd->wqs[wq_id];

	bitmap_zero(ctx->issued_mask, DSA_MAX_NUM_DESCS_PER_THREAD);

	ctx->curr_desc = 0;
	ctx->num_descs = num_descs;
	ctx->last_wait_time = ULONG_MAX;
	ctx->dev_id = dev_id;
	ctx->wq_id = wq_id;
	ctx->wq = wq;
	ctx->timeout = timeout;

	/* alloc private data_list (e.g., callback params) */
	ctx->data = kzalloc(sizeof(void *) * num_descs, GFP_KERNEL);
	if (!ctx->data) {
		SWARMIO_ERROR("failed to allocate private data array\n");
		return -ENOMEM;
	}

	ctx->single_descs =
		kzalloc(sizeof(struct idxd_desc *) * num_descs, GFP_KERNEL);
	if (!ctx->single_descs) {
		ret = -ENOMEM;
		SWARMIO_ERROR("failed to allocate single descs\n");
		goto err_alloc_single_descs;
	}

	for (i = 0; i < ctx->num_descs; i++) {
		/* alloc and prep single desc */
		ctx->single_descs[i] = idxd_alloc_desc(wq, IDXD_OP_BLOCK);
		if (IS_ERR(ctx->single_descs[i])) {
			SWARMIO_ERROR("allocate desc for wq %d failed\n",
				      wq->id);
			return -ENODEV;
		}
		__dsa_prep_hw_desc(ctx->single_descs[i]->hw, DSA_OPCODE_MEMMOVE,
				   0, 0, 0, ctx->single_descs[i]->compl_dma,
				   IDXD_OP_FLAG_CRAV | IDXD_OP_FLAG_RCR);
		if (ret < 0) {
			SWARMIO_ERROR("failed to prep single desc for %s\n",
				      wq->name);
			goto err_prep_single_descs;
		}
	}

	return 0;

err_prep_single_descs:
	i--;
	while (i >= 0) {
		if (ctx->single_descs[i]) {
			idxd_free_desc(wq, ctx->single_descs[i]);
			ctx->single_descs[i] = NULL;
		}
		i--;
	}
	kfree(ctx->single_descs);
	ctx->single_descs = NULL;
err_alloc_single_descs:
	kfree(ctx->data);
	ctx->data = NULL;

	return ret;
}

void dsa_dma_ctx_single_remove(struct dsa_dma_ctx_single *ctx)
{
	int i;
	struct idxd_device *idxd;
	struct idxd_wq *wq;

	if (!ctx)
		return;

	wq = ctx->wq;
	idxd = wq->idxd;

	for (i = 0; i < ctx->num_descs; i++) {
		if (ctx->single_descs[i])
			idxd_free_desc(wq, ctx->single_descs[i]);
	}
	kfree(ctx->single_descs);
	kfree(ctx->data);
}

int dsa_dma_issue_async_single(struct dsa_dma_ctx_single *ctx,
			       dma_addr_t dma_dst, dma_addr_t dma_src,
			       size_t size, int *desc_id, void *data,
			       void (*callback)(void *))
{
	int ret = 0;
	int i;
	struct idxd_desc *single_desc;
	struct dsa_hw_desc *hw_desc;

	i = ctx->curr_desc;
	if (unlikely(i >= ctx->num_descs)) {
		SWARMIO_DEBUG_RL("all single descs are already in flight\n");
		ret = dsa_dma_wait_all_single(ctx, callback);
		if (unlikely(ret))
			return ret;
		i = 0;
		ctx->curr_desc = i;
	}
	if (desc_id != NULL)
		*desc_id = i;

	single_desc = ctx->single_descs[i];
	hw_desc = single_desc->hw;

	/* populate desc */
	// hw_desc->opcode = DSA_OPCODE_MEMMOVE;
	hw_desc->src_addr = dma_src;
	hw_desc->dst_addr = dma_dst;
	hw_desc->xfer_size = size;
	single_desc->completion->status = DSA_COMP_NONE;

	ctx->data[i] = data;

	/* issue single desc */
	ret = idxd_submit_desc(ctx->wq, single_desc);
	if (ret < 0) {
		SWARMIO_ERROR("failed to issue single desc: %d", ret);
		return ret;
	}
	set_bit(i, ctx->issued_mask);

	ctx->curr_desc = find_first_zero_bit(ctx->issued_mask, ctx->num_descs);

	return 0;
}

int dsa_dma_issue_sync_single(struct dsa_dma_ctx_single *ctx,
			      dma_addr_t dma_dst, dma_addr_t dma_src,
			      size_t size, void *data, void (*callback)(void *))
{
	int ret = 0;
	int desc_id;
	ret = dsa_dma_issue_async_single(ctx, dma_dst, dma_src, size, &desc_id,
					 data, callback);
	if (ret) {
		return ret;
	}
	ret = dsa_dma_wait_one_single(ctx, desc_id, callback);

	return ret;
}

int dsa_dma_wait_one_single(struct dsa_dma_ctx_single *ctx, int desc_id,
			    void (*callback)(void *))
{
	struct idxd_desc *single_desc;

	if (unlikely(desc_id < 0 || desc_id >= ctx->num_descs)) {
		SWARMIO_ERROR("invalid desc_id %d\n", desc_id);
		return -EINVAL;
	}

	if (!test_bit(desc_id, ctx->issued_mask)) {
		SWARMIO_ERROR("desc_id %d is not marked as issued\n", desc_id);
		return -EINVAL;
	}

	single_desc = ctx->single_descs[desc_id];

	/* poll completion for single desc */
	__dsa_sync_wait(single_desc, ctx->timeout);
	clear_bit(desc_id, ctx->issued_mask);
	if (single_desc->completion->status != DSA_COMP_SUCCESS) {
		SWARMIO_ERROR_RL("DMA polling failed!: %d\n",
				 single_desc->completion->status);
		ctx->last_wait_time = jiffies;
		return -EIO;
	}

	/* invoke callback (e.g., writing cqe) */
	if (callback) {
		callback(ctx->data[desc_id]);
	}

	ctx->curr_desc = desc_id;
	ctx->last_wait_time = jiffies;

	return 0;
}

int dsa_dma_wait_all_single(struct dsa_dma_ctx_single *ctx,
			    void (*callback)(void *))
{
	int i;
	struct idxd_desc *single_desc;

	/* wait for issued singles */
	for_each_set_bit(i, ctx->issued_mask, ctx->num_descs) {
		single_desc = ctx->single_descs[i];

		/* poll completion for single desc */
		__dsa_sync_wait(single_desc, ctx->timeout);
		clear_bit(i, ctx->issued_mask);
		if (single_desc->completion->status != DSA_COMP_SUCCESS) {
			SWARMIO_ERROR_RL("DMA polling failed!: %d\n",
					 single_desc->completion->status);
			ctx->last_wait_time = jiffies;
			return -EIO;
		}

		/* invoke callback (e.g., writing cqe) */
		if (callback) {
			callback(ctx->data[i]);
		}
	}

	ctx->curr_desc = 0;
	ctx->last_wait_time = jiffies;

	return 0;
}

inline bool __dsa_dma_should_sync_single(struct dsa_dma_ctx_single *ctx)
{
	unsigned long next_wait_time =
		ctx->last_wait_time + usecs_to_jiffies(5);

	return (ctx->curr_desc >= ctx->num_descs ||
		time_after(jiffies, next_wait_time));
}
