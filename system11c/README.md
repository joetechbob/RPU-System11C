# RPU for Williams System 11C (local fork)

This is a private fork of the RPU library for one machine: a Williams System 11C pinball
(Diner) with an RPU Rev 102 in the CPU socket. It is the owner's own working copy.

**This fork is not intended for upstream.** It is not a proposal, nothing here is written for
anyone else to accept, and it will not be offered. It is shared, if at all, as-is: read it, copy
what is useful, run it at your own risk.

> **Safety.** This code drives real 50 V solenoid coils. The only supervision the board gives the
> coil drivers is the BLANKING one-shot, which watches the display strobe, not the coil logic, and
> the RPU image has no watchdog of its own. A wrong write can hold a coil on and burn it out.
> **Nothing in this fork has been run on a real board.** Every fix and every test was checked
> in emulation or in the host model in `tests/` only. Before the first power-up on a machine:
> fuses in place, coil drivers watched, one coil at a time, and a hand on the power switch.
> See [HARDWARE.md](HARDWARE.md) for what is and is not known about the board.

## What is in the box

| Path | What |
|---|---|
| `RPU.cpp`, `RPU.h`, `RPU_Config.h`, `RPU_AlphaScan.h` | the library, with the System 11C changes |
| `system11c/FIXES.md` | each fix: the problem, the evidence, what changed, the test, the verification status |
| `system11c/HARDWARE.md` | the bus, E and reset, pin map, solenoid map, timing table, schematic references |
| `system11c/tests/` | host tests that show why each fix is needed, and the runner |
| everything else | upstream, untouched (the example machine, schematics, PDFs) |

The example machine in the repository root (`ExampleMachineMEGA.ino` and friends) is upstream's. It
has not been adapted or tried for System 11C.

## How this relates to upstream

* Upstream is RetroPinUpgrade/ExampleMachine (GPL-3.0). The git remote `upstream` points at it.
* Branch `main` holds upstream, unchanged (`21b13a3` when this fork was made).
* Branch `system11c` is `main` plus the System 11C work: the alpha display scan, the System 11C
  configuration, fixes 0001-0013 (15 commits, `git log --oneline 21b13a3..system11c`), and then
  these documents and the tests.
* The fixes change code in `RPU.cpp`, `RPU.h`, `RPU_Config.h`, `RPU_AlphaScan.h` only. The docs and
  tests are in `system11c/`. Comments added later (to say why) do not change any code: that is checked
  with `tests/run.sh codediff`.
* The library is GPL-3.0 like upstream; `LICENSE` is upstream's.

## Using the library in a sketch

1. Use an Arduino Mega 2560 on an RPU Rev 102, in the CPU socket of a System 11C board.
2. Put the four files in the sketch folder (or an Arduino library folder):
   `RPU.cpp`, `RPU.h`, `RPU_Config.h`, `RPU_AlphaScan.h`.
3. Leave `RPU_Config.h` as it is. It sets Rev 102, architecture 15 (System 11), 6802/6808 mode (the
   Mega makes E), the W-type 11 sound path, and `RPU_OS_USE_DINER_ALPHA_SCAN 1` (the raw alpha scan,
   used only on this exact combination, and only for a 7-digit ATmega2560 build).
4. `RPU.h` does not declare the functions added by this fork. Declare them in the sketch:

   ```cpp
   void RPU_SetSolenoidHoldPwm(byte solNum, byte onSlots, byte periodSlots);   // 0009
   void RPU_SetContinuousSolenoidMax(boolean solOn, byte solNum, unsigned short maxMs);  // 0011
   boolean RPU_SolenoidStackBusyShared();                                       // 0010
   ```
   `RPU_SetAlphaRawFrame()` and `RPU_ReleaseAlphaRawFrame()` are declared in `RPU.h`.

Behaviour that differs from upstream, and that a sketch must know about:

* Solenoid numbers are library indices: Williams number minus 1. 0-7 are SOL1-8, 8-15 are SOL9-16,
  16-21 are the special solenoids 17-22.
* `RPU_PushToSolenoidStack(n, N)`: N is now N consecutive solenoid slots (2.0718 ms each) in one
  FIFO entry, at most 255. The stack holds 32 entries.
* The same ordinary coil cannot start again within 15 idle slots (31 ms) of its last pulse.
* `RPU_SetContinuousSolenoid()` takes effect on the next solenoid pass (up to 2.07 ms later), not
  immediately. Indices of 16 or more are ignored. A plain call clears any deadline set by
  `RPU_SetContinuousSolenoidMax()`.
* `RPU_SetDisableFlippers(false)` turns GameOn on (PIA0 CB2 low). Jets and slings registered with
  `RPU_SetupGameSwitches()` fire from the Timer1 ISR only while GameOn is on. The switch table must
  be in PROGMEM, in the lower 64 KiB of flash.
* `RPU_PlayW11Sound()` and `RPU_PlayW11Music()` wait 1 ms after each command (the foreground
  blocks, Timer1 keeps running).
* Every bus access holds off the Timer1 interrupt for about 3.5-4 us.

## Running the tests

Needs a C++17 compiler (`c++`, `clang++` or `g++`), `git`, and `python3`. Nothing else for the host
tests; they take a few seconds.

```
system11c/tests/run.sh            # run the tests on this branch: all must PASS
system11c/tests/run.sh upstream   # run the same tests on upstream RPU.cpp (21b13a3): all must FAIL, with the reason
system11c/tests/run.sh both       # both, then one table: fails on upstream / passes on branch
system11c/tests/run.sh list       # list the tests and the hazard each one states
system11c/tests/run.sh mutants    # break each fix in a scratch copy; the tests must notice
system11c/tests/run.sh codediff   # code identical to HEAD apart from comments?  (add a ref: codediff 42aa5ea)
system11c/tests/run.sh pulse      # optional: count AVR cycles of the E pulse in the compiled code
```

Exit status is non-zero when a test fails on the branch, or when a test passes on upstream (a test
that passes on upstream does not demonstrate its fix).

Upstream RPU.cpp is read with `git show 21b13a3:RPU.cpp`; its Rev 3 / architecture 1 configuration
is changed to Rev 102 / architecture 15 / 6802 mode / W-type 11 sound, which is how the branch is
configured, and nothing else. `UPSTREAM_COMMIT` at the top of `run.sh` names the commit.

How the tests work:

* `harness/fakeavr.*`: every AVR register the library touches (`PORTA`, `PINA`, `DDRA`, `PORTG`,
  `TIMSK1`, ...) is a `Reg` object that logs every access, charges AVR cycles, and advances a model
  clock. A register access is also where a Timer1 interrupt can arrive.
* `harness/board.*`: a model of the System 11C CPU board seen from the socket. It decodes the address,
  VMA, R/W and E pins into bus transactions, and models the U28 latch at `$2200-$2203` (write only,
  A1:A0 ignored), six MC6821 PIAs with their CA2/CB2 lines, the lamp matrix, the switch matrix, the
  sound-board strobe, and the BLANKING one-shot (36 ms, retriggered by display PIA PA2). The
  solenoid request lines, and the E pulse widths and sample points, are logged as they happen.
* `harness/tests.cpp`: the tests. Each states a hazard plainly, for example "a write to `$2201`
  strobes coils on the real board" or "E is low between transactions", and checks the library against
  it. Tests run in separate processes because the library keeps global state.
* `harness/shim/`: the minimum of `Arduino.h`, `avr/io.h`, `avr/interrupt.h` and so on to compile
  `RPU.cpp` unmodified. The runner makes one change to a staged copy of `RPU.cpp`: the single
  inline-assembly `rjmp .+0` (a 2-cycle wait) becomes a call that advances the model clock by 2 cycles.
  The branch's `RPU.cpp` is never modified.
* `harness/upstream_absent.cpp`: linked only for upstream. Four functions exist only on this
  branch (`RPU_SetContinuousSolenoidMax`, `RPU_SetSolenoidHoldPwm`, `RPU_SolenoidStackBusyShared`,
  `RPU_SetAlphaRawFrame`). Upstream gets do-nothing stubs so the tests build, and the failure
  says "not in upstream, stubbed".
* `check_pulse_cycles.py` (`run.sh pulse`): disassembles a compiled `RPU_DataWrite` /
  `RPU_DataRead` with `avr-objdump` (from the Arduino IDE's avr-gcc 7.3; set `AVR_TOOLS` if it is
  elsewhere) and counts the cycles between the E rise and fall (write) or the PINA sample (read).
  With no file argument it builds an object from `RPU.cpp` with `avr-g++` and the installed Arduino
  AVR core. Give it an ELF or object of your own build to check that instead. `run.sh pulse upstream`
  runs the same count on upstream, where it fails.

What the tests do not show: that anything works on the real board. See "Verification" in
[FIXES.md](FIXES.md) and the limits in [HARDWARE.md](HARDWARE.md).

## Rebasing onto a newer upstream

The fork is a stack of commits on top of an upstream commit, so it moves with a rebase.

```
git fetch upstream
git rebase --onto upstream/main 21b13a3 system11c      # 21b13a3 = the upstream commit this fork started from
```

Expect conflicts in `RPU.cpp`: the bus functions (`RPU_DataWrite`, `RPU_DataRead`), the Timer1
ISR, `RPU_InitializePIAs`, `RPU_TestPIAs`, and the solenoid functions are where this fork changes the
most. One conflict has already been resolved by hand once and will probably come back: upstream
commit `d28a14f` changed the check in `RPU_TestPIAs` for the solenoid PIA's control B (it expects
`$34` on the older Williams systems and `$3C` on System 11). Fix 0002 replaces that read on
architecture 15 with PIA0 control B (`$2103`, expecting `$3C`, because `$2203` is the SOL1-8 latch and
reads as open bus), and keeps upstream's check for every other architecture.

After the rebase:

1. Change `UPSTREAM_COMMIT` in `tests/run.sh` to the new base commit.
2. `tests/run.sh both`. On the branch every test must pass. On the new upstream every test must still
   fail; if one now passes, upstream fixed that hazard and the fork's fix can be dropped or reduced.
3. `tests/run.sh mutants` (the mutants patch exact text in `RPU.cpp`; update a pattern if the line moved).
4. Read the diff of the library against the old branch tip. Coil-safety code (the Timer1 solenoid
   pass, `PullFirstFromSolenoidStack`, the bus functions) deserves a line-by-line look, not only a
   green test run.

## Limits

* Validated in emulation and in a host model only. Never on a board, never with a scope.
* The model encodes what the schematic and the stock ROM's behaviour say; where the model and the
  board disagree the board wins. [HARDWARE.md](HARDWARE.md) lists what is measured, what is read from
  the schematic, and what is a guess.
* No hardware watchdog: if Timer1 stops, the coil drivers keep their last state (fixes 0009-0011 cannot
  act without the ISR).
