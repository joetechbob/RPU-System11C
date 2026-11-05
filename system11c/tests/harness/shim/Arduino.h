// Minimal Arduino.h for building RPU.cpp on the host against the fake AVR (see ../fakeavr.h).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "../fakeavr.h"
#include <avr/io.h>
#include <avr/pgmspace.h>
#include <avr/interrupt.h>

typedef bool boolean;
typedef uint8_t byte;
typedef uint16_t word;

#define HIGH 0x1
#define LOW 0x0
#define INPUT 0x0
#define OUTPUT 0x1
#define INPUT_PULLUP 0x2
#define LSBFIRST 0
#define MSBFIRST 1
#define A0 54
#define A1 55
#define A2 56
#define A3 57
#define A4 58
#define A5 59
#define A6 60
#define A7 61

void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t val);
int digitalRead(uint8_t pin);
unsigned long millis(void);
unsigned long micros(void);
void delay(unsigned long ms);
void delayMicroseconds(unsigned int us);
// Replaces the one inline-asm "rjmp .+0" of RPU.cpp (2 cycles), see run.sh.
void rpu_test_wait_cycles(unsigned n);

#define lowByte(w) ((uint8_t)((w) & 0xff))
#define highByte(w) ((uint8_t)((w) >> 8))
#define bitRead(v, b) (((v) >> (b)) & 1)
#define bitSet(v, b) ((v) |= (1UL << (b)))
#define bitClear(v, b) ((v) &= ~(1UL << (b)))
#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif

// Serial is not part of what is tested: swallow it.
struct HardwareSerial {
  void begin(unsigned long, uint8_t = 0) {}
  int available() { return 0; }
  int read() { return -1; }
  template <class... A> size_t print(A...) { return 0; }
  template <class... A> size_t println(A...) { return 0; }
  template <class... A> size_t write(A...) { return 0; }
  void flush() {}
};
extern HardwareSerial Serial, Serial1, Serial2, Serial3;
