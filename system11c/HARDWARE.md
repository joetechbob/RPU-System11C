# The hardware this fork talks to

The RPU Rev 102 sits in the CPU socket of a Williams System 11C board (Diner). This file describes
the bus, E and reset, the pins, the solenoid map and the timing, with the source of each statement.

**Nothing here has been measured on a board.** The sources are the Williams schematics, the RPU Rev 102
schematic, the stock ROM running in PinMAME, MAME/PinMAME `s11.c`, an AVR emulator and cycle counts. Where
something is a reading of a schematic or a guess, it says so.

References:

* **16-9019**: Williams System 11C CPU board schematic, in the Diner Operations Manual (pages DINER 85-88).
* **Rev 102**: `Schematics/EasyEDASchematic_Rev102.json` and `Schematics/RPU V102 Schematic.png` in this repository.
* **s11.c**: the System 11 driver of MAME/PinMAME.

## 1. The bus

```
   RPU Rev 102 (Mega 2560)                         System 11C CPU board
   -----------------------                         ---------------------
   A0-A7   PF0-7  (Mega A0-A7)  ----------------> address A0-A7   --+
   A8-A15  PK0-7  (Mega A8-A15) ----------------> address A8-A15  --+--> U37 LS138 decode (with VMA)
   D0-D7   PA0-7  (Mega D22-29) <---------------> data D0-D7 (direct, no buffer)
   E       PG2    (Mega D39)    <---------------> 6808 socket pin 37 --> U11 74LS244 --> every PIA pin 25
   VMA     PG1    (Mega D40)    ----------------> socket VMA
   R/W     PE5    (Mega D3)     ----------------> socket R/W
```

* The Mega is the bus master. It drives the address, VMA, R/W and (in 6802/6808 mode) E, and drives D0-D7 only while writing.
* Data lines D0-D7 go straight from the socket to Mega D22-D29. There is no buffer on the Rev 102 data bus (Rev 102).
* The board only buffers E (U11 74LS244 to pin 25 of every PIA): there is no stretching and no re-timing (16-9019). The E the Mega makes
  is the E the PIAs see.
* Chip selects: U37 (74LS138) decodes the address together with VMA, not E (16-9019). PIAs are therefore selected by address + VMA, and take
  their data on the falling edge of E.
* The solenoid latch strobe (`2000 LATCH`, U28 clock) is gated with E and write (16-9019). The exact edge it latches on is per the schematic (a 74LS374
  latches on a rising clock); the tests therefore require the bus to be stable across the whole E pulse rather than assume an edge.
* 6800 mode (the socket carries an external 6800 clock) is detected at start-up by `CheckForMPUClock()`. This fork is built and tested for 6802/6808
  mode only (`RPU_MPU_BUILD_FOR_6800 0`); the 6800 code paths are upstream's, unchanged and untested here.

### Address map used by the library (System 11C)

| Address | Device | Used for |
|---|---|---|
| `$2200-$2203` | U28 74LS374 latch, **write only**, A1:A0 not decoded | SOL1-8, bit n-1. A read is open bus. A write to `$2201-$2203` strobes the latch exactly like `$2200`. |
| `$2100-$2103` PIA0 | port A, CA2: sound path of System 11/11A (not used on 11C after init); port B (`$2102`): SOL9-16; CB2 (`$2103`): GameOn |
| `$2400-$2403` PIA1 | lamps: port A rows, port B columns; CA2 = SOL22, CB2 = SOL21 |
| `$2800-$2803` PIA2 (U51) | display strobe on port A; **PA2 retriggers BLANKING**; PA7 is an input (W7); port B: numeric display |
| `$2C00-$2C03` PIA3 | alpha display data; CA2 = SOL18, CB2 = SOL19 |
| `$3000-$3003` PIA4 | switch matrix: port B strobe, port A return; CA2 = SOL17, CB2 = SOL20 |
| `$3400-$3403` PIA5 | widget PIA: port B = sound board command, CB2 = command strobe |

## 2. E

In 6802 mode the Mega generates E. MC6821 limits that matter (data sheet, 1 MHz part): E high at least **450 ns**; read data valid within **290 ns** (tDDR)
after E rises.

| | Upstream | This branch (fix 0013) | Limit |
|---|---|---|---|
| E pulses per transaction | 2 (rise, fall, rise) | 1 | 1 |
| E high on a write | 125 ns (2 cycles) | 500 ns (8 cycles) | >= 450 ns |
| Read: PINA sampled after E rises | about 94 ns (AVR emulator); 125 ns (count of the compiled code) | about 600 ns (AVR emulator); 625 ns (count of the compiled code) | >= 290 ns |
| E between transactions | high | low | (parked low; all PIAs share the E net) |

How each number was obtained: the 94 ns and 600 ns figures are from an AVR emulator and cycle counts. The 125 / 500 / 125 / 625 ns figures are
`tests/run.sh pulse` (avr-objdump of `RPU_DataWrite` and `RPU_DataRead` compiled with avr-gcc 7.3, `-Os`, a 16 MHz clock assumed exact; upstream's
count differs from the emulator because the compiler lays the code out differently). The host tests use a cycle model (one sbi/cbi is 2 cycles, an in/out is 1;
a write's E pulse is 8 cycles = 500 ns, a read samples 9 cycles = 560 ns after the rise) that orders events correctly but is not exact. **No scope measurement.**

## 3. Reset and BLANKING

* **BLANKING** is a 555 (U43) wired as a missing-pulse detector with a timeout of about 36 ms. It is retriggered by the display PIA's port A bit 2 (PA2,
  `$2800` bit 2) through U36 (16-9019). BLANKING is an input of the gates in front of every solenoid driver (SOL1-8: U19/U20 with U28; SOL9-16: U17/U18
  with PIA0 port B), the lamp columns and the special-solenoid and flipper-relay gates. It is high (outputs enabled) only while PA2 keeps changing.
  So the stock board supervises the *interrupt handler* (which writes `$2800`), not the main loop. This reading of the schematic has not been measured.
* The model used in the tests: any change of PA2 while the pin is an output retriggers BLANKING for 36 ms (`tests/harness/board.cpp`).
* **Board `/RESET`** goes to the CPU (socket pin 40), to the reset pin of every PIA, and to U43 pin 4 (it holds BLANKING low). It does not reach U28.
* On the **Rev 102**: board `/RESET` (socket pin 40) goes to Mega **D45**, which this library never reads. Mega **D42** drives the retained-CPU reset only
  (`RPU_RESET_PIN`). So the library cannot see a board reset, and an AVR reset does not reset the board.
* **Consequence.** After an AVR-only reset (watchdog, brown-out, the RPU's own reset) the PIAs and the U28 latch keep whatever the interrupted game had
  written: display PIA still configured, solenoid requests still present, BLANKING timed out. The first display-PIA write that moves PA2 re-enables the drivers.
  Any solenoid output still requested at that moment is live. That is why fix 0012 clears every solenoid output before the display PIA.
* The stock ROM is safe in this respect by construction: it clears U28 about 14 us after the reset vector, SOL9-16 within about 0.3 ms, and the first PA2 edge needs several interrupts (about 4 ms), and `/RESET` has already reset the PIAs.
* If a PIA port is an input (after `/RESET`, before init) it floats; an LS input that floats reads high. The SOL9-16 gates then see "request on" until port B is made an output
  (the model reads floating inputs as high). Special solenoids use active-low enables with pull-ups, so a floating CA2/CB2 leaves them off.

## 4. Pin map (Rev 102)

| Signal | Mega pin | AVR | Where used | Source |
|---|---|---|---|---|
| A0-A7 | A0-A7 | PF0-PF7 | address low | `PORTF = address & 0xFF` in `RPU.cpp` |
| A8-A15 | A8-A15 | PK0-PK7 | address high | `PORTK = address / 256` in `RPU.cpp` |
| D0-D7 | D22-D29 | PA0-PA7 | data, direct | Rev 102; `PORTA`, `PINA`, `DDRA` |
| E (phi2) | D39 | PG2 | 6808 socket pin 37 | Rev 102; `RPU_PHI2_PIN` |
| VMA | D40 | PG1 | socket VMA | `RPU_VMA_PIN` |
| R/W | D3 | PE5 | socket R/W | `RPU_RW_PIN` |
| /RESET from the board (socket pin 40) | D45 | PL4 | **never read** | Rev 102 |
| CPU reset (retained CPU) | D42 | PL7 | `RPU_RESET_PIN` | Rev 102 |
| /HALT | D41 | PG0 | `RPU_HALT_PIN` | |
| BA | D43 | PL6 | `RPU_BA_PIN` | |
| Diagnostic jumper | D44 | PL5 | `RPU_DIAGNOSTIC_PIN` | |
| Selector switch | D38 | PD7 | `RPU_SWITCH_PIN` | |
| Buffer disable | D5 | PE3 | `RPU_BUFFER_DISABLE` | |
| Disable phi from MPU / from CPU | D7 / D6 | PH4 / PH3 | `RPU_DISABLE_PHI_FROM_*` (6800 / 6802 clock path) | |
| Board select 0-3 | D30-D33 | PC7-PC4 | `RPU_BOARD_SEL_*` | |

The AVR port of each Mega pin is the standard Arduino Mega 2560 mapping. D22-D29, D39, D42 and D45 are as drawn on the Rev 102 schematic; the other rows come from `RPU.cpp` (the `#define`s and the port writes) and were not checked against the schematic.

## 5. Solenoid map (System 11C)

Library index = Williams solenoid number minus 1. Evidence: the stock ROM in PinMAME (the control register whose CA2/CB2 field was 110 while each coil was on),
`s11.c`, and the schematic.

| Solenoid | Library index | Hardware | Write |
|---|---|---|---|
| 1-8 | 0-7 | U28 74LS374 latch `$2200`, write only, bit n-1, active high | the byte |
| 9-16 | 8-15 | PIA0 port B `$2102`, bit n-9, active high | the byte |
| 17 | 16 | PIA4 CA2 `$3001` | CRA `0x34` on, `0x3C` off |
| 18 | 17 | PIA3 CA2 `$2C01` | same |
| 19 | 18 | PIA3 CB2 `$2C03` | same |
| 20 | 19 | PIA4 CB2 `$3003` | same |
| 21 | 20 | PIA1 CB2 `$2403` | same |
| 22 | 21 | PIA1 CA2 `$2401` (shooter lane feeder) | same |
| 23 | n/a | PIA0 CB2 `$2103`: GameOn, flipper relay, special-solenoid enable | CRB `0x34` on (CB2 low), `0x3C` off |

Notes: CA2/CB2 are plain outputs when control bits 5-3 are `11x` (`110` low, `111` high). SOL10 is the general-illumination relay, SOL12 the A/C select relay
(energised = C side; it must not switch under a coil pulse on drivers 1-8, fix 0010). The specials need GameOn.

Diner uses solenoids 1-22 only. The legacy architecture 15 table of the library (System 11, not 11C) is wrong for all of 17-22.

## 6. Timing

| Quantity | Value | Where it comes from |
|---|---|---|
| Timer1 tick | 16575 counts at 16 MHz = 1.0359 ms (965.3 Hz) | `INTERRUPT_OCR1A_COUNTER 16574` |
| ISR pass 0 (even tick) | display, lamp column, switch scan | `InterruptPass` |
| ISR pass 1 (odd tick) | display, solenoid slot | |
| Solenoid slot | 2 ticks = 2.0718 ms | |
| Longest FIFO pulse | 255 slots = 528 ms | one byte |
| Jet / sling re-fire gap | 7 slots = 14.5 ms | fix 0007 |
| Minimum off time, same ordinary coil | 15 idle slots = 31.1 ms (stock 29.7 ms) | fix 0011 |
| Continuous deadline unit | 2 slots = 4.14 ms, 255 units = 1.05 s | fix 0011 |
| Hold-PWM | 2 of 16 slots = 4.14 ms of 33.2 ms (12.5 %); stops after 5300 slots = 10.98 s | fix 0009 |
| Diverter full-power pull | 651 ms (157 units = 314 slots = 650.5 ms) | the sketch, using fix 0011 |
| Sound command spacing | 1000 us after each command | fix 0004 (from PinMAME; real board not measured) |
| BLANKING timeout | about 36 ms | schematic (R79 / C58); not measured |
| Bus transaction | about 3.5-4 us with Timer1 held off meanwhile (57 / 52 cycles for write / read, measured before fix 0013 lengthened the E pulse; not re-measured) | fix 0003 |
| E high | 500 ns write, read sampled at about 600 ns | fix 0013 |

## 7. What is known, what is read, what is a guess

| | |
|---|---|
| Read from the schematic | the bus connections, U11, U37, the latch strobe gating, BLANKING and `/RESET` wiring, the Rev 102 pin map |
| From the stock ROM in PinMAME | the solenoid map, the GameOn writes, the display and lamp sequences, the switch scan |
| Measured in an AVR emulator or by cycle count | the E timing, the exposure window of fix 0012, the interrupt sweep of fix 0003 |
| Guess or assumption | the exact latch edge of the `2000 LATCH` gate; the BLANKING timing; the sound board's real command latency; that all PIAs of the real board behave at the MC6821 data sheet limits |
| Not done | any measurement on a real board, any scope capture, any soak or thermal test of a coil |
