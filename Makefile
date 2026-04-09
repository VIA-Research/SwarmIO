# SPDX-License-Identifier: GPL-2.0

KERNELDIR := /lib/modules/$(shell uname -r)/build
PWD       := $(shell pwd)
MODDIR := /lib/modules/$(shell uname -r)/extra

max_queues ?= 64
max_fetch_sqes ?= 1024
worker_queue_size ?= 4096
coalesce_sqe ?= n
prefetch_sqe ?= n
skip_io ?= n
batch_worker_dma ?= n
use_cont_qid ?= n
assign_worker_rr ?= n
use_committer ?= n
profile_req ?= n
use_agg_timing_update ?= y
use_cas_timing_update ?= n
use_disp_dma ?= y
use_worker_dma ?= y

default:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) \
			NR_MAX_IO_QUEUE=$(max_queues) \
			NR_MAX_FETCH_SQE=$(max_fetch_sqes) \
			NR_WORKER_QUEUE_SIZE=$(worker_queue_size) \
			CONFIG_SWARMIO_WORKER_SKIP_IO=$(skip_io) \
			CONFIG_SWARMIO_WORKER_BATCH_IO=$(batch_worker_dma) \
			CONFIG_SWARMIO_DISP_PREFETCH_SQE=$(prefetch_sqe) \
			CONFIG_SWARMIO_DISP_COALESCE_SQE=$(coalesce_sqe) \
			CONFIG_SWARMIO_DISP_ASSIGN_QID_CONT=$(use_cont_qid) \
			CONFIG_SWARMIO_DISP_ALLOC_WORKER_RR=$(assign_worker_rr) \
			CONFIG_SWARMIO_DMA_DISPATCHER=$(use_disp_dma) \
			CONFIG_SWARMIO_DMA_WORKER=$(use_worker_dma) \
			CONFIG_SWARMIO_AGGREGATED_TIMING_UPDATE=$(use_agg_timing_update) \
			CONFIG_SWARMIO_ATOMIC_CAS_TIMING_UPDATE=$(use_cas_timing_update) \
			CONFIG_SWARMIO_COMMIT=$(use_committer) \
			CONFIG_SWARMIO_PROFILE_REQ=$(profile_req) \
			modules

install: default
	@mkdir -p $(MODDIR)
	@if [ ! -f $(MODDIR)/swarmio.ko ]; then \
		cp swarmio.ko $(MODDIR); \
		depmod -a; \
	fi

.PHONY: clean
clean:
	   $(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	   rm -f cscope.out tags swarmio.S
	   rm -f $(MODDIR)/swarmio.ko

.PHONY: cscope
cscope:
		cscope -b -R
		ctags *.[ch]

.PHONY: tags
tags: cscope

.PHONY: format
format:
	clang-format -i src/*.[ch] nvmev/*.[ch] dsa/*.[ch]

.PHONY: dis
dis:
	objdump -d -S swarmio.ko > swarmio.S
