#include <stdint.h>

#define MICROPY_CONFIG_ROM_LEVEL       (MICROPY_CONFIG_ROM_LEVEL_MINIMUM)
#define MICROPY_ENABLE_COMPILER        (1)
#define MICROPY_HELPER_REPL            (1)
#define MICROPY_ENABLE_GC              (1)
#define MICROPY_ENABLE_EXTERNAL_IMPORT (0)
#define MICROPY_PY_SYS_MODULES         (0)
#define MICROPY_PY_SYS_EXIT            (0)
#define MICROPY_PY_SYS_PATH            (0)
#define MICROPY_PY_SYS_ARGV            (0)
#define MICROPY_HAL_HAS_VT100          (0)
#define MICROPY_STACK_CHECK            (1)
#define MICROPY_STACK_CHECK_MARGIN     (4096)
#define MICROPY_ALLOC_PATH_MAX         (64)
#define MICROPY_ALLOC_PARSE_CHUNK_INIT (16)
#define MICROPY_HW_BOARD_NAME          "maiOs"
#define MICROPY_HW_MCU_NAME            "i386"
#define MICROPY_FLOAT_IMPL             (MICROPY_FLOAT_IMPL_NONE)
#define MP_STATE_PORT                  MP_STATE_VM

#define alloca(size) __builtin_alloca(size)

typedef long mp_off_t;
