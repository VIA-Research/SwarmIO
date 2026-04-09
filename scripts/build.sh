#!/bin/bash

set -e

script_dir=$(dirname $(realpath $0))
cd "${script_dir}/.."

usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -i, --input-config FILE       Read options from config file"
    echo "  -q, --max-queues NUM          Max queues (default: 64)"
    echo "  -s, --max-fetch-sqes NUM      Max fetch SQEs (default: 1024)"
    echo "  --worker-queue-size NUM       Worker queue size (default: 4096)"
    echo "  --coalesce-sqe                Use coalesce SQE"
    echo "  --prefetch-sqe                Use prefetch SQE"
    echo "  --skip-io                     Skip I/O"
    echo "  --batch-worker-dma            Use batched worker DMA"
    echo "  --use-committer               Use committer"
    echo "  --use-agg-timing-update       Use batched timing update"
    echo "  --use-cas-timing-update       Use atomic CAS timing update"
    echo "  --profile-req                 Profile requests"
    echo "  --use-disp-dma                Build dispatcher DMA support"
    echo "  --use-worker-dma              Build worker DMA support"
    echo "  --clean                       Clean up before make"
    echo "  -h, --help                    Display this help message"
    exit 0
}

OPTIONS="i:q:s:h"
LONGOPTS="input-config:,max-queues:,max-fetch-sqes:,worker-queue-size:,\
coalesce-sqe,prefetch-sqe,skip-io,batch-worker-dma,use-cont-qid,assign-worker-rr,use-committer,use-agg-timing-update,use-cas-timing-update,\
profile-req,use-disp-dma,use-worker-dma,clean,help"

PARSED_ARGS=$(getopt -o $OPTIONS --long "$LONGOPTS" --name "$0" -- "$@")
if [[ $? -ne 0 ]]; then exit 1; fi
eval set -- "$PARSED_ARGS"

max_queues=64
max_fetch_sqes=1024
worker_queue_size=4096
coalesce_sqe="n"
prefetch_sqe="n"
skip_io="n"
batch_worker_dma="n"
use_cont_qid="n"
assign_worker_rr="n"
use_agg_timing_update="n"
use_cas_timing_update="n"
use_committer="n"
profile_req="n"
use_disp_dma="y"
use_worker_dma="y"
clean=0

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
            [[ "${BASH_REMATCH[1]}" == "build" ]] && section_match=true || section_match=false
            continue
        fi

        if $section_match; then
            case "$k" in
                max_queues)       max_queues="$v" ;;
                max_fetch_sqes)   max_fetch_sqes="$v" ;;
                worker_queue_size) worker_queue_size="$v" ;;
                coalesce_sqe)         [[ $v -eq 1 ]] && coalesce_sqe="y" || coalesce_sqe="n" ;;
                prefetch_sqe)         [[ $v -eq 1 ]] && prefetch_sqe="y" || prefetch_sqe="n" ;;
                skip_io)              [[ $v -eq 1 ]] && skip_io="y" || skip_io="n" ;;
                batch_worker_dma)     [[ $v -eq 1 ]] && batch_worker_dma="y" || batch_worker_dma="n" ;;
                use_cont_qid)         [[ $v -eq 1 ]] && use_cont_qid="y" || use_cont_qid="n" ;;
                assign_worker_rr)     [[ $v -eq 1 ]] && assign_worker_rr="y" || assign_worker_rr="n" ;;
                use_agg_timing_update) [[ $v -eq 1 ]] && use_agg_timing_update="y" || use_agg_timing_update="n" ;;
                use_cas_timing_update) [[ $v -eq 1 ]] && use_cas_timing_update="y" || use_cas_timing_update="n" ;;
                use_committer)        [[ $v -eq 1 ]] && use_committer="y" || use_committer="n" ;;
                profile_req)          [[ $v -eq 1 ]] && profile_req="y" || profile_req="n" ;;
                use_disp_dma)         [[ $v -eq 1 ]] && use_disp_dma="y" || use_disp_dma="n" ;;
                use_worker_dma)       [[ $v -eq 1 ]] && use_worker_dma="y" || use_worker_dma="n" ;;
                clean)            [[ $v -eq 1 ]] && clean=1 || clean=0 ;;
            esac
        fi
    done < "$config_file"
}


while true; do
    case "$1" in
        -i|--input-config)     parse_config "$2";    shift 2 ;;
        -q|--max-queues)       max_queues="$2";      shift 2 ;;
        -s|--max-fetch-sqes)   max_fetch_sqes="$2";  shift 2 ;;
        --worker-queue-size)   worker_queue_size="$2"; shift 2 ;;
        --coalesce-sqe)            coalesce_sqe="y";         shift ;;
        --prefetch-sqe)            prefetch_sqe="y";         shift ;;
        --skip-io)                 skip_io="y";              shift ;;
        --batch-worker-dma)        batch_worker_dma="y";     shift ;;
        --use-cont-qid)            use_cont_qid="y";         shift ;;
        --assign-worker-rr)        assign_worker_rr="y";     shift ;;
        --use-committer)           use_committer="y";        shift ;;
        --use-agg-timing-update)   use_agg_timing_update="y"; shift ;;
        --use-cas-timing-update)   use_cas_timing_update="y"; shift ;;
        --profile-req)             profile_req="y";          shift ;;
        --use-disp-dma)            use_disp_dma="y";         shift ;;
        --use-worker-dma)          use_worker_dma="y";       shift ;;
        --clean)               clean=1;              shift ;;
        -h|--help)             usage ;;
        --) shift; break ;;
        *) echo "Error: Internal error"; exit 1 ;;
    esac
done

if [[ $clean -eq 1 ]]; then
    sudo make clean
fi

# set -x

sudo make install -j$(nproc) \
    max_queues="$max_queues" \
    max_fetch_sqes="$max_fetch_sqes" \
    worker_queue_size="$worker_queue_size" \
    coalesce_sqe="$coalesce_sqe" \
    prefetch_sqe="$prefetch_sqe" \
    skip_io="$skip_io" \
    batch_worker_dma="$batch_worker_dma" \
    use_cont_qid="$use_cont_qid" \
    assign_worker_rr="$assign_worker_rr" \
    use_agg_timing_update="$use_agg_timing_update" \
    use_cas_timing_update="$use_cas_timing_update" \
    use_committer="$use_committer" \
    use_disp_dma="$use_disp_dma" \
    use_worker_dma="$use_worker_dma" \
    profile_req="$profile_req"
