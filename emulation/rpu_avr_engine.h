/* AVR-exact execution of an RPU firmware image.
 *
 * Parallel to rpu/host/rpu_host.h, but instead of compiling the firmware
 * sources for the host this runs the pinned ATmega2560 flash image
 * (a sketch built for RPU_OS_HARDWARE_REV 102, architecture 15) on
 * a simavr ATmega2560 at 16 MHz and models the RPU Rev 102 board at pin level
 * around it. The host sees exactly what a 6802 socket would see: one
 * (address, R/W, data) access per 6802 bus cycle.
 *
 * ---------------------------------------------------------------------
 * RPU Rev 102 pin protocol (derived from RPU.cpp, hex image built from
 * the sketch copy of RPU.cpp; line numbers below are for that copy):
 *
 *   Signal            Arduino pin  AVR pin   Dir (Mega)   RPU.cpp
 *   A0..A7            A0..A7       PF0..PF7  out          PORTF  (SetAddressPinsDirection 977; DataWrite 991-1050, DataRead 1056-1105)
 *   A8..A15           A8..A15      PK0..PK7  out          PORTK  (same)
 *   D0..D7            22..29       PA0..PA7  bidir        DDRA/PORTA/PINA (SetDataPinsDirection 984)
 *   R/W (high=read)   3            PE5       out          PORTE bit 5 (RPU_RW_PIN 959; DataWrite 997, DataRead 1062)
 *   VMA               40           PG1       out          PORTG bit 1 (RPU_VMA_PIN 958)
 *   PHI2 / E          39           PG2       out (6802/6808 path) or in (6800 path)  PORTG/PING bit 2 (RPU_PHI2_PIN 960)
 *   /HALT             41           PG0       out          (RPU_HALT_PIN 963)
 *   /RESET            42           PL7       out          (RPU_RESET_PIN 964)
 *   BA                43           PL6       out (low)    (RPU_BA_PIN 965)
 *   BUFFER_DISABLE    5            PE3       out          (RPU_BUFFER_DISABLE 962)
 *   RPU_SWITCH_PIN    38           PD7       in           (RPU_SWITCH_PIN 961)
 *   DIAGNOSTIC        44           PL5       in           (RPU_DIAGNOSTIC_PIN 966; read InitializeMPUArch10)
 *   DISABLE_PHI_FROM_MPU 7         PH4       out          (CheckForMPUClock 2904-2943; defines 967-968)
 *   DISABLE_PHI_FROM_CPU 6         PH3       out          (CheckForMPUClock 2904-2943; defines 967-968)
 *   BOARD_SEL_0..3    30..33       PC7..PC4  (declared, not used by this firmware path)
 *
 * Transaction (UsesM6800Processor == false, i.e. the RPU generates E: 125 ns
 * high / 125 ns low pulses, only during accesses;
 * RPU_DataWrite/RPU_DataRead):
 *   write: DDRA=FF; R/W low; PORTA=data; PORTF/PORTK=address; E low; VMA high;
 *          E high; E low; E high; VMA low; address=0; R/W high; DDRA=0
 *   read:  DDRA=0;  R/W high; PORTF/PORTK=address; E low; VMA high;
 *          E high; E low; E high; sample PINA; VMA low; R/W low; address=0
 * (UsesM6800Processor == true: identical, but every "E ..." step is a wait on
 *  the PING bit 2 level driven by the MPU clock instead of a pin write.)
 *
 * Board model (this engine), edge driven from the pins as simavr executes
 * them (interrupts may land between any two instructions, so torn accesses
 * reproduce):
 *   - E (PG2) falling edge while VMA (PG1) is high and R/W (PE5) is low: the
 *     host bus_write(address, PORTA) is made. This is the 6821 write strobe.
 *   - E rising edge while VMA is high and R/W is high: the host bus_read is
 *     made (once per address while VMA stays high) and the byte is driven on
 *     PINA for the E-high phase; PINA floats (0xFF) while E is low and after
 *     VMA drops. read_data_delay_avr_cycles models PIA data-valid delay.
 *   - Address decode is NOT done here: every (address, R/W) goes to the host
 *     callback. In particular the System 11C $2200-$22FF page is the
 *     write-only U28 latch (any write strobes it; reads are open bus -- the
 *     last byte on the data bus; the fake bus in the tests makes this
 *     selectable) and is the host's job.
 *   - EEPROM writes take 3.4 ms (EEPE stays set; simavr alone completes them
 *     instantly); data is committed to the host buffer when programming starts.
 *
 * Who sources E (inferred where only the firmware is visible): see
 * rpu_avr_clock_mode below. Default (RPU_AVR_CLOCK_FROM_RPU) is what
 * CheckForMPUClock selects when the MPU supplies no clock, which is the
 * System 11 case (6802/6808 socket, the CPU makes E): PG2 is an output and E
 * is the Mega's own bit-banged waveform, so E is not locked to 1 MHz.
 *
 * Time: the AVR clock is exactly 16 MHz; one "E cycle" for run()/micros is
 * 16e6/e_clock_hz AVR cycles (16 at the 1 MHz default). Deterministic.
 *
 * One engine instance per process (simavr's logger is global). All calls
 * from one thread.
 */
#ifndef RPU_AVR_ENGINE_H
#define RPU_AVR_ENGINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint8_t (*rpu_avr_bus_read_fn)(void *context, uint16_t address);
typedef void (*rpu_avr_bus_write_fn)(void *context, uint16_t address, uint8_t data);
typedef void (*rpu_avr_log_fn)(void *context, const char *text);

enum rpu_avr_clock_mode {
    /* RPU sources E on PG2 (what the firmware chooses unless it sees a
     * toggling clock on PG2 during CheckForMPUClock). */
    RPU_AVR_CLOCK_FROM_RPU = 0,
    /* The MPU/socket supplies E on PG2 (firmware "6800" path): the engine
     * drives a 50% duty clock at e_clock_hz. */
    RPU_AVR_CLOCK_FROM_MPU = 1
};

struct rpu_avr_engine_config {
    void *context;
    rpu_avr_bus_read_fn bus_read;
    rpu_avr_bus_write_fn bus_write;
    rpu_avr_log_fn log;                /* Serial0 lines; may be null */
    const uint8_t *flash_image;        /* raw flash bytes from address 0 */
    uint32_t flash_size;               /* <= 262144 */
    uint32_t e_clock_hz;               /* 0 means 1,000,000 */
    uint8_t *eeprom;                   /* 4096-byte image owned by the host */
    uint16_t eeprom_size;              /* must be 4096 */
    uint8_t rpu_switch_closed;         /* PD7 level */
    uint8_t diagnostic_requested;      /* PL5 level */
    uint8_t clock_mode;                /* enum rpu_avr_clock_mode */
    /* Optional instrumentation. 0 disables / uses default. */
    uint32_t heap_start;               /* data-space address of __heap_start */
    uint32_t timer1_vector_byte_addr;  /* default 0x44 (ATmega2560 vector 17) */
    uint32_t read_data_delay_avr_cycles; /* PIA data-valid delay after E rise */
    /* 1 = stock simavr behaviour: an interrupt flag raised while its enable
     * bit is clear is never delivered later. Only for demonstrating the bug;
     * the default (0) latches it like a real AVR (see simavr-src/PATCHES). */
    uint8_t legacy_simavr_irq_drop;
    /* 1 = paint [heap_start, RAMEND] with 0xA5 at every reset so the stack high-water mark can
     * be measured from what the program actually wrote (needs heap_start). Off by default: it
     * changes the power-on contents of unused SRAM from 0x00 to 0xA5, which a program that reads
     * uninitialised stack would notice. (Appended field: pre-existing callers zero-fill.) */
    uint8_t stack_paint;
};

/* Returns 0 on success. Does not run any instruction. */
int rpu_avr_engine_init(const struct rpu_avr_engine_config *config);

/* Power-on reset (registers, SRAM zeroed, pins idle). EEPROM and the stats
 * are preserved/continued? No: stats are cleared. EEPROM is preserved. */
void rpu_avr_engine_reset(void);

/* Advances the AVR by e_cycles * (16e6 / e_clock_hz) AVR cycles. Overshoot of
 * the last instruction is carried to the next call so time never drifts.
 * Returns e_cycles (> 0 when e_cycles > 0). */
int rpu_avr_engine_run(int e_cycles);

/* Virtual time since reset: AVR cycles / 16. */
uint64_t rpu_avr_engine_micros(void);
/* Batch j: resets the AVR made itself during run() (watchdog).  Engine time (micros, avr_cycles) stays
 * monotonic across them; simavr alone restarts its cycle counter at 0. */
uint32_t rpu_avr_engine_in_run_resets(void);

/* Level of a PIN-side input from the board (for tests / live switch). */
void rpu_avr_engine_set_rpu_switch(int closed);
void rpu_avr_engine_set_diagnostic(int requested);

/* Debug: current byte of AVR data space (registers, I/O, SRAM). */
uint8_t rpu_avr_engine_peek_data(uint16_t addr);

struct rpu_avr_engine_stats {
    uint64_t bus_reads;
    uint64_t bus_writes;
    uint64_t timer1_isr_entries;       /* fetches of the TIMER1_COMPA vector */
    uint64_t eeprom_writes;            /* EECR EEPE with EEMPE set */
    uint64_t avr_cycles;
    uint64_t e_pulses;                 /* E rising edges seen by the board */
    uint32_t sp_min;                   /* lowest SP observed */
    uint32_t ramend;
    uint32_t heap_start;
    /* WHAT THESE TWO COUNTERS PROVE (and do not):
     *  sp_below_static_end_steps: instruction boundaries at which the SP VALUE was below heap_start
     *     (the end of static RAM). 0 means "no observed SP/static overlap". It does not see a
     *     write that lands in static RAM through a pointer while SP is still above it.
     *  ramend_wrap_messages: simavr "Wrapping" log messages, i.e. data accesses above RAMEND that
     *     simavr wrapped. 0 means "no RAMEND wrapping observed". It is not a bounds check on any
     *     object; a buffer overrun inside RAM is invisible to it. Use stack_paint_* and the
     *     write watches below for those. */
    uint64_t sp_below_static_end_steps;
    uint64_t ramend_wrap_messages;
    uint64_t protocol_violations;      /* bus cycle with wrong pin directions */
    uint64_t sim_errors;               /* simavr LOG_ERROR messages */
    uint64_t trace_hash;               /* FNV-1a over every bus/eeprom event */
    uint64_t serial_bytes;
    uint32_t min_read_margin_avr_cycles; /* E rise -> PINA sample, min */
    uint32_t min_e_high_cycles;        /* shortest E-high / E-low phase seen, */
    uint32_t min_e_low_cycles;         /*   in AVR cycles (62.5 ns each) */
    uint8_t original_cpu_released;     /* HALT/RESET high with buffers enabled */
    uint8_t crashed;
    /* ---- appended instrumentation (see config.stack_paint and the write watches) ----
     * Stack paint: lowest byte in [stack_paint_boundary, RAMEND] that no longer holds 0xA5.
     * stack_paint_hw_bytes = RAMEND+1 - that address (0 if the painted area is untouched).
     * Limit: a stored 0xA5 at the very deepest slot is not seen (cross-check with sp_min).
     * stack_paint_below_sp_bytes: bytes the paint says were written BELOW the deepest SP seen
     * (> 0 means something wrote under the stack pointer or SP sampling missed an excursion). */
    uint8_t stack_paint_enabled;
    uint32_t stack_paint_boundary;
    uint32_t stack_paint_lowest_modified;
    uint32_t stack_paint_hw_bytes;
    uint32_t stack_paint_below_sp_bytes;
    /* Write watches: totals over all watches (details via rpu_avr_engine_get_write_watch). */
    uint32_t write_watch_count;
    uint64_t write_watch_violations;
    uint64_t write_watch_foreign_writes;
    uint64_t write_watch_owner_writes;
};
void rpu_avr_engine_get_stats(struct rpu_avr_engine_stats *stats);

/* Count executions of a byte address (e.g. ELF `loop`). Up to 8 slots.
 * Returns slot index or -1. */
int rpu_avr_engine_watch_pc(uint32_t byte_address);
uint64_t rpu_avr_engine_pc_hits(int slot);

/* Cycle cost of a function or ISR: entry byte address (for an ISR, its vector
 * address, e.g. 0x44) to the matching RET/RETI, in AVR cycles, including time
 * spent in nested interrupts. Counts toward the 8 watch slots. */
struct rpu_avr_call_timing {
    uint64_t count, total_cycles;
    uint32_t min_cycles, max_cycles;
    uint32_t first[16];
    uint8_t first_n;
};
int rpu_avr_engine_time_call(uint32_t byte_address);
int rpu_avr_engine_get_call_timing(int slot, struct rpu_avr_call_timing *timing);

/* Socket-control pin history (HALT, RESET, BA, BUFFER_DISABLE, PHI buffers),
 * first 256 changes since reset. */
enum rpu_avr_pin {
    RPU_AVR_PIN_HALT = 0, RPU_AVR_PIN_RESET, RPU_AVR_PIN_BA,
    RPU_AVR_PIN_BUFFER_DISABLE, RPU_AVR_PIN_PHI_FROM_MPU_DISABLE,
    RPU_AVR_PIN_PHI_FROM_CPU_DISABLE, RPU_AVR_PIN_PHI2, RPU_AVR_PIN_COUNT
};
struct rpu_avr_pin_event {
    uint64_t avr_cycle;
    uint8_t pin;     /* enum rpu_avr_pin */
    uint8_t level;
    uint8_t is_output;
};
int rpu_avr_engine_pin_event_count(void);
int rpu_avr_engine_get_pin_event(int index, struct rpu_avr_pin_event *event);

/* Write watches: detect modification of data-space bytes by code that does not own them.
 *
 * After every executed instruction each armed watch compares its bytes with a snapshot; a
 * difference is attributed to the PC of the instruction just executed (an interrupt entry shows
 * the PC it pre-empted). Consequences: a write that stores the byte's current value is not seen;
 * two instructions' changes are never merged. A watch is armed when the PC first reaches
 * arm_pc after a reset (0 = armed at power on), so the startup code's .data/.bss writes are not
 * counted.
 *   RPU_AVR_WATCH_GUARD: the bytes are padding/guard after a buffer; ANY change is a violation
 *     (owner_writes then means "the owner overran its own buffer").
 *   otherwise the bytes are an object; a change by a PC outside every owner range is a violation.
 * Up to 16 watches of up to 256 bytes. Persist across rpu_avr_engine_reset. */
#define RPU_AVR_WATCH_GUARD 1u
struct rpu_avr_pc_range { uint32_t lo, hi; };   /* flash byte addresses, [lo, hi) */
struct rpu_avr_write_watch_report {
    char name[32];
    uint32_t addr, length, flags;
    uint64_t changes, owner_writes, foreign_writes;
    uint32_t first_violation_pc, first_violation_addr, first_violation_value, last_foreign_pc;
    uint64_t first_violation_avr_cycle;
    uint8_t violated;
};
void rpu_avr_engine_set_write_watch_arm_pc(uint32_t byte_address);
int rpu_avr_engine_add_write_watch(uint32_t data_addr, uint32_t length, const char *name, uint32_t flags,
                                   const struct rpu_avr_pc_range *owners, uint32_t n_owners);
int rpu_avr_engine_get_write_watch(int index, struct rpu_avr_write_watch_report *report);

/* One-line summary of the safety instrumentation (what the shutdown log prints). */
int rpu_avr_engine_format_safety_summary(char *buf, unsigned size);

/* Intel HEX -> flat bytes. Returns 0 on success. */
int rpu_avr_engine_parse_ihex(const char *text, uint32_t text_len,
                              uint8_t *out, uint32_t out_capacity,
                              uint32_t *out_size);

/* SHA-256 of the flash image given to init, lower-case hex. Never null. */
const char *rpu_avr_engine_firmware_id(void);

void rpu_avr_engine_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
