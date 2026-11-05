// System 11C tests: each one states a hazard on the real board and checks the library against it.
//
// Every test is written to FAIL on upstream RPU (commit 21b13a3, configured for architecture 15,
// Rev 102) and PASS on branch system11c.  run.sh builds this file against either library.
// See ../README.md for how to run, ../../FIXES.md for the fix each test belongs to.
#include "Arduino.h"
#undef min
#undef max
#include "RPU_Config.h"
#include "RPU.h"
#include "board.h"
#include <stdarg.h>
#include <unistd.h>
#include <sys/wait.h>
#include <string>
#include <vector>
#include <functional>
#include <algorithm>

using namespace s11;

// ---- library entry points that RPU.h does not declare (same signatures as RPU.cpp) --------------
void RPU_DataWrite(int address, byte data);
void RPU_InitializePIAs();
void RPU_ClearVariables();
unsigned long RPU_TestPIAs();
extern volatile byte LampStates[];
// New on this branch.  Upstream has none of them; upstream_absent.cpp stubs them so the tests still
// build and say so when they fail.
void RPU_SetContinuousSolenoidMax(boolean solOn, byte solNum, unsigned short maxMs);
void RPU_SetSolenoidHoldPwm(byte solNum, byte onSlots, byte periodSlots);
boolean RPU_SolenoidStackBusyShared();
boolean RPU_SetAlphaRawFrame(const uint16_t cells[2][16]);
extern int g_absent_calls;
extern const char* g_absent_names[8];

// ---- tiny test framework --------------------------------------------------------------------------
static std::vector<std::string> g_fail;
static void fail(const char* fmt, ...) {
  char buf[400];
  va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
  g_fail.push_back(buf);
}
#define CHECK(cond, ...) do { if (!(cond)) fail(__VA_ARGS__); } while (0)

struct Test { const char* id; const char* fix; const char* statement; void (*fn)(); };
static std::vector<Test>& registry() { static std::vector<Test> r; return r; }
struct Reg_ { Reg_(const char* id, const char* fix, const char* st, void (*fn)()) { registry().push_back({id, fix, st, fn}); } };
#define TEST(name, fix, statement) \
  static void name(); static Reg_ reg_##name(#name, fix, statement, name); static void name()

// Run f in a child process (library state is global); returns "" on success, else the first failure.
static std::string isolated(const std::function<void()>& f) {
  int p[2]; if (pipe(p)) return "pipe failed";
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    close(p[0]); g_fail.clear();
    f();
    if (!g_fail.empty()) { ssize_t r = write(p[1], g_fail[0].c_str(), g_fail[0].size()); (void)r; }
    _exit(g_fail.empty() ? 0 : 1);
  }
  close(p[1]);
  char buf[400]; ssize_t n = read(p[0], buf, sizeof buf - 1); if (n < 0) n = 0; buf[n] = 0; close(p[0]);
  int st = 0; waitpid(pid, &st, 0);
  if (WIFEXITED(st) && WEXITSTATUS(st) == 0) return "";
  if (!WIFEXITED(st)) return "child crashed";
  return buf[0] ? buf : "failed";
}

// ---- helpers -----------------------------------------------------------------------------------------
static const uint64_t SLOT_NS = 2ull * TICK_NS;     // one solenoid slot, 2.0718 ms
static double slots_of(uint64_t ns) { return (double)ns / (double)SLOT_NS; }

struct Interval { uint64_t a, b; };
static std::vector<Interval> intervals(uint32_t bit) {          // times the request line `bit` was asserted
  std::vector<Interval> out; bool on = false; uint64_t a = 0;
  for (auto& e : board.mlog) {
    bool now_on = (e.raw & bit) != 0;
    if (now_on && !on) { a = e.ns; on = true; }
    if (!now_on && on) { out.push_back({a, e.ns}); on = false; }
  }
  if (on) out.push_back({a, fake::now_ns});
  return out;
}
static double on_slots(uint32_t bit) { return slots_of(board.raw_on_ns(bit, fake::now_ns)); }

static bool wrote(uint16_t addr) { for (auto& a : board.acc) if (a.write && a.addr == addr) return true; return false; }
static int writes_to_range(uint16_t lo, uint16_t hi) { int n = 0; for (auto& a : board.acc) if (a.write && a.addr >= lo && a.addr <= hi) n++; return n; }

static void note_absent() {
  if (g_absent_calls) {
    std::string s = "NOTE: not in upstream, stubbed:";
    for (int i = 0; i < g_absent_calls && i < 8; i++) { s += " "; s += g_absent_names[i]; }
    fail("%s", s.c_str());
  }
}

// =================================================================================================
// 0013  One full-width E pulse per bus transaction (6802 mode)
// =================================================================================================
TEST(e_pulse_one_per_write, "0013", "A write gives the PIA exactly one E pulse, not two") {
  bring_up();
  RPU_DataWrite(0x2102, 0x55);
  CHECK(board.txns.size() == 1, "expected 1 transaction, saw %zu", board.txns.size());
  if (board.txns.size() == 1) CHECK(board.txns[0].pulses == 1, "write produced %d E pulses (MC6821 needs one)", board.txns[0].pulses);
}
TEST(e_pulse_one_per_read, "0013", "A read gives the PIA exactly one E pulse, not two") {
  bring_up();
  RPU_DataRead(0x2103);
  CHECK(board.txns.size() == 1, "expected 1 transaction, saw %zu", board.txns.size());
  if (board.txns.size() == 1) CHECK(board.txns[0].pulses == 1, "read produced %d E pulses (MC6821 needs one)", board.txns[0].pulses);
}
TEST(e_high_width_on_write, "0013", "E is high at least 450 ns on a write (MC6821 minimum), 500 ns as the stock board") {
  bring_up();
  RPU_DataWrite(0x2102, 0x55);
  CHECK(board.v.e_pulse_short == 0, "%d E pulse(s) shorter than 450 ns", board.v.e_pulse_short);
  uint32_t w = board.txns.empty() || board.txns[0].widths_ns.empty() ? 0 : board.txns[0].widths_ns[0];
  CHECK(w >= E_HIGH_MIN_NS, "E high for %u ns on a write", w);
}
TEST(e_read_sample_point, "0013", "Read data is sampled at least tDDR (290 ns) after E rises and before E falls") {
  bring_up();
  board.pia[0].crb = 0x3C;
  byte v = RPU_DataRead(0x2103);
  CHECK(v == 0x3C, "read returned 0x%02X, expected 0x3C", v);
  CHECK(board.v.read_early == 0, "data was sampled before the PIA could drive it (%d read(s) early)", board.v.read_early);
  if (!board.txns.empty())
    CHECK(board.txns[0].sample_after_rise_ns >= (int64_t)TDDR_MAX_NS, "sampled %lld ns after E rose (tDDR 290 ns)", (long long)board.txns[0].sample_after_rise_ns);
}
TEST(e_low_between_transactions, "0013", "E is low between transactions, so no PIA sees a stray E edge") {
  bring_up();
  CHECK((PORTG.v & 4) == 0, "E high before the first transaction");
  RPU_DataWrite(0x2102, 0x55);
  CHECK((PORTG.v & 4) == 0, "E left high after a write");
  RPU_DataRead(0x2103);
  CHECK((PORTG.v & 4) == 0, "E left high after a read");
  CHECK(board.v.e_high_at_txn_end == 0, "E was high when VMA dropped at the end of %d transaction(s)", board.v.e_high_at_txn_end);
}

// =================================================================================================
// 0003  Bus transactions are atomic against the Timer1 ISR
// =================================================================================================
struct Tx { bool read; uint16_t addr; int data; bool operator==(const Tx& o) const { return read == o.read && addr == o.addr && data == o.data; } };
static std::vector<Tx> tx_list() { std::vector<Tx> l; for (auto& t : board.txns) l.push_back({t.read, t.addr, t.data}); return l; }

// Run body in a child after bring_up() and return the bus transactions it caused.  A child per run keeps
// the library's own static state (strobe counters, interrupt pass) identical between runs.
static std::vector<Tx> capture(const std::function<void()>& body, long* accesses = nullptr, long inject = -1) {
  int p[2]; if (pipe(p)) return {};
  fflush(stdout);
  pid_t pid = fork();
  if (pid == 0) {
    close(p[0]);
    bring_up(); fake::access_count = 0; fake::inject_at = inject;
    body();
    long n = fake::access_count;
    fake::sei();                                              // an interrupt still pending after the access runs now
    std::vector<Tx> l = tx_list();
    size_t cnt = l.size(); bool bc = board.v.bus_changed != 0;
    ssize_t r = write(p[1], &n, sizeof n); r = write(p[1], &bc, sizeof bc); r = write(p[1], &cnt, sizeof cnt);
    if (cnt) r = write(p[1], l.data(), cnt * sizeof(Tx)); (void)r;
    _exit(0);
  }
  close(p[1]);
  long n = 0; bool bc = false; size_t cnt = 0; std::vector<Tx> l;
  ssize_t r = read(p[0], &n, sizeof n); r = read(p[0], &bc, sizeof bc); r = read(p[0], &cnt, sizeof cnt);
  if (cnt && cnt < 100000) { l.resize(cnt); size_t got = 0; while (got < cnt * sizeof(Tx)) { r = read(p[0], (char*)l.data() + got, cnt * sizeof(Tx) - got); if (r <= 0) break; got += r; } }
  (void)r; close(p[0]);
  int st = 0; waitpid(pid, &st, 0);
  if (accesses) *accesses = n;
  if (bc) l.push_back({false, 0xFFFF, -2});                  // marks "bus changed under a transaction"
  return l;
}

static void sweep(const char* what, const std::function<void()>& op) {
  std::vector<Tx> S = capture([] { board.clear_logs(); fake::isr(); });          // the ISR alone
  long N = 0; std::vector<Tx> F = capture(op, &N);                                // the foreground access alone
  std::vector<Tx> FS = F; FS.insert(FS.end(), S.begin(), S.end());
  std::vector<Tx> SF = S; SF.insert(SF.end(), F.begin(), F.end());
  int bad = 0, total = 0; long first_bad = -1;
  for (long k = 1; k <= N; k++) {
    total++;
    std::vector<Tx> L = capture(op, nullptr, k);                                  // compare match at the k-th register access
    if (!(L == FS || L == SF)) { bad++; if (first_bad < 0) first_bad = k; }
  }
  CHECK(bad == 0, "%s: the Timer1 ISR broke the bus transaction at %d of %d interrupt points (first at register access %ld)", what, bad, total, first_bad);
}
TEST(atomic_write_against_isr, "0003", "A Timer1 interrupt at any point of a foreground write cannot corrupt, lose or move that write") {
  sweep("write", [] { RPU_DataWrite(0x2402, 0x5A); });
}
TEST(atomic_read_against_isr, "0003", "A Timer1 interrupt at any point of a foreground read cannot corrupt or lose it") {
  sweep("read", [] { board.pia[1].crb = 0x3C; byte v = RPU_DataRead(0x2403); (void)v; });
}

// =================================================================================================
// 0001  GameOn / flipper relay is PIA0 CB2 ($2103), low = on
// =================================================================================================
TEST(gameon_boot_state_off_without_touching_latch, "0001", "At boot the library turns GameOn off through PIA0 CB2 and does not strobe the SOL1-8 latch") {
  bring_up(false, true);
  CHECK(writes_to_range(0x2201, 0x2203) == 0, "%d write(s) to $2201-$2203 during init: each strobes SOL1-8 on the real board", writes_to_range(0x2201, 0x2203));
  CHECK(board.raw_on_ns(LATCH_SOLS, fake::now_ns) == 0, "SOL1-8 requested for %.0f us during init (a write to $2203 latches its data)", board.raw_on_ns(LATCH_SOLS, fake::now_ns) / 1000.0);
  CHECK(board.pia[0].cb2() == true, "PIA0 CB2 is low after init: GameOn would be on");
}
TEST(gameon_on_off_polarity_and_register, "0001", "RPU_SetDisableFlippers drives PIA0 CB2: low = GameOn enabled, high = disabled") {
  bring_up();
  RPU_SetDisableFlippers(false);
  CHECK(board.raw_mask() & (1u << GAMEON), "GameOn not enabled (PIA0 CB2 not low) after RPU_SetDisableFlippers(false)");
  CHECK(board.raw_on_ns(LATCH_SOLS, fake::now_ns) == 0, "enabling GameOn strobed the SOL1-8 latch (latch = 0x%02X)", board.latch);
  RPU_SetDisableFlippers(true);
  CHECK(!(board.raw_mask() & (1u << GAMEON)), "GameOn still enabled after RPU_SetDisableFlippers(true)");
  CHECK(writes_to_range(0x2201, 0x2203) == 0, "%d write(s) to $2201-$2203", writes_to_range(0x2201, 0x2203));
}
TEST(gameon_solenoid_stack_enable_respects_game_over, "0001", "Enabling the solenoid stack during game over does not switch the flipper relay on") {
  bring_up();
  RPU_DisableSolenoidStack();
  RPU_EnableSolenoidStack();
  CHECK(!(board.raw_mask() & (1u << GAMEON)), "GameOn enabled by RPU_EnableSolenoidStack while game over");
  CHECK(board.raw_on_ns(LATCH_SOLS, fake::now_ns) == 0, "SOL1-8 requested by Enable/DisableSolenoidStack (latch = 0x%02X)", board.latch);
  RPU_SetDisableFlippers(false);
  RPU_DisableSolenoidStack();
  CHECK(!(board.raw_mask() & (1u << GAMEON)), "GameOn still on after RPU_DisableSolenoidStack");
  RPU_EnableSolenoidStack();
  CHECK(board.raw_mask() & (1u << GAMEON), "GameOn not restored by RPU_EnableSolenoidStack in a running game");
}

// =================================================================================================
// 0002  $2201-$2203 are the SOL1-8 latch; special and continuous solenoid mapping
// =================================================================================================
TEST(special_solenoid_right_coil_only, "0002", "Each special solenoid 17-22 asserts its own CA2/CB2 line and nothing on the SOL1-8 latch") {
  static const char* names[6] = {"17 PIA4 CA2", "18 PIA3 CA2", "19 PIA3 CB2", "20 PIA4 CB2", "21 PIA1 CB2", "22 PIA1 CA2"};
  for (int i = 0; i < 6; i++) {
    std::string r = isolated([&] {
      bring_up();
      RPU_SetDisableFlippers(false);
      RPU_PushToSolenoidStack(16 + i, 3);
      run_slots(8);
      uint32_t want = 1u << (16 + i);
      CHECK(on_slots(want) > 2.5, "solenoid %s was not asserted for its pulse (%.1f slots)", names[i], on_slots(want));
      CHECK(board.raw_on_ns(SPECIALS & ~want, fake::now_ns) == 0, "a different special line was asserted for solenoid %s", names[i]);
      CHECK(board.raw_on_ns(LATCH_SOLS | PORTB_SOLS, fake::now_ns) == 0, "SOL1-16 requested while pulsing solenoid %s (latch = 0x%02X)", names[i], board.latch);
      CHECK(board.v.latch_offset_write == 0, "wrote $2201-$2203 while pulsing solenoid %s", names[i]);
    });
    CHECK(r.empty(), "special solenoid %d: %s", 17 + i, r.c_str());
  }
}
TEST(special_solenoid_released_after_pulse, "0002", "A special solenoid line is released (high) when its pulse ends") {
  bring_up();
  RPU_PushToSolenoidStack(21, 3);
  run_slots(10);
  CHECK((board.raw_mask() & SPECIALS) == 0, "a special line is still asserted after the pulse (mask 0x%06X)", board.raw_mask() & SPECIALS);
  CHECK(on_slots(1u << 21) > 0, "solenoid 22 was never asserted");
}
TEST(continuous_solenoid_right_coil, "0002", "RPU_SetContinuousSolenoid(n) requests SOL n+1 only, via $2200 or PIA0 port B, without reading the write-only latch") {
  for (int n = 0; n < 16; n++) {
    std::string r = isolated([&] {
      bring_up();
      RPU_SetContinuousSolenoid(true, n);
      run_slots(3);
      uint32_t m = board.raw_mask() & (LATCH_SOLS | PORTB_SOLS | SPECIALS);
      CHECK(m == (1u << n), "continuous solenoid %d: requested lines 0x%06X, expected 0x%06X", n, m, 1u << n);
      CHECK(board.v.latch_read == 0, "read of the write-only latch $2200-$2203 (%d)", board.v.latch_read);
      CHECK(board.v.latch_offset_write == 0, "wrote $2201-$2203");
      RPU_SetContinuousSolenoid(false, n);
      run_slots(3);
      CHECK((board.raw_mask() & (LATCH_SOLS | PORTB_SOLS)) == 0, "continuous solenoid %d did not turn off", n);
    });
    CHECK(r.empty(), "solenoid index %d: %s", n, r.c_str());
  }
}
TEST(never_reads_or_writes_latch_offsets, "0002", "Normal solenoid, GameOn and continuous operation never touches $2201-$2203") {
  bring_up();
  RPU_SetDisableFlippers(false);
  RPU_SetContinuousSolenoid(true, 9); RPU_SetContinuousSolenoid(true, 2);
  RPU_PushToSolenoidStack(4, 5); RPU_PushToSolenoidStack(18, 4);
  run_slots(20);
  RPU_SetContinuousSolenoid(false, 9); RPU_SetContinuousSolenoid(false, 2);
  RPU_SetDisableFlippers(true);
  run_slots(4);
  CHECK(board.v.latch_offset_write == 0, "%d write(s) to $2201-$2203", board.v.latch_offset_write);
  CHECK(board.v.latch_read == 0, "%d read(s) of the write-only latch", board.v.latch_read);
}
TEST(testpias_does_not_read_latch, "0002", "RPU_TestPIAs does not read the write-only latch and reports a healthy board as healthy") {
  bring_up();
  unsigned long r = RPU_TestPIAs();
  CHECK(board.v.latch_read == 0, "RPU_TestPIAs read the write-only latch $2200-$2203 (%d time(s)): open bus, never a PIA", board.v.latch_read);
  CHECK(r == 0, "RPU_TestPIAs reported error bits 0x%lX on a healthy board", r);
}

// =================================================================================================
// 0004  Sound commands go to the sound board through the widget PIA
// =================================================================================================
TEST(sound_command_reaches_sound_board, "0004", "RPU_PlayW11Sound and RPU_PlayW11Music each latch one command on the sound board (widget PIA port B, CB2 strobe)") {
  bring_up();
  RPU_PlayW11Sound(0x56);
  RPU_PlayW11Music(0x0A);
  CHECK(board.sound.size() == 2, "sound board latched %zu command(s), expected 2", board.sound.size());
  if (board.sound.size() >= 1) CHECK(board.sound[0].cmd == 0x56, "first command 0x%02X, expected 0x56", board.sound[0].cmd);
  if (board.sound.size() >= 2) CHECK(board.sound[1].cmd == 0x0A, "second command 0x%02X, expected 0x0A", board.sound[1].cmd);
  CHECK(!wrote(0x2100) && !wrote(0x2101), "wrote PIA0 port A / CA2 ($2100/$2101): the System 11 on-board sound path, absent on System 11C");
}
TEST(sound_commands_are_spaced, "0004", "Back-to-back sound commands are at least 1 ms apart so the sound board latches both") {
  bring_up();
  RPU_PlayW11Sound(0x00);
  RPU_PlayW11Sound(0x56);
  CHECK(board.sound.size() == 2, "sound board latched %zu of 2 commands", board.sound.size());
  if (board.sound.size() == 2) {
    uint64_t gap = board.sound[1].ns - board.sound[0].ns;
    CHECK(gap >= 1000000ull, "commands only %llu us apart", (unsigned long long)gap / 1000);
  }
}

// =================================================================================================
// 0005 / 0007  Switch-driven solenoids (jets, slingshots)
// =================================================================================================
static const PlayfieldAndCabinetSwitch kSwitches[] = {
  {9, 16, 0x80 | 9},     // jet:  switch 9 (strobe 1, return 1) -> solenoid 17, 9 hold slots, re-fire
  {17, 17, 8},           // sling: switch 17 (strobe 2, return 1) -> solenoid 18, 8 hold slots, lockout
};
static void switches_up() {
  bring_up();
  RPU_SetupGameSwitches(2, 2, (PlayfieldAndCabinetSwitch*)kSwitches);
  RPU_ClearVariables();
  RPU_SetDisableFlippers(false);       // GameOn: the switch coils only fire in a running game
  run_slots(4);                        // let the switch scan see the open switches
  board.clear_logs();
}
static void hit(int col, int row, int slots_closed) { board.sw[col] |= 1 << row; run_slots(slots_closed); board.sw[col] &= ~(1 << row); }

TEST(jet_fires_from_the_isr, "0005", "A registered jet switch fires its solenoid from the Timer1 ISR, and only while GameOn is on") {
  switches_up();
  RPU_SetDisableFlippers(true);
  hit(1, 1, 3); run_slots(15);
  CHECK(board.raw_on_ns(SPECIALS, fake::now_ns) == 0, "jet fired with GameOn off (attract mode / tilt)");
  RPU_SetDisableFlippers(false);
  hit(1, 1, 3); run_slots(25);
  double s = on_slots(1u << 16);
  CHECK(s >= 9.0 && s <= 12.0, "jet solenoid 17 on for %.1f slots, expected about 10", s);
  CHECK(board.raw_on_ns(SPECIALS & ~(1u << 16), fake::now_ns) == 0, "another special line was asserted");
  CHECK(board.raw_on_ns(LATCH_SOLS | PORTB_SOLS, fake::now_ns) == 0, "SOL1-16 requested by a jet");
}
TEST(sling_fires_and_ignores_hits_during_its_pulse, "0005", "A slingshot fires once; closures during its pulse do not extend or repeat it (lockout)") {
  switches_up();
  hit(2, 1, 3); run_slots(1);                                       // a hit (closed for three switch scans)
  hit(2, 1, 3); run_slots(1);                                       // another during the pulse
  run_slots(25);
  auto iv = intervals(1u << 17);
  CHECK(iv.size() >= 1, "sling did not fire");
  for (auto& i : iv) CHECK(slots_of(i.b - i.a) <= 10.5, "sling pulse lasted %.1f slots (expected about 9): hits stacked pulses", slots_of(i.b - i.a));
  CHECK(on_slots(1u << 17) <= 10.5, "sling on for %.1f slots in total after one burst of hits", on_slots(1u << 17));
}
TEST(jet_refire_is_single_and_spaced, "0007", "A jet hit during its pulse queues exactly one re-fire after a 7-slot gap; duty stays bounded") {
  switches_up();
  hit(1, 1, 3); run_slots(1);                                       // a hit (closed for three switch scans)
  hit(1, 1, 3); run_slots(1);                                       // another during the pulse
  hit(1, 1, 3); run_slots(1);                                       // and a third: still only ONE re-fire
  run_slots(40);
  auto iv = intervals(1u << 16);
  CHECK(iv.size() >= 1, "jet did not fire");
  CHECK(iv.size() == 2, "jet fired %zu times for one burst of hits, expected the pulse plus ONE re-fire", iv.size());
  for (auto& i : iv) CHECK(slots_of(i.b - i.a) <= 11.5, "jet pulse lasted %.1f slots", slots_of(i.b - i.a));
  if (iv.size() >= 2) CHECK(slots_of(iv[1].a - iv[0].b) >= 6.5, "re-fire only %.1f slots after the first pulse (needs >= 7)", slots_of(iv[1].a - iv[0].b));
}
TEST(special_handover_on_preemption, "0011", "A jet pre-empting the running feeder pulse hands the CA2/CB2 line over: never two specials asserted at once") {
  switches_up();
  RPU_PushToSolenoidStack(21, 40);                  // feeder, 40 slots
  run_slots(10);
  hit(1, 1, 3);                                     // a jet hit in the middle of it
  run_slots(60);
  CHECK(board.max_simultaneous(SPECIALS) <= 1, "%d special lines asserted at the same time", board.max_simultaneous(SPECIALS));
  CHECK((board.raw_mask() & SPECIALS) == 0, "a special line is still asserted at the end (mask 0x%06X)", board.raw_mask() & SPECIALS);
  CHECK(on_slots(1u << 16) >= 8.0, "the jet did not fire");
  double fe = on_slots(1u << 21);
  CHECK(fe >= 39.0 && fe <= 42.0, "feeder on for %.1f slots, expected its 40 (no time lost or added)", fe);
}

// =================================================================================================
// 0007  A solenoid-stack entry is one pulse with an on-time
// =================================================================================================
TEST(three_long_pulses_fit, "0007", "Three 85-slot pulses queued together all play in full (one FIFO entry each)") {
  bring_up();
  RPU_PushToSolenoidStack(0, 85); RPU_PushToSolenoidStack(1, 85); RPU_PushToSolenoidStack(2, 85);
  run_slots(300);
  for (int c = 0; c < 3; c++) {
    double s = on_slots(1u << c);
    CHECK(s >= 84.0 && s <= 86.0, "coil %d played %.1f slots of the 85 requested", c + 1, s);
  }
}
TEST(coin_lockout_default_does_not_fire_sol1, "0007", "RPU_SetCoinLockout's default index (0x20) does not turn on SOL1 (the outhole kicker)") {
  bring_up();
  RPU_SetCoinLockout(false);
  run_slots(6);
  CHECK(board.raw_on_ns(1u << 0, fake::now_ns) == 0, "SOL1 requested (latch = 0x%02X): the mask 0x20 was used as a shift count; 1 << 32 wraps to bit 0 on a 32-bit shift", board.latch);
}

// =================================================================================================
// 0009  Hold-PWM
// =================================================================================================
TEST(hold_pwm_duty_from_isr, "0009", "RPU_SetSolenoidHoldPwm holds one coil at 2 slots on in 16 (12.5%) from the ISR, with no further calls") {
  bring_up();
  RPU_SetSolenoidHoldPwm(13, 2, 16);
  run_slots(16 * 20);
  note_absent();
  auto iv = intervals(1u << 13);
  CHECK(iv.size() >= 19, "coil 14 pulsed %zu times in 20 periods", iv.size());
  for (auto& i : iv) CHECK(slots_of(i.b - i.a) >= 1.8 && slots_of(i.b - i.a) <= 2.2, "pulse of %.2f slots, expected 2", slots_of(i.b - i.a));
  double duty = on_slots(1u << 13) / (16.0 * 20);
  CHECK(duty > 0.115 && duty < 0.135, "duty %.3f, expected 0.125", duty);
  CHECK(board.raw_on_ns(~(1u << 13) & (LATCH_SOLS | PORTB_SOLS), fake::now_ns) == 0, "another coil was requested");
}
TEST(hold_pwm_stops_by_itself, "0009", "The hold-PWM ends by itself after 5300 slots (10.98 s) if the foreground never stops it") {
  bring_up();
  RPU_SetSolenoidHoldPwm(13, 2, 16);
  run_slots(5300 + 40);
  uint64_t t_end = fake::now_ns;
  CHECK(on_slots(1u << 13) > 600, "hold never ran (%.0f slots on)", on_slots(1u << 13));
  run_slots(100);
  CHECK(board.raw_on_ns(1u << 13, fake::now_ns) == board.raw_on_ns(1u << 13, t_end), "coil 14 still pulsing after the ISR's own limit");
  CHECK(!(board.raw_mask() & (1u << 13)), "coil 14 left on");
}

// =================================================================================================
// 0010  Shared-driver busy flag
// =================================================================================================
TEST(busy_shared_tracks_drivers_1_to_8, "0010", "RPU_SolenoidStackBusyShared is true while a pulse on drivers 1-8 is queued or playing, false otherwise") {
  bring_up();
  CHECK(!RPU_SolenoidStackBusyShared(), "busy with nothing queued");
  RPU_PushToSolenoidStack(4, 5);
  CHECK(RPU_SolenoidStackBusyShared(), "not busy with a driver 5 pulse queued");
  run_slots(2);
  CHECK(RPU_SolenoidStackBusyShared(), "not busy while a driver 5 pulse is playing");
  run_slots(10);
  CHECK(!RPU_SolenoidStackBusyShared(), "still busy after the pulse ended");
  RPU_PushToSolenoidStack(12, 5);                 // driver 13: not on the A/C relay's drivers
  CHECK(!RPU_SolenoidStackBusyShared(), "busy for a pulse on driver 13");
  run_slots(10);
  RPU_PushToSolenoidStack(18, 5);                 // special: no
  CHECK(!RPU_SolenoidStackBusyShared(), "busy for a special solenoid");
  note_absent();
}

// =================================================================================================
// 0011  Coil safety at the output scheduler
// =================================================================================================
TEST(special_handover_back_to_back, "0011", "Two special solenoid pulses queued back to back are never asserted at the same time") {
  bring_up();
  RPU_PushToSolenoidStack(16, 4);
  RPU_PushToSolenoidStack(17, 4);
  run_slots(14);
  CHECK(board.max_simultaneous(SPECIALS) <= 1, "%d special lines asserted at once: the first line was left on when the second took over", board.max_simultaneous(SPECIALS));
  CHECK((board.raw_mask() & SPECIALS) == 0, "a special line is still asserted at the end");
}
TEST(continuous_deadline_enforced_by_isr, "0011", "RPU_SetContinuousSolenoidMax drops the output itself at its deadline when the foreground stalls") {
  bring_up();
  RPU_SetContinuousSolenoidMax(true, 13, 651);
  run_slots(600);                                   // the foreground never calls again
  note_absent();
  double s = on_slots(1u << 13);
  CHECK(s > 100, "coil never driven (%.1f slots)", s);
  CHECK(s >= 312.0 && s <= 316.0, "coil on for %.1f slots, expected 314 (651 ms in 2-slot units)", s);
  CHECK(!(board.raw_mask() & (1u << 13)), "coil still on at the end");
}
TEST(plain_continuous_call_has_no_deadline, "0011", "A plain RPU_SetContinuousSolenoid (GI, A/C relay) keeps its output until told otherwise") {
  bring_up();
  RPU_SetContinuousSolenoidMax(true, 13, 651);
  RPU_SetContinuousSolenoid(true, 13);              // replaces the deadline
  run_slots(600);
  note_absent();
  CHECK(board.raw_mask() & (1u << 13), "output dropped although the plain call cancelled the deadline");
}
TEST(same_coil_minimum_off_time, "0011", "The same ordinary coil never restarts within 15 idle slots (31 ms) of its last pulse") {
  bring_up();
  RPU_PushToSolenoidStack(2, 10);
  RPU_PushToSolenoidStack(2, 10);
  run_slots(70);
  auto iv = intervals(1u << 2);
  CHECK(iv.size() == 2, "coil 3 pulsed %zu time(s), expected 2 separate pulses", iv.size());
  if (iv.size() >= 2) CHECK(slots_of(iv[1].a - iv[0].b) >= 14.5, "coil 3 off for only %.1f slots between pulses (needs 15): the two pulses merged", slots_of(iv[1].a - iv[0].b));
}

// =================================================================================================
// 0012  Init order after an AVR-only reset
// =================================================================================================
TEST(init_clears_requests_before_blanking_retriggers, "0012", "RPU_InitializePIAs writes every solenoid output off before the display PIA can retrigger BLANKING") {
  int bad = 0; std::string first;
  for (int strobe = 0; strobe < 16; strobe++) {
    std::string out;
    std::string r = isolated([&] {
      bring_up(true);
      load_running_game_state((uint8_t)strobe);
      uint64_t t0 = fake::now_ns;
      RPU_InitializePIAs();
      uint64_t e = board.energised_ns(LATCH_SOLS | PORTB_SOLS | SPECIALS, fake::now_ns);
      char buf[200];
      snprintf(buf, sizeof buf, "retained requests were live for %.0f us with BLANKING enabled (strobe %d)", e / 1000.0, strobe);
      CHECK(e == 0, "%s", buf);
      (void)t0;
    });
    if (!r.empty()) { if (!bad) first = r; bad++; }
  }
  CHECK(bad == 0, "%d of 16 retained display-strobe values: %s", bad, first.c_str());
}

// =================================================================================================
// 0008  Lamp strobe order
// =================================================================================================
TEST(lamp_rows_match_column_when_selected, "0008", "Whenever a lamp column is selected, the row lines already hold that column's data (no ghost from the previous column)") {
  bring_up();
  for (int c = 0; c < 8; c++) RPU_SetLampState(c * 8 + c, 1);        // one lamp per column, a different row each
  run_slots(10);
  board.lamp_drive.clear();
  run_slots(40);                                                     // five full lamp passes
  int ghosts = 0, selected = 0;
  for (auto& d : board.lamp_drive) {
    if (!d.cols) continue;
    selected++;
    int c = 0; while (!(d.cols & (1 << c)) && c < 8) c++;
    if ((d.cols & (d.cols - 1)) != 0 || d.rows != LampStates[c]) ghosts++;
  }
  CHECK(selected > 0, "no lamp column was ever selected");
  CHECK(ghosts == 0, "%d of %d column selects had the wrong row data on the lines (previous column's rows lit on the new column)", ghosts, selected);
}

// =================================================================================================
// e56a2fb  Raw alpha display scan, Diner display PIA setup
// =================================================================================================
TEST(alpha_scan_nine_write_sequence, "alpha", "Each display strobe writes the stock nine-write sequence (alpha ports blanked, strobe, then the inverted words)") {
  bring_up();
  static uint16_t cells[2][16];
  for (int d = 0; d < 16; d++) { cells[0][d] = (uint16_t)(0x1111 * (d + 1) ^ 0x0A55); cells[1][d] = (uint16_t)(0x0101 * (d + 3) ^ 0x1234); }
  bool ok = RPU_SetAlphaRawFrame(cells);
  CHECK(ok, "RPU_SetAlphaRawFrame refused the frame");
  run_ticks(18);                                                     // let the frame take effect at digit 0
  board.acc.clear();
  run_ticks(16);                                                     // one full 16-digit scan: one digit per Timer1 tick
  note_absent();
  // keep the display writes only
  std::vector<DevAccess> w;
  for (auto& a : board.acc) if (a.write && (a.addr == 0x2C02 || a.addr == 0x2C00 || a.addr == 0x2802 || a.addr == 0x3400 || a.addr == 0x2800)) w.push_back(a);
  int groups = 0, wrong = 0;
  std::string first;
  for (size_t i = 0; i + 9 <= w.size(); i += 9) {
    int digit = w[i + 4].data & 0x0F;
    uint16_t up = cells[0][digit], lo = cells[1][digit];
    uint8_t sp = digit == 15 ? 0x80 : 0;
    const uint16_t want_addr[9] = {0x2C02, 0x2C00, 0x2802, 0x3400, 0x2800, 0x2C02, 0x2C00, 0x2802, 0x3400};
    const int want_data[9] = {0xFF, 0xFF, 0xFF, 0xFF, -1, (uint8_t)(~up) | sp, (uint8_t)(~(up >> 8)) | sp, (uint8_t)(~lo) | sp, (uint8_t)(~(lo >> 8)) | sp};
    bool good = true;
    for (int k = 0; k < 9; k++) {
      if (w[i + k].addr != want_addr[k]) good = false;
      if (want_data[k] >= 0 && w[i + k].data != (uint8_t)want_data[k]) good = false;
    }
    groups++;
    if (!good) { wrong++; if (first.empty()) { char b[80]; snprintf(b, sizeof b, "digit %d", digit); first = b; } }
  }
  CHECK(w.size() == 16 * 9, "%zu display writes in 16 strobes, expected %d", w.size(), 16 * 9);
  CHECK(wrong == 0, "%d of %d display strobes did not match the stock sequence (first: %s)", wrong, groups, first.c_str());
}
TEST(display_pia_pa7_stays_input, "alpha", "The display PIA keeps PA7 an input (W7 on the stock board); PA0-PA6 outputs") {
  bring_up();
  CHECK(board.pia[2].ddra == 0x7F, "display PIA DDRA is 0x%02X, expected 0x7F (PA7 is an input on the Diner board)", board.pia[2].ddra);
}

// =================================================================================================
int main(int argc, char** argv) {
  if (argc >= 2 && !strcmp(argv[1], "--list")) {
    for (auto& t : registry()) printf("%s\t%s\t%s\n", t.id, t.fix, t.statement);
    return 0;
  }
  if (argc < 2) { fprintf(stderr, "usage: %s --list | <test id>\n", argv[0]); return 2; }
  for (auto& t : registry()) {
    if (strcmp(t.id, argv[1])) continue;
    t.fn();
    std::string reasons;
    for (size_t i = 0; i < g_fail.size() && i < 4; i++) { if (i) reasons += " | "; reasons += g_fail[i]; }
    if (g_fail.size() > 4) reasons += " | (+" + std::to_string(g_fail.size() - 4) + " more)";
    printf("RESULT\t%s\t%s\t%s\t%s\n", t.id, g_fail.empty() ? "PASS" : "FAIL", t.fix, reasons.c_str());
    return g_fail.empty() ? 0 : 1;
  }
  fprintf(stderr, "no such test: %s\n", argv[1]);
  return 2;
}
