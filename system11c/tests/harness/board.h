// Model of the Williams System 11C CPU board as an RPU Rev 102 sees it, decoded from the
// pins the library drives (see ../../HARDWARE.md for the schematic references).
//
//   address  PORTF = A0-A7, PORTK = A8-A15 (Mega A0-A15)
//   data     PORTA (Mega D22-D29), driven by the Mega only while DDRA = 0xFF
//   E        PORTG bit 2 (Mega D39)        VMA  PORTG bit 1 (D40)      R/W  PORTE bit 5 (D3)
//
// Devices (addresses as the stock board decodes them):
//   $2200-$2203  U28 74LS374 latch, SOL1-8.  WRITE ONLY, A1:A0 not decoded.  A read is open bus.
//   $2100 PIA0   sol 9-16 on port B ($2102); CB2 ($2103) = GameOn / special-solenoid enable
//   $2400 PIA1   lamps: port A rows, port B columns; CA2 = sol 22, CB2 = sol 21
//   $2800 PIA2   display strobe; PA2 retriggers the BLANKING one-shot
//   $2C00 PIA3   alpha display; CA2 = sol 18, CB2 = sol 19
//   $3000 PIA4   switches: port B strobes, port A returns; CA2 = sol 17, CB2 = sol 20
//   $3400 PIA5   widget PIA: port B = sound board command, CB2 = command strobe
//
// Bus timing is checked, not enforced: a write is always taken at the falling edge of E
// and a read always returns the device data, so that a test about one hazard is not
// masked by another.  Every timing hazard is counted in `v` and asserted by its own test.
#pragma once
#include <stdint.h>
#include <vector>
#include <string>
#include "fakeavr.h"

namespace s11 {

// MC6821 timing limits used by the checks (datasheet, 1 MHz part)
const uint32_t E_HIGH_MIN_NS = 450;   // minimum E pulse width high
const uint32_t TDDR_MAX_NS   = 290;   // data delay time: read data valid up to this long after E rises

// One bit per solenoid request line (see Board::raw_mask)
enum { SOL1 = 0, SOL9 = 8, SOL17 = 16, SOL22 = 21, GAMEON = 22 };
inline uint32_t sol_bit(int solenoid_number) { return 1u << (solenoid_number - 1); }  // 1..22
const uint32_t SPECIALS = 0x3F0000;   // sol 17-22
const uint32_t LATCH_SOLS = 0x0000FF; // sol 1-8
const uint32_t PORTB_SOLS = 0x00FF00; // sol 9-16

struct Pia {
  uint8_t ora = 0, ddra = 0, cra = 0, orb = 0, ddrb = 0, crb = 0;
  uint8_t in_a = 0xFF, in_b = 0xFF;      // what the outside drives on input pins (floating TTL reads high)
  uint8_t pin_a() const { return (uint8_t)((ora & ddra) | (in_a & ~ddra)); }
  uint8_t pin_b() const { return (uint8_t)((orb & ddrb) | (in_b & ~ddrb)); }
  // CA2/CB2: control register bits 5-3 = 11x is a plain output (110 low, 111 high); an input floats high.
  static bool out_level(uint8_t cr) { return ((cr & 0x30) == 0x30) ? ((cr & 0x08) != 0) : true; }
  bool ca2() const { return out_level(cra); }
  bool cb2() const { return out_level(crb); }
};

struct Txn {                 // one bus transaction: VMA high from rise to fall
  uint64_t t0 = 0;
  uint16_t addr = 0;         // address pins when VMA rose
  bool read = false;
  int data = -1;             // write: byte driven; read: byte sampled
  int pulses = 0;            // E rising edges while VMA was high
  std::vector<uint32_t> widths_ns;   // width of each completed E pulse
  bool e_high_at_end = false;        // E still high when VMA fell
  bool bus_changed = false;          // address, R/W or data changed between VMA rise and the E fall
  int64_t sample_after_rise_ns = -1; // read: PINA sampled this long after E rose
  bool sampled_while_e_high = false;
};

struct DevAccess { uint64_t ns; bool write; uint16_t addr; uint8_t data; };
struct MaskEv { uint64_t ns; uint32_t raw; uint64_t blank_until; };
struct SoundCmd { uint64_t ns; uint8_t cmd; };

struct Violations {
  int e_pulse_short = 0;        // E fell with VMA high after less than E_HIGH_MIN_NS
  int e_high_at_txn_end = 0;    // E left high when VMA dropped (E not low between transactions)
  int read_early = 0;           // PINA sampled less than tDDR after E rose
  int read_without_e = 0;       // PINA read with no read cycle in progress
  int bus_changed = 0;
  int unmapped_write = 0;
  int unmapped_read = 0;
  int latch_read = 0;           // read of the write-only $2200-$2203 latch
  int latch_offset_write = 0;   // write to $2201-$2203 (strobes SOL1-8 on the real board)
};

struct Board : fake::Hooks {
  bool dynamic_blanking = false;      // false: outputs always enabled; true: U43 one-shot, 36 ms
  Pia pia[6];
  uint8_t latch = 0;
  uint8_t sw[8] = {0};                // closed switches, one byte per strobe column (return line bits)
  struct LampDrive { uint8_t cols, rows; };
  std::vector<LampDrive> lamp_drive;  // (column select lines, row lines) after every write to the lamp PIA

  std::vector<Txn> txns;
  std::vector<DevAccess> acc;
  std::vector<MaskEv> mlog;
  std::vector<SoundCmd> sound;
  Violations v;

  // internals
  Txn cur;
  bool vma = false, e_high = false;
  uint64_t t_rise = 0;
  uint16_t rise_addr = 0; bool rise_read = false; int rise_data = -1;
  bool strobed = false;
  bool last_pa2 = true;
  uint64_t blank_until = 0;
  uint32_t last_raw = 0;

  Board();
  void power_on();                    // board and AVR registers as at power-up
  void clear_logs();

  // fake::Hooks
  void on_write(int id, uint8_t oldv, uint8_t newv) override;
  uint8_t on_read(int id, uint8_t raw) override;

  uint16_t addr_pins() const;
  void port_g(uint8_t o, uint8_t n);
  void dev_write(uint16_t addr, uint8_t d);
  uint8_t dev_read(uint16_t addr);
  int pia_index(uint16_t addr) const;
  void after_pia_write(int idx, uint8_t old_crb);
  void update_mask(bool force = false);

  // Solenoid request lines as the drivers see them (before BLANKING)
  uint32_t raw_mask() const;
  uint64_t blanking_until_now() const;

  // Measurements over mlog up to time `t_end` (ns)
  uint64_t raw_on_ns(uint32_t bits, uint64_t t_end) const;       // time any of `bits` was requested
  uint64_t energised_ns(uint32_t bits, uint64_t t_end) const;    // ... and BLANKING allowed it
  int max_simultaneous(uint32_t bits) const;                     // most lines of `bits` requested at once
  // first time (ns, from clear_logs) a line in `bits` is requested while BLANKING is enabled, or -1
  int64_t first_energised(uint32_t bits) const;
};

extern Board board;

const unsigned TICK_NS = 1035938;     // Timer1 tick: (16574 + 1) / 16 MHz; a solenoid slot is two ticks

// Power the board and the AVR registers up and run the library's own RPU_InitializeMPU(), then clear
// every log unless `keep_init_log`.  `dynamic_blanking` selects the U43 one-shot model.
void bring_up(bool dynamic_blanking = false, bool keep_init_log = false);
// Run n Timer1 compare ticks (one ISR call each).  Tick 0 of the ISR is the display/lamp/switch pass,
// tick 1 is the solenoid pass, and so on.
void run_ticks(int n);
void run_slots(int n);                // n solenoid slots = 2n ticks
extern uint64_t tick_clock;
// Retained state of a game that was running when the AVR (not the board) was reset
void load_running_game_state(uint8_t strobe);
}
