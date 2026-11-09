#ifndef MICROPY_INCLUDED_MAIOS_MPHALPORT_H
#define MICROPY_INCLUDED_MAIOS_MPHALPORT_H

#include <stdint.h>

static inline mp_uint_t mp_hal_ticks_ms(void) {
    return 0;
}

static inline void mp_hal_set_interrupt_char(char c) {
    (void)c;
}

void mp_hal_move_cursor_back(unsigned int count);
void mp_hal_erase_line_from_cursor(unsigned int count);

#endif
