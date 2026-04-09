// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on code from https://github.com/snu-csl/nvmevirt
 * Copyright (C) 2026 VIA-Research, modified on 2026-04-07
 */

#include <linux/pci.h>
#include <linux/irq.h>
#include <linux/version.h>

#include <linux/sched/clock.h>

#include <asm/irq_remapping.h>

#include "common.h"
#include "dispatcher.h"
#include "nvme_vdev.h"
#include "queue.h"
#include "pci.h"

static void __signal_irq(const char *type, unsigned int irq)
{
	struct irq_data *data = irq_get_irq_data(irq);
	struct irq_chip *chip = irq_data_get_irq_chip(data);

	SWARMIO_DEBUG_RL("irq: %s %d, vector %d\n", type, irq,
			 irqd_cfg(data)->vector);
	BUG_ON(!chip->irq_retrigger);
	chip->irq_retrigger(data);

	return;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 17, 0)
static void __process_msi_irq(int msi_index)
{
	unsigned int virq = msi_get_virq(&vdev->pdev->dev, msi_index);

	BUG_ON(virq == 0);
	__signal_irq("msi", virq);
}
#else
static void __process_msi_irq(int msi_index)
{
	struct msi_desc *msi_desc, *tmp;

	for_each_msi_entry_safe(msi_desc, tmp, (&vdev->pdev->dev)) {
		if (msi_desc->msi_attrib.entry_nr == msi_index) {
			__signal_irq("msi", msi_desc->irq);
			return;
		}
	}
	SWARMIO_INFO("Failed to send IPI\n");
	BUG_ON(!msi_desc);
}
#endif

void vdev_signal_irq(int msi_index)
{
	if (vdev->pdev->msix_enabled) {
		__process_msi_irq(msi_index);
	} else {
		vdev->pcihdr->sts.is = 1;

		__signal_irq("int", vdev->pdev->irq);
	}
}

static int vdev_pci_read(struct pci_bus *bus, unsigned int devfn, int where,
			 int size, u32 *val)
{
	if (devfn != 0)
		return 1;

	memcpy(val, vdev->virtDev + where, size);

	SWARMIO_DEBUG_RL("[R] 0x%x, size: %d, val: 0x%x\n", where, size, *val);

	return 0;
};

static int vdev_pci_write(struct pci_bus *bus, unsigned int devfn, int where,
			  int size, u32 _val)
{
	u32 mask = ~(0U);
	u32 val = 0x00;
	int target = where;

	WARN_ON(size > sizeof(_val));

	memcpy(&val, vdev->virtDev + where, size);

	if (where < OFFS_PCI_PM_CAP) {
		// PCI_HDR
		if (target == PCI_COMMAND) {
			mask = PCI_COMMAND_INTX_DISABLE;
			if ((val ^ _val) & PCI_COMMAND_INTX_DISABLE) {
				vdev->intx_disabled =
					!!(_val & PCI_COMMAND_INTX_DISABLE);
				if (!vdev->intx_disabled) {
					vdev->pcihdr->sts.is = 0;
				}
			}
		} else if (target == PCI_STATUS) {
			mask = 0xF200;
		} else if (target == PCI_BIST) {
			mask = PCI_BIST_START;
		} else if (target == PCI_BASE_ADDRESS_0) {
			mask = 0xFFF00000;
		} else if (target == PCI_INTERRUPT_LINE) {
			mask = 0xFF;
		} else {
			mask = 0x0;
		}
	} else if (where < OFFS_PCI_MSIX_CAP) {
		// PCI_PM_CAP
	} else if (where < OFFS_PCIE_CAP) {
		// PCI_MSIX_CAP
		target -= OFFS_PCI_MSIX_CAP;
		if (target == PCI_MSIX_FLAGS) {
			mask = PCI_MSIX_FLAGS_MASKALL | /* 0x4000 */
			       PCI_MSIX_FLAGS_ENABLE; /* 0x8000 */

			if ((vdev->pdev) &&
			    ((val ^ _val) & PCI_MSIX_FLAGS_ENABLE)) {
				vdev->pdev->msix_enabled =
					!!(_val & PCI_MSIX_FLAGS_ENABLE);
			}
		} else {
			mask = 0x0;
		}
	} else if (where < OFFS_PCI_EXT_CAP) {
		// PCIE_CAP
	} else {
		// PCI_EXT_CAP
	}
	SWARMIO_DEBUG_RL(
		"[W] 0x%x, mask: 0x%x, val: 0x%x -> 0x%x, size: %d, new: 0x%x\n",
		where, mask, val, _val, size, (val & (~mask)) | (_val & mask));

	val = (val & (~mask)) | (_val & mask);
	memcpy(vdev->virtDev + where, &val, size);

	return 0;
};

static struct pci_ops vdev_pci_ops = {
	.read = vdev_pci_read,
	.write = vdev_pci_write,
};

static struct pci_sysdata vdev_pci_sysdata = {
	.domain = VDEV_PCI_DOMAIN_NUM,
	.node = 0,
};

static int __init_nvme_ctrl_regs(struct pci_dev *dev)
{
	struct nvme_ctrl_regs *bar = memremap(pci_resource_start(dev, 0),
					      PAGE_SIZE + NR_MAX_DBS_SIZE,
					      MEMREMAP_WT);
	if (!bar) {
		SWARMIO_ERROR("Failed to map NVMe control registers\n");
		return -ENOMEM;
	}

	vdev->bar = bar;
	memset(bar, 0x0, PAGE_SIZE + NR_MAX_DBS_SIZE);

	vdev->dbs = ((void *)bar) + PAGE_SIZE;

	*bar = (struct nvme_ctrl_regs) {
		.cap = {
			.to = 1,
			.mpsmin = 0,
			.mqes = NR_MAX_QUEUE_SIZE - 1, // 0-based value
			.cqr = 1,
		},
		.vs = {
			.mjr = 1,
			.mnr = 0,
		},
		};
	return 0;
}

static int __create_pci_bus(struct nvme_vdev *vdev)
{
	struct pci_bus *bus = NULL;
	struct pci_dev *dev;
	struct irq_domain *d;
	int ret;

	vdev_pci_sysdata.node =
		cpu_to_node(cpumask_first(vdev->config.cpu_mask));

	bus = pci_scan_bus(VDEV_PCI_BUS_NUM, &vdev_pci_ops, &vdev_pci_sysdata);

	if (!bus) {
		SWARMIO_ERROR("Unable to create PCI bus\n");
		return -ENODEV;
	}

	list_for_each_entry(dev, &bus->devices, bus_list) {
		struct resource *res = &dev->resource[0];
		res->parent = &iomem_resource;

		vdev->pdev = dev;
		dev->irq = vdev->pcihdr->intr.iline;

		d = dev_get_msi_domain(&dev->dev);
		if (!d) {
			d = dev_get_msi_domain(&dev->bus->dev);
			if (!d)
				d = arch_get_ir_parent_domain();
		}
		dev_set_msi_domain(&dev->dev, d);

		ret = __init_nvme_ctrl_regs(dev);
		if (ret)
			goto err_bus;

		vdev->old_dbs_orig =
			kzalloc(NR_MAX_DBS_SIZE + SMP_CACHE_BYTES, GFP_KERNEL);
		if (!vdev->old_dbs_orig) {
			SWARMIO_ERROR(
				"Failed to allocate old doorbell snapshot\n");
			ret = -ENOMEM;
			goto err_bus;
		}
		vdev->old_dbs = (u32 *)((void *)vdev->old_dbs_orig +
					(SMP_CACHE_BYTES - 2 * sizeof(u32)));
		memcpy(vdev->old_dbs, vdev->dbs, NR_MAX_DBS_SIZE);

		vdev->old_bar = kzalloc(PAGE_SIZE, GFP_KERNEL);
		if (!vdev->old_bar) {
			SWARMIO_ERROR("Failed to allocate shadow BAR\n");
			ret = -ENOMEM;
			goto err_bus;
		}
		memcpy(vdev->old_bar, vdev->bar, sizeof(*vdev->old_bar));

		vdev->msix_table = memremap(
			pci_resource_start(vdev->pdev, 0) + VDEV_PCI_MSIX_TO,
			VDEV_PCI_MSIX_TS * PCI_MSIX_ENTRY_SIZE, MEMREMAP_WT);
		if (!vdev->msix_table) {
			SWARMIO_ERROR("Failed to map MSI-X table\n");
			ret = -ENOMEM;
			goto err_bus;
		}
		memset(vdev->msix_table, 0x00,
		       VDEV_PCI_MSIX_TS * PCI_MSIX_ENTRY_SIZE);
	}

	vdev->virt_bus = bus;
	SWARMIO_INFO("Virtual PCI bus created (node %d)\n",
		     vdev_pci_sysdata.node);

	return 0;

err_bus:
	pci_stop_root_bus(bus);
	pci_remove_root_bus(bus);
	return ret;
}

static void PCI_HEADER_SETTINGS(struct pci_header *pcihdr,
				unsigned long base_pa)
{
	pcihdr->id.did = VDEV_PCI_DEV_ID;
	pcihdr->id.vid = VDEV_PCI_VENDOR_ID;
	/*
	pcihdr->cmd.id = 1;
	pcihdr->cmd.bme = 1;
	*/
	pcihdr->cmd.mse = 1;
	pcihdr->sts.cl = 1;

	pcihdr->htype.mfd = 0;
	pcihdr->htype.hl = PCI_HEADER_TYPE_NORMAL;

	pcihdr->rid = 0x01;

	pcihdr->cc.bcc = PCI_BASE_CLASS_STORAGE;
	pcihdr->cc.scc = 0x08;
	pcihdr->cc.pi = 0x02;

	pcihdr->mlbar.tp = PCI_BASE_ADDRESS_MEM_TYPE_64 >> 1;
	pcihdr->mlbar.ba = (base_pa & 0xFFFFFFFF) >> 14;

	pcihdr->mulbar = base_pa >> 32;

	pcihdr->ss.ssid = VDEV_PCI_SUBSYSTEM_ID;
	pcihdr->ss.ssvid = VDEV_PCI_SUBSYSTEM_VENDOR_ID;

	pcihdr->erom = 0x0;

	pcihdr->cap = OFFS_PCI_PM_CAP;

	pcihdr->intr.ipin = 0;
	pcihdr->intr.iline = VDEV_PCI_INTX_IRQ;
}

static void PCI_PMCAP_SETTINGS(struct pci_pm_cap *pmcap)
{
	pmcap->pid.cid = PCI_CAP_ID_PM;
	pmcap->pid.next = OFFS_PCI_MSIX_CAP;

	pmcap->pc.vs = 3;
	pmcap->pmcs.nsfrst = 1;
	pmcap->pmcs.ps = PCI_PM_CAP_PME_D0 >> 16;
}

static void PCI_MSIXCAP_SETTINGS(struct pci_msix_cap *msixcap)
{
	msixcap->mxid.cid = PCI_CAP_ID_MSIX;
	msixcap->mxid.next = OFFS_PCIE_CAP;

	msixcap->mxc.mxe = 1;
	msixcap->mxc.ts = VDEV_PCI_MSIX_TS - 1; // encoded as n-1

	msixcap->mtab.tbir = 0;
	msixcap->mtab.to = (VDEV_PCI_MSIX_TO >> 3);

	msixcap->mpba.pbao = (ALIGN((VDEV_PCI_MSIX_TO +
				     VDEV_PCI_MSIX_TS * PCI_MSIX_ENTRY_SIZE),
				    PAGE_SIZE) >>
			      3);
	msixcap->mpba.pbir = 0;
}

static void PCI_PCIECAP_SETTINGS(struct pcie_cap *pciecap)
{
	pciecap->pxid.cid = PCI_CAP_ID_EXP;
	pciecap->pxid.next = 0x0;

	pciecap->pxcap.ver = PCI_EXP_FLAGS;
	pciecap->pxcap.imn = 0;
	pciecap->pxcap.dpt = PCI_EXP_TYPE_ENDPOINT;

	pciecap->pxdcap.mps = 1;
	pciecap->pxdcap.pfs = 0;
	pciecap->pxdcap.etfs = 1;
	pciecap->pxdcap.l0sl = 6;
	pciecap->pxdcap.l1l = 2;
	pciecap->pxdcap.rer = 1;
	pciecap->pxdcap.csplv = 0;
	pciecap->pxdcap.cspls = 0;
	pciecap->pxdcap.flrc = 1;
}

static void PCI_EXTCAP_SETTINGS(struct pci_ext_cap *ext_cap)
{
	off_t offset = 0;
	void *ext_cap_base = ext_cap;

	/* AER */
	ext_cap->cid = PCI_EXT_CAP_ID_ERR;
	ext_cap->cver = 1;
	ext_cap->next = PCI_CFG_SPACE_SIZE + 0x50;

	ext_cap = ext_cap_base + 0x50;
	ext_cap->cid = PCI_EXT_CAP_ID_VC;
	ext_cap->cver = 1;
	ext_cap->next = PCI_CFG_SPACE_SIZE + 0x80;

	ext_cap = ext_cap_base + 0x80;
	ext_cap->cid = PCI_EXT_CAP_ID_PWR;
	ext_cap->cver = 1;
	ext_cap->next = PCI_CFG_SPACE_SIZE + 0x90;

	ext_cap = ext_cap_base + 0x90;
	ext_cap->cid = PCI_EXT_CAP_ID_ARI;
	ext_cap->cver = 1;
	ext_cap->next = PCI_CFG_SPACE_SIZE + 0x170;

	ext_cap = ext_cap_base + 0x170;
	ext_cap->cid = PCI_EXT_CAP_ID_DSN;
	ext_cap->cver = 1;
	ext_cap->next = PCI_CFG_SPACE_SIZE + 0x1a0;

	ext_cap = ext_cap_base + 0x1a0;
	ext_cap->cid = PCI_EXT_CAP_ID_SECPCI;
	ext_cap->cver = 1;
	ext_cap->next = 0;

	/*
	*(ext_cap + 1) = (struct pci_ext_cap) {
		.id = {
			.cid = 0xdead,
			.cver = 0xc,
			.next = 0xafe,
		},
	};

	PCI_CFG_SPACE_SIZE + ...;

	ext_cap = ext_cap + ...;
	ext_cap->id.cid = PCI_EXT_CAP_ID_DVSEC;
	ext_cap->id.cver = 1;
	ext_cap->id.next = 0;
	*/
}

int vdev_pci_init(struct nvme_vdev *vdev)
{
	PCI_HEADER_SETTINGS(vdev->pcihdr, vdev->config.memmap_start);
	PCI_PMCAP_SETTINGS(vdev->pmcap);
	PCI_MSIXCAP_SETTINGS(vdev->msixcap);
	PCI_PCIECAP_SETTINGS(vdev->pciecap);
	PCI_EXTCAP_SETTINGS(vdev->extcap);

	vdev->intx_disabled = false;

	return __create_pci_bus(vdev);
}
