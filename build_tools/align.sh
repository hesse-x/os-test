#!/bin/bash

if [ "$#" -lt 2 ]; then
    echo "need input and output file name"
    exit 1
fi


input_file=$1
output_file=$2
alignment=4096

# 计算对齐填充
file_size=$(stat -c%s "$input_file")
padding_size=$(( (alignment - (file_size % alignment)) % alignment ))

# 创建对齐后的文件
cp "$input_file" "$output_file"
dd if=/dev/zero bs=1 count="$padding_size" >> "$output_file"
