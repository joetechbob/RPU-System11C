#pragma once
#include "../../fakeavr.h"
#define TIMER1_COMPA_vect TIMER1_COMPA_vect
#define ISR(vec) extern "C" void isr_##vec(void); extern "C" void isr_##vec(void)
static inline void sei(void) { fake::sei(); }
static inline void cli(void) { fake::cli(); }
static inline void interrupts(void) { fake::sei(); }
static inline void noInterrupts(void) { fake::cli(); }
