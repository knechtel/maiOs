CC = gcc
AS = gcc
LD = ld
AR = ar
CFLAGS = -m32 -march=i386 -mno-sse -mno-mmx -std=c11 -ffreestanding -fno-pie -fno-stack-protector \
         -fno-asynchronous-unwind-tables -Wall -Wextra -O2
BEARSSL_CFLAGS = -m32 -march=i386 -mno-sse -mno-mmx -std=c99 -ffreestanding -fno-pie \
                 -fno-stack-protector -fno-builtin -Os -Ithird_party/bearssl/port \
                 -Ithird_party/bearssl/inc \
                 -Ithird_party/bearssl/src -DBR_USE_URANDOM=0 -DBR_USE_UNIX_TIME=0
ASFLAGS = -m32 -ffreestanding -fno-pie
LDFLAGS = -m elf_i386 -T linker.ld
BEARSSL_SOURCES := $(shell find third_party/bearssl/src -name '*.c')
BEARSSL_OBJECTS := $(patsubst third_party/bearssl/src/%.c,build/bearssl/%.o,$(BEARSSL_SOURCES))
MICROPYTHON_PORT = third_party/micropython/ports/maios
MICROPYTHON_LIBRARY = $(MICROPYTHON_PORT)/build/libmicropython-maios.a
MICROPYTHON_GIT_TAG = v1.30.0-preview
MICROPYTHON_GIT_HASH = c1c4e7b
CPYTHON_PORT = third_party/cpython/ports/maios
CPYTHON_BUILD = build/cpython-maios
CPYTHON_SYSROOT = $(CURDIR)/$(CPYTHON_PORT)/sysroot
CPYTHON_CC = $(CC) -m32
CPYTHON_CFLAGS = -m32 -march=i386 -mno-sse -mno-mmx -O2 -ffreestanding -fno-pie -fno-stack-protector
CPYTHON_STAMP = $(CPYTHON_BUILD)/.configured
CPYTHON_ARCHIVE = $(CPYTHON_BUILD)/libpython3.11.a
CPYTHON_RUNTIME = $(CPYTHON_BUILD)/libmaiosrt.a
CPYTHON_ADAPTER = $(CPYTHON_BUILD)/cpython_port.o
CPYTHON_RUNTIME_SOURCES = $(wildcard $(CPYTHON_PORT)/runtime/*.c) $(wildcard third_party/micropython/lib/libm_dbl/*.c)
CPYTHON_LIBGCC = $(shell $(CC) -m32 -print-libgcc-file-name)
CPYTHON_KERNEL_OBJECT = build/cpython-kernel.o
KERNEL_IMAGE ?= kernel-cpython.elf

.PHONY: all run clean micropython-force

all: $(KERNEL_IMAGE)

$(KERNEL_IMAGE) kernel.elf: boot.o $(CPYTHON_KERNEL_OBJECT) gitpack.o ext4.o e1000.o net.o https.o https_roots.o freestanding.o libbearssl-maios.a $(CPYTHON_ARCHIVE) $(CPYTHON_RUNTIME) $(CPYTHON_ADAPTER) linker.ld
	$(LD) $(LDFLAGS) -o $@ boot.o $(CPYTHON_KERNEL_OBJECT) gitpack.o ext4.o e1000.o net.o https.o https_roots.o freestanding.o libbearssl-maios.a $(CPYTHON_ADAPTER) $(CPYTHON_ARCHIVE) $(CPYTHON_RUNTIME) $(CPYTHON_LIBGCC)

boot.o: boot.s
	$(AS) $(ASFLAGS) -c $< -o $@

kernel.o: kernel.c third_party/cpython/ports/maios/cpython_port.h
	$(CC) $(CFLAGS) -I$(CPYTHON_PORT) -c $< -o $@

$(CPYTHON_KERNEL_OBJECT): kernel.c third_party/cpython/ports/maios/cpython_port.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(CPYTHON_PORT) -c $< -o $@

$(CPYTHON_STAMP): $(CPYTHON_PORT)/Makefile $(CPYTHON_PORT)/config.site $(CPYTHON_PORT)/Modules.Setup.local.example third_party/cpython/configure third_party/cpython/configure.ac third_party/cpython/Makefile.pre.in third_party/cpython/Tools/scripts/freeze_modules.py
	$(MAKE) -C $(CPYTHON_PORT) configure CC='$(CPYTHON_CC)' CFLAGS='$(CPYTHON_CFLAGS)' SYSROOT='$(CPYTHON_SYSROOT)' BUILD_DIR='$(abspath $(CPYTHON_BUILD))'
	@touch $@

$(CPYTHON_ARCHIVE): $(CPYTHON_STAMP) third_party/cpython/Python/pylifecycle.c third_party/cpython/Python/frozen.c $(CPYTHON_PORT)/webserver.c $(CPYTHON_PORT)/maiosposix.c $(CPYTHON_PORT)/maiosfs.c
	$(MAKE) -C $(CPYTHON_PORT) core BUILD_DIR='$(abspath $(CPYTHON_BUILD))'

$(CPYTHON_RUNTIME): $(CPYTHON_STAMP) $(CPYTHON_PORT)/Makefile $(CPYTHON_RUNTIME_SOURCES)
	$(MAKE) -C $(CPYTHON_PORT) runtime CC='$(CPYTHON_CC)' CFLAGS='$(CPYTHON_CFLAGS)' SYSROOT='$(CPYTHON_SYSROOT)' BUILD_DIR='$(abspath $(CPYTHON_BUILD))'

$(CPYTHON_ADAPTER): $(CPYTHON_STAMP) $(CPYTHON_PORT)/cpython_port.c $(CPYTHON_PORT)/cpython_port.h
	$(MAKE) -C $(CPYTHON_PORT) adapter CC='$(CPYTHON_CC)' CFLAGS='$(CPYTHON_CFLAGS)' SYSROOT='$(CPYTHON_SYSROOT)' BUILD_DIR='$(abspath $(CPYTHON_BUILD))'

$(MICROPYTHON_LIBRARY): micropython-force
	MICROPY_GIT_TAG=$(MICROPYTHON_GIT_TAG) MICROPY_GIT_HASH=$(MICROPYTHON_GIT_HASH) $(MAKE) -C $(MICROPYTHON_PORT)

micropython-force:

gitpack.o: gitpack.c gitpack.h ext4.h third_party/bearssl/inc/bearssl.h
	$(CC) $(CFLAGS) -Ithird_party/bearssl/port -Ithird_party/bearssl/inc -c $< -o $@

ext4.o: ext4.c ext4.h
	$(CC) $(CFLAGS) -c $< -o $@

e1000.o: e1000.c e1000.h
	$(CC) $(CFLAGS) -c $< -o $@

net.o: net.c net.h e1000.h
	$(CC) $(CFLAGS) -c $< -o $@

https.o: https.c net.h third_party/bearssl/inc/bearssl.h
	$(CC) $(CFLAGS) -Ithird_party/bearssl/port -Ithird_party/bearssl/inc -c $< -o $@

https_roots.o: https_roots.c third_party/bearssl/inc/bearssl.h
	$(CC) $(CFLAGS) -Ithird_party/bearssl/port -Ithird_party/bearssl/inc -c $< -o $@

freestanding.o: freestanding.c
	$(CC) $(CFLAGS) -fno-builtin -c $< -o $@

build/bearssl/%.o: third_party/bearssl/src/%.c
	mkdir -p $(dir $@)
	$(CC) $(BEARSSL_CFLAGS) -c $< -o $@

libbearssl-maios.a: $(BEARSSL_OBJECTS)
	$(AR) rcs $@ $(BEARSSL_OBJECTS)

disk.img:
	truncate -s 1M disk.img

ext4-2g.img:
	truncate -s 2G ext4-2g.img
	mke2fs -q -F -t ext4 -b 1024 -O ^has_journal,^metadata_csum,^64bit,^extent,^flex_bg,^dir_index ext4-2g.img

run: $(KERNEL_IMAGE) ext4-2g.img
	qemu-system-i386 -cpu max -k pt-br -kernel $(KERNEL_IMAGE) -drive file=ext4-2g.img,format=raw,if=ide -nic user,model=e1000,hostfwd=tcp:127.0.0.1:8070-:8070

clean:
	rm -f boot.o kernel.o gitpack.o ext4.o e1000.o net.o https.o https_roots.o freestanding.o libbearssl-maios.a kernel.elf kernel-cpython.elf
	rm -rf build/bearssl
	rm -rf $(CPYTHON_BUILD)
	$(MAKE) -C $(MICROPYTHON_PORT) clean
