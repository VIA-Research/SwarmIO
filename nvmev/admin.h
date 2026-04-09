// SPDX-License-Identifier: GPL-2.0-only
/*
 * Based on code from https://github.com/snu-csl/nvmevirt
 * Copyright (C) 2026 VIA-Research, modified on 2026-04-07
 */

#ifndef _SWARMIO_ADMIN_H
#define _SWARMIO_ADMIN_H

struct nvme_submission_queue;
struct nvme_completion_queue;

void vdev_delete_sq(struct nvme_submission_queue *sq);
void vdev_delete_cq(struct nvme_completion_queue *cq);
void vdev_proc_admin_sq_entry(int entry_id);

#endif /* _SWARMIO_ADMIN_H */
