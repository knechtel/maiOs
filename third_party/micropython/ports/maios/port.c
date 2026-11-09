#include <stddef.h>
#include <stdint.h>

#include "ext4.h"
#include "py/builtin.h"
#include "py/compile.h"
#include "py/gc.h"
#include "py/lexer.h"
#include "py/mperrno.h"
#include "py/mpprint.h"
#include "py/nlr.h"
#include "py/parse.h"
#include "py/runtime.h"
#include "py/stackctrl.h"
#include "shared/runtime/pyexec.h"

#define MAIOS_PY_HEAP_SIZE (1024u * 1024u)
#define MAIOS_PY_SOURCE_SIZE (64u * 1024u)

extern int maios_python_read_char(void);
extern void maios_python_write_char(char value);
extern void maios_python_move_cursor_back(unsigned int count);
extern void maios_python_erase_line_from_cursor(unsigned int count);

static uint8_t python_heap[MAIOS_PY_HEAP_SIZE];
static char python_source[MAIOS_PY_SOURCE_SIZE];

int mp_hal_stdin_rx_chr(void) {
    return maios_python_read_char();
}

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len) {
    for (size_t i = 0; i < len; i++) {
        maios_python_write_char(str[i]);
    }
    return len;
}

void mp_hal_move_cursor_back(unsigned int count) {
    maios_python_move_cursor_back(count);
}

void mp_hal_erase_line_from_cursor(unsigned int count) {
    maios_python_erase_line_from_cursor(count);
}

void gc_collect(void) {
    volatile mp_uint_t stack_dummy;
    mp_uint_t stack_top = (mp_uint_t)MP_STATE_THREAD(stack_top);
    gc_collect_start();
    gc_collect_root((void **)&stack_dummy,
        (stack_top - (mp_uint_t)&stack_dummy) / sizeof(mp_uint_t));
    gc_collect_end();
}

static void python_init(void) {
    gc_init(python_heap, python_heap + sizeof(python_heap));
    mp_init();
    mp_stack_ctrl_init();
    mp_stack_set_limit(48u * 1024u);
}

static void python_report_exception(void *exception) {
    mp_obj_print_exception(&mp_plat_print, (mp_obj_t)exception);
}

void maios_python_repl(void) {
    volatile int stack_top;
    python_init();
    mp_stack_set_top((void *)&stack_top);
    pyexec_friendly_repl();
    mp_deinit();
}

int maios_python_run_file(const char *filename) {
    uint32_t size;
    if (!ext4_read_start(filename, &size)) {
        return 0;
    }
    if (size >= sizeof(python_source)) {
        ext4_read_end();
        return 0;
    }
    for (uint32_t block = 0, offset = 0; offset < size; block++) {
        uint8_t data[1024];
        if (!ext4_read_block(block, data)) {
            ext4_read_end();
            return 0;
        }
        uint32_t count = size - offset;
        if (count > sizeof(data)) count = sizeof(data);
        for (uint32_t i = 0; i < count; i++) python_source[offset + i] = (char)data[i];
        offset += count;
    }
    ext4_read_end();
    python_source[size] = 0;

    volatile int stack_top;
    python_init();
    mp_stack_set_top((void *)&stack_top);
    int success = 0;
    nlr_buf_t nlr;
    if (nlr_push(&nlr) == 0) {
        mp_lexer_t *lexer = mp_lexer_new_from_str_len(MP_QSTR__lt_stdin_gt_,
            python_source, size, 0);
        mp_parse_tree_t parse_tree = mp_parse(lexer, MP_PARSE_FILE_INPUT);
        mp_obj_t module_fun = mp_compile(&parse_tree, lexer->source_name, true);
        mp_call_function_0(module_fun);
        nlr_pop();
        success = 1;
    } else {
        python_report_exception(nlr.ret_val);
    }
    mp_deinit();
    return success;
}

void nlr_jump_fail(void *value) {
    (void)value;
    for (;;) __asm__ volatile ("cli; hlt");
}

void MP_NORETURN __fatal_error(const char *message) {
    while (*message) maios_python_write_char(*message++);
    for (;;) __asm__ volatile ("cli; hlt");
}

void __assert_func(const char *file, int line, const char *function, const char *expression) {
    (void)file;(void)line;(void)function;(void)expression;
    __fatal_error("MicroPython assertion failed\n");
}

mp_lexer_t *mp_lexer_new_from_file(qstr filename) {
    (void)filename;
    mp_raise_OSError(MP_ENOENT);
}

mp_import_stat_t mp_import_stat(const char *path) {
    (void)path;
    return MP_IMPORT_STAT_NO_EXIST;
}
