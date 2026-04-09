// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on code from https://github.com/snu-csl/nvmevirt
 * Copyright (C) 2026 VIA-Research, modified on 2026-04-07
 */

#include <linux/delay.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "common.h"
#include "committer.h"
#include "dispatcher.h"
#include "nvmev/nvme_vdev.h"
#include "proc.h"
#include "worker.h"

static int __proc_file_read(struct seq_file *m, void *data)
{
	const char *filename = m->private;
	struct swarmio_cfg *cfg = &vdev->config;

	if (strcmp(filename, "read_times") == 0) {
		seq_printf(m, "%u %u\n", cfg->read_sched_delay,
			   cfg->read_min_delay);
	} else if (strcmp(filename, "write_times") == 0) {
		seq_printf(m, "%u %u\n", cfg->write_sched_delay,
			   cfg->write_min_delay);
	} else if (strcmp(filename, "num_sched_insts") == 0) {
		seq_printf(m, "%u\n", cfg->num_sched_insts);
	} else if (strcmp(filename, "io_unit_shift") == 0) {
		seq_printf(m, "%u\n", cfg->io_unit_shift);
	} else if (strcmp(filename, "stat") == 0) {
		if (!vdev->is_threads_initialized) {
			seq_puts(m, "\n");
		} else {
			int i;
			struct swarmio_dispatcher *dispatcher;
			struct swarmio_worker *worker;
#ifdef CONFIG_SWARMIO_COMMIT
			struct swarmio_committer *committer;
#endif
#ifdef CONFIG_SWARMIO_PROFILE_REQ
			struct swarmio_stats t_target = { 0 };
			struct swarmio_stats t_dispatch = { 0 };
			struct swarmio_stats t_wait_issue = { 0 };
			struct swarmio_stats t_copy = { 0 };
			struct swarmio_stats t_wait_cpl = { 0 };
			struct swarmio_stats t_fill_cpl = { 0 };
			struct swarmio_stats error = { 0 };
#endif
#ifdef CONFIG_SWARMIO_PROFILE_REQ
			for (i = 0; i < cfg->num_service_units; i++) {
				dispatcher = &vdev->dispatchers[i];

				__set_stats_merge(&t_target,
						  &dispatcher->stats_target);
				__set_stats_merge(&t_dispatch,
						  &dispatcher->stats_dispatch);

				__reset_stats(&dispatcher->stats_target);
				__reset_stats(&dispatcher->stats_dispatch);
			}

			for (i = 0; i < cfg->num_workers; i++) {
				worker = &vdev->workers[i];

				__set_stats_merge(&t_wait_issue,
						  &worker->stats_wait_issue);
				__set_stats_merge(&t_copy, &worker->stats_copy);
				__reset_stats(&worker->stats_wait_issue);
				__reset_stats(&worker->stats_copy);
			}

#ifdef CONFIG_SWARMIO_COMMIT
			for (i = 0; i < cfg->num_service_units; i++) {
				committer = &vdev->committers[i];

				__set_stats_merge(&t_wait_cpl,
						  &committer->stats_wait_cpl);
				__set_stats_merge(&t_fill_cpl,
						  &committer->stats_fill_cpl);

				__reset_stats(&committer->stats_wait_cpl);
				__reset_stats(&committer->stats_fill_cpl);
			}
#else
			for (i = 0; i < cfg->num_workers; i++) {
				worker = &vdev->workers[i];

				__set_stats_merge(&t_wait_cpl,
						  &worker->stats_wait_cpl);
				__set_stats_merge(&t_fill_cpl,
						  &worker->stats_fill_cpl);
				__set_stats_merge(&error, &worker->stats_error);

				__reset_stats(&worker->stats_wait_cpl);
				__reset_stats(&worker->stats_fill_cpl);
				__reset_stats(&worker->stats_error);
			}
#endif
#endif
#ifdef CONFIG_SWARMIO_PROFILE_REQ
			seq_printf(m, "%llu %llu %llu %llu %llu %llu %llu\n",
				   __get_stats_avg(&t_target),
				   __get_stats_avg(&t_dispatch),
				   __get_stats_avg(&t_wait_issue),
				   __get_stats_avg(&t_copy),
				   __get_stats_avg(&t_wait_cpl),
				   __get_stats_avg(&t_fill_cpl),
				   __get_stats_avg(&error));
#endif
			mb();
		}
	}

	return 0;
}

static ssize_t __proc_file_write(struct file *file, const char __user *buf,
				 size_t len, loff_t *offp)
{
	ssize_t count = len;
	const char *filename = file->f_path.dentry->d_name.name;
	char input[128];
	struct sched_inst_stat_t *new_stat;
	unsigned int ret;
	struct sched_inst_stat_t *old_stat;
	struct swarmio_cfg *cfg = &vdev->config;
	size_t num_copied;

	num_copied = copy_from_user(input, buf, min(len, sizeof(input)));

	if (!strcmp(filename, "read_times")) {
		ret = sscanf(input, "%u %u", &cfg->read_sched_delay,
			     &cfg->read_min_delay);
	} else if (!strcmp(filename, "write_times")) {
		ret = sscanf(input, "%u %u", &cfg->write_sched_delay,
			     &cfg->write_min_delay);
	} else if (!strcmp(filename, "num_sched_insts")) {
		ret = sscanf(input, "%u", &cfg->num_sched_insts);
		if (ret != 1)
			goto out;

		old_stat = vdev->sched_inst_stat;
		new_stat = kzalloc(sizeof(*vdev->sched_inst_stat) *
					   cfg->num_sched_insts,
				   GFP_KERNEL);
		if (!new_stat) {
			SWARMIO_ERROR("Failed to resize sched_inst_stat\n");
			return -ENOMEM;
		}
		vdev->sched_inst_stat = new_stat;

		mdelay(100);
		kfree(old_stat);
	} else if (!strcmp(filename, "io_unit_shift")) {
		ret = sscanf(input, "%u", &cfg->io_unit_shift);
	} else if (!strcmp(filename, "stat")) {
		int i;
#ifdef CONFIG_SWARMIO_PROFILE_REQ
		for (i = 0; i < cfg->num_service_units; i++) {
			struct swarmio_dispatcher *dispatcher =
				&vdev->dispatchers[i];
			__reset_stats(&dispatcher->stats_target);
			__reset_stats(&dispatcher->stats_dispatch);
		}

		for (i = 0; i < cfg->num_workers; i++) {
			struct swarmio_worker *worker = &vdev->workers[i];
			__reset_stats(&worker->stats_wait_issue);
			__reset_stats(&worker->stats_copy);
		}

#ifdef CONFIG_SWARMIO_COMMIT
		for (i = 0; i < cfg->num_service_units; i++) {
			struct swarmio_committer *committer =
				&vdev->committers[i];
			__reset_stats(&committer->stats_wait_cpl);
			__reset_stats(&committer->stats_fill_cpl);
		}
#else
		for (i = 0; i < cfg->num_workers; i++) {
			struct swarmio_worker *worker = &vdev->workers[i];
			__reset_stats(&worker->stats_wait_cpl);
			__reset_stats(&worker->stats_fill_cpl);
			__reset_stats(&worker->stats_error);
		}
#endif
#endif
	}

out:
	return count;
}

static int __proc_file_open(struct inode *inode, struct file *file)
{
	return single_open(file, __proc_file_read,
			   (char *)file->f_path.dentry->d_name.name);
}

#if LINUX_VERSION_CODE > KERNEL_VERSION(5, 0, 0)
static const struct proc_ops proc_file_fops = {
	.proc_open = __proc_file_open,
	.proc_write = __proc_file_write,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};
#else
static const struct file_operations proc_file_fops = {
	.open = __proc_file_open,
	.write = __proc_file_write,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};
#endif

int swarmio_proc_init(struct nvme_vdev *vdev)
{
	vdev->proc_root = proc_mkdir("swarmio", NULL);
	if (!vdev->proc_root)
		return -ENOMEM;

	vdev->proc_read_times = proc_create("read_times", 0664, vdev->proc_root,
					    &proc_file_fops);
	vdev->proc_write_times = proc_create("write_times", 0664,
					     vdev->proc_root, &proc_file_fops);
	vdev->proc_num_sched_insts = proc_create(
		"num_sched_insts", 0664, vdev->proc_root, &proc_file_fops);
	vdev->proc_io_unit_shift = proc_create(
		"io_unit_shift", 0664, vdev->proc_root, &proc_file_fops);
	vdev->proc_stat =
		proc_create("stat", 0664, vdev->proc_root, &proc_file_fops);

	if (!vdev->proc_read_times || !vdev->proc_write_times ||
	    !vdev->proc_num_sched_insts || !vdev->proc_io_unit_shift ||
	    !vdev->proc_stat) {
		swarmio_proc_exit(vdev);
		return -ENOMEM;
	}

	return 0;
}

void swarmio_proc_exit(struct nvme_vdev *vdev)
{
	if (vdev->proc_read_times) {
		remove_proc_entry("read_times", vdev->proc_root);
		vdev->proc_read_times = NULL;
	}
	if (vdev->proc_write_times) {
		remove_proc_entry("write_times", vdev->proc_root);
		vdev->proc_write_times = NULL;
	}
	if (vdev->proc_num_sched_insts) {
		remove_proc_entry("num_sched_insts", vdev->proc_root);
		vdev->proc_num_sched_insts = NULL;
	}
	if (vdev->proc_io_unit_shift) {
		remove_proc_entry("io_unit_shift", vdev->proc_root);
		vdev->proc_io_unit_shift = NULL;
	}
	if (vdev->proc_stat) {
		remove_proc_entry("stat", vdev->proc_root);
		vdev->proc_stat = NULL;
	}

	if (vdev->proc_root) {
		remove_proc_entry("swarmio", NULL);
		vdev->proc_root = NULL;
	}
}
