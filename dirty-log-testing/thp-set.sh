#!/bin/bash

field=$1
value=$2

thp_base_path=/sys/kernel/mm/transparent_hugepage
#thp_path=$thp_base_path/khugepaged
thp_path=$thp_base_path

if [ ! -f $thp_path/$field ]; then
	thp_path=$thp_path/khugepaged
fi

if [ ! -f $thp_path/$field ]; then
	echo "field '$field' not found"
	exit 1
fi

old_value=$(cat $thp_path/$field)
echo $value >$thp_path/$field
new_value=$(cat $thp_path/$field)

echo "set $field to $new_value (was: $old_value)"
