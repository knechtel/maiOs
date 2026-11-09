include ../../py/mkenv.mk

QSTR_DEFS = qstrdefsport.h
MICROPY_ROM_TEXT_COMPRESSION ?= 1

include $(TOP)/py/py.mk

INC += -Iport_include -I. -I$(TOP) -I$(BUILD) -I../../../../
CFLAGS += -m32 -march=i386 -mno-sse -mno-mmx -std=c99 -ffreestanding -fno-pie \
          -fno-stack-protector -fno-builtin -Os -DNDEBUG -fdata-sections \
          -ffunction-sections $(INC)

SRC_C = \
	port.c \
	shared/libc/printf.c \
	shared/readline/readline.c \
	shared/runtime/pyexec.c \
	shared/runtime/stdout_helpers.c

SRC_QSTR += shared/readline/readline.c shared/runtime/pyexec.c

OBJ += $(PY_CORE_O)
OBJ += $(addprefix $(BUILD)/, $(SRC_C:.c=.o))

all: $(BUILD)/libmicropython-maios.a

.PHONY: all

$(BUILD)/libmicropython-maios.a: $(OBJ)
	$(Q)$(MKDIR) -p $(dir $@)
	$(Q)$(AR) rcs $@ $^

include $(TOP)/py/mkrules.mk
