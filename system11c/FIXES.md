# Fixes in branch `system11c`

Branch `system11c` is upstream `21b13a3` plus 15 commits. Two come first (the alpha display scan and
the System 11C configuration); then fixes 0001-0013, numbered as in the original patch series. This file
has one section per commit: the problem on the real board, the evidence, what changed, what goes wrong
without it, the test that shows it, and how far it has been verified.

This fork is for the owner's own machine and is not intended for upstream. Nothing here is a proposal to
anyone else.

**Verification, in one place.** Everything below was checked in emulation (a PinMAME run of the stock ROM,
an AVR emulator, cycle counts) and in the host tests in `tests/`. **Nothing has been run on a real board and
nothing has been measured with a scope.** "Host test" means a test in `tests/harness/tests.cpp` run by
`tests/run.sh`. "Fails on upstream" means it fails against upstream `21b13a3` configured for Rev 102,
architecture 15, 6802 mode (`tests/run.sh upstream`).

Evidence sources used throughout:

* **Schematic:** Williams System 11C CPU schematic 16-9019 (Diner Operations Manual, pages DINER 85-88),
  and the RPU Rev 102 schematic in `Schematics/`. See [HARDWARE.md](HARDWARE.md).
* **Stock ROM in PinMAME:** the stock game ROM run headless, with every PIA write logged. Only addresses and
  line names are quoted here, never ROM contents.
* **MAME/PinMAME `s11.c`:** the emulated System 11 board (`pia0cb2_w`, `pia5b_w`, `pia5cb2_w`, ...).

Test results for the table at the end are from `tests/run.sh both`.

---

## Alpha display scan (`e56a2fb`)

**Problem.** The LA-4 alpha displays are driven by the stock ROM with a four-byte, two-row sequence per
display strobe. The library's own scan writes four bytes per strobe (one 14-segment word split in two, and the numeric digit) and does not use that sequence.
The display PIA's PA7 is an input on the stock board (it is wired to W7); the library initialised it as an
output.

**Evidence.** The stock ROM's display routine at `$D28B-$D2E2`: nine writes per strobe in a fixed order, data inverted,
bit 7 set on the data of strobe 15 (this is what `RPU_AlphaScan.h` reproduces). Stock init at `$D51B` makes PA0-PA6
outputs and PA7 an input.

**What changed.**
* `RPU_AlphaScan.h`: the nine-write sequence for one strobe (`$2C02, $2C00, $2802, $3400` blanked, `$2800` strobe,
  then the four inverted words).
* `RPU_SetAlphaRawFrame()` / `RPU_ReleaseAlphaRawFrame()`: a 2 x 16 frame of finished words, triple-buffered,
  taken at digit 0.
* The display ISR plays the raw frame when one is set, and otherwise builds the words from the text and digit
  buffers and plays them the same way.
* `RPU_InitializePIAs()` (alpha build): display PIA DDRA `0x7F`, alpha and numeric ports idle at `0xFF`.
* Hardening that does not change behaviour for valid input: `RPU_SetDisplayText` keeps the font index inside the
  table and ignores a null string, credit and ball displays ignore negative numbers, `CheckSwitchStack` cannot loop
  forever on a corrupt index, `RPU_ClearVariables` clears the raw frame and the display state, the constant tables
  live in flash (`RPU_OS_PROGMEM`) to save SRAM.
* Enabled only for `RPU_OS_USE_S11C_ALPHA_SCAN`, Rev 102, architecture 15, 6802 mode, 7 digits, ATmega2560.
  Every other build keeps the original scan.

**Without it.** The alpha displays are not driven the way the stock board drives them, and PA7 is driven against whatever the
board puts on W7.

**Tests.** `alpha_scan_nine_write_sequence` (each of the 16 strobes writes the nine addresses in order with the
inverted data, and strobe 15 keeps bit 7), `display_pia_pa7_stays_input` (display PIA DDRA is `0x7F`).

**Fails on upstream.** Yes, both. Upstream has no `RPU_SetAlphaRawFrame` (stubbed; the test says so), writes 4
display writes per strobe instead of 9, and sets DDRA to `0xFF`.

**Gap.** The hardening items (text index clamp, negative credit, `CheckSwitchStack` bound, flash tables,
`ClearVariables`) have no test: they change nothing for valid input, and an out-of-range read on the host is
not observable without a sanitizer.

**Verified.** Host test. Emulation of the stock ROM and display. Not on hardware.

---

## System 11C configuration (`7ac2c87`)

**Problem.** Upstream ships `RPU_Config.h` for Rev 3 / architecture 1 / 6800.

**What changed.** `RPU_OS_HARDWARE_REV 102`, `RPU_MPU_ARCHITECTURE 15`, `RPU_MPU_BUILD_FOR_6800 0` (the Mega
generates E), `RPU_OS_USE_WTYPE_11_SOUND`, `RPU_OS_USE_S11C_ALPHA_SCAN 1`; `RPU_OS_USE_WAV_TRIGGER_1p3` removed;
trailing whitespace trimmed. (Both lines of the diff that look like noise are the whitespace.)

**Without it.** The library builds for a different board.

**Test.** None, and none is possible: a configuration cannot fail against the same configuration. The test
runner configures upstream exactly as the branch is configured (see `stage()` in `run.sh`), which is what lets the
other tests isolate the library changes.

**Verified.** It compiles (host and avr-gcc 7.3).

---

## 0001: GameOn / flipper relay is PIA0 CB2 (`3e7e821`)

**Problem.** For architectures of 10 and up the library switches the flipper and solenoid enable line by writing
`PIA_SOLENOID_CONTROL_B`, `$2203`. On System 11C there is no PIA at `$2200-$2203`: that is the write-only U28 latch for
SOL1-8, decoded without A1:A0. The write strobes the latch with `0x34` or `0x3C`: a pulse on SOL3, 5 and 6, or on SOL3-6. The
real GameOn line (solenoid 23) is CB2 of PIA0, control register `$2103`, and its polarity is the other way round:
CB2 low is enabled.

**Evidence.** Schematic (U28 strobe, decode of `2000 LATCH`). Stock ROM in PinMAME: during attract only `0x3C`
(and the `0x38/0x3C` DDR refresh) is written to `$2103`; at game start `$2103 <- 0x34` and the emulated SOL 23
comes on; the ROM boots PIA0 with `$2103 <- 0x3C, 0x38, 0x3C`. `s11.c` `pia0cb2_w`: GameOn is `!data`.

**What changed (architecture 15 only).**
* `RPU_SetDisableFlippers()`: `$2103 <- 0x3C` (disabled) or `0x34` (enabled).
* `RPU_DisableSolenoidStack()` / `RPU_EnableSolenoidStack()`: same line; Enable does not raise GameOn while
  game over is set.
* `RPU_InitializeMPUArch10()`: the boot write that turns the solenoids off is `$2103 <- 0x38`.

**Without it.** The flippers are never enabled, and every GameOn, stack enable or boot write pulses SOL3-6 on
the latch (flasher or coil on a real machine).

**Tests.** `gameon_boot_state_off_without_touching_latch`, `gameon_on_off_polarity_and_register`,
`gameon_solenoid_stack_enable_respects_game_over`.

**Fails on upstream.** Yes, all three: writes to `$2201-$2203` (the model's latch takes them), GameOn never goes
low, SOL1-8 requested.

**Verified.** Host test, PinMAME. Not on hardware.

---

## 0002: never touch `$2201-$2203`; special and continuous solenoid map (`820cd51`)

**Problem.** The library wrote `$2201` for the special solenoids, read and wrote `$2200/$2202` for continuous
solenoids, and read `$2201/$2203` in `RPU_TestPIAs`. `$2200` is the write-only U28 latch (SOL1-8) with A1:A0 ignored:
a write to `$2201-$2203` strobes SOL1-8 with the byte written (CRA `0x3C` pulses SOL3-6, `0x34` pulses SOL3, 5, 6), and a
read is open bus. The legacy table for special solenoids is the System 9 / 11 (not 11C) assignment, wrong for every
one of 17-22.

**Evidence.** Schematic (latch and PIA decode). Stock ROM in PinMAME: for each special-solenoid event the PIA
control register whose CA2/CB2 field was `110` while the coil was on was SOL17 `$3001`, SOL18 `$2C01`, SOL19 `$2C03`,
SOL20 `$3003`, SOL21 `$2403`, SOL22 `$2401` (always together with `$2103` CB2 low). `$2200` is only ever written at
offset 0 (79 writes, single-bit values); the ROM never writes `$2201-$2203`. The map is in
[HARDWARE.md](HARDWARE.md).

**What changed (architecture 15).**
* The Timer1 ISR uses `S11CSpecialSolenoidControl[]` (`0x34` on, `0x3C` off), the same number of bus writes as before.
* `RPU_SetContinuousSolenoid()` no longer reads the latch or writes `$2202`. It updates the shadow
  `ContinuousSolenoidBits` atomically; the solenoid pass (every 2.07 ms) writes `$2200` and `$2102`. Latency: up to 2.07 ms.
* `RPU_TestPIAs()` checks PIA0 control B (`$2103`, expects `0x3C`) instead of reading `$2201/$2203`.
* `PIA_SOLENOID_CONTROL_A/B` and `PIA_SOLENOID_PORT_B` are redefined to undeclared identifiers, so any remaining use
  fails to compile.

**The one hand-resolved conflict.** Upstream commit `d28a14f` ("RPU_TestPIAs: expect $34 on the Williams
solenoid PIA's control B") changed the same lines of `RPU_TestPIAs` to expect `0x34` below architecture 15 and `0x3C` on
System 11. Resolution: the architecture 15 path reads `PIA_SOLENOID_11_CONTROL_B` and expects `0x3C`; the other
architectures keep upstream's code, including its `0x34` check, unchanged.

**Without it.** Special solenoid pulses fire the wrong coil, or strobe SOL1-8 (a pulse on SOL3, 5, 6 for a write of `0x34`);
continuous-solenoid changes write wrong coils (SOL9-16 requests go to the SOL1-8 latch) and read garbage;
`RPU_TestPIAs` reports a fault on a good board.

**Tests.** `special_solenoid_right_coil_only` (each of 17-22 asserts its own line, nothing else, nothing on the
latch), `special_solenoid_released_after_pulse`, `continuous_solenoid_right_coil` (all 16 indices, one coil each, no
latch read), `never_reads_or_writes_latch_offsets`, `testpias_does_not_read_latch`.

**Fails on upstream.** Yes, all five. Upstream special solenoids do not assert their lines; its latch is read and
written at offsets 1-3; `RPU_TestPIAs` reports error bit `0x20` on a healthy board.

**Gap.** The compile-time poisoning of the legacy names cannot be tested at run time.

**Verified.** Host test, PinMAME. Not on hardware.

---

## 0003: bus transactions are atomic against the Timer1 ISR (`7e0298c`)

**Problem.** The Timer1 ISR (lamps, switches, display, solenoids; 30 and 12-18 bus accesses per pass) uses the same
bus pins as foreground code. An interrupt in the middle of a foreground `RPU_DataWrite` or `RPU_DataRead` changes the
address, R/W, VMA or data under it: the access is lost, goes to the wrong address, or carries the wrong data.
On a solenoid board a wrong address can be a coil.

**Evidence.** AVR emulator sweep (25 interrupt offsets across one write): 13 corrupted, 5 lost, 7 fine.

**What changed.** For one transaction the `OCIE1A` bit of `TIMSK1` is cleared, then restored. A compare match during
the window stays latched in `OCF1A` and runs when `TIMSK1` is restored, as with `cli`. Masking `TIMSK1` rather than saving
`SREG` and using `cli` avoids an `out SREG` in code reachable from an ISR, which the owner's ISR-nesting check cannot
classify. Cost: Timer1 is held off for about 3.5-4 us per access.

**Without it.** Random corrupted bus accesses, at the ISR rate, with no pattern.

**Tests.** `atomic_write_against_isr`, `atomic_read_against_isr`. Each runs the real ISR and injects a compare match at
every register access of a foreground write (or read) in turn, in a fresh process each time, and requires the
resulting bus transactions to be the foreground access then the ISR's, or the ISR's then the foreground access, with the
bus never changing under a transaction.

**Fails on upstream.** Yes. Upstream breaks the transaction at 12 of 19 interrupt points for a write and 8 of 20 for a read.

**Gap.** Interrupts are placed between register accesses, not between every AVR instruction; the register accesses are
the only places the bus can change, so no state is missed. Timer0 and USART0 are not modelled (they do not use the bus).

**Verified.** Host test, AVR emulator. Not on hardware.

---

## 0004: sound goes to the sound board through the widget PIA (`efeb553`)

**Problem.** System 11C has no on-board sound. `RPU_PlayW11Sound` wrote PIA0 port A (`$2100`) and strobed CA2 (`$2101`),
the System 11 / 11A on-board path, so every effect command was lost. Only `RPU_PlayW11Music`, which already used
the widget PIA, reached the sound board.

**Evidence.** `s11.c`: `pia5b_w` (port B `$3402`) feeds the sound board's data, `pia5cb2_w` (CB2) its strobe; the stock ROM
sends every effect and the music that way. Measured in PinMAME against the S11C sound board: two commands issued 11 us
apart lose the second; 100 us still lost one; 300 us and 1000 us delivered both.

**What changed (architecture 15).** `RPU_PlayW11Sound` and `RPU_PlayW11Music` share one sequence: data on `$3402`, CB2 low (`$3403 <- 0x34`),
CB2 high (`0x3C`), then wait `RPU_S11C_SOUND_COMMAND_GAP_US` (1000 us, overridable). `$2100/$2101` are not written after init.

**Without it.** No sound effects. A second command sent soon after the first is dropped.

**Tests.** `sound_command_reaches_sound_board` (two commands, both latched in order, nothing written to `$2100/$2101`),
`sound_commands_are_spaced` (at least 1 ms between strobes).

**Fails on upstream.** Yes. One of two commands is latched, and `$2100/$2101` are written.

**Gap.** The 1 ms gap comes from the PinMAME sound board model. The real sound board's latency has not been measured; tune
`RPU_S11C_SOUND_COMMAND_GAP_US` if a pair drops on the machine.

**Verified.** Host test, PinMAME. Not on hardware.

---

## 0005: switch-driven solenoids in the Timer1 ISR (`9b2c063`)

**Problem.** `RPU_SetupGameSwitches()` registers a switch-to-solenoid table (pop bumpers, slingshots), but only the
architecture below 10 ISR acted on it. On architecture 10 and up the jets and slingshots never fired; the stock ROM fires
them from software on the switch closure.

**Evidence.** Stock ROM in PinMAME: jets and slings are fired by the game software on the closure; the specials need
GameOn.

**What changed.** The architecture 15 ISR does what the streamlined older scan does: when a registered switch first reads
closed, one slot goes to the front of the solenoid stack (probe); when the closure is validated (off, on, on) the
table's hold time goes to the front. Only while GameOn is on (`!GameOverLine`), so attract mode and tilt are quiet. The
closure still goes to the switch stack. The table is read from PROGMEM, no per-entry SRAM; the ISR filters with the
per-column mask `ImmediateSolenoidSwitchMask`. `RPU_SetupGameSwitches` clamps its counts.

**Without it.** Jets and slings do nothing.

**Tests.** `jet_fires_from_the_isr` (fires for about its hold time, and not at all with GameOn off),
`sling_fires_and_ignores_hits_during_its_pulse`.

**Fails on upstream.** Yes: the jet and sling never fire.

**Verified.** Host test, PinMAME. Not on hardware.

---

## 0006: smaller switch and sound stacks (`ebe41fd`)

**Problem.** SRAM. `SWITCH_STACK_SIZE` 60 and `SOUND_STACK_SIZE` 64 were far larger than needed.

**Evidence.** Measured high-water marks in PinMAME scenarios and a scripted multi-ball game: switch stack 1, sound stack 0.

**What changed.** 24 and 16 entries; 36 B and 96 B of SRAM returned.

**Without it.** The game still runs, but 132 B of the Mega's 8 KB SRAM go to queues that never fill. That memory
comes out of the margin left for the call stack, and a stack overrun on this board corrupts state while coils are
live. With fix 0007, the solenoid queue also drops from 150 one-byte entries to 32 two-byte entries, saving 86 B more.

**Test.** `queues_fit_the_sram_budget`: the solenoid and switch queues must fit 96 B and still hold one pulse for each of
the 22 coils. The sound queue is not checked, because its free-space counter is not built for this sound type. Capacities come from the library's free-space counters, so this is a size check, not a failure you would see
in play.

**Verified.** Compiles; sized from emulation. Not on hardware.

---

## 0007: a solenoid-stack entry is one pulse with an on-time (`42a8866`)

**Problem.** `RPU_PushToSolenoidStack(n, N)` wrote N one-slot entries, so a 28-85 slot pulse (drop resets, ejects, the
outhole and feeder at 87 ms, the subway at 176 ms) took 28-85 of the 150 FIFO entries. A few long pulses overflowed it and
the overflow was silently dropped (a short pulse). Jets and slings fire from the same FIFO, so they needed per-coil tracking (lockout, re-fire). `RPU_SetCoinLockout()` passes its default bit mask `0x20` as a shift count: `1 << 32` is undefined, the AVR gives 0
but a 32-bit shift wraps to bit 0, SOL1 (the outhole kicker).

**Evidence.** Measured on the emulated bus: stock pulse widths, FIFO occupancy, the dropped pulses.

**What changed (architecture 15).**
* An entry is a coil byte plus an on-time in slots (1-255, 2.0718 ms each). `RPU_PushToSolenoidStack(n, N)` keeps its
  signature; N is consecutive slots in one entry. The FIFO is 32 entries.
* `PullFirstFromSolenoidStack()` plays one coil per slot as before. A front push suspends the running pulse; its remaining
  slots follow the new one, so no on-time is lost or added.
* Switch-driven coils (index 16-21) are tracked per coil: a hit during the pulse is ignored (sling lockout) or, with
  table bit 7 (jets), queues ONE re-fire after a 7-slot (14.5 ms) off gap. Duty under repeated hits is at most pulse / (pulse + 7 slots).
* Pushes check space and append inside one interrupt-off block.
* `RPU_SetContinuousSolenoid()` ignores indices of 16 and up.

**Without it.** Long pulses are cut short or dropped when several are queued; repeated jet or sling hits can stack long
pulses (a coil that overheats); SOL1 fires on a coin lockout call on a 32-bit shift.

**Tests.** `three_long_pulses_fit` (three 85-slot pulses all play 85 slots), `coin_lockout_default_does_not_fire_sol1`,
`jet_refire_is_single_and_spaced` (a burst of hits gives the pulse plus one re-fire, at least 7 slots later). The sling
lockout is in `sling_fires_and_ignores_hits_during_its_pulse` (fix 0005).

**Fails on upstream.** Yes, all three. Upstream plays 64 slots of the second pulse and none of the third (150 entries
are not enough); SOL1 is requested; the jet never fires (the last needs fix 0005 as well, so on upstream it fails for that reason).

**Gap.** `coin_lockout_default_does_not_fire_sol1` shows a defect that exists on a 32-bit shift, which is the host. On the
8-bit AVR the shift is 0 and the call is harmless, as the commit says; the guard is for any wider port of the code.
The jet and sling tests share the upstream failure of fix 0005 (upstream never fires them).

**Verified.** Host test, AVR emulator. Not on hardware.

---

## 0008: lamp strobe deselect before row data (`f1fb6b2`)

**Problem.** The lamp column was selected (`PIA_LAMPS_PORT_B`) before the new row data was written, so for one bus
cycle the previous column's rows sat on the new column: a ghost lamp.

**Evidence.** PinMAME ORs every lamp write into its matrix: the ghost showed as a full-brightness lamp (lamp 5 together with
lamp 61, row 7 of every column with lamp 64).

**What changed.** Port B is written 0 (deselect), then the rows, then the column. One more bus write per column pass
(+57 AVR cycles of the 4,430 in a pass-0 ISR, about 0.17 % of the CPU).

**Without it.** Ghost lamps, for a bus cycle each, every column pass.

**Test.** `lamp_rows_match_column_when_selected` (whenever a column line is high the row lines hold that column's own data).

**Fails on upstream.** Yes: 40 of 80 column selects carried the previous column's rows.

**Verified.** Host test, PinMAME. Not on hardware.

---

## 0009: hold-PWM for the cup diverter (`b729173`)

**Problem.** The stock cup diverter (SOL14) is pulled for 560-583 ms, then held at 3.7 ms on / 29.7 ms (12.5 % duty) for 10.44 s.
Switching it from `loop()` makes the duty depend on the sketch's loop period, and a held coil at the wrong duty overheats. The
image has no watchdog: a stalled loop would leave the hold running.

**Evidence.** Stock timing in PinMAME. Coil duty is the thing that overheats a hold winding.

**What changed.** `RPU_SetSolenoidHoldPwm(sol, onSlots, periodSlots)`: the Timer1 solenoid pass ORs one coil into the `$2200` / `$2102` write for
`onSlots` of every `periodSlots` passes (2 of 16 slots = 4.14 ms of 33.2 ms, 12.5 %). Separate from `ContinuousSolenoidBits`,
one coil at a time. The ISR counts the hold down itself: 5300 slots (10.98 s) and it clears itself.

**Without it.** The duty of a held coil depends on the foreground; a stalled foreground leaves it running forever.

**Tests.** `hold_pwm_duty_from_isr` (2-slot pulses, 12.5 % duty, no other coil, no further calls),
`hold_pwm_stops_by_itself` (stops after 5300 slots without the foreground).

**Fails on upstream.** Yes, but by absence: the function does not exist upstream. The stub does nothing, the coil is never pulsed.
There is nothing upstream to compare with.

**Verified.** Host test. Not on hardware.

---

## 0010: shared-driver busy flag for the A/C relay (`d09d7ca`)

**Problem.** The A/C select relay (SOL12) must never switch while a coil on drivers 1-8 is on, or the relay arcs under coil current
and the rest of the pulse goes to the C-side flasher on the same driver. The caller times a pulse from when it hands it to the library,
but a jet or sling front push suspends and stretches a running pulse, so a timer alone cannot know when it ended.

**What changed.** `RPU_SolenoidStackBusyShared()`: true when the active pulse or any queued entry is a coil of index 0-7. Idle entries and
the switch-driven coils do not count. Read with interrupts off. 0 B SRAM.

**Without it.** The relay can be switched under a live coil.

**Test.** `busy_shared_tracks_drivers_1_to_8` (true while queued, true while playing, false after, false for driver 13, false for a special).

**Fails on upstream.** Yes, by absence (the function does not exist; the stub returns false).

**Verified.** Host test. Not on hardware.

---

## 0011: coil safety at the output scheduler (`a975d08`)

**Problem.** Three defects in the Timer1 solenoid pass.
1. **Special-to-special handover.** The specials are CA2/CB2 outputs held low by CRx `0x34`. A jet or sling pre-empting the 87 ms
   feeder (or a jet followed by a sling) wrote the new special on and left the previous line asserted: the feeder stayed on
   through the other pulse and beyond its own slots.
2. **No ISR deadline on continuous coils.** `RPU_SetContinuousSolenoid()` holds an output until the foreground calls again; a stalled
   foreground left the diverter pull and the C-side flasher lines on.
3. **No minimum off time.** Two FIFO entries of the same ordinary coil (an overdue retry burst, any upstream bug) merged into one long pulse.

**Evidence.** External review of the output path; bus-measured stock gap 31.1 ms (stock 29.7 ms).

**What changed.**
1. `SpecialActive` records the asserted special; a different special taking the slot releases the outgoing line first; ordinary and empty slots release every special.
2. `RPU_SetContinuousSolenoidMax(on, sol, maxMs)`: an ISR-enforced deadline (2-slot units of 4.14 ms, rounded down, 1.05 s max); the pass forces the bit off. A plain call clears the deadline.
3. After an ordinary coil's pulse ends the same coil may not start again for 15 idle slots (31.1 ms). Specials keep their own 7-slot re-fire gap.

**Without it.** A feeder (or jet, or sling) coil stays energised; a continuous coil runs until the foreground notices; two pulses merge into one long one.

**Tests.** `special_handover_back_to_back`, `special_handover_on_preemption` (a jet in the middle of the feeder: never two specials at once, the feeder
still gets its 40 slots), `continuous_deadline_enforced_by_isr` (651 ms request: 314 slots, then off, with no foreground call),
`plain_continuous_call_has_no_deadline`, `same_coil_minimum_off_time`.

**Fails on upstream.** Yes, all five. Two specials are asserted at once; the second pulse of the same coil merges into the first; the deadline function does not exist
(stub). `plain_continuous_call_has_no_deadline` fails on upstream only because the function it calls is absent.

**Gap.** If Timer1 itself stops, none of the three can act (no hardware watchdog in this image: owner decision).

**Verified.** Host test, AVR-engine model. Not on hardware.

---

## 0012: init writes every solenoid output off before the display PIA (`d28aedb`)

**Problem.** On System 11C the solenoid drivers are enabled by BLANKING, a 555 one-shot (U43) that the display PIA's PA2 (`$2800` bit 2) retriggers (36 ms timeout). An
AVR-only reset (watchdog, brown-out, the RPU reset button) does not reset the board: the PIAs and the U28 latch keep their last state, so the display
PIA is still configured and U28, PIA0 port B and the special lines still hold the interrupted game's requests; BLANKING has timed out during the 1 s init delay.
`RPU_InitializePIAs()` wrote the display PIA first. Its `$2800 <- 0x70` can move PA2 and retrigger BLANKING about 100 us before U28 and PIA0 port B were cleared, so a retained
request was live for that long.

**Evidence.** Schematic (BLANKING, reset: [HARDWARE.md](HARDWARE.md)); AVR-engine model of the init sequence: sol 1, 0.1 ms. Stock code has the same order but
resets the PIAs through `/RESET`, which the RPU does not.

**What changed (architecture 15).** The display PIA block is `RPU_InitializeDisplayPIA()` and runs last in `RPU_InitializePIAs()`, after the switch, lamp, U28, PIA0, alpha, numeric,
sound and widget PIA writes. Same writes, same values, only the order.

**Without it.** After a mid-game reset, a coil that was on can be energised for tens of microseconds to a tenth of a millisecond again. Short, but on a coil that was being held it
is an unplanned pulse.

**Test.** `init_clears_requests_before_blanking_retriggers`: the board is left as a running game would leave it (SOL1, SOL10, SOL17, SOL21 requested, display PIA configured,
BLANKING timed out), once for each of the 16 retained strobe values; `RPU_InitializePIAs()` runs; no solenoid request may be live while BLANKING is enabled.

**Fails on upstream.** Yes, for 8 of 16 retained strobe values (those with PA2 high, where the first `$2800` write is an edge): 34 us of exposure in the model.

**Gap.** The BLANKING model is read from the schematic analysis (any PA2 edge retriggers, 36 ms): it has not been measured. The exposure time depends on the model's bus timing.

**Verified.** Host test, AVR-engine model. Not on hardware.

---

## 0013: one full-width E pulse per bus transaction in 6802 mode (`42aa5ea`)

**Problem.** In 6802 mode the Mega generates E. Upstream writes two back-to-back pulses of 125 ns with E left high between transactions. The Diner board does nothing to E but buffer it (U11 74LS244 to every PIA pin 25;
no stretching), so each PIA saw an E pulse of 125 ns against an MC6821 minimum of 450 ns, the U28 latch strobe (E and write gated) is just as short, and a read was sampled about 94 ns after the second rise
against a data delay of up to 290 ns. A short E is an unreliable write, to a coil latch among other things.

**Evidence.** Schematic 16-9019 (E path, U11, PIA select from the address decode with VMA, not E; latch strobe gated with E and write). MC6821 data sheet: E high minimum 450 ns, tDDR up to 290 ns.
Old and new E timing, from an AVR emulator and cycle counts: two 125 ns pulses against one pulse with 500 ns high on a write; read sample from about 94 ns to about 600 ns after E rises.
Static count of the code as compiled here (`tests/run.sh pulse`, avr-gcc 7.3, `-Os`): the new write has E high for 8 cycles (500 ns) and the read samples PINA 10 cycles (625 ns) after E rises. The same count on upstream gives E high
for 2 cycles (125 ns) on a write and a sample 2 cycles (125 ns) after the second rise; the emulator's 94 ns and this count differ because the compiler lays the code out differently here. Both are below the 290 ns limit.
Not measured with a scope.

**What changed.** A single E pulse per transaction, parked low between transactions. A write holds E high 8 cycles (500 ns, the stock width); a read holds E high and samples PINA 8 cycles after the
rise plus the jump back, then drops E. The external-clock (6800) path is unchanged.

**Without it.** Writes to PIAs and to the SOL1-8 latch happen with an E pulse that is a quarter of the minimum; reads can return data the PIA has not driven yet, which the library takes as a switch or status value.

**Tests.** `e_pulse_one_per_write`, `e_pulse_one_per_read` (exactly one E pulse per transaction), `e_high_width_on_write` (at least 450 ns), `e_read_sample_point` (the value read is right, sampled at least 290 ns after
E rises and before it falls), `e_low_between_transactions`. Optional: `tests/run.sh pulse` on the compiled code.

**Fails on upstream.** Yes, all five (two pulses, 125 ns, sample 62 ns after the rise, E left high), and `run.sh pulse upstream` fails.

**Gap.** The host test uses a cycle model (`sbi/cbi` 2 cycles, `in/out` 1) and a lenient device model (a write is always taken at E fall; a read always returns the data; only the timing is checked). Real cycle counts come only from `run.sh pulse`,
which needs avr-gcc and counts the code as compiled, not as run. No scope measurement has been made.

**Verified.** Host test, AVR emulator, avr-objdump cycle count of the compiled code. Not on hardware.

---

## Results

`tests/run.sh both`: 34 tests. Every test fails on upstream and passes on the branch. Mutation check (`tests/run.sh mutants`): 20 deliberate single-point defects in the branch library, 19 caught by at least one test; the one not caught is the solenoid FIFO depth (32 against 150).

| Fix | Tests | Fails on upstream | Passes on branch | Notes |
|---|---|---|---|---|
| alpha (`e56a2fb`) | 2 | yes | yes | hardening items untested |
| config (`7ac2c87`) | 0 | n/a | n/a | configuration: nothing to fail |
| 0001 | 3 | yes | yes | |
| 0002 | 5 | yes | yes | compile-time poisoning untestable |
| 0003 | 2 | yes | yes | |
| 0004 | 2 | yes | yes | 1 ms gap from PinMAME, not the real board |
| 0005 | 2 | yes | yes | |
| 0006 | 1 | yes | yes | size budget, read from the free-space counters |
| 0007 | 3 | yes | yes | coin-lockout shift is a 32-bit hazard only; jet test also fails for 0005 |
| 0008 | 1 | yes | yes | |
| 0009 | 2 | yes (absent) | yes | no upstream equivalent |
| 0010 | 1 | yes (absent) | yes | no upstream equivalent |
| 0011 | 5 | yes (2 by absence) | yes | |
| 0012 | 1 | yes (8 of 16 cases) | yes | BLANKING model not measured |
| 0013 | 5 | yes | yes | cycle model; `run.sh pulse` for real counts |

Where a fix builds on another (jets need GameOn, which needs 0001; specials need 0002), a test can fail on upstream for more than one reason; the failure text lists them.
