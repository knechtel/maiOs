#ifndef MAIOS_E1000_H
#define MAIOS_E1000_H

#include <stdint.h>

int e1000_init(uint8_t mac[6]);
int e1000_send(const uint8_t *frame, uint16_t length);
int e1000_receive(uint8_t *frame, uint16_t capacity);

#endif
