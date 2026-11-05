// Arduino functions the library calls, on the model clock and the fake registers.
#include <Arduino.h>
#include <EEPROM.h>
#include "fakeavr.h"

using namespace fake;


HardwareSerial Serial, Serial1, Serial2, Serial3;
EEPROMClass EEPROM;

static Reg* pin_reg(uint8_t pin, bool ddr, int* bit) {
  if (pin >= 22 && pin <= 29) { *bit = pin - 22; return ddr ? &DDRA : &PORTA; }
  if (pin >= 54 && pin <= 61) { *bit = pin - 54; return ddr ? &DDRF : &PORTF; }
  if (pin >= 62 && pin <= 69) { *bit = pin - 62; return ddr ? &DDRK : &PORTK; }
  if (pin == 39) { *bit = 2; return ddr ? &DDRG : &PORTG; }
  if (pin == 40) { *bit = 1; return ddr ? &DDRG : &PORTG; }
  if (pin == 3)  { *bit = 5; return ddr ? &DDRE : &PORTE; }
  return nullptr;
}
void pinMode(uint8_t pin, uint8_t mode) {
  int bit; Reg* d = pin_reg(pin, true, &bit);
  if (!d) return;
  Reg* p = pin_reg(pin, false, &bit);
  if (mode == OUTPUT) d->v |= 1 << bit; else d->v &= ~(1 << bit);
  if (mode == INPUT_PULLUP) p->v |= 1 << bit;
}
void digitalWrite(uint8_t pin, uint8_t val) {
  int bit; Reg* p = pin_reg(pin, false, &bit);
  if (!p) return;
  if (val) p->v |= 1 << bit; else p->v &= ~(1 << bit);
}
int digitalRead(uint8_t) { return 0; }
unsigned long millis(void) { return (unsigned long)(now_ns / 1000000ull); }
unsigned long micros(void) { return (unsigned long)(now_ns / 1000ull); }
void delay(unsigned long ms) { now_ns += (uint64_t)ms * 1000000ull; }
void delayMicroseconds(unsigned int us) { now_ns += (uint64_t)us * 1000ull; }
void rpu_test_wait_cycles(unsigned n) { fake::cycles(n); }
