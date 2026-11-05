#include "fakeavr.h"

namespace fake {
uint64_t now_ns = 0;
Hooks* hooks = nullptr;
bool I_flag = false;
bool irq_pending = false;
long access_count = 0;
long inject_at = -1;
int isr_depth = 0;
void (*isr)() = nullptr;
static bool glue = false;   // the previous access was a read: its write follows without an interrupt in between

void cycles(unsigned n) { now_ns += (uint64_t)n * 625 / 10; }

void raise_compare_match() { irq_pending = true; }

static void run_isr() {
  irq_pending = false;
  I_flag = false;
  isr_depth++;
  if (isr) isr();
  isr_depth--;
  I_flag = true;
}

void maybe_interrupt() {
  if (isr_depth == 0 && irq_pending && I_flag && (TIMSK1.v & 0x02)) run_isr();
}

void sei() { I_flag = true; maybe_interrupt(); }
void cli() { I_flag = false; }

static void before(bool is_read) {
  if (isr_depth == 0) {
    access_count++;
    if (inject_at >= 0 && access_count == inject_at) irq_pending = true;
    if (!glue) maybe_interrupt();
  }
  glue = is_read;
}
static void after_write(int id) { if (id == ID_TIMSK1 && isr_depth == 0) maybe_interrupt(); }
}

using namespace fake;

Reg::operator uint8_t() const {
  before(true);
  cycles(1);
  return hooks ? hooks->on_read(id, v) : v;
}
uint8_t Reg::operator=(int x) {
  before(false);
  cycles(1);
  uint8_t o = v;
  v = (uint8_t)x;
  if (hooks) hooks->on_write(id, o, v);
  after_write(id);
  return v;
}
Reg& Reg::operator|=(int x) {
  before(false);
  cycles(2);
  uint8_t o = v;
  v = (uint8_t)(v | x);
  if (hooks) hooks->on_write(id, o, v);
  after_write(id);
  return *this;
}
Reg& Reg::operator&=(int x) {
  before(false);
  cycles(2);
  uint8_t o = v;
  v = (uint8_t)(v & x);
  if (hooks) hooks->on_write(id, o, v);
  after_write(id);
  return *this;
}
Reg& Reg::operator^=(int x) {
  before(false);
  cycles(2);
  uint8_t o = v;
  v = (uint8_t)(v ^ x);
  if (hooks) hooks->on_write(id, o, v);
  after_write(id);
  return *this;
}

#define X(n) Reg n(#n, fake::ID_##n);
FAKE_REG_LIST(X)
#undef X
Reg16 TCNT1("TCNT1"), OCR1A("OCR1A"), OCR1B("OCR1B");
