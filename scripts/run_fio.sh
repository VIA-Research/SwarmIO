#!/bin/bash

set -e

script_dir=$(dirname $(realpath $0))
cd "${script_dir}/.."

############ CONFIG ############
VDEV_BDF="0001:10:00.0"
VDEV_DEV_NAME="nvme2n1"  
############ CONFIG ############

usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -i, --input-config FILE       Path to build/load config file (default: configs/defconfig.conf)"
    echo "  -j, --num-threads LIST        List of fio thread counts (e.g., 1,2,4,8)"
    echo "  -d, --io-depths LIST          List of I/O depths (e.g., 1,4,16,64)"
    echo "  -n, --num-service-units LIST  List of service unit counts (e.g., 1,2,4)"
    echo "  -l, --block-size NUM          I/O block size (default: 4096)"
    echo "      --target-miops NUM        Target MIOPS (default: 10)"
    echo "      --build                   Build SwarmIO module before running"
    echo "  -h, --help                    Display this help message"
    exit 1
}

cleanup() {
    if [[ -e "/sys/bus/pci/devices/${VDEV_BDF}" ]]; then
        sudo driverctl set-override "${VDEV_BDF}" nvme
        scripts/load.sh --unload || true
    fi
}

trap cleanup EXIT

OPTIONS="i:j:d:n:l:h"
LONGOPTS="input-config:,num-threads:,io-depths:,num-service-units:,block-size:,help,target-miops:,build"

PARSED_ARGS=$(getopt -o $OPTIONS --long "$LONGOPTS" --name "$0" -- "$@")
if [[ $? -ne 0 ]]; then exit 1; fi
eval set -- "$PARSED_ARGS"

FIO_BIN=$(which fio)
if [[ -z "${FIO_BIN}" ]]; then
    echo "Error: fio not found in PATH"
    exit 1
fi

num_threads_list=()
num_service_units_list=()
io_depth_list=()
block_size=4096
target_miops=10
config_file="configs/defconfig.conf"
build=0

while true; do
    case "$1" in
        -j|--num-threads)       IFS=',' read -r -a num_threads_list <<< "$2"; shift 2 ;;
        -d|--io-depths)         IFS=',' read -r -a io_depth_list <<< "$2"; shift 2 ;;
        -n|--num-service-units) IFS=',' read -r -a num_service_units_list <<< "$2"; shift 2 ;;
        -l|--block-size)        block_size="$2"; shift 2 ;;
        -i|--input-config)      config_file="$2"; shift 2 ;;
        --target-miops)         target_miops="$2"; shift 2 ;;
        --build)                build=1; shift ;;
        -h|--help)              usage ;;
        --) shift; break ;;
        *) echo "Error: Internal error"; exit 1 ;;
    esac
done

if [[ $build -eq 1 ]]; then
    echo "Building SwarmIO with config: ${config_file}"
    scripts/build.sh -i "$config_file"
fi

OUTDIR=results/fio/trace
mkdir -p $OUTDIR

type=$(basename $config_file | sed 's/\.[^.]*$//')
if grep -q CONFIG_SWARMIO_PROFILE_REQ src/.main.o.cmd; then
    show_breakdown=1
    csv_header="block_size,io_depth,num_cpu_threads,ssd_type,target(us),disp(us),wait_issue(us),copy(us),wait_cpl(us),fill_cpl(us),error(us),MIOPS,slat(us),clat(us)"
else
    show_breakdown=0
    csv_header="block_size,io_depth,num_cpu_threads,ssd_type,MIOPS,slat(us),clat(us)"
fi
echo $csv_header >> "${OUTDIR}/summary.csv"

for num_service_units in "${num_service_units_list[@]}"; do
    echo "Loading SwarmIO with $num_service_units service units"
    scripts/load.sh -i "$config_file" --num-service-units "$num_service_units" --block-size "$block_size" \
        --target-miops "$target_miops"

    sync && sleep 0.5
    cat /proc/swarmio/stat > /dev/null

    for io_depth in "${io_depth_list[@]}"; do
        if [[ "$io_depth" -eq 1 ]]; then
            engine="sync"
        else
            engine="libaio"
        fi
    for num_threads in "${num_threads_list[@]}"; do
        OUTFILE="${OUTDIR}/randread_l${block_size}_d${io_depth}_j${num_threads}_${type}_n${num_service_units}.log"

        echo "Running fio with D=$io_depth, J=$num_threads"
        sudo $FIO_BIN \
            --name=randread \
            --filename=/dev/$VDEV_DEV_NAME \
            --rw=randread \
            --random_distribution=random \
            --ioengine=$engine \
            --bs=$block_size \
            --size=32G \
            --iodepth=$io_depth \
            --direct=1 \
            --thread=1 \
            --runtime=15 \
            --time_based \
            --group_reporting \
            --numjobs=$num_threads > $OUTFILE || true

        # flush stats
        sync && sleep 0.5
        proc_stats=$(
            cat /proc/swarmio/stat | tail -n 1 |
            awk '
            {
                for (i = 1; i <= NF; i++)     {
                    printf "%.3f", $i / 1e3
                    if (i < NF)
                        printf ","
                }
                printf "\n"
            }'
        )

        perf_stats=$(
            awk '
            function to_usec(line) {
                unit = "";
                if (line ~ /\(nsec\)/) factor = 0.001;
                else if (line ~ /\(usec\)/) factor = 1;
                else if (line ~ /\(msec\)/) factor = 1000;
                else factor = 1;

                match(line, /avg=[0-9.]+/);
                avg = substr(line, RSTART+4, RLENGTH-4);
                return avg * factor;
            }

            /read: IOPS=/ {
                match($0, /IOPS=[0-9.]+[kKM]?/);
                s = substr($0, RSTART+5, RLENGTH-5);
                
                val = s;
                if (s ~ /k/) { sub(/k/, "", val); val *= 1000; }
                else if (s ~ /M/) { sub(/M/, "", val); val *= 1000000; }
                
                miops = val / 1000000;
            }

            /slat \(/ { slat = to_usec($0); }

            /clat \(/ { clat = to_usec($0); }

            END { printf "%.3f,%.3f,%.3f\n", miops, slat, clat }
            ' "$OUTFILE"
        )


        if [[ "$show_breakdown" -eq 1 ]]; then
            echo "${block_size},${io_depth},${num_threads},${type}_n${num_service_units},${proc_stats},${perf_stats}" >> "${OUTDIR}/summary.csv"
        else
            echo "${block_size},${io_depth},${num_threads},${type}_n${num_service_units},${perf_stats}" >> "${OUTDIR}/summary.csv"
        fi
    done
    done

    echo "Unloading SwarmIO"
    scripts/load.sh --unload
done
