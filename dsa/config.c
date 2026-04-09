// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 VIA-Research
 */

#include "dsa.h"

struct dsa_engine_cfg dsa_engine_cfg_list[DSA_MAX_ENGINES] = {
	[0 ... DSA_MAX_ENGINES - 1] = { .group_id = -1 }
};

struct dsa_wq_cfg dsa_wq_cfg_list[DSA_MAX_WQS] = {
	[0 ... DSA_MAX_WQS - 1] = { .size = DSA_MIN_WQ_SIZE,
				    .priority = DSA_MIN_PRIORITY,
				    .group_id = -1 }
};

struct dsa_group_cfg dsa_group_cfg_list[DSA_MAX_GROUPS] = {
	[0 ... DSA_MAX_GROUPS -
	 1] = { .wq_ids = { [0 ... DSA_MAX_WQS - 1] = -1 },
		.engine_ids = { [0 ... DSA_MAX_ENGINES - 1] = -1 },
		.num_wqs = 0,
		.num_engines = 0 },
};

struct dsa_dev_cfg dsa_dev_cfg_list[DSA_MAX_DEVS] = {
	[0 ... DSA_MAX_DEVS - 1] = { .groups = dsa_group_cfg_list,
				     .num_groups = 0 },
};

unsigned int dsa_num_devs = 1;
unsigned int dsa_num_groups_per_dev = 4;
unsigned int dsa_num_wqs_per_group = 2;
unsigned int dsa_num_engines_per_group = 1;
