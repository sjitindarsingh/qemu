#!/bin/bash

# CONFIG: set these to correspond to your HMP socket path (/tmp/mdroth-vm0-hmp.sock in this case)
vm_socket_dir=/tmp
vm_prefix=surajjs
vm_id="vm$1"

hmp() {
        path=/$vm_socket_dir/$vm_prefix-$1-hmp.sock
	shift
        echo "$@" | socat stdio unix-connect:$path
	return $?
}

wait_for_file() {
	file=$1
	expected_size=$2
	echo checking file $file expected size $expected_size
	size=0
	while [ $size -ne $expected_size ]; do
		size=$(stat --format=%s $save_dir/$file || echo 0)
		sleep 1
	done
}

guest_dirty_logging_clear_bitmap() {
	ram=$1

	state=1
	while [ $state -ne 0 ]; do
		hmp $vm_id dirty_logging_clear_bitmap $ram | grep dirty
		state=$?
		echo
		sleep 1
	done
}

guest_cont() {
	state=1
	while [ $state -ne 0 ]; do
		hmp $vm_id cont
		sleep 1
		hmp $vm_id info status | grep running
		state=$?
	done
}

guest_stop() {
	state=1
	while [ $state -ne 0 ]; do
		hmp $vm_id stop
		sleep 1
		hmp $vm_id info status | grep paused
		state=$?
	done
}

guest_pmemsave() {
	start=$1
	file_size=$2
	file_name=$3

	rm -f $save_dir/$file_name
	while [ ! -f $save_dir/$file_name ]; do
		hmp $vm_id pmemsave $start $file_size $file_name
		sleep 1
	done

	wait_for_file $file_name $file_size
}

guest_dirty_logging_save_bitmap() {
	ram=$1
	start=$2
	file_size=$3
	file_name=$4

	rm -f $save_dir/$file_name
	while [ ! -f $save_dir/$file_name ]; do
		hmp $vm_id dirty_logging_save_bitmap $ram $start $file_size $file_name
		sleep 1;
	done

	wait_for_file $file_name $(($file_size / 4096 / 8))
}

base_dir=/home/surajjs/qemu/dirty-log-testing
# compiled from $base_dir/dirty-bitmap-checker.c
check_bitmap=$base_dir/dirty-bitmap-checker

# CONFIG: match this with your QEMU command-line, use dirty_logging_list_ramblocks HMP command to list blocks for a running guest
ramblock=ppc_spapr.ram
# CONFIG: this should be the pwd for the QEMU process since that's where the block/bitmaps will be saved
save_dir=/home/surajjs

# by default we focus on testing just the first 2GB of memory
save_start=0
save_size=$3
save_end=$(($save_start + $save_size))
page_size=4096 # Hard coded in qemu
bmap_size=$(($save_size / $page_size / 8))
run_duration=$2
i=0

log() {
	echo
	echo "=> $@"
}

### Initial Setup ###

# Pause VM
log "Stopping VM"
guest_stop
# Enable Logging
log "enabling logging"
sleep 1
hmp $vm_id dirty_logging_enable
sleep 1
# Clear the dirty bitmap
log "clearing initial bitmap"
guest_dirty_logging_clear_bitmap $ramblock
# Save intial memory state
log "saving initial memory state (from $save_start to $save_end bytes)"
guest_pmemsave $save_start $save_size $vm_id.mem.a

### Work Loop ###
# Run the guest, save memory state and bitmap, compare with previous run, repeat
while true; do
	# Run Guest
	log "running guest for $run_duration seconds..."
	guest_cont
	sleep $run_duration
	guest_stop
	log "saving modified memory state (from $save_start to $save_end bytes)"
	# Save memory state
	guest_pmemsave $save_start $save_size $vm_id.mem.b
	# Save bitmap
	log "saving dirty bitmap (from $save_start to $save_end bytes)"
	guest_dirty_logging_save_bitmap $ramblock $save_start $save_size $vm_id.bmap
	# Compare Bitmap
	log "checking for inconsistencies..."
	sleep 1
	if ! $check_bitmap $save_dir/$vm_id.mem.a $save_dir/$vm_id.mem.b $save_dir/$vm_id.bmap $save_start $save_end; then
		echo "dirty bitmap inconsistency found, aborting and leaving guest paused"
		echo "bitmap inconsistency: $(date)" >/dev/kmsg
		exit 1
	fi
	# Prepare for next iteration
	guest_dirty_logging_clear_bitmap $ramblock
	cp $save_dir/$vm_id.mem.b $save_dir/$vm_id.mem.a
	sleep 1
done
hmp $vm_id dirty_logging_disable
