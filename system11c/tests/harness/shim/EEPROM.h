#pragma once
#include <stdint.h>
struct EEPROMClass {
  uint8_t mem[4096];
  uint8_t read(int i) { return mem[i & 4095]; }
  void write(int i, uint8_t v) { mem[i & 4095] = v; }
  void update(int i, uint8_t v) { mem[i & 4095] = v; }
};
extern EEPROMClass EEPROM;
