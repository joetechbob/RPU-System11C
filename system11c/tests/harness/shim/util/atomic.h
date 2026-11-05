#pragma once
#include "../avr/interrupt.h"
struct fake_atomic_guard {
  bool saved, run;
  fake_atomic_guard() : saved(fake::I_flag), run(true) { fake::cli(); }
  ~fake_atomic_guard() { if (saved) fake::sei(); }
};
#define ATOMIC_RESTORESTATE 0
#define ATOMIC_BLOCK(type) for (fake_atomic_guard fake_ag; fake_ag.run; fake_ag.run = false)
