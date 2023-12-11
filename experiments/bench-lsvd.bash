#!/usr/bin/env bash

set -xeuo pipefail

if [ -z "${1:-}" ]
  then echo "Please provide a pool name"
  exit
fi

# pool must already exist
pool_name=$1
cur_time=$(date +"%FT%T")

lsvd_dir=$(git rev-parse --show-toplevel)
gw_ip=$(ip addr | perl -lane 'print $1 if /inet (10.1.[0-9.]+)\/24/')
client_ip=${client_ip:-10.1.0.6}
rcache=/mnt/nvme/
wlog=/mnt/nvme-remote/
outfile=$lsvd_dir/experiments/results/$cur_time.lsvd.$pool_name.txt

echo "Running gateway on $gw_ip, client on $client_ip"

imgname=lsvd-benchmark
imgsize=80g
blocksize=4096

source $lsvd_dir/experiments/common.bash

# Build LSVD
echo '===Building LSVD...'
cd $lsvd_dir
make clean
make -j20 release

# create_lsvd_thick $pool_name $imgname $imgsize
#create_lsvd_thin $pool_name $imgname $imgsize
create_lsvd_thick $pool_name $imgname $imgsize

kill_nvmf

fstrim /mnt/nvme
launch_lsvd_gw_background $rcache $wlog $((240 * 1024 * 1024 * 1024))
configure_nvmf_common $gw_ip
add_rbd_img $pool_name $imgname
trap "cleanup_nvmf_rbd bdev_$imgname; cleanup_nvmf; exit" SIGINT SIGTERM EXIT

run_client_bench $client_ip $outfile client-bench.bash "read_entire_img=1"
