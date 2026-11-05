// Flash is ordinary memory on the host.
#pragma once
#include <stdint.h>
#define PROGMEM
#define PSTR(s) (s)
#define pgm_read_byte(a) (*(const uint8_t *)(a))
#define pgm_read_word(a) (*(const uint16_t *)(a))
