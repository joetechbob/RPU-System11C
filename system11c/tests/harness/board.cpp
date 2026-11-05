#include "board.h"
#include <string.h>
#include <algorithm>

using namespace fake;

namespace s11 {

Board board;
uint64_t tick_clock = 0;

static const uint16_t PIA_BASE[6] = {0x2100, 0x2400, 0x2800, 0x2C00, 0x3000, 0x3400};
// solenoid number (17-22) -> {pia index, 'A' = CA2 / 'B' = CB2}
struct SpecialLine { int pia; char line; };
static const SpecialLine SPECIAL[6] = {
  {4, 'A'},   // sol 17  $3001
  {3, 'A'},   // sol 18  $2C01
  {3, 'B'},   // sol 19  $2C03
  {4, 'B'},   // sol 20  $3003
  {1, 'B'},   // sol 21  $2403
  {1, 'A'},   // sol 22  $2401
};

Board::Board() {}

void Board::power_on() {
  for (auto& p : pia) p = Pia();
  latch = 0;
  memset(sw, 0, sizeof sw);
  vma = e_high = strobed = false;
  last_pa2 = true;
  blank_until = 0;
  cur = Txn();
  last_raw = 0;
  clear_logs();
}

void Board::clear_logs() {
  txns.clear(); acc.clear(); mlog.clear(); sound.clear(); lamp_drive.clear();
  v = Violations();
  last_raw = raw_mask();
  mlog.push_back({now_ns, last_raw, dynamic_blanking ? blank_until : ~0ull});
}

uint16_t Board::addr_pins() const { return (uint16_t)(((uint16_t)PORTK.v << 8) | PORTF.v); }

int Board::pia_index(uint16_t addr) const {
  for (int i = 0; i < 6; i++) if ((addr & 0xFFFC) == PIA_BASE[i]) return i;
  return -1;
}

uint32_t Board::raw_mask() const {
  uint32_t m = latch;
  m |= (uint32_t)pia[0].pin_b() << 8;
  for (int i = 0; i < 6; i++) {
    const Pia& p = pia[SPECIAL[i].pia];
    bool level = SPECIAL[i].line == 'A' ? p.ca2() : p.cb2();
    if (!level) m |= 1u << (16 + i);
  }
  if (!pia[0].cb2()) m |= 1u << GAMEON;
  return m;
}

void Board::update_mask(bool force) {
  uint32_t m = raw_mask();
  uint64_t bu = dynamic_blanking ? blank_until : ~0ull;
  if (force || m != last_raw || bu != mlog.back().blank_until) {
    last_raw = m;
    mlog.push_back({now_ns, m, bu});
  }
}

// ---- devices ---------------------------------------------------------------------------------

void Board::dev_write(uint16_t addr, uint8_t d) {
  acc.push_back({now_ns, true, addr, d});
  if (addr >= 0x2200 && addr <= 0x2203) {          // U28 latch, A1:A0 ignored
    if (addr != 0x2200) v.latch_offset_write++;
    latch = d;
    update_mask();
    return;
  }
  int i = pia_index(addr);
  if (i < 0) { v.unmapped_write++; return; }
  Pia& p = pia[i];
  uint8_t old_crb = p.crb;
  switch (addr & 3) {
    case 0: if (p.cra & 4) p.ora = d; else p.ddra = d; break;
    case 1: p.cra = d & 0x3F; break;
    case 2: if (p.crb & 4) p.orb = d; else p.ddrb = d; break;
    case 3: p.crb = d & 0x3F; break;
  }
  after_pia_write(i, old_crb);
}

void Board::after_pia_write(int i, uint8_t old_crb) {
  Pia& p = pia[i];
  if (i == 2) {                                    // display PIA: PA2 retriggers BLANKING (any edge)
    bool pa2 = (p.pin_a() & 4) != 0;
    if (pa2 != last_pa2) { last_pa2 = pa2; blank_until = now_ns + 36000000ull; }
  }
  if (i == 5) {                                    // widget PIA: CB2 falling edge latches the sound command
    bool was_high = Pia::out_level(old_crb);
    if (was_high && !p.cb2()) sound.push_back({now_ns, p.orb});
  }
  if (i == 1) {                                    // lamp matrix: rows on port A, columns on port B
    lamp_drive.push_back({p.pin_b(), p.pin_a()});
  }
  update_mask();
}

uint8_t Board::dev_read(uint16_t addr) {
  if (addr >= 0x2200 && addr <= 0x2203) { v.latch_read++; acc.push_back({now_ns, false, addr, 0xFF}); return 0xFF; }
  int i = pia_index(addr);
  if (i < 0) { v.unmapped_read++; acc.push_back({now_ns, false, addr, 0xFF}); return 0xFF; }
  Pia& p = pia[i];
  uint8_t r = 0xFF;
  switch (addr & 3) {
    case 0:
      if (p.cra & 4) {
        r = p.pin_a();
        if (i == 4) {                              // switch returns: OR of the closed switches of the strobed columns
          uint8_t strobe = p.pin_b() & p.ddrb, ret = 0;
          for (int c = 0; c < 8; c++) if (strobe & (1 << c)) ret |= sw[c];
          r = (uint8_t)((p.ora & p.ddra) | (ret & ~p.ddra));
        }
      } else r = p.ddra;
      break;
    case 1: r = p.cra; break;
    case 2: r = (p.crb & 4) ? p.pin_b() : p.ddrb; break;
    case 3: r = p.crb; break;
  }
  acc.push_back({now_ns, false, addr, r});
  return r;
}

// ---- bus -------------------------------------------------------------------------------------

void Board::port_g(uint8_t o, uint8_t n) {
  bool vma_o = o & 2, vma_n = n & 2, e_o = o & 4, e_n = n & 4;
  bool rw_read = (PORTE.v & 0x20) != 0;
  bool driven = DDRA.v == 0xFF;
  if (vma_n && !vma_o) {                           // VMA rises: a transaction begins
    cur = Txn();
    cur.t0 = now_ns;
    cur.addr = addr_pins();
    cur.read = rw_read;
    cur.data = (!rw_read && driven) ? PORTA.v : -1;
    vma = true;
    strobed = false;
  }
  if (e_n && !e_o) {                               // E rises
    e_high = true;
    t_rise = now_ns;
    if (vma) {
      cur.pulses++;
      rise_addr = addr_pins(); rise_read = rw_read; rise_data = (!rw_read && driven) ? PORTA.v : -1;
    }
  }
  if (!e_n && e_o) {                               // E falls: a write is taken here
    e_high = false;
    if (vma) {
      uint32_t w = (uint32_t)(now_ns - t_rise);
      cur.widths_ns.push_back(w);
      if (w < E_HIGH_MIN_NS) v.e_pulse_short++;
      if (!strobed) {
        strobed = true;
        uint16_t a = addr_pins();
        int d = (!rw_read && driven) ? PORTA.v : -1;
        if (a != cur.addr || rw_read != cur.read || (!rw_read && d != cur.data)) { cur.bus_changed = true; v.bus_changed++; }
        if (!rw_read) dev_write(a, d < 0 ? 0xFF : (uint8_t)d);
      }
    }
  }
  if (!vma_n && vma_o) {                           // VMA falls: the transaction ends
    if (e_high) { cur.e_high_at_end = true; v.e_high_at_txn_end++; }
    txns.push_back(cur);
    vma = false;
  }
}

void Board::on_write(int id, uint8_t oldv, uint8_t newv) {
  if (id == ID_PORTG) port_g(oldv, newv);
}

uint8_t Board::on_read(int id, uint8_t raw) {
  if (id == ID_PINA) {
    if (!vma || !cur.read || !e_high) { v.read_without_e++; return 0xFF; }
    int64_t dt = (int64_t)(now_ns - t_rise);
    cur.sample_after_rise_ns = dt;
    cur.sampled_while_e_high = true;
    if (dt < (int64_t)TDDR_MAX_NS) v.read_early++;
    uint8_t d = dev_read(addr_pins());
    cur.data = d;
    return d;
  }
  if (id == ID_PING) return (uint8_t)(PORTG.v & 0x04);   // E pin level; only CheckForMPUClock reads it
  return raw;
}

// ---- measurements ----------------------------------------------------------------------------

static int popcount(uint32_t x) { int n = 0; while (x) { n += x & 1; x >>= 1; } return n; }

uint64_t Board::raw_on_ns(uint32_t bits, uint64_t t_end) const {
  uint64_t tot = 0;
  for (size_t i = 0; i < mlog.size(); i++) {
    uint64_t a = mlog[i].ns, b = (i + 1 < mlog.size()) ? mlog[i + 1].ns : t_end;
    if (b > t_end) b = t_end;
    if (b > a && (mlog[i].raw & bits)) tot += b - a;
  }
  return tot;
}

uint64_t Board::energised_ns(uint32_t bits, uint64_t t_end) const {
  uint64_t tot = 0;
  for (size_t i = 0; i < mlog.size(); i++) {
    uint64_t a = mlog[i].ns, b = (i + 1 < mlog.size()) ? mlog[i + 1].ns : t_end;
    if (b > t_end) b = t_end;
    if (b > mlog[i].blank_until) b = std::max(a, mlog[i].blank_until) ;
    if (b > a && (mlog[i].raw & bits)) tot += b - a;
  }
  return tot;
}

int64_t Board::first_energised(uint32_t bits) const {
  for (size_t i = 0; i < mlog.size(); i++)
    if ((mlog[i].raw & bits) && mlog[i].ns < mlog[i].blank_until) return (int64_t)mlog[i].ns;
  return -1;
}

int Board::max_simultaneous(uint32_t bits) const {
  int m = 0;
  for (auto& e : mlog) m = std::max(m, popcount(e.raw & bits));
  return m;
}

// ---- bring-up and Timer1 ---------------------------------------------------------------------

}  // namespace s11

extern "C" void isr_TIMER1_COMPA_vect(void);
extern unsigned long RPU_InitializeMPU(unsigned long initOptions, unsigned char creditResetSwitch);

namespace s11 {

void bring_up(bool dynamic_blanking, bool keep_init_log) {
  fake::hooks = &board;
  fake::isr = isr_TIMER1_COMPA_vect;
  fake::now_ns = 0;
  fake::I_flag = false;
  fake::irq_pending = false;
  fake::access_count = 0;
  fake::inject_at = -1;
  fake::isr_depth = 0;
#define X(n) n.v = 0;
  FAKE_REG_LIST(X)
#undef X
  board.dynamic_blanking = dynamic_blanking;
  board.power_on();
  RPU_InitializeMPU(0, 0xFF);        // the library's own init: boot write, PIA init, Timer1 setup
  tick_clock = now_ns;
  if (!keep_init_log) board.clear_logs();
}

void run_ticks(int n) {
  for (int i = 0; i < n; i++) {
    tick_clock += TICK_NS;
    if (now_ns < tick_clock) now_ns = tick_clock;
    fake::raise_compare_match();
    fake::maybe_interrupt();
  }
}
void run_slots(int n) { run_ticks(2 * n); }

void load_running_game_state(uint8_t strobe) {
  // What the board still holds when only the AVR is reset: PIAs configured as the running game left
  // them, requests present on U28 / PIA0 port B / special lines, BLANKING timed out.
  Pia& d = board.pia[2];
  d.cra = 0x3D; d.ddra = 0x7F; d.ora = (uint8_t)(0x70 | strobe); d.crb = 0x3D; d.ddrb = 0xFF; d.orb = 0xFF;
  board.pia[0].ddrb = 0xFF; board.pia[0].crb = 0x3C; board.pia[0].orb = 0x02;     // sol 10 requested
  board.pia[0].ddra = 0xFF; board.pia[0].cra = 0x3C;
  board.latch = 0x01;                                                               // sol 1 requested
  board.pia[4].cra = 0x34;                                                          // sol 17 requested
  board.pia[1].crb = 0x34;                                                          // sol 21 requested
  board.pia[1].cra = 0x3C;
  board.pia[3].cra = 0x3C; board.pia[3].crb = 0x3C; board.pia[4].crb = 0x3C;
  board.last_pa2 = (d.pin_a() & 4) != 0;
  board.blank_until = 0;
  board.clear_logs();
}

}  // namespace s11

