// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on code from https://github.com/snu-csl/nvmevirt
 * Copyright (C) 2026 VIA-Research, modified on 2026-04-07
 */

#include <linux/ktime.h>
#include <linux/sched/clock.h>
#include <linux/delay.h>

#include "simple_timing_model.h"
#include "dispatcher.h"
#include "nvmev/nvme_vdev.h"

/* derive the target completion time */
static unsigned long long __sched_io_units(int opcode, unsigned long lba,
					   unsigned int length,
					   unsigned long long nsecs_start)
{
	unsigned int io_unit_size = 1 << vdev->config.io_unit_shift;
	unsigned int unit_id =
		(lba >> (vdev->config.io_unit_shift - LBA_BITS)) %
		vdev->config.num_sched_insts;
	int num_sched_insts = min(vdev->config.num_sched_insts,
				  DIV_ROUND_UP(length, io_unit_size));

	unsigned long long io_unit_stime, io_unit_etime_old, io_unit_etime;
	unsigned long long latest = nsecs_start;
	unsigned int min_delay = 0;
	unsigned int sched_delay = 0;

	struct sched_inst_stat_t *sched_inst_stat = vdev->sched_inst_stat;

	if (opcode == nvme_cmd_write) {
		min_delay = vdev->config.write_min_delay;
		sched_delay = vdev->config.write_sched_delay;
	} else if (opcode == nvme_cmd_read) {
		min_delay = vdev->config.read_min_delay;
		sched_delay = vdev->config.read_sched_delay;
	}

#ifndef CONFIG_SWARMIO_WORKER_SKIP_IO
#ifdef CONFIG_SWARMIO_ATOMIC_CAS_TIMING_UPDATE
	do {
		int backoff_ns = 1;
		do {
			io_unit_etime_old = atomic64_read(
				&sched_inst_stat[unit_id].t_next_available);
			io_unit_stime = max(io_unit_etime_old, nsecs_start);
			io_unit_etime = io_unit_stime + sched_delay;

			if (atomic64_cmpxchg(
				    &sched_inst_stat[unit_id].t_next_available,
				    io_unit_etime_old,
				    io_unit_etime) == io_unit_etime_old)
				break;

			ndelay(backoff_ns);

			if (backoff_ns < 64)
				backoff_ns <<= 1;
		} while (1);

		length -= min(length, io_unit_size);
		if (++unit_id >= vdev->config.num_sched_insts)
			unit_id = 0;

		latest = max(latest, io_unit_etime);
	} while (length > 0);
#else
	spin_lock(&vdev->sched_inst_stat_lock);
	do {
		io_unit_etime_old = sched_inst_stat[unit_id].t_next_available;
		io_unit_stime = max(io_unit_etime_old, nsecs_start);
		io_unit_etime = io_unit_stime + sched_delay;
		sched_inst_stat[unit_id].t_next_available = io_unit_etime;

		length -= min(length, io_unit_size);
		if (++unit_id >= vdev->config.num_sched_insts)
			unit_id = 0;

		latest = max(latest, io_unit_etime);
	} while (length > 0);
	spin_unlock(&vdev->sched_inst_stat_lock);
#endif
#endif
	return latest + min_delay;
}

static unsigned long long __sched_flush(struct nvme_sched_info *sched_info)
{
	unsigned long long latest = 0;
	unsigned long long io_unit_etime_old;
	int i;

	struct sched_inst_stat_t *sched_inst_stat = vdev->sched_inst_stat;

#ifndef CONFIG_SWARMIO_ATOMIC_CAS_TIMING_UPDATE
	spin_lock(&vdev->sched_inst_stat_lock);
#endif
	for (i = 0; i < vdev->config.num_sched_insts; i++) {
#ifdef CONFIG_SWARMIO_ATOMIC_CAS_TIMING_UPDATE
		io_unit_etime_old =
			atomic64_read(&sched_inst_stat[i].t_next_available);
#else
		io_unit_etime_old = sched_inst_stat[i].t_next_available;
#endif
		latest = max(latest, io_unit_etime_old);
	}
#ifndef CONFIG_SWARMIO_ATOMIC_CAS_TIMING_UPDATE
	spin_unlock(&vdev->sched_inst_stat_lock);
#endif

	return latest;
}

bool simple_sched_nvme_cmd(struct swarmio_nvme_ns *ns,
			   struct nvme_sched_info *sched_info)
{
	struct nvme_command *cmd = sched_info->cmd;

	switch (cmd->common.opcode) {
	case nvme_cmd_write:
	case nvme_cmd_read:
		sched_info->nsecs_target = __sched_io_units(
			cmd->common.opcode, cmd->rw.slba,
			__cmd_io_size((struct nvme_rw_command *)cmd),
			sched_info->nsecs_start);
		break;
	case nvme_cmd_flush:
		sched_info->nsecs_target = __sched_flush(sched_info);
		break;
	default:
		SWARMIO_ERROR("command not implemented: I/O Cmd (0x%x)\n",
			      cmd->common.opcode);
		break;
	}

	return true;
}

#ifdef CONFIG_SWARMIO_AGGREGATED_TIMING_UPDATE
void simple_sched_nvme_cmd_aggregated(struct swarmio_nvme_ns *ns,
				      struct nvme_command *cmds,
				      unsigned long long nsecs_start,
#ifdef CONFIG_SWARMIO_PROFILE_REQ
				      unsigned long long *nsecs_starts,
#endif
				      unsigned long long *nsecs_targets,
				      int num_reqs,
				      struct swarmio_dispatcher *dispatcher)
{
	int i, j;
	unsigned int num_sched_insts = vdev->config.num_sched_insts;
	unsigned int io_unit_shift = vdev->config.io_unit_shift;
	unsigned int io_unit_size = 1 << io_unit_shift;
	unsigned long long *timing_workspace;
	unsigned long long *unit_sched_delay_sum;
	unsigned long long *unit_next_available_time;
	struct nvme_command *cmd;
	int opcode;
	unsigned int length, remaining, unit_id, sched_delay, min_delay;
	unsigned long lba;
	struct sched_inst_stat_t *sched_inst_stat = vdev->sched_inst_stat;
	unsigned long long latest, io_unit_stime, io_unit_etime,
		io_unit_etime_old;
	unsigned int num_sched_insts_mask = num_sched_insts - 1;

	timing_workspace = dispatcher->timing_workspace;
	memset(timing_workspace, 0,
	       num_sched_insts * 2 * sizeof(unsigned long long));
	unit_sched_delay_sum = timing_workspace;
	unit_next_available_time = timing_workspace + num_sched_insts;

#ifndef CONFIG_SWARMIO_WORKER_SKIP_IO
	/* Step 1: Accumulate total latency per I/O unit */
	for (i = 0; i < num_reqs; i++) {
		cmd = &cmds[i];
		opcode = cmd->common.opcode;

		if (likely(opcode == nvme_cmd_read ||
			   opcode == nvme_cmd_write)) {
			length = __cmd_io_size((struct nvme_rw_command *)cmd);
			lba = cmd->rw.slba;
			unit_id = (lba >> (io_unit_shift - LBA_BITS)) &
				  num_sched_insts_mask;
			sched_delay = (opcode == nvme_cmd_write) ?
					      vdev->config.write_sched_delay :
					      vdev->config.read_sched_delay;

			if (likely(length <= io_unit_size)) {
				unit_sched_delay_sum[unit_id] += sched_delay;
			} else {
				remaining = length;
				do {
					unit_sched_delay_sum[unit_id] +=
						sched_delay;
					remaining -=
						min(remaining, io_unit_size);
					if (++unit_id >= num_sched_insts)
						unit_id = 0;
				} while (remaining > 0);
			}
		}
	}

	/* Step 2: Atomic update ONCE per unit and store the BATCH END time */
#ifndef CONFIG_SWARMIO_ATOMIC_CAS_TIMING_UPDATE
	spin_lock(&vdev->sched_inst_stat_lock);
#endif
	nsecs_start = ktime_get_ns();
	for (i = 0; i < num_sched_insts; i++) {
		if (unit_sched_delay_sum[i] > 0) {
#ifdef CONFIG_SWARMIO_ATOMIC_CAS_TIMING_UPDATE
			do {
				io_unit_etime_old = atomic64_read(
					&sched_inst_stat[i].t_next_available);
				io_unit_stime =
					max(io_unit_etime_old, nsecs_start);
				io_unit_etime =
					io_unit_stime + unit_sched_delay_sum[i];

				if (atomic64_cmpxchg(
					    &sched_inst_stat[i].t_next_available,
					    io_unit_etime_old, io_unit_etime) ==
				    io_unit_etime_old) {
					break;
				}

				SWARMIO_DEBUG_RL(
					"atomic update failed for unit %d, retrying...\n",
					i);

				cpu_relax();
			} while (1);
#else
			io_unit_etime_old = sched_inst_stat[i].t_next_available;
			io_unit_stime = max(io_unit_etime_old, nsecs_start);
			io_unit_etime = io_unit_stime + unit_sched_delay_sum[i];
			sched_inst_stat[i].t_next_available = io_unit_etime;
#endif

			unit_next_available_time[i] = io_unit_stime;
		} else {
#ifdef CONFIG_SWARMIO_ATOMIC_CAS_TIMING_UPDATE
			unit_next_available_time[i] = atomic64_read(
				&sched_inst_stat[i].t_next_available);
#else
			unit_next_available_time[i] =
				sched_inst_stat[i].t_next_available;
#endif
		}
	}
#ifndef CONFIG_SWARMIO_ATOMIC_CAS_TIMING_UPDATE
	spin_unlock(&vdev->sched_inst_stat_lock);
#endif
#endif

	/* Step 3: Assign Target Times */
	for (i = 0; i < num_reqs; i++) {
		cmd = &cmds[i];
		opcode = cmd->common.opcode;

#ifdef CONFIG_SWARMIO_PROFILE_REQ
		nsecs_starts[i] = nsecs_start;
#endif
#ifndef CONFIG_SWARMIO_WORKER_SKIP_IO
		if (likely(opcode == nvme_cmd_read ||
			   opcode == nvme_cmd_write)) {
			length = __cmd_io_size((struct nvme_rw_command *)cmd);
			lba = cmd->rw.slba;
			unit_id = (lba >> (io_unit_shift - LBA_BITS)) &
				  num_sched_insts_mask;
			sched_delay = (opcode == nvme_cmd_write) ?
					      vdev->config.write_sched_delay :
					      vdev->config.read_sched_delay;
			min_delay = (opcode == nvme_cmd_write) ?
					    vdev->config.write_min_delay :
					    vdev->config.read_min_delay;

			if (likely(length <= io_unit_size)) {
				unit_next_available_time[unit_id] +=
					sched_delay;
				nsecs_targets[i] =
					unit_next_available_time[unit_id] +
					min_delay;
			} else {
				latest = 0;
				remaining = length;
				do {
					unit_next_available_time[unit_id] +=
						sched_delay;
					latest = max(latest,
						     unit_next_available_time
							     [unit_id]);
					remaining -=
						min(remaining, io_unit_size);
					if (++unit_id >= num_sched_insts)
						unit_id = 0;
				} while (remaining > 0);
				nsecs_targets[i] = latest + min_delay;
			}
		} else if (opcode == nvme_cmd_flush) {
			latest = 0;
			for (j = 0; j < num_sched_insts; j++) {
				latest = max(latest,
					     unit_next_available_time[j]);
			}
			nsecs_targets[i] = latest;
		} else {
			nsecs_targets[i] = nsecs_start;
		}
#else
		min_delay = (opcode == nvme_cmd_write) ?
				    vdev->config.write_min_delay :
				    vdev->config.read_min_delay;
		nsecs_targets[i] = nsecs_start + min_delay;
#endif
	}
}
#endif

void simple_init_namespace(struct swarmio_nvme_ns *ns, uint32_t id,
			   uint64_t size, void *mapped_addr)
{
	ns->id = id;
	ns->csi = NVME_CSI_NVM;
	ns->size = size;
	ns->mapped = mapped_addr;
	ns->sched_nvme_cmd = simple_sched_nvme_cmd;
#ifdef CONFIG_SWARMIO_AGGREGATED_TIMING_UPDATE
	ns->sched_nvme_cmd_aggregated = simple_sched_nvme_cmd_aggregated;
#endif

	return;
}

void simple_remove_namespace(struct swarmio_nvme_ns *ns)
{
	;
}
