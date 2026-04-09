// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on code from https://github.com/snu-csl/nvmevirt
 * Copyright (C) 2026 VIA-Research, modified on 2026-04-07
 */

#ifndef _SWARMIO_LOGGING_H
#define _SWARMIO_LOGGING_H

#include <linux/kernel.h>

#define SWARMIO_DRV_NAME "SwarmIO"

#define SWARMIO_INFO(string, args...) \
	pr_info("%s: " string, SWARMIO_DRV_NAME, ##args)
#define SWARMIO_INFO_RL(string, args...) \
	pr_info_ratelimited("%s: " string, SWARMIO_DRV_NAME, ##args)
#define SWARMIO_WARN(string, args...) \
	pr_warn("%s: " string, SWARMIO_DRV_NAME, ##args)
#define SWARMIO_WARN_ONCE(string, args...) \
	pr_warn_once("%s: " string, SWARMIO_DRV_NAME, ##args)
#define SWARMIO_WARN_RL(string, args...) \
	pr_warn_ratelimited("%s: " string, SWARMIO_DRV_NAME, ##args)
#define SWARMIO_ERROR(string, args...) \
	pr_err("%s: " string, SWARMIO_DRV_NAME, ##args)
#define SWARMIO_ERROR_ONCE(string, args...) \
	pr_err_once("%s: " string, SWARMIO_DRV_NAME, ##args)
#define SWARMIO_ERROR_RL(string, args...) \
	pr_err_ratelimited("%s: " string, SWARMIO_DRV_NAME, ##args)
#define SWARMIO_DEBUG(string, args...) \
	pr_debug("%s: " string, SWARMIO_DRV_NAME, ##args)
#define SWARMIO_DEBUG_RL(string, args...) \
	pr_debug_ratelimited("%s: " string, SWARMIO_DRV_NAME, ##args)

#endif /* _SWARMIO_LOGGING_H */
