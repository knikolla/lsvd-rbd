#!/usr/bin/env bash

set -xeuo pipefail

if [ "$EUID" -ne 0 ]
  then echo "Please run as root"
  exit
fi

if [ -z "${1:-}" ]
  then echo "Please provide a pool name"
  exit
fi

# pool must already exist
pool_name=$1
cur_time=$(date +"%FT%T")

lsvd_dir=$(git rev-parse --show-toplevel)
gw_ip=$(ip addr | perl -lane 'print $1 if /inet (10\.1\.[0-9.]+)\/24/' | head -n 1)
client_ip=${client_ip:-10.1.0.6}
outfile=$lsvd_dir/experiments/results/$cur_time.rbd.$pool_name.txt

echo "Running gateway on $gw_ip, client on $client_ip"

imgname=test-cpu
imgsize=10G
blocksize=4096

source $lsvd_dir/tools/utils.bash

# Create the image
rbd -p $pool_name rm $imgname || true
rbd -p $pool_name create --size $imgsize --thick-provision $imgname

kill_nvmf
launch_gw_background
configure_nvmf_common $gw_ip
add_rbd_img $pool_name $imgname

wait

# run_client_bench $client_ip $outfile

cleanup_nvmf
rbd -p $pool_name rm $imgname || true
