// Fake AVR for the System 11C tests.
//
// The library drives the board by writing ATmega2560 port registers.  Here every register
// is a Reg object: each read and write is counted, costs AVR cycles (a plain access 1,
// a read-modify-write such as `PORTG |= 4` 2, as sbi/cbi on the AVR), advances a model
// clock (16 MHz, 62.5 ns per cycle) and calls the board model, which decodes what the
// pins mean on a System 11C CPU board (see board.h).
//
// The cycle costs are a model, not a measurement.  They order events correctly and give
// the right widths for the code as written (8 cycles = 500 ns between the E rise and fall
// of a write).  Exact cycle counts of the compiled code are checked separately with
// avr-objdump (check_pulse_cycles.py).
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace fake {

extern uint64_t now_ns;            // model time
void cycles(unsigned n);           // advance n CPU cycles (62.5 ns each)

struct Hooks {
  virtual void on_write(int id, uint8_t oldv, uint8_t newv) = 0;
  virtual uint8_t on_read(int id, uint8_t raw) = 0;
  virtual ~Hooks() {}
};
extern Hooks* hooks;

// Interrupt model.  The Timer1 compare ISR runs between two register accesses when the
// I flag is set, OCIE1A is set in TIMSK1 and a compare match is pending.  inject_at makes
// a compare match occur when the foreground makes its N-th register access.
extern bool I_flag;
extern bool irq_pending;
extern long access_count;          // register accesses made by foreground code
extern long inject_at;             // -1 = none
extern int  isr_depth;
extern void (*isr)();              // the TIMER1_COMPA handler
void sei();
void cli();
void maybe_interrupt();
void raise_compare_match();

struct TraceEntry { uint64_t ns; int id; bool write; uint8_t value; };
}

struct Reg {
  const char* name;
  volatile uint8_t v;
  int id;
  Reg(const char* n, int i) : name(n), v(0), id(i) {}
  operator uint8_t() const;
  uint8_t operator=(int x);
  Reg& operator=(const Reg& o) { *this = (int)(uint8_t)o; return *this; }
  Reg& operator|=(int x);
  Reg& operator&=(int x);
  Reg& operator^=(int x);
};

struct Reg16 {
  const char* name;
  volatile uint16_t v;
  Reg16(const char* n) : name(n), v(0) {}
  operator uint16_t() const { fake::cycles(2); return v; }
  uint16_t operator=(int x) { fake::cycles(2); v = (uint16_t)x; return v; }
  Reg16& operator|=(int x) { fake::cycles(4); v |= (uint16_t)x; return *this; }
};

#define FAKE_REG_LIST(X) \
  X(PORTA) X(DDRA) X(PINA) X(PORTB) X(DDRB) X(PINB) X(PORTC) X(DDRC) X(PINC) \
  X(PORTD) X(DDRD) X(PIND) X(PORTE) X(DDRE) X(PINE) X(PORTF) X(DDRF) X(PINF) \
  X(PORTG) X(DDRG) X(PING) X(PORTH) X(DDRH) X(PINH) X(PORTJ) X(DDRJ) X(PINJ) \
  X(PORTK) X(DDRK) X(PINK) X(PORTL) X(DDRL) X(PINL) \
  X(TCCR1A) X(TCCR1B) X(TCCR1C) X(TIMSK1) X(TIFR1) X(TCCR2A) X(TCCR2B) X(TIMSK2) \
  X(TIFR2) X(TCNT2) X(OCR2A) X(OCR2B) X(SREG)

namespace fake {
enum RegId {
#define X(n) ID_##n,
  FAKE_REG_LIST(X)
#undef X
  ID_COUNT
};
}
#define X(n) extern Reg n;
FAKE_REG_LIST(X)
#undef X
extern Reg16 TCNT1, OCR1A, OCR1B;
