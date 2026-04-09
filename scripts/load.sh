#!/bin/bash

set -e

script_dir=$(dirname $(realpath $0))
cd "${script_dir}/.."
module_path="${script_dir}/../swarmio.ko"

usage()     {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -i, --input-config FILE        Read options from config file"
    echo "  -L, --target-latency NUM       Target latency in nsec (default: 25000)"
    echo "  -T, --target-miops NUM         Target MIOPS (default: 2.5)"
    echo "  -l, --block-size NUM           I/O block size (default: 4096)"
    echo "  -m, --num-sched-insts NUM      Number of scheduling instances (default: 1)"
    echo "  -u, --unload                   Unload module"
    echo "  --memmap-start NUM             Reserved mem start address (default: 132G)"
    echo "  --memmap-size NUM              Reserved mem size (default: 124G)"
    echo "  --num-service-units NUM        Number of service units (default: 1)"
    echo "  --num-workers-per-service-unit NUM Number of workers in each service unit (default: 4)"
    echo "  --cpus NUM                     CPU list (default: 48-95)"
    echo "  --use-disp-dma                 Use dispatcher DMA"
    echo "  --use-worker-dma               Use worker DMA"
    echo "  --dsa-num-devs NUM             Number of DSA devices (default: 1)"
    echo "  --dsa-num-grps-per-dev NUM     Number of DSA groups per device (default: 4)"
    echo "  --dsa-num-wqs-per-grp NUM      Number of DSA WQs per group (default: 2)"
    echo "  --dsa-num-engs-per-grp NUM     Number of DSA engines per group (default: 1)"
    echo "  --dsa-wq-sizes LIST            DSA WQ sizes list (default: 16,16)"
    echo "  --dsa-wq-priorities LIST       DSA WQ priorities list (default: 1,1)"
    echo "  --dma-batch-size NUM           DMA batch size (default: 32)"
    echo "  --num-dma-descs-per-worker     NUM Number of DMA descriptors per worker (default: 2)"
    echo "  -h, --help                     Display this help message"
    exit 1
}

OPTIONS="i:L:T:l:m:uh"
LONGOPTS="input-config:,target-latency:,target-miops:,unload,\
memmap-start:,memmap-size:,num-service-units:,num-workers-per-service-unit:,cpus:,use-disp-dma,use-worker-dma,\
dsa-num-devs:,dsa-num-grps-per-dev:,dsa-num-wqs-per-grp:,dsa-num-engs-per-grp:,\
dsa-wq-sizes:,dsa-wq-priorities:,dma-batch-size:,num-dma-descs-per-worker:,help,\
block-size:,num-sched-insts:"

PARSED_ARGS=$(getopt -o $OPTIONS --long "$LONGOPTS" --name "$0" -- "$@")
if [[ $? -ne 0 ]]; then exit 1; fi
eval set -- "$PARSED_ARGS"

target_latency=25000
target_miops=2.5
memmap_start=132G
memmap_size=124G
block_size=4096
num_sched_insts=1
num_service_units=1
num_workers_per_service_unit=4
cpus="48-95"
use_disp_dma=0
use_worker_dma=0
dsa_num_devs=1
dsa_num_grps_per_dev=4
dsa_num_wqs_per_grp=2
dsa_num_engs_per_grp=1
dsa_wq_sizes="16,16"
dsa_wq_priorities="1,1"
dma_batch_size=32
num_dma_descs_per_worker=2
unload=0

parse_config() {
    local config_file=$1
    local section_match=false

    if [[ ! -f "$config_file" ]]; then
        echo "Error: '$config_file' not found." >&2
        exit 1
    fi

    echo "--> Loading config from $config_file"

    while IFS='=' read -r k v; do
        [[ -z "$k" || "$k" =~ ^# ]] && continue
        k=$(echo "$k" | xargs); v=$(echo "$v" | xargs)

        if [[ "$k" =~ ^\[(.*)\]$ ]]; then
            [[ "${BASH_REMATCH[1]}" == "load" ]] && section_match=true || section_match=false
            continue
        fi

        if $section_match; then
            case "$k" in
                target_latency)       target_latency="$v" ;;
                target_miops)         target_miops="$v" ;;
                block_size)           block_size="$v" ;;
                num_sched_insts)       num_sched_insts="$v" ;;
                memmap_start)         memmap_start="$v" ;;
                memmap_size)          memmap_size="$v" ;;
                num_service_units)       num_service_units="$v" ;;
                num_workers_per_service_unit) num_workers_per_service_unit="$v" ;;
                cpus)                 cpus="$v" ;;
                use_disp_dma)         [[ $v -eq 1 ]] && use_disp_dma=1 || use_disp_dma=0 ;;
                use_worker_dma)       [[ $v -eq 1 ]] && use_worker_dma=1 || use_worker_dma=0 ;;
                dsa_num_devs)          dsa_num_devs="$v" ;;
                dsa_num_grps_per_dev)  dsa_num_grps_per_dev="$v" ;;
                dsa_num_wqs_per_grp)   dsa_num_wqs_per_grp="$v" ;;
                dsa_num_engs_per_grp)  dsa_num_engs_per_grp="$v" ;;
                dsa_wq_sizes)         dsa_wq_sizes="$v" ;;
                dsa_wq_priorities)    dsa_wq_priorities="$v" ;;
                dma_batch_size)       dma_batch_size="$v" ;;
                num_dma_descs_per_worker) num_dma_descs_per_worker="$v" ;;
                unload)               [[ $v -eq 1 ]] && unload=1 || unload=0 ;;
            esac
        fi
    done < "$config_file"
}

while true; do
    case "$1" in
        -i|--input-config)        parse_config "$2"; shift 2 ;;
        -L|--target-latency)      target_latency="$2"; shift 2 ;;
        -l|--block-size)          block_size="$2"; shift 2 ;;
        -m|--num-sched-insts)      num_sched_insts="$2"; shift 2 ;;
        -T|--target-miops)        target_miops="$2"; shift 2 ;;
        -u|--unload)              unload=1; shift ;;
        --memmap-start)           memmap_start="$2"; shift 2 ;;
        --memmap-size)            memmap_size="$2"; shift 2 ;;
        --num-service-units)         num_service_units="$2"; shift 2 ;;
        --num-workers-per-service-unit) num_workers_per_service_unit="$2"; shift 2 ;;
        --cpus)                   cpus="$2"; shift 2 ;;
        --use-disp-dma)           use_disp_dma=1; shift ;;
        --use-worker-dma)         use_worker_dma=1; shift ;;
        --dsa-num-devs)           dsa_num_devs="$2"; shift 2 ;;
        --dsa-num-grps-per-dev)   dsa_num_grps_per_dev="$2"; shift 2 ;;
        --dsa-num-wqs-per-grp)    dsa_num_wqs_per_grp="$2"; shift 2 ;;
        --dsa-num-engs-per-grp)   dsa_num_engs_per_grp="$2"; shift 2 ;;
        --dsa-wq-sizes)           dsa_wq_sizes="$2"; shift 2 ;;
        --dsa-wq-priorities)      dsa_wq_priorities="$2"; shift 2 ;;
        --dma-batch-size)         dma_batch_size="$2"; shift 2 ;;
        --num-dma-descs-per-worker) num_dma_descs_per_worker="$2"; shift 2 ;;
        -h|--help)                usage ;;

        --) shift; break ;;
        *) echo "Error: Internal error"; exit 1 ;;
    esac
done

if [[ $unload -eq 1 ]]; then
    echo "Unloading swarmio..."
    sudo modprobe -r swarmio || true
    exit 0
fi

if (( num_sched_insts <= 0 || (num_sched_insts & (num_sched_insts - 1)) != 0 )); then
    echo "Error: num-sched-insts ($num_sched_insts) must be a power of 2." >&2
    exit 1
fi

read_sched_delay=$(awk "BEGIN {printf \"%d\", ((1000 / $target_miops * $num_sched_insts))}")
write_sched_delay=$read_sched_delay
read_min_delay=$(awk "BEGIN {printf \"%d\", $target_latency - $read_sched_delay}")
write_min_delay=$read_min_delay
io_unit_shift=$(echo "l($block_size)/l(2)" | bc -l | xargs printf "%.0f")

if ! modinfo swarmio > /dev/null 2>&1; then
    echo "Error: swarmio module is not registered in kernel. Make install first."
    exit 1
fi

set -x

sudo modprobe swarmio \
    memmap_start="$memmap_start" \
    memmap_size="$memmap_size" \
    num_sched_insts="$num_sched_insts" \
	io_unit_shift="$io_unit_shift" \
    read_min_delay="$read_min_delay" \
    write_min_delay="$write_min_delay" \
    read_sched_delay="$read_sched_delay" \
    write_sched_delay="$write_sched_delay" \
    num_service_units="$num_service_units" \
    num_workers_per_service_unit="$num_workers_per_service_unit" \
    cpus="$cpus" \
    disp_using_dma="$use_disp_dma" \
    worker_using_dma="$use_worker_dma" \
    dsa_num_devs="$dsa_num_devs" \
    dsa_num_grps_per_dev="$dsa_num_grps_per_dev" \
    dsa_num_wqs_per_grp="$dsa_num_wqs_per_grp" \
    dsa_num_engs_per_grp="$dsa_num_engs_per_grp" \
    dsa_wq_sizes="$dsa_wq_sizes" \
    dsa_wq_priorities="$dsa_wq_priorities" \
    num_dma_descs_per_worker="$num_dma_descs_per_worker" \
    dma_batch_size="$dma_batch_size"

set +x
