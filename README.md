# SwarmIO 
SwarmIO is an IOPS-scalable NVMe SSD emulation framework built for end-to-end modeling of next-generation GPU-centric storage systems.

SwarmIO builds on top of [NVMeVirt](https://github.com/snu-csl/nvmevirt) and is implemented as a Linux kernel module that exposes a virtual PCIe/NVMe device to the host system.
It extends NVMeVirt's software architecture with a highly parallel design and integrates hardware-accelerated data movement through Intel Data Streaming Accelerator (DSA), enabling faithful modeling of high-IOPS GPU-centric storage systems.
SwarmIO sustains real-time performance of up to 40 MIOPS (million IOPS) on a single-socket Intel Xeon 6787P CPU node equipped with four DSA devices, while concurrently operating with a real NVIDIA H200 GPU.

For more details, please refer to our [paper](https://arxiv.org/abs/2604.06668).

⚠️ **(Stay tuned!)** We will soon provide end-to-end support for building and running GPU-centric storage I/O benchmarks with [BaM](https://github.com/ZaidQureshi/bam).
This support is intended to resolve build incompatibilities between the original BaM and the kernel versions recommended for SwarmIO.

## Requirements
### Hardware requirements

SwarmIO requires a platform equipped with an Intel CPU with integrated DSA support.

For requirements specific to GPU-centric storage system setup, please also refer to [BaM](https://github.com/ZaidQureshi/bam).

### System requirements

To instantiate a virtual SSD device, the host system should be configured as follows.

- Reserve sufficient system RAM for emulated storage space and NVMe
  registers.
- Reserve dedicated CPU cores for service units and workers.
- Disable IOMMU or configure it in pass-through mode to avoid DMA remapping overhead during storage I/O emulation.

Edit the GRUB command line:

```bash
$ sudo vi /etc/default/grub
```

If IOMMU can be disabled, use:

```bash
GRUB_CMDLINE_LINUX="intel_iommu=off memmap=124G$134G isolcpus=53-85"
```

If IOMMU must remain enabled, use pass-through mode instead:

```bash
GRUB_CMDLINE_LINUX="intel_iommu=on iommu=pt memmap=124G$134G isolcpus=53-85"
```

In these examples, `memmap=124G$134G` reserves 124 GiB of physical memory
starting at the 134 GiB offset (in host physical address space), and `isolcpus=53-85` isolates CPU cores
53 through 85 from the scheduler. Adjust both settings to match your testbed.
For more information about kernel command-line parameters, see
[the Linux kernel documentation](https://docs.kernel.org/admin-guide/kernel-parameters.html).

Update GRUB and reboot:

```bash
$ sudo update-grub
$ sudo reboot
```

### Build dependencies

To build SwarmIO with DSA support, you need access to the Linux kernel headers for the `idxd` driver,
such as `drivers/dma/idxd/idxd.h`.

- You can obtain them from the Linux source tree that matches your running kernel.
- SwarmIO currently supports DSA on Linux kernel 6.8 or newer due to
  symbol export requirements in the `idxd` driver.
- We tested a manually installed mainline Linux kernel 6.16.10 on Ubuntu 25.10,
  as well as the default Ubuntu kernel, version 6.8.0, on Ubuntu 24.04.
- Ubuntu 22.04, which ships with the Ubuntu 5.15 kernel by default, must be upgraded to a newer kernel version.

Follow the instructions that match how your current kernel was installed.
- If `uname -v` contains `Ubuntu`, follow the [Downloading Ubuntu kernel source](#1-downloading-ubuntu-kernel-source) method below.
- If you are running a manually installed mainline kernel, follow the [Downloading mainline kernel source](#2-downloading-mainline-kernel-source) method below.

#### 1. Downloading Ubuntu kernel source:

```bash
export KBASE="$(uname -r | sed -E 's/^([0-9]+\.[0-9]+\.[0-9]+).*/\1/')"

sudo apt update
sudo apt install -y linux-source

cd "/usr/src/linux-source-${KBASE}"

sudo tar -xjf "linux-source-${KBASE}.tar.bz2" \
  --strip-components=1 \
  --wildcards \
  "linux-source-${KBASE}/drivers/dma/idxd/*"
```

#### 2. Downloading mainline kernel source:

```bash
export KBASE="$(uname -r | sed -E 's/^([0-9]+\.[0-9]+\.[0-9]+).*/\1/')"
export KMAJOR="$(echo "$KBASE" | cut -d. -f1)"

cd /usr/src
sudo mkdir -p "linux-source-${KBASE}"

sudo wget "https://www.kernel.org/pub/linux/kernel/v${KMAJOR}.x/linux-${KBASE}.tar.xz"
sudo tar -xJf "linux-${KBASE}.tar.xz" \
  -C "linux-source-${KBASE}" \
  --strip-components=1 \
  --wildcards \
  "linux-${KBASE}/drivers/dma/idxd/*"
```

## Building SwarmIO

SwarmIO uses `*.conf` configuration files to manage build-time and runtime parameters. 
Example configuration files are provided under [`configs/`](configs/).

### Build-time configuration

Build-time parameters are read from the `[build]` section of the configuration
file (`*.conf`) and control how the kernel module is compiled.

1. `max_queues`: Maximum number of I/O queue pairs supported by the
   emulated device.
2. `max_fetch_sqes`: Maximum number of SQ entries fetched at a time by
   the dispatcher path when coalesced request fetching is enabled.
3. `worker_queue_size`: Size of each worker-local queue.
4. `coalesce_sqe`: Enables coalesced request fetching in the dispatcher
   path.
5. `prefetch_sqe`: Enables SQ entry prefetching across SQs within each
   service unit.
6. `skip_io`: Skips actual data movement for functional experiments and
   oracle settings.
7. `batch_worker_dma`: Enables batched DMA submission in workers.
8. `use_cont_qid`: Assigns queue identifiers to service units in
   contiguous ranges.
9. `assign_worker_rr`: Assigns work (i.e., I/O requests) to workers in
   round-robin order.
10. `use_committer`: Enables the committer thread path, which separates
    request completion handling from workers.
11. `use_agg_timing_update`: Enables aggregated timing-model updates.
12. `use_cas_timing_update`: Enables atomic CAS timing updates for
    scheduling state.
13. `profile_req`: Enables request latency breakdown profiling.
14. `use_disp_dma`: Compiles dispatcher-side DMA support.
15. `use_worker_dma`: Compiles worker-side DMA support.
16. `clean`: Runs `make clean` before building.

### Build script
Build SwarmIO with the provided helper script (`--clean` ensures a clean build):

```bash
$ scripts/build.sh -i configs/defconfig.conf --clean
```

You can also override selected build parameters from the configuration
file on the command line:

```bash
$ scripts/build.sh \
  -i configs/defconfig.conf \
  --max-queues 256 \
  --max-fetch-sqes 1024 \
  --worker-queue-size 4096 \
  --use-disp-dma \
  --use-worker-dma \
  --clean
```

## Loading SwarmIO

### Runtime configuration

Runtime parameters are read from the `[load]` section of the configuration file (`*.conf`)
and control how the emulated device is instantiated.

1. `num_sched_insts`: Number of parallel scheduling instances used by the timing
   model.
2. `block_size`: Emulated SSD block size in bytes.
3. `target_latency`: Target per-request latency in nanoseconds used to
   enforce the minimum latency.
4. `target_miops`: Target throughput in MIOPS used to derive scheduling
   delay.
5. `memmap_start`: Start address of the reserved host physical memory
   region used by SwarmIO.
6. `memmap_size`: Size of the reserved host physical memory region.
7. `num_service_units`: Number of service units. Each service unit
   consists of one dispatcher and its associated workers.
8. `num_workers_per_service_unit`: Number of workers assigned to each
   service unit.
9. `cpus`: Comma-separated list of CPU IDs and CPU ID ranges used for
   binding dispatcher and worker threads.
10. `use_disp_dma`: Enables dispatcher-side DMA.
11. `use_worker_dma`: Enables worker-side DMA.
12. `dsa_num_devs`: Number of DSA devices to use.
13. `dsa_num_grps_per_dev`: Number of DSA groups configured per device.
14. `dsa_num_wqs_per_grp`: Number of DSA WQs configured per
    DSA group.
15. `dsa_num_engs_per_grp`: Number of DSA engines configured per DSA
    group.
16. `dsa_wq_sizes`: Comma-separated list of DSA WQ sizes within each
    DSA group.
17. `dsa_wq_priorities`: Comma-separated list of DSA WQ priorities
    within each DSA group.
18. `dma_batch_size`: DMA batch size used by the worker DMA path.
19. `num_dma_descs_per_worker`: Number of DMA descriptors reserved for
    each worker (i.e., per-worker DSA WQ depth).
    
### Load script

Load SwarmIO with the provided helper script:

```bash
$ scripts/load.sh -i configs/defconfig.conf
```

You can override individual runtime parameters on the command line:

```bash
$ scripts/load.sh \
  --num-service-units 16 \
  --block-size 512 \
  --target-miops 40 \
```

After loading, the virtual NVMe SSD should be visible to the host
system. For example:

```bash
$ lsblk | grep -i nvme
nvme2n1     259:2    0   120G  0 disk

$ sudo dmesg
...
[1178582.807408] nvme nvme2: pci function 0001:10:00.0
[1178582.807576] SwarmIO: controller enabled
[1178582.811741] SwarmIO: swarmio_dispatcher_0 assigned sq [01-86, stride: 16]
[1178582.811751] SwarmIO: swarmio_dispatcher_1 assigned sq [02-86, stride: 16]
...
[1178582.811794] SwarmIO: swarmio_dispatcher_15 assigned sq [16-86, stride: 16]
[1178582.819517] nvme nvme2: 86/0/0 default/read/poll queues
[1178582.829290] SwarmIO: Virtual NVMe device created
```

To remove the emulated device, unload the module with:

```bash
$ scripts/load.sh --unload
```

## Usage
### Benchmarking CPU-centric storage systems
You can use [fio](https://github.com/axboe/fio) to run a simple
CPU-centric storage benchmark on SwarmIO. 
Our preprint reports results with an SPDK plugin built on top of this setup.

Example:

```bash
$ scripts/run_fio.sh \
  --input-config configs/swarmio.conf \
  --target-miops 5 \
  --num-service-units 16 \
  --num-threads 8 \
  --io-depths 32 \
  --build
```

However, host-side CPU IOPS can become a bottleneck when evaluating SwarmIO at high target IOPS.

### Benchmarking GPU-centric storage systems
🚧 Support for emulating GPU-centric storage systems using BaM will be *released soon*.

## Acknowledgment
This research is funded by the generous support from the following organizations:
- Institute of Information & Communications Technology Planning & Evaluation(IITP) grant funded by the Korea government (MSIT) (No.RS-2024-00438851, (SW Starlab) High-performance Privacy-preserving Machine Learning System and System Software) and (No.RS-2025-02264029, Implementation and Validation of an AI Semiconductor-Based Data Center Composable Cluster Infrastructure)
- SK hynix

We appreciate their commitment to advancing research in this field.

## Citation
```bibtex
@article{kim2026swarmio,
  title={SwarmIO: Towards 100 Million IOPS SSD Emulation for Next-generation GPU-centric Storage Systems},
  author={Kim, Hyeseong and Yeo, Gwangoo and Rhu, Minsoo},
  journal = {arXiv preprint arXiv:2604.06668},
  year={2026},
}
```
