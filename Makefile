C_SOURCES = $(wildcard os-test/kernel/*.c os-test/drivers/*.c os-test/cpu/*.c os-test/libc/*.c)
HEADERS = $(wildcard os-test/kernel/*.h os-test/drivers/*.h os-test/cpu/*.h os-test/libc/*.h)
# Nice syntax for file extension replacement
OBJ = ${C_SOURCES:.c=.o os-test/cpu/interrupt.o} 

# Change this if your cross-compiler is somewhere else
# CC=$(HOME)/llvm-project/build/bin/clang
# LD=$(HOME)/llvm-project/build/bin/ld.lld
CC=gcc
LD=ld
# -g: Use debugging symbols in gcc
CFLAGS = -g -m32 -fno-builtin -fno-stack-protector -nodefaultlibs -fno-pic -Wall -Wextra -Werror -mno-sse -I.

# First rule is run by default
os-image.bin: bootsect.bin kernel.bin
	cat $^ > os-image.bin

bootsect.o: os-test/boot/bootsect.s
	$(CC) $< -c -o $@

bootsect.bin: bootsect.o
	$(LD) -o $@ -Ttext 0x7c00 $^ --oformat binary

# '--oformat binary' deletes all symbols as a collateral, so we don't need
# to 'strip' them manually on this case
kernel.bin: os-test/boot/kernel_entry.o ${OBJ}
	$(LD) -o $@ -m elf_i386 -e kernel_start -Ttext 0x1000 $^ --oformat binary

# kernel.elf: boot/kernel_entry.o ${OBJ}
# 	$(LD) -o $@ -Ttext 0x1000 $^ 

run: os-image.bin
	qemu-system-x86_64 -fda os-image.bin

# Generic rules for wildcards
# To make an object, always compile from its .c
%.o: %.c ${HEADERS}
	${CC} ${CFLAGS} -ffreestanding -c $< -o $@

%.o: %.s
	${CC} $< -m32 -c -o $@

clean:
	rm -rf *.bin *.dis *.o os-image.bin *.elf
	rm -rf os-test/kernel/*.o os-test/boot/*.bin os-test/drivers/*.o os-test/boot/*.o os-test/cpu/*.o os-test/libc/*.o
