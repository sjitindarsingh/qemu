#!/bin/bash

# CONFIG: set these to correspond to your HMP socket path (/tmp/mdroth-vm0-hmp.sock in this case)
vm_socket_dir=/tmp
vm_prefix=surajjs
vm_id=${1:-vm0}

vm_idx() {
	echo $1 | sed 's/vm//'
}

hmp() {
        path=/$vm_socket_dir/$vm_prefix-$1-hmp.sock
	shift
        echo "$@" | socat stdio unix-connect:$path
	return $?
}

wait_for_unpause() {
	was_paused=0
	while [ -f /tmp/pause_dirty_check ]; do
		if [ $was_paused -eq 0 ]; then
			echo "migration loop paused via /tmp/pause_migration"
		fi
		was_paused=1
		sleep 30
	done
	if [ $was_paused -eq 1 ]; then
		echo "unpaused"
	fi
}

base_dir=/home/surajjs/qemu/dirty-log-testing
# compiled from $base_dir/dirty-bitmap-checker.c
check_bitmap=$base_dir/dirty-bitmap-checker

thp_set() {
	$base_dir/thp-set.sh $@
}

i=1

# CONFIG: match this with your QEMU command-line, use dirty_logging_list_ramblocks HMP command to list blocks for a running guest
ramblock=ppc_spapr.ram
# CONFIG: this should be the pwd for the QEMU process since that's where the block/bitmaps will be saved
save_dir=/home/surajjs
# CONFIG: set to where you have debugfs mounted
debug_dir=/sys/kernel/debug

# by default we focus on testing just the first 2GB of memory
save_start=0
save_end=2147483648
run_duration=5
i=0
save_size=0
thp_split_path=$debug_dir/split_huge_pages
# how many normal loops till we try forced THP splitting
thp_loops_till_split=2
# seconds between forced THP splits
thp_split_interval=60

thp_split() {
	if [ -e $thp_split_path ]; then
		echo 1 >$thp_split_path
	fi
}

log() {
	echo
	echo "=> $@"
}

#thp_set enabled never
#thp_set scan_sleep_millisecs 100000
thp_set enabled always
thp_set scan_sleep_millisecs 10000
expected_size=$(($save_end - $save_start))
hmp $vm_id cont
hmp $vm_id dirty_logging_disable
sleep 5
# Enable Logging
log "enabling logging"
hmp $vm_id dirty_logging_enable
sleep 5
# Clear the dirty bitmap
log "clearing initial bitmap"
hmp $vm_id stop
sleep 5
hmp $vm_id info status
sleep 1
hmp $vm_id dirty_logging_clear_bitmap $ramblock
# Save memory state
log "saving initial memory state (from $save_start to $save_end bytes)"
hmp $vm_id pmemsave $save_start $save_end $vm_id.mem.${i}a
save_size=0
while [ $save_size -ne $expected_size ]; do
	save_size=$(stat --format=%s $save_dir/$vm_id.mem.${i}a || echo 0)
	sleep 1
done
while true; do
	hmp $vm_id info status
	sleep 5
	log "running guest for $run_duration seconds..."
	hmp $vm_id cont
	hmp $vm_id info status
	sleep $run_duration
	hmp $vm_id stop
	hmp $vm_id info status
	log "saving modified memory state and dirty bitmap (from $save_start to $save_end bytes)"
	hmp $vm_id pmemsave $save_start $save_end $vm_id.mem.${i}b
	save_size=0
	while [ $save_size -ne $expected_size ]; do
		save_size=$(stat --format=%s $save_dir/$vm_id.mem.${i}b || echo 0)
		sleep 1
	done
	hmp $vm_id dirty_logging_save_bitmap $ramblock $save_start $save_end $vm_id.bmap.${i}
	#hmp $vm_id pmemsave 0 2147483648 $vm_id.mem.${i}c
	#TODO: if we have race here too try to match filesize like with guest mem
	sleep 10
	wait_for_unpause
	sleep 10
	while [ ! -f $save_dir/$vm_id.bmap.${i} ]; do
		sleep 1
	done
	md5_old=""
	md5_new=$(md5sum $save_dir/$vm_id.bmap.${i})
	while [ "$md5_new" != "$md5_old" ]; do
		sleep 1
		md5_old=$md5_new
		md5_new=$(md5sum $save_dir/$vm_id.bmap.${i})
	done
	log "checking for inconsistencies..."
	if ! $check_bitmap $save_dir/$vm_id.mem.${i}a $save_dir/$vm_id.mem.${i}b $save_dir/$vm_id.bmap.${i} $save_start $save_end; then
		echo "dirty bitmap inconsistency found, aborting and leaving guest paused"
		echo "bitmap inconsistency: $(date)" >/dev/kmsg
		exit 1
	fi
	#md5_old=""
	#md5_new=$(md5sum $vm_id.mem.${i}c)
	#while [ "$md5_new" != "$md5_old" ]; do
	#	sleep 1
	#	md5_old=$md5_new
	#	md5_new=$(md5sum $vm_id.mem.${i}c)
	#done
	#if ! cmp $vm_id.mem.${i}b $vm_id.mem.${i}c; then
	#	echo "memory changed after bitmap sync, aborting and leaving guest paused"
	#	exit 1
	#fi
	#rm $save_dir/$vm_id.mem.${i}a $save_dir/$vm_id.mem.${i}b $save_dir/$vm_id.mem.${i}c $save_dir/$vm_id.bmap.${i}
	next_i=$(($i + 1))
	rm $save_dir/$vm_id.mem.${i}a $save_dir/$vm_id.bmap.${i}
	mv $save_dir/$vm_id.mem.${i}b $save_dir/$vm_id.mem.${next_i}a
	# Clear the dirty bitmap
	log "clearing bitmap"
	hmp $vm_id info status
	sleep 1
	hmp $vm_id dirty_logging_clear_bitmap $ramblock
	i=$(($i + 1))
done
hmp $vm_id dirty_logging_disable
