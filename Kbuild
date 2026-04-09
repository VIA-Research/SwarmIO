# SPDX-License-Identifier: GPL-2.0

# config parameters
NR_MAX_IO_QUEUE      ?= 64
NR_MAX_FETCH_SQE     ?= 1024
NR_WORKER_QUEUE_SIZE ?= 4096

# worker/dispatcher features
CONFIG_SWARMIO_WORKER_SKIP_IO       ?= n
CONFIG_SWARMIO_WORKER_BATCH_IO      ?= n
CONFIG_SWARMIO_DISP_PREFETCH_SQE    ?= n
CONFIG_SWARMIO_DISP_COALESCE_SQE    ?= n
CONFIG_SWARMIO_DISP_ASSIGN_QID_CONT ?= n
CONFIG_SWARMIO_DISP_ALLOC_WORKER_RR ?= n

CONFIG_SWARMIO_DMA_DISPATCHER       ?= y
CONFIG_SWARMIO_DMA_WORKER           ?= y

CONFIG_SWARMIO_AGGREGATED_TIMING_UPDATE      ?= n
CONFIG_SWARMIO_ATOMIC_CAS_TIMING_UPDATE   ?= n

# profiling
CONFIG_SWARMIO_PROFILE_REQ      ?= n

#committer
CONFIG_SWARMIO_COMMIT ?= n

# ---------------------------------------------------------

obj-m   := swarmio.o
swarmio-objs := src/main.o nvmev/nvme_vdev.o src/proc.o nvmev/pci.o nvmev/admin.o src/dispatcher.o \
			  src/worker.o dsa/config.o dsa/dsa.o dsa/batch.o dsa/single.o src/simple_timing_model.o
ccflags-y += -Wno-unused-variable -Wno-unused-function
ccflags-y += -I$(src) -I$(src)/src
ccflags-y += -I/usr/src/linux-source-$(shell uname -r | sed -E 's/^([0-9]+\.[0-9]+\.[0-9]+).*/\1/')/drivers/dma/idxd

ccflags-$(CONFIG_SWARMIO_PROFILE_REQ) += -DCONFIG_SWARMIO_PROFILE_REQ=1

ccflags-y += -DNR_MAX_IO_QUEUE=$(NR_MAX_IO_QUEUE)
ccflags-y += -DNR_MAX_FETCH_SQE=$(NR_MAX_FETCH_SQE)
ccflags-y += -DNR_WORKER_QUEUE_SIZE=$(NR_WORKER_QUEUE_SIZE)

ccflags-$(CONFIG_SWARMIO_WORKER_SKIP_IO) += -DCONFIG_SWARMIO_WORKER_SKIP_IO=1
ccflags-$(CONFIG_SWARMIO_WORKER_BATCH_IO) += -DCONFIG_SWARMIO_WORKER_BATCH_IO=1
ccflags-$(CONFIG_SWARMIO_DISP_PREFETCH_SQE) += -DCONFIG_SWARMIO_DISP_PREFETCH_SQE=1
ccflags-$(CONFIG_SWARMIO_DISP_COALESCE_SQE) += -DCONFIG_SWARMIO_DISP_COALESCE_SQE=1
ccflags-$(CONFIG_SWARMIO_DISP_ASSIGN_QID_CONT) += -DCONFIG_SWARMIO_DISP_ASSIGN_QID_CONT=1
ccflags-$(CONFIG_SWARMIO_DISP_ALLOC_WORKER_RR) += -DCONFIG_SWARMIO_DISP_ALLOC_WORKER_RR=1

ccflags-$(CONFIG_SWARMIO_DMA_DISPATCHER) += -DCONFIG_SWARMIO_DMA_DISPATCHER=1
ccflags-$(CONFIG_SWARMIO_DMA_WORKER) += -DCONFIG_SWARMIO_DMA_WORKER=1

ccflags-$(CONFIG_SWARMIO_AGGREGATED_TIMING_UPDATE) += -DCONFIG_SWARMIO_AGGREGATED_TIMING_UPDATE=1
ccflags-$(CONFIG_SWARMIO_ATOMIC_CAS_TIMING_UPDATE) += -DCONFIG_SWARMIO_ATOMIC_CAS_TIMING_UPDATE=1

ccflags-$(CONFIG_SWARMIO_COMMIT) += -DCONFIG_SWARMIO_COMMIT=1
swarmio-$(CONFIG_SWARMIO_COMMIT) += src/committer.o
