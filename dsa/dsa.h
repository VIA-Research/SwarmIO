// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 VIA-Research
 */

#ifndef __SWARMIO_DSA_H
#define __SWARMIO_DSA_H

#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/idxd.h>
#include <linux/bitmap.h>

#include <idxd.h>

#include "logging.h"

#define DSA_MAX_DEVS 4
#define DSA_SCAN_MAX_DEVS 16
#define DSA_MAX_WQS 16
#define DSA_MAX_ENGINES 4
#define DSA_MAX_GROUPS 4
#define DSA_MAX_BATCH_SIZE 32
#define DSA_MIN_WQ_SIZE 1
#define DSA_MAX_WQ_SIZE 256
#define DSA_MIN_PRIORITY 1
#define DSA_MAX_PRIORITY 15
#define DSA_MAX_NUM_DESCS_PER_THREAD 64

#define DSA_BATCH_MAX_SUBMIT_TIMEOUT_US 5LL
#define DSA_BATCH_MAX_SYNC_TIMEOUT_US 5LL

enum dsa_wq_alloc_mode {
	DSA_WQ_ALLOC_DEV_ORDERED = 0,
	DSA_WQ_ALLOC_DEV_INTERLEAVE,
};

struct dsa_cfg_params {
	unsigned int num_devs;
	unsigned int num_groups_per_dev;
	unsigned int num_wqs_per_group;
	unsigned int num_engines_per_group;
	const unsigned int *wq_sizes;
	const unsigned int *wq_priorities;
};

struct dsa_engine_cfg {
	int group_id;
};

struct dsa_wq_cfg {
	u32 size;
	u32 priority;
	int group_id;
};

struct dsa_group_cfg {
	int wq_ids[DSA_MAX_WQS];
	int engine_ids[DSA_MAX_ENGINES];
	int num_wqs;
	int num_engines;
};

struct dsa_dev_cfg {
	const struct dsa_group_cfg *groups;
	int num_groups;
};

struct dsa_dma_ctx_single {
	struct idxd_desc **single_descs;

	struct idxd_wq *wq;
	void **data;

	DECLARE_BITMAP(issued_mask, DSA_MAX_NUM_DESCS_PER_THREAD);

	unsigned long last_wait_time;
	int num_descs;
	int curr_desc;

	int dev_id;
	int wq_id;
	int timeout;
};

struct dsa_batch_desc {
	struct idxd_desc *idxd_desc;

	struct dsa_hw_desc *lv2_hw_descs;
	struct dsa_completion_record *lv2_compls;

	void **data;
	struct list_head node;
	int id;

	int num_filled;
};

struct dsa_dma_ctx_batch {
	struct dsa_batch_desc *batch_descs;
	struct dsa_hw_desc *lv2_hw_desc_list;
	struct dsa_completion_record *lv2_compl_list;
	dma_addr_t lv2_hw_desc_list_dma;
	dma_addr_t lv2_compl_list_dma;

	struct idxd_wq *wq;
	void **data_list;

	struct list_head issued_descs;
	DECLARE_BITMAP(issued_mask, DSA_MAX_NUM_DESCS_PER_THREAD);

	u64 start_issue_time;
	u64 last_wait_time;
	s64 batch_issue_timeout_ns;
	s64 batch_sync_timeout_ns;
	int batch_size;
	int num_descs;
	int curr_desc;
	int curr_num_filled;

	int dev_id;
	int wq_id;
	int timeout;
};

static inline void __dsa_prep_hw_desc(struct dsa_hw_desc *hw, char opcode,
				      u64 src_addr, u64 dst_addr, u64 xfer_size,
				      u64 compl_dma, u32 flags)
{
	hw->flags = flags;
	hw->opcode = opcode;
	hw->src_addr = src_addr;
	hw->dst_addr = dst_addr;
	hw->xfer_size = xfer_size;
	hw->priv = 0;
	hw->completion_addr = compl_dma;
}

static inline void __dsa_sync_wait(struct idxd_desc *desc, int timeout)
{
	unsigned long timeout_jiffies = jiffies + msecs_to_jiffies(timeout);

	while (desc->completion->status == DSA_COMP_NONE) {
		if (time_after(jiffies, timeout_jiffies)) {
			SWARMIO_WARN_RL("wq %d polling timed out!\n",
					desc->wq->id);
			break;
		}
		cpu_relax();
	}
}

int dsa_set_params(const struct dsa_cfg_params *p);
int dsa_enable_devices(void);
void dsa_disable_devices(void);
/* assign a dsa group to a service unit; 
 * allocate wqs in rr order within the dsa group */
void dsa_assign_wq_by_group_rr(int team_id, int local_id, int *dev_id,
			       int *wq_id);
/* assign dsa group to service unit; 
 * alloc wq0 dedicated to the leader (i.e., local_id = 0),
 * while remaining wqs are shared among other members */
void dsa_assign_wq_by_group_disagg(int team_id, int local_id, int *dev_id,
				   int *wq_id);

/* batch */
int dsa_dma_ctx_batch_init(struct dsa_dma_ctx_batch *ctx, int dev_id, int wq_id,
			   int batch_size, int num_descs, int timeout);
void dsa_dma_ctx_batch_remove(struct dsa_dma_ctx_batch *ctx);

int dsa_dma_issue_async_batch(struct dsa_dma_ctx_batch *ctx, dma_addr_t dma_dst,
			      dma_addr_t dma_src, size_t size, void *data,
			      void (*callback)(void *));
int dsa_dma_issue_async_remaining_batch(struct dsa_dma_ctx_batch *ctx);
int dsa_dma_wait_oldest_batch(struct dsa_dma_ctx_batch *ctx,
			      void (*callback)(void *));
int dsa_dma_wait_all_batch(struct dsa_dma_ctx_batch *ctx,
			   void (*callback)(void *));
inline bool __dsa_dma_is_issue_timeout_batch(struct dsa_dma_ctx_batch *ctx);
inline bool __dsa_dma_is_wait_timeout_batch(struct dsa_dma_ctx_batch *ctx);
inline bool __dsa_dma_all_busy_batch(struct dsa_dma_ctx_batch *ctx);

/* single */
int dsa_dma_ctx_single_init(struct dsa_dma_ctx_single *ctx, int dev_id,
			    int wq_id, int num_descs, int timeout);
void dsa_dma_ctx_single_remove(struct dsa_dma_ctx_single *ctx);

int dsa_dma_issue_async_single(struct dsa_dma_ctx_single *ctx,
			       dma_addr_t dma_dst, dma_addr_t dma_src,
			       size_t size, int *desc_id, void *data,
			       void (*callback)(void *));
int dsa_dma_issue_sync_single(struct dsa_dma_ctx_single *ctx,
			      dma_addr_t dma_dst, dma_addr_t dma_src,
			      size_t size, void *data,
			      void (*callback)(void *));
int dsa_dma_wait_one_single(struct dsa_dma_ctx_single *ctx, int desc_id,
			    void (*callback)(void *));
int dsa_dma_wait_all_single(struct dsa_dma_ctx_single *ctx,
			    void (*callback)(void *));
inline bool __dsa_dma_should_sync_single(struct dsa_dma_ctx_single *ctx);

#endif
