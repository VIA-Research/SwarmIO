// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on code from https://github.com/snu-csl/nvmevirt
 * Copyright (C) 2026 VIA-Research, modified on 2026-04-07
 */

#ifndef _SWARMIO_SIMPLE_TIMING_MODEL_H
#define _SWARMIO_SIMPLE_TIMING_MODEL_H

#include "common.h"

struct ssd;

struct simple_timing_model {
	struct ssd *ssd;
};

void simple_init_namespace(struct swarmio_nvme_ns *ns, uint32_t id,
			   uint64_t size, void *mapped_addr);
void simple_remove_namespace(struct swarmio_nvme_ns *ns);

bool simple_sched_nvme_cmd(struct swarmio_nvme_ns *ns,
			   struct nvme_sched_info *sched_info);

#ifdef CONFIG_SWARMIO_AGGREGATED_TIMING_UPDATE
void simple_sched_nvme_cmd_aggregated(struct swarmio_nvme_ns *ns,
				      struct nvme_command *cmds,
				      unsigned long long nsecs_start,
#ifdef CONFIG_SWARMIO_PROFILE_REQ
				      unsigned long long *nsecs_starts,
#endif
				      unsigned long long *nsecs_targets,
				      int num_reqs,
				      struct swarmio_dispatcher *dispatcher);
#endif

#endif
