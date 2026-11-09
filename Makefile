CC = gcc
AS = gcc
LD = ld
CFLAGS = -m32 -march=i386 -mno-sse -mno-mmx -std=c11 -ffreestanding -fno-pie -fno-stack-protector \
         -fno-asynchronous-unwind-tables -Wall -Wextra -O2
ASFLAGS = -m32 -ffreestanding -fno-pie
LDFLAGS = -m elf_i386 -T linker.ld

.PHONY: all run clean

all: kernel.elf

kernel.elf: boot.o kernel.o ext4.o e1000.o net.o linker.ld
	$(LD) $(LDFLAGS) -o $@ boot.o kernel.o ext4.o e1000.o net.o

boot.o: boot.s
	$(AS) $(ASFLAGS) -c $< -o $@

kernel.o: kernel.c
	$(CC) $(CFLAGS) -c $< -o $@

ext4.o: ext4.c ext4.h
	$(CC) $(CFLAGS) -c $< -o $@

e1000.o: e1000.c e1000.h
	$(CC) $(CFLAGS) -c $< -o $@

net.o: net.c net.h e1000.h
	$(CC) $(CFLAGS) -c $< -o $@

disk.img:
	truncate -s 1M disk.img

ext4-2g.img:
	truncate -s 2G ext4-2g.img
	mke2fs -q -F -t ext4 -b 1024 -O ^has_journal,^metadata_csum,^64bit,^extent,^flex_bg,^dir_index ext4-2g.img

run: kernel.elf ext4-2g.img
	qemu-system-i386 -kernel kernel.elf -drive file=ext4-2g.img,format=raw,if=ide -nic user,model=e1000

clean:
	rm -f boot.o kernel.o ext4.o e1000.o net.o kernel.elf
