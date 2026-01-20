#!/bin/bash
if [[ "$1" == "clear" ]]; then
    rm -f *.o  # -f 参数：强制删除，无文件时不报错
    rm -f *.iso  # -f 参数：强制删除，无文件时不报错
    rm -f *.bin  # -f 参数：强制删除，无文件时不报错
    exit 0     # 正常退出脚本，确保后续逻辑不执行
fi

# 1. 编译入口文件boot.cc，生成目标文件boot.o
g++ -m32 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -c boot.cc -o boot.o -fno-pic -fno-pie

# 2. 编译C语言内核文件kernel.c，生成目标文件kernel.o（裸机编译，无libc依赖）
g++ -m32 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -c kernel.cc -o kernel.o

# 3. 链接目标文件，生成ELF内核镜像myos.bin
ld -m elf_x86_64 -T linker.ld boot.o kernel.o -o myos.bin

# 4. 验证：检查myos.bin是否为有效ELF镜像（可选）
file myos.bin

# 1. 创建临时目录结构（GRUB2要求的目录格式）
mkdir -p iso/boot/grub

# 2. 将内核镜像myos.bin复制到iso/boot目录下
cp myos.bin iso/boot/myos.bin

# 3. 编写GRUB2配置文件（iso/boot/grub/grub.cfg）
cat > iso/boot/grub/grub.cfg << EOF
# GRUB2配置文件：定义启动菜单和内核路径
insmod video_bochs
insmod vbe
insmod video
insmod video_fb
insmod gfxterm

set gfxpayload=auto
set gfxterm=auto
terminal_output gfxterm  # 必须执行，切换到图形终端
set default=0
set timeout=5

menuentry "My Operating System (GRUB2)" {
#    insmod png            # （可选）支持PNG格式背景图片，如需美化可添加
#    insmod jpeg           # （可选）支持JPEG格式背景图片
    insmod multiboot2
    # 加载内核镜像：指定路径和Multiboot规范
    multiboot2 /boot/myos.bin
    boot
}
EOF

# 4. 制作ISO镜像（myos.iso），集成GRUB2引导程序
grub-mkrescue -o myos.iso iso

# 5. 清理临时目录（可选）
rm -rf iso 
