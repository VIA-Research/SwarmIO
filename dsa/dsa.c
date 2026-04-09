// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 VIA-Research
 */

#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/dma-mapping.h>
#include <linux/delay.h>
#include <linux/idxd.h>

#include <idxd.h>

#include "dsa.h"

// extern const struct bus_type dsa_bus_type;
extern struct dsa_engine_cfg dsa_engine_cfg_list[];
extern struct dsa_wq_cfg dsa_wq_cfg_list[];
extern struct dsa_group_cfg dsa_group_cfg_list[];
extern struct dsa_dev_cfg dsa_dev_cfg_list[];

struct idxd_device *dsa_idxd_list[DSA_MAX_DEVS] = { NULL };
extern unsigned int dsa_num_devs;
extern unsigned int dsa_num_groups_per_dev;
extern unsigned int dsa_num_wqs_per_group;
extern unsigned int dsa_num_engines_per_group;

static inline int __num_wqs_per_dev(void)
{
	return dsa_num_groups_per_dev * dsa_num_wqs_per_group;
}

static inline int __num_engines_per_dev(void)
{
	return dsa_num_groups_per_dev * dsa_num_engines_per_group;
}

static inline int __wq_id_to_group_id(int wq_id)
{
	if (unlikely(wq_id < 0 || wq_id >= __num_wqs_per_dev()))
		return -1;

	return dsa_wq_cfg_list[wq_id].group_id;
}

static inline int __engine_id_to_group_id(int engine_id)
{
	if (unlikely(engine_id < 0 || engine_id >= __num_engines_per_dev()))
		return -1;

	return dsa_engine_cfg_list[engine_id].group_id;
}

static struct idxd_device *__dev_id_to_idxd(int id)
{
	struct device *dev;
	struct idxd_dev *idxd_dev;
	char name[8];

	snprintf(name, sizeof(name), "dsa%d", id);

	dev = bus_find_device_by_name(&dsa_bus_type, NULL, name);
	if (!dev) {
		return NULL;
	}

	idxd_dev = confdev_to_idxd_dev(dev);
	put_device(dev);

	return idxd_dev_to_idxd(idxd_dev);
}

static int dsa_validate_params(const struct dsa_cfg_params *p)
{
	int i;
	int total_wqs = p->num_groups_per_dev * p->num_wqs_per_group;
	int total_engines = p->num_groups_per_dev * p->num_engines_per_group;
	struct idxd_device *idxd = __dev_id_to_idxd(0);

	if (p->num_devs > DSA_MAX_DEVS) {
		SWARMIO_ERROR("num_devs exceeds max limit\n");
		return -EINVAL;
	}

	if (idxd) {
		if (total_wqs > idxd->max_wqs) {
			SWARMIO_ERROR("num_wqs %d exceeds idxd limit %d\n",
				      total_wqs, idxd->max_wqs);
			return -EINVAL;
		}

		if (total_engines > idxd->max_engines) {
			SWARMIO_ERROR("num_engines %d exceeds idxd limit %d\n",
				      total_engines, idxd->max_engines);
			return -EINVAL;
		}
	} else {
		if (p->num_groups_per_dev > DSA_MAX_GROUPS) {
			SWARMIO_ERROR("num_groups exceeds max limit\n");
			return -EINVAL;
		}

		if (total_wqs > DSA_MAX_WQS) {
			SWARMIO_ERROR("num_wqs exceeds max limit\n");
			return -EINVAL;
		}

		if (total_engines > DSA_MAX_ENGINES) {
			SWARMIO_ERROR("num_engines exceeds max limit\n");
			return -EINVAL;
		}
	}

	for (i = 0; i < p->num_wqs_per_group; i++) {
		if (idxd) {
			if (p->wq_sizes[i] > idxd->max_wq_size) {
				SWARMIO_ERROR(
					"wq_sizes[%d] %u exceeds idxd limit %u\n",
					i, p->wq_sizes[i], idxd->max_wq_size);
				return -EINVAL;
			}
		} else if (p->wq_sizes[i] > DSA_MAX_WQ_SIZE) {
			SWARMIO_ERROR("wq_sizes[%d] %u is too large\n", i,
				      p->wq_sizes[i]);
			return -EINVAL;
		}
		if (p->wq_priorities[i] < DSA_MIN_PRIORITY ||
		    p->wq_priorities[i] > DSA_MAX_PRIORITY) {
			SWARMIO_ERROR(
				"wq_priorities[%d] %u is out of range (%d - %d)\n",
				i, p->wq_priorities[i], DSA_MIN_PRIORITY,
				DSA_MAX_PRIORITY);
			return -EINVAL;
		}
	}

	return 0;
}

int dsa_set_params(const struct dsa_cfg_params *p)
{
	int ret;
	int i, j, k;
	int wq_id, eng_id;

	ret = dsa_validate_params(p);

	SWARMIO_DEBUG("set params for %u devices\n", p->num_devs);

	for (i = 0; i < p->num_devs; i++) {
		struct dsa_dev_cfg *dev = &dsa_dev_cfg_list[i];

		dev->num_groups = p->num_groups_per_dev;
		wq_id = 0;
		eng_id = 0;

		if (i != 0)
			break;

		for (j = 0; j < dev->num_groups; j++) {
			struct dsa_group_cfg *grp = &dsa_group_cfg_list[j];

			grp->num_wqs = p->num_wqs_per_group;
			grp->num_engines = p->num_engines_per_group;

			for (k = 0; k < grp->num_wqs; k++) {
				struct dsa_wq_cfg *wq = &dsa_wq_cfg_list[wq_id];
				grp->wq_ids[k] = wq_id++;
				wq->size = p->wq_sizes[k];
				wq->priority = p->wq_priorities[k];
				wq->group_id = j;
			}

			for (k = 0; k < grp->num_engines; k++) {
				struct dsa_engine_cfg *eng =
					&dsa_engine_cfg_list[eng_id];
				grp->engine_ids[k] = eng_id++;
				eng->group_id = j;
			}
		}
	}

	dsa_num_devs = p->num_devs;
	dsa_num_groups_per_dev = p->num_groups_per_dev;
	dsa_num_wqs_per_group = p->num_wqs_per_group;
	dsa_num_engines_per_group = p->num_engines_per_group;

	return 0;
}

static int dsa_wqs_config(struct idxd_device *idxd)
{
	int i, j;
	struct idxd_wq *wq;
	int num_wqs = __num_wqs_per_dev();

	if (num_wqs <= 0)
		return -1;

	for (i = 0; i < dsa_num_groups_per_dev; i++) {
		idxd->groups[i]->rdbufs_allowed =
			idxd->max_rdbufs / dsa_num_groups_per_dev;
		idxd->groups[i]->rdbufs_reserved =
			idxd->max_rdbufs / dsa_num_groups_per_dev;
	}

	for (i = 0; i < num_wqs; i++) {
		wq = idxd->wqs[i];
		j = __wq_id_to_group_id(i);
		if (j < 0) {
			continue;
		}

		wq->size = dsa_wq_cfg_list[i].size;
		wq->priority = dsa_wq_cfg_list[i].priority;

		wq->type = IDXD_WQT_KERNEL;
		wq->threshold = 0;
		wq->group = idxd->groups[j];
		wq->flags = 0;
		set_bit(WQ_FLAG_DEDICATED, &wq->flags);
		memset(wq->name, 0, WQ_NAME_SIZE);
		snprintf(wq->name, WQ_NAME_SIZE, "wq%d.%d", idxd->id, i);
		wq->max_xfer_bytes = 256ULL << 10;
		wq->max_batch_size = DSA_MAX_BATCH_SIZE;
	}

	return 0;
}

static int dsa_engines_config(struct idxd_device *idxd)
{
	int i, j;
	struct idxd_engine *eng;
	int num_engines = __num_engines_per_dev();

	if (num_engines <= 0)
		return -1;

	for (i = 0; i < num_engines; i++) {
		eng = idxd->engines[i];
		j = __engine_id_to_group_id(i);
		if (j < 0) {
			continue;
		}

		eng->group = idxd->groups[j];
	}

	return 0;
}

static int dsa_wqs_enable(struct idxd_device *idxd)
{
	int i, rc;
	struct idxd_wq *wq;
	int num_enabled = 0;

	int num_wqs = __num_wqs_per_dev();
	if (num_wqs <= 0)
		return -EINVAL;

	for (i = 0; i < num_wqs; i++) {
		wq = idxd->wqs[i];
		mutex_lock(&wq->wq_lock);
		rc = idxd_drv_enable_wq(wq);
		mutex_unlock(&wq->wq_lock);
		if (rc) {
			SWARMIO_WARN("failed to enable %s: %d\n", wq->name, rc);
			continue;
		}
		SWARMIO_DEBUG("enabled wq %d\n", wq->id);
		num_enabled++;
	}

	if (num_enabled == 0) {
		return -ENODEV;
	}

	return 0;
}

static void dsa_wqs_disable(struct idxd_device *idxd)
{
	int i;
	struct idxd_wq *wq;

	int num_wqs = __num_wqs_per_dev();
	if (num_wqs <= 0)
		return;

	for (i = 0; i < num_wqs; i++) {
		wq = idxd->wqs[i];
		mutex_lock(&wq->wq_lock);
		__idxd_wq_quiesce(wq);
		idxd_drv_disable_wq(wq);
		SWARMIO_DEBUG("disabled wq %d\n", wq->id);
		mutex_unlock(&wq->wq_lock);
	}
}

int dsa_enable_devices(void)
{
	int i, rc;
	int dsa_id;
	int requested_devs = dsa_num_devs;
	int num_enabled_devs = 0;
	struct device *dev;
	struct idxd_device *idxd;

	for (dsa_id = 0;
	     dsa_id < DSA_SCAN_MAX_DEVS && num_enabled_devs < requested_devs;
	     dsa_id++) {
		idxd = __dev_id_to_idxd(dsa_id);
		if (!idxd) {
			continue;
		}

		/* config wqs and engines *before* enabling device */
		if (dsa_wqs_config(idxd) < 0)
			break;

		if (dsa_engines_config(idxd) < 0)
			break;

		dev = &idxd->idxd_dev.conf_dev;

		/* attach dsa device to idxd driver */
		if (!dev->driver) {
			rc = device_attach(dev);
			if (rc <= 0) {
				SWARMIO_WARN("failed to attch dsa%d: %d\n",
					     dsa_id, rc);
				break;
			}
		}
		if (!dev->driver) {
			SWARMIO_WARN("dsa%d is not attached to a driver\n",
				     dsa_id);
			rc = -ENODEV;
			break;
		}

		/* enable wqs */
		rc = dsa_wqs_enable(idxd);
		if (rc < 0) {
			SWARMIO_WARN("failed to enable dsa%d wqs: %d\n", dsa_id,
				     rc);
			device_release_driver(dev);
			break;
		}

		dsa_idxd_list[num_enabled_devs] = idxd;
		num_enabled_devs++;
	}

	if (num_enabled_devs == 0) {
		dsa_num_devs = 0;
		return -ENODEV;
	} else if (num_enabled_devs < requested_devs) {
		SWARMIO_ERROR("only %d dsa devices enabled (requested: %d)\n",
			      num_enabled_devs, requested_devs);
		dsa_num_devs = num_enabled_devs;
		dsa_disable_devices();
		return -ENODEV;
	}
	dsa_num_devs = num_enabled_devs;

	return 0;
}

void dsa_disable_devices(void)
{
	int i;
	struct device *dev;
	struct idxd_device *idxd;

	for (i = 0; i < dsa_num_devs; i++) {
		idxd = dsa_idxd_list[i];
		dev = &idxd->idxd_dev.conf_dev;

		dsa_wqs_disable(idxd);
		if (dev->driver)
			device_release_driver(dev);
		dsa_idxd_list[i] = NULL;
	}

	dsa_num_devs = 0;
}

void dsa_assign_wq_by_group_rr(int team_id, int local_id, int *dev_id,
			       int *wq_id)
{
	int grp_id, grp_wq_id;

	*dev_id = (team_id / dsa_num_groups_per_dev) % dsa_num_devs;

	grp_id = (team_id % dsa_num_groups_per_dev);

	grp_wq_id = local_id % dsa_num_wqs_per_group;

	*wq_id = dsa_dev_cfg_list[*dev_id].groups[grp_id].wq_ids[grp_wq_id];
}

void dsa_assign_wq_by_group_disagg(int team_id, int local_id, int *dev_id,
				   int *wq_id)
{
	int grp_id, grp_wq_id;

	*dev_id = (team_id / dsa_num_groups_per_dev) % dsa_num_devs;

	grp_id = (team_id % dsa_num_groups_per_dev);

	grp_wq_id = local_id == 0 ?
			    0 :
			    1 + (local_id - 1) % (dsa_num_wqs_per_group - 1);

	*wq_id = dsa_dev_cfg_list[*dev_id].groups[grp_id].wq_ids[grp_wq_id];
}
