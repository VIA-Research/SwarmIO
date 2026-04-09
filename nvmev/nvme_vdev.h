// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on code from https://github.com/snu-csl/nvmevirt
 * Copyright (C) 2026 VIA-Research, modified on 2026-04-07
 */

#ifndef _SWARMIO_NVME_VDEV_H
#define _SWARMIO_NVME_VDEV_H

#include "common.h"
#include "queue.h"

struct nvme_vdev {
	struct pci_bus *virt_bus;
	void *virtDev;
	struct pci_header *pcihdr;
	struct pci_pm_cap *pmcap;
	struct pci_msix_cap *msixcap;
	struct pcie_cap *pciecap;
	struct pci_ext_cap *extcap;

	struct pci_dev *pdev;

	struct swarmio_cfg config;

	void *storage_mapped;

	struct swarmio_dispatcher *dispatchers;
	struct swarmio_worker *workers;
	unsigned int num_service_units_initialized;
	unsigned int num_workers_initialized;
#ifdef CONFIG_SWARMIO_COMMIT
	struct swarmio_committer *committers;
	unsigned int num_committers_initialized;
#endif

	void __iomem *msix_table;

	bool intx_disabled;

	struct __nvme_bar *old_bar;
	struct nvme_ctrl_regs __iomem *bar;

	u32 *old_dbs_orig;
	u32 *old_dbs;
	u32 __iomem *dbs;

	struct swarmio_nvme_ns *ns;
	unsigned int num_ns;
	unsigned int num_sq;
	unsigned int num_cq;

	struct nvme_admin_queue *admin_q;
	struct nvme_submission_queue *sqes[NR_MAX_IO_QUEUE + 1];
	struct nvme_completion_queue *cqes[NR_MAX_IO_QUEUE + 1];

	unsigned int mdts;

	struct proc_dir_entry *proc_root;
	struct proc_dir_entry *proc_read_times;
	struct proc_dir_entry *proc_write_times;
	struct proc_dir_entry *proc_num_sched_insts;
	struct proc_dir_entry *proc_io_unit_shift;
	struct proc_dir_entry *proc_stat;

	struct sched_inst_stat_t *sched_inst_stat;
#ifndef CONFIG_SWARMIO_ATOMIC_CAS_TIMING_UPDATE
	spinlock_t sched_inst_stat_lock;
#endif

	wait_queue_head_t wait_queue;
	int stop_flag;
	atomic_t num_waiting;

	bool dsa_enabled;
	bool is_threads_initialized;
};

extern struct nvme_vdev *nvme_vdev;
extern struct nvme_vdev *vdev;

int vdev_init(struct swarmio_cfg *config);
void vdev_stop_pci(struct nvme_vdev *vdev);
void vdev_exit(struct nvme_vdev *vdev);

#endif /* _SWARMIO_NVME_VDEV_H */
