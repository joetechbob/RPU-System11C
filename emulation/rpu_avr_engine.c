/* AVR-exact RPU Rev 102 engine. See rpu_avr_engine.h for the pin protocol. */
#include "rpu_avr_engine.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sim_avr.h"
#include "sim_irq.h"
#include "sim_io.h"
#include "sim_cycle_timers.h"
#include "avr_ioport.h"
#include "avr_eeprom.h"
#include "avr_uart.h"
#include "sim_interrupts.h"

#define AVR_HZ 16000000ull
#define EEPROM_BYTES 4096
#define MAX_WATCH 8
#define MAX_PIN_EVENTS 256
#define MAX_WWATCH 16
#define PAINT_BYTE 0xA5
#define BUS_FLOAT 0xFF

/* ATmega2560 data-space addresses (PINx, DDRx = PINx+1, PORTx = PINx+2). */
#define A_PINA 0x20
#define A_PINC 0x26
#define A_PIND 0x29
#define A_PINE 0x2C
#define A_PINF 0x2F
#define A_PING 0x32
#define A_PINH 0x100
#define A_PINK 0x106
#define A_PINL 0x109
#define DDR(p) ((p) + 1)
#define PORT(p) ((p) + 2)
#define A_EECR 0x3F
#define EE_EEPE 0x02
#define EE_EEMPE 0x04

struct engine {
    struct rpu_avr_engine_config cfg;
    avr_t *avr;
    uint32_t hz;
    uint64_t e_budget;          /* cumulative E cycles requested */
    uint64_t cycle_base;        /* AVR cycles before the last in-run reset (simavr restarts avr->cycle at 0) */
    uint32_t in_run_resets;     /* resets the AVR made itself (watchdog), batch j */
    uint8_t flash_copy_ok;
    char fw_id[65];

    /* bus state */
    uint8_t clk;                /* last PG2 level seen */
    uint8_t latched;            /* read data latched for this VMA window */
    uint32_t latched_key;
    uint8_t read_data;
    uint8_t driving;
    uint64_t last_rise_cycle;
    uint64_t last_edge_cycle;
    uint64_t edge_time;         /* scheduled time of an engine-made edge */
    uint8_t pin_out[RPU_AVR_PIN_COUNT];
    uint8_t have_edge;
    uint8_t sample_pending;
    uint64_t half_period;

    /* eeprom */
    avr_eeprom_t *ee;
    uint8_t *ee_orig;
    avr_io_write_t ee_orig_w;
    void *ee_orig_param;
    avr_io_read_t pina_orig_r;
    void *pina_orig_param;

    struct rpu_avr_engine_stats st;
    uint32_t t1vec;
    uint32_t watch_addr[MAX_WATCH];
    uint64_t watch_hits[MAX_WATCH];
    int watch_n;
    uint8_t watch_timed[MAX_WATCH];
    struct { uint64_t start; uint32_t sp; } cstack[MAX_WATCH][4];
    int cdepth[MAX_WATCH];
    struct rpu_avr_call_timing ct[MAX_WATCH];
    uint8_t ee_busy;

    /* write watches (configuration persists across reset; snapshots/armed do not) */
    struct {
        char name[32];
        uint32_t addr, len, flags, nown;
        struct rpu_avr_pc_range own[8];
        uint8_t snap[256];
        struct rpu_avr_write_watch_report rep;
    } ww[MAX_WWATCH];
    int ww_n;
    uint32_t ww_arm_pc;
    uint8_t ww_armed;

    struct rpu_avr_pin_event ev[MAX_PIN_EVENTS];
    int ev_n;
    uint8_t pin_level[RPU_AVR_PIN_COUNT];
    char linebuf[256];
    int linelen;
    int ee_has_log;
};

static struct engine E;
static int g_inited;

/* ---- hashing ---------------------------------------------------------- */
static void hash_mix(uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        E.st.trace_hash ^= (v >> (i * 8)) & 0xFF;
        E.st.trace_hash *= 1099511628211ull;
    }
}

/* ---- SHA-256 ---------------------------------------------------------- */
static uint32_t rr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
static void sha256_hex(const uint8_t *d, size_t len, char out[65])
{
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    size_t total = ((len + 8) / 64 + 1) * 64;
    uint8_t *m = calloc(1, total);
    memcpy(m, d, len);
    m[len] = 0x80;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++) m[total - 1 - i] = (uint8_t)(bits >> (8 * i));
    for (size_t o = 0; o < total; o += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)m[o+4*i] << 24 | m[o+4*i+1] << 16 | m[o+4*i+2] << 8 | m[o+4*i+3];
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = rr(w[i-15],7) ^ rr(w[i-15],18) ^ (w[i-15] >> 3);
            uint32_t s1 = rr(w[i-2],17) ^ rr(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],dd=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = rr(e,6) ^ rr(e,11) ^ rr(e,25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = rr(a,2) ^ rr(a,13) ^ rr(a,22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh=g; g=f; f=e; e=dd+t1; dd=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=dd;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
    }
    free(m);
    for (int i = 0; i < 8; i++) sprintf(out + 8 * i, "%08x", h[i]);
}

/* ---- Intel HEX -------------------------------------------------------- */
static int hexv(const char *p, int n, unsigned *v)
{
    unsigned r = 0;
    for (int i = 0; i < n; i++) {
        char c = p[i]; unsigned d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return -1;
        r = r * 16 + d;
    }
    *v = r;
    return 0;
}

int rpu_avr_engine_parse_ihex(const char *text, uint32_t len, uint8_t *out,
                              uint32_t cap, uint32_t *out_size)
{
    uint32_t base = 0, maxaddr = 0, i = 0;
    memset(out, 0xFF, cap);
    while (i < len) {
        while (i < len && (text[i] == '\r' || text[i] == '\n' || text[i] == ' ')) i++;
        if (i >= len) break;
        if (text[i] != ':') return -1;
        i++;
        unsigned n, a, t, sum;
        if (i + 8 > len || hexv(text + i, 2, &n) || hexv(text + i + 2, 4, &a) ||
            hexv(text + i + 6, 2, &t)) return -1;
        if (i + 8 + 2 * n + 2 > len) return -1;
        sum = n + (a >> 8) + (a & 0xFF) + t;
        uint8_t data[256];
        for (unsigned k = 0; k < n; k++) {
            unsigned b;
            if (hexv(text + i + 8 + 2 * k, 2, &b)) return -1;
            data[k] = (uint8_t)b; sum += b;
        }
        unsigned ck;
        if (hexv(text + i + 8 + 2 * n, 2, &ck)) return -1;
        if (((sum + ck) & 0xFF) != 0) return -1;
        i += 8 + 2 * n + 2;
        if (t == 0) {
            if (base + a + n > cap) return -1;
            memcpy(out + base + a, data, n);
            if (base + a + n > maxaddr) maxaddr = base + a + n;
        } else if (t == 1) break;
        else if (t == 2) base = ((data[0] << 8) | data[1]) << 4;
        else if (t == 4) base = ((data[0] << 8) | data[1]) << 16;
    }
    *out_size = maxaddr;
    return 0;
}

/* ---- logging ---------------------------------------------------------- */
static void sim_logger(struct avr_t *avr, const int level, const char *fmt, va_list ap)
{
    (void)avr;
    char buf[512];
    vsnprintf(buf, sizeof buf, fmt, ap);
    if (strstr(buf, "Wrapping")) E.st.ramend_wrap_messages++;
    if (level == LOG_ERROR) {
        E.st.sim_errors++;
        if (E.cfg.log && E.st.sim_errors <= 8) {
            char line[560];
            snprintf(line, sizeof line, "[simavr] %s", buf);
            E.cfg.log(E.cfg.context, line);
        }
    }
}

/* ---- bus model -------------------------------------------------------- */
static inline uint8_t *D(int a) { return &E.avr->data[a]; }

static void drive_pina(uint8_t v)
{
    *D(A_PINA) = v;
}

static avr_cycle_count_t delayed_drive(struct avr_t *avr, avr_cycle_count_t when, void *p)
{
    (void)avr; (void)when; (void)p;
    if (E.driving) drive_pina(E.read_data);
    return 0;
}

static void present_read_data(void)
{
    E.driving = 1;
    E.sample_pending = 1;
    if (E.cfg.read_data_delay_avr_cycles)
        avr_cycle_timer_register(E.avr, E.cfg.read_data_delay_avr_cycles, delayed_drive, NULL);
    else
        drive_pina(E.read_data);
}

static void release_bus(void)
{
    if (E.driving) {
        drive_pina(BUS_FLOAT);
        E.driving = 0;
    }
}

/* Snapshot of what the socket pins present at this instant. */
static void sample_bus(uint16_t *addr, int *rw)
{
    uint8_t ddre = *D(DDR(A_PINE)), porte = *D(PORT(A_PINE));
    if (!(ddre & 0x20) || *D(DDR(A_PINF)) != 0xFF || *D(DDR(A_PINK)) != 0xFF)
        E.st.protocol_violations++;
    *rw = (porte & 0x20) != 0;
    *addr = (uint16_t)(*D(PORT(A_PINF)) | (*D(PORT(A_PINK)) << 8));
}

/* 6802 bus-cycle model, edge driven (so interrupted/"torn" accesses behave as
 * the pins dictate):
 *  - E falling edge with VMA high and R/W low: write committed to the host.
 *  - E rising edge with VMA high and R/W high: read performed (once per
 *    address/R-W combination while VMA stays high) and its data driven on
 *    PINA for the E-high phase, released when E falls or VMA drops. */
static void clock_edge(int level)
{
    uint64_t now = E.edge_time ? E.edge_time : E.avr->cycle;
    if (E.have_edge) {
        uint32_t w = (uint32_t)(now - E.last_edge_cycle);
        if (E.clk && w < E.st.min_e_high_cycles) E.st.min_e_high_cycles = w;
        if (!E.clk && w < E.st.min_e_low_cycles) E.st.min_e_low_cycles = w;
    }
    E.have_edge = 1;
    E.last_edge_cycle = now;
    E.clk = (uint8_t)level;
    int vma = (*D(PORT(A_PING)) & *D(DDR(A_PING)) & 0x02) != 0;
    uint16_t addr; int rw;
    if (!level) {
        if (vma) {
            sample_bus(&addr, &rw);
            if (!rw) {
                uint8_t v = *D(PORT(A_PINA));
                if (*D(DDR(A_PINA)) != 0xFF) E.st.protocol_violations++;
                if (E.cfg.bus_write) E.cfg.bus_write(E.cfg.context, addr, v);
                E.st.bus_writes++;
                hash_mix(((uint64_t)2 << 40) | ((uint64_t)addr << 8) | v);
                hash_mix(E.avr->cycle);
            }
        }
        release_bus();
        return;
    }
    E.st.e_pulses++;
    E.last_rise_cycle = E.avr->cycle;
    if (!vma) return;
    sample_bus(&addr, &rw);
    if (!rw) return;
    uint32_t key = ((uint32_t)addr << 1) | 1;
    if (!(E.latched && E.latched_key == key)) {
        if (*D(DDR(A_PINA)) != 0x00) E.st.protocol_violations++;
        uint8_t v = E.cfg.bus_read ? E.cfg.bus_read(E.cfg.context, addr) : 0xFF;
        E.read_data = v;
        E.latched = 1;
        E.latched_key = key;
        E.st.bus_reads++;
        hash_mix(((uint64_t)1 << 40) | ((uint64_t)addr << 8) | v);
        hash_mix(E.avr->cycle);
    }
    present_read_data();
}

static void irq_pg2(struct avr_irq_t *irq, uint32_t value, void *param)
{
    (void)irq; (void)param;
    if (E.cfg.clock_mode == RPU_AVR_CLOCK_FROM_MPU) return;
    if (!(*D(DDR(A_PING)) & 0x04)) return;     /* only while PG2 is our output */
    int level = (*D(PORT(A_PING)) >> 2) & 1;
    (void)value;
    if (level != E.clk) clock_edge(level);
}

static void irq_pg1(struct avr_irq_t *irq, uint32_t value, void *param)
{
    (void)irq; (void)value; (void)param;
    int vma = (*D(PORT(A_PING)) & *D(DDR(A_PING)) & 0x02) != 0;
    if (!vma) {
        /* VMA dropped: the 6802 bus cycle is over. */
        E.latched = 0;
        E.sample_pending = 0;
        release_bus();
    }
}

static avr_cycle_count_t mpu_clock(struct avr_t *avr, avr_cycle_count_t when, void *p)
{
    (void)p;
    int level = !E.clk;
    /* PG2 is an input: present the level on PING. */
    if (level) *D(A_PING) |= 0x04; else *D(A_PING) &= (uint8_t)~0x04;
    E.edge_time = when;
    clock_edge(level);
    E.edge_time = 0;
    (void)avr;
    return when + E.half_period;
}

static uint8_t pina_read(struct avr_t *avr, avr_io_addr_t addr, void *param)
{
    (void)param;
    uint8_t v = E.pina_orig_r(avr, addr, E.pina_orig_param);
    if (E.sample_pending && E.driving) {
        uint64_t m = avr->cycle - E.last_rise_cycle;
        if (m < E.st.min_read_margin_avr_cycles) E.st.min_read_margin_avr_cycles = (uint32_t)m;
        E.sample_pending = 0;
    }
    return v;
}

/* ---- socket control pins --------------------------------------------- */
static const struct { int port; int bit; } pinmap[RPU_AVR_PIN_COUNT] = {
    {A_PING, 0}, {A_PINL, 7}, {A_PINL, 6}, {A_PINE, 3}, {A_PINH, 4}, {A_PINH, 3}, {A_PING, 2}};

static void update_released(void)
{
    /* The original CPU on the RPU board is released when its buffers are
     * enabled (BUFFER_DISABLE low) and HALT and RESET are both high. */
    uint8_t halt = E.pin_level[RPU_AVR_PIN_HALT], rst = E.pin_level[RPU_AVR_PIN_RESET];
    uint8_t buf = E.pin_level[RPU_AVR_PIN_BUFFER_DISABLE];
    E.st.original_cpu_released = (halt && rst && !buf &&
        (*D(DDR(A_PING)) & 1) && (*D(DDR(A_PINL)) & 0x80) && (*D(DDR(A_PINE)) & 0x08));
}

static void irq_ctl(struct avr_irq_t *irq, uint32_t value, void *param)
{
    (void)irq; (void)value;
    int p = (int)(intptr_t)param;
    int port = pinmap[p].port, bit = pinmap[p].bit;
    int out = (*D(DDR(port)) >> bit) & 1;
    int lvl = (*D(PORT(port)) >> bit) & 1;
    if (!out) lvl = E.pin_level[p];
    if (lvl != E.pin_level[p] || (out && !E.pin_out[p])) {
        E.pin_level[p] = (uint8_t)lvl;
        E.pin_out[p] = (uint8_t)out;
        if (p != RPU_AVR_PIN_PHI2 && E.ev_n < MAX_PIN_EVENTS) {
            E.ev[E.ev_n].avr_cycle = E.avr->cycle;
            E.ev[E.ev_n].pin = (uint8_t)p;
            E.ev[E.ev_n].level = (uint8_t)lvl;
            E.ev[E.ev_n].is_output = (uint8_t)out;
            E.ev_n++;
        }
        update_released();
    }
}

/* ---- serial ------------------------------------------------------------ */
static void uart_out(struct avr_irq_t *irq, uint32_t value, void *param)
{
    (void)irq; (void)param;
    char c = (char)value;
    E.st.serial_bytes++;
    if (c == '\n' || E.linelen >= (int)sizeof(E.linebuf) - 1) {
        E.linebuf[E.linelen] = 0;
        if (E.cfg.log) E.cfg.log(E.cfg.context, E.linebuf);
        E.linelen = 0;
    } else if (c != '\r') {
        E.linebuf[E.linelen++] = c;
    }
}

/* ---- eeprom ------------------------------------------------------------ */
static avr_cycle_count_t ee_done(struct avr_t *avr, avr_cycle_count_t when, void *p)
{
    (void)when; (void)p;
    E.ee_busy = 0;
    avr->data[A_EECR] &= (uint8_t)~EE_EEPE;
    return 0;
}

/* simavr completes an EEPROM write instantly and drops EEPE; the real cell
 * takes 3.4 ms (EEPE stays set, further programming requests are ignored). */
static void ee_write(struct avr_t *avr, avr_io_addr_t addr, uint8_t v, void *param)
{
    (void)param;
    if (E.ee_busy) {
        v &= (uint8_t)~(EE_EEPE | EE_EEMPE);       /* write request ignored while busy */
        E.ee_orig_w(avr, addr, v, E.ee_orig_param);
        avr->data[A_EECR] |= EE_EEPE;
        return;
    }
    if ((avr->data[A_EECR] & EE_EEMPE) && (v & EE_EEPE)) {
        E.ee_busy = 1;
        avr_cycle_timer_register_usec(avr, 3400, ee_done, NULL);
        uint16_t a = (uint16_t)(avr->data[0x41] | (avr->data[0x42] << 8)) & 0xFFF;
        E.st.eeprom_writes++;
        hash_mix(((uint64_t)3 << 40) | ((uint64_t)a << 8) | avr->data[0x40]);
        E.ee_orig_w(avr, addr, v, E.ee_orig_param);
        avr->data[A_EECR] |= EE_EEPE;
        return;
    }
    E.ee_orig_w(avr, addr, v, E.ee_orig_param);
}

/* ---- lifecycle --------------------------------------------------------- */
static void apply_inputs(void)
{
    if (E.cfg.rpu_switch_closed) *D(A_PIND) |= 0x80; else *D(A_PIND) &= 0x7F;
    if (E.cfg.diagnostic_requested) *D(A_PINL) |= 0x20; else *D(A_PINL) &= 0xDF;
}

static avr_irq_t *pin_irq(char port, int bit)
{
    return avr_io_getirq(E.avr, AVR_IOCTL_IOPORT_GETIRQ(port), bit);
}

static void destroy_avr(void)
{
    if (!E.avr) return;
    if (E.ee) {
        E.ee->eeprom = malloc(EEPROM_BYTES);   /* dealloc frees this, not the host's */
        memset(E.ee->eeprom, 0xFF, EEPROM_BYTES);
    }
    avr_terminate(E.avr);
    free(E.avr);
    E.avr = NULL;
    E.ee = NULL;
}

static int create_avr(void)
{
    uint64_t keep_hash_unused = 0; (void)keep_hash_unused;
    memset(&E.st, 0, sizeof E.st);
    E.st.trace_hash = 1469598103934665603ull;
    E.st.sp_min = 0xFFFFFFFFu;
    E.st.min_read_margin_avr_cycles = 0xFFFFFFFFu;
    E.st.min_e_high_cycles = E.st.min_e_low_cycles = 0xFFFFFFFFu;
    E.have_edge = 0;
    E.clk = 0; E.latched = 0; E.driving = 0; E.sample_pending = 0;
    E.ev_n = 0; E.linelen = 0;
    memset(E.pin_level, 0, sizeof E.pin_level);
    memset(E.pin_out, 0, sizeof E.pin_out);
    memset(E.watch_hits, 0, sizeof E.watch_hits);
    memset(E.ct, 0, sizeof E.ct);
    memset(E.cdepth, 0, sizeof E.cdepth);
    E.ee_busy = 0;
    E.e_budget = 0;

    avr_global_logger_set(sim_logger);
    avr_interrupt_enable_latch_disabled = E.cfg.legacy_simavr_irq_drop;
    E.avr = avr_make_mcu_by_name("atmega2560");
    if (!E.avr) return -1;
    avr_init(E.avr);
    E.avr->frequency = (uint32_t)AVR_HZ;
    E.avr->log = LOG_WARNING;
    avr_loadcode(E.avr, (uint8_t *)E.cfg.flash_image, E.cfg.flash_size, 0);
    E.avr->codeend = E.cfg.flash_size;
    avr_reset(E.avr);

    E.st.ramend = E.avr->ramend;
    E.st.heap_start = E.cfg.heap_start;
    if (E.cfg.stack_paint && E.cfg.heap_start && E.cfg.heap_start <= E.avr->ramend) {
        memset(&E.avr->data[E.cfg.heap_start], PAINT_BYTE, E.avr->ramend + 1 - E.cfg.heap_start);
        E.st.stack_paint_enabled = 1;
        E.st.stack_paint_boundary = E.cfg.heap_start;
    }
    E.ww_armed = 0;
    for (int i = 0; i < E.ww_n; i++) {
        struct rpu_avr_write_watch_report keep = E.ww[i].rep;
        memset(&E.ww[i].rep, 0, sizeof E.ww[i].rep);
        memcpy(E.ww[i].rep.name, E.ww[i].name, sizeof E.ww[i].name);
        E.ww[i].rep.addr = E.ww[i].addr; E.ww[i].rep.length = E.ww[i].len; E.ww[i].rep.flags = E.ww[i].flags;
        (void)keep;
    }
    E.st.write_watch_count = (uint32_t)E.ww_n;

    /* EEPROM: back the core with the caller's buffer. */
    for (avr_io_t *io = E.avr->io_port; io; io = io->next)
        if (io->kind && !strcmp(io->kind, "eeprom")) E.ee = (avr_eeprom_t *)io;
    if (!E.ee || E.ee->size != EEPROM_BYTES) return -2;
    free(E.ee->eeprom);
    E.ee->eeprom = E.cfg.eeprom;
    avr_io_addr_t eio = AVR_DATA_TO_IO(A_EECR);
    E.ee_orig_w = E.avr->io[eio].w.c;
    E.ee_orig_param = E.avr->io[eio].w.param;
    E.avr->io[eio].w.c = ee_write;

    avr_io_addr_t pio = AVR_DATA_TO_IO(A_PINA);
    E.pina_orig_r = E.avr->io[pio].r.c;
    E.pina_orig_param = E.avr->io[pio].r.param;
    E.avr->io[pio].r.c = pina_read;

    avr_irq_register_notify(pin_irq('G', 2), irq_pg2, NULL);
    avr_irq_register_notify(pin_irq('G', 1), irq_pg1, NULL);
    for (int p = 0; p < RPU_AVR_PIN_COUNT; p++)
        avr_irq_register_notify(pin_irq("GLLEHHG"[p], pinmap[p].bit), irq_ctl, (void *)(intptr_t)p);
    avr_irq_register_notify(avr_io_getirq(E.avr, AVR_IOCTL_UART_GETIRQ('0'), UART_IRQ_OUTPUT),
                            uart_out, NULL);

    E.half_period = (AVR_HZ / E.hz) / 2;
    if (E.half_period < 1) E.half_period = 1;
    *D(A_PING) &= (uint8_t)~0x04;
    E.clk = 0;
    if (E.cfg.clock_mode == RPU_AVR_CLOCK_FROM_MPU)
        avr_cycle_timer_register(E.avr, E.half_period, mpu_clock, NULL);
    apply_inputs();
    drive_pina(BUS_FLOAT);
    return 0;
}

int rpu_avr_engine_init(const struct rpu_avr_engine_config *config)
{
    if (!config || !config->flash_image || !config->flash_size ||
        config->flash_size > 262144 || !config->eeprom || config->eeprom_size != EEPROM_BYTES ||
        !config->bus_read || !config->bus_write)
        return -1;
    if (g_inited) rpu_avr_engine_shutdown();
    memset(&E, 0, sizeof E);
    E.cfg = *config;
    E.hz = config->e_clock_hz ? config->e_clock_hz : 1000000u;
    if (E.hz > 4000000u || E.hz < 1000u) return -1;
    if (!E.cfg.timer1_vector_byte_addr) E.cfg.timer1_vector_byte_addr = 0x44;
    E.t1vec = E.cfg.timer1_vector_byte_addr;
    sha256_hex(config->flash_image, config->flash_size, E.fw_id);
    int rc = create_avr();
    if (rc) { destroy_avr(); return rc; }
    g_inited = 1;
    return 0;
}

void rpu_avr_engine_reset(void)
{
    if (!g_inited) return;
    int n = E.watch_n;
    uint32_t wa[MAX_WATCH];
    uint8_t wt[MAX_WATCH];
    memcpy(wa, E.watch_addr, sizeof wa);
    memcpy(wt, E.watch_timed, sizeof wt);
    destroy_avr();
    create_avr();
    E.watch_n = n;
    memcpy(E.watch_addr, wa, sizeof wa);
    memcpy(E.watch_timed, wt, sizeof wt);
    for (int i = 0; i < n; i++)
        if (wt[i]) E.ct[i].min_cycles = 0xFFFFFFFFu;
}

/* ---- write watches ----------------------------------------------------- */
static void ww_arm(void)
{
    E.ww_armed = 1;
    for (int i = 0; i < E.ww_n; i++)
        memcpy(E.ww[i].snap, &E.avr->data[E.ww[i].addr], E.ww[i].len);
}

static void ww_check(uint32_t pc)
{
    for (int i = 0; i < E.ww_n; i++) {
        uint32_t len = E.ww[i].len;
        const uint8_t *cur = &E.avr->data[E.ww[i].addr];
        if (!memcmp(cur, E.ww[i].snap, len)) continue;
        uint32_t k = 0;
        while (k < len && cur[k] == E.ww[i].snap[k]) k++;
        int owner = 0;
        for (uint32_t o = 0; o < E.ww[i].nown; o++)
            if (pc >= E.ww[i].own[o].lo && pc < E.ww[i].own[o].hi) owner = 1;
        struct rpu_avr_write_watch_report *r = &E.ww[i].rep;
        int guard = (E.ww[i].flags & RPU_AVR_WATCH_GUARD) != 0;
        r->changes++;
        if (owner) { r->owner_writes++; E.st.write_watch_owner_writes++; }
        else { r->foreign_writes++; r->last_foreign_pc = pc; E.st.write_watch_foreign_writes++; }
        if (guard || !owner) {
            E.st.write_watch_violations++;
            if (!r->violated) {
                r->violated = 1;
                r->first_violation_pc = pc;
                r->first_violation_addr = E.ww[i].addr + k;
                r->first_violation_value = cur[k];
                r->first_violation_avr_cycle = E.avr->cycle;
            }
        }
        memcpy(E.ww[i].snap, cur, len);
    }
}

static inline void step_to(uint64_t target)
{
    avr_t *avr = E.avr;
    const uint32_t t1 = E.t1vec, heap = E.cfg.heap_start;
    uint64_t t1n = 0, below = 0;
    uint32_t spmin = E.st.sp_min;
    const int nw = E.watch_n;
    const int wwn = E.ww_n; (void)wwn;
    while (avr->cycle < target) {
        uint32_t pc = avr->pc;
        if (pc == t1) t1n++;
        uint32_t sp = avr->data[R_SPL] | ((uint32_t)avr->data[R_SPH] << 8);
        for (int i = 0; i < nw; i++) {
            if (E.watch_timed[i]) {
                int d = E.cdepth[i];
                if (d > 0 && sp > E.cstack[i][d - 1].sp) {       /* RET/RETI completed */
                    struct rpu_avr_call_timing *c = &E.ct[i];
                    uint32_t dur = (uint32_t)(avr->cycle - E.cstack[i][d - 1].start);
                    E.cdepth[i] = d - 1;
                    c->count++; c->total_cycles += dur;
                    if (dur < c->min_cycles) c->min_cycles = dur;
                    if (dur > c->max_cycles) c->max_cycles = dur;
                    if (c->first_n < 16) c->first[c->first_n++] = dur;
                }
            }
            if (pc == E.watch_addr[i]) {
                E.watch_hits[i]++;
                if (E.watch_timed[i] && E.cdepth[i] < 4) {
                    E.cstack[i][E.cdepth[i]].start = avr->cycle;
                    E.cstack[i][E.cdepth[i]].sp = sp;
                    E.cdepth[i]++;
                }
            }
        }
        if (sp < spmin) spmin = sp;
        if (heap && sp < heap) below++;
        if (E.ww_n) {
            if (!E.ww_armed && pc == E.ww_arm_pc) ww_arm();
        }
        const uint64_t before = avr->cycle;
        int s = avr_run(avr);
        if (avr->cycle < before) {
            /* Batch j: a reset made by the AVR itself (the watchdog's TIMER_RESET): simavr restarts its cycle
             * counter at 0 and drops every cycle timer.  Keep engine time monotonic and drop the engine state
             * that was timed against the old counter (an EEPROM programming cut by the reset, edges). */
            E.cycle_base += before;
            target -= before;
            E.in_run_resets++;
            E.ee_busy = 0;
            E.edge_time = 0;
            E.last_rise_cycle = 0;
            E.last_edge_cycle = 0;
            if (E.cfg.clock_mode == RPU_AVR_CLOCK_FROM_MPU)
                avr_cycle_timer_register(avr, E.half_period, mpu_clock, NULL);
        }
        if (E.ww_armed) ww_check(pc);
        if (s == cpu_Crashed || s == cpu_Done || s == cpu_Stopped) {
            E.st.crashed = 1;
            break;
        }
    }
    E.st.timer1_isr_entries += t1n;
    E.st.sp_below_static_end_steps += below;
    E.st.sp_min = spmin;
}

int rpu_avr_engine_run(int e_cycles)
{
    if (!g_inited || e_cycles <= 0) return 0;
    E.e_budget += (uint64_t)e_cycles;
    uint64_t target = E.e_budget * AVR_HZ / E.hz;
    if (E.st.crashed) {
        return e_cycles;
    }
    step_to(target > E.cycle_base ? target - E.cycle_base : 0);
    E.st.avr_cycles = E.cycle_base + E.avr->cycle;
    return e_cycles;
}

uint64_t rpu_avr_engine_micros(void)
{
    return g_inited ? (E.cycle_base + E.avr->cycle) / 16 : 0;
}

uint32_t rpu_avr_engine_in_run_resets(void)
{
    return g_inited ? E.in_run_resets : 0;
}

void rpu_avr_engine_set_rpu_switch(int closed)
{
    if (!g_inited) return;
    E.cfg.rpu_switch_closed = closed != 0;
    apply_inputs();
}

uint8_t rpu_avr_engine_peek_data(uint16_t addr)
{
    return (g_inited && addr < 0x2200) ? E.avr->data[addr] : 0;
}

void rpu_avr_engine_set_diagnostic(int requested)
{
    if (!g_inited) return;
    E.cfg.diagnostic_requested = requested != 0;
    apply_inputs();
}

void rpu_avr_engine_get_stats(struct rpu_avr_engine_stats *s)
{
    if (!s) return;
    *s = E.st;
    if (!g_inited) return;
    s->avr_cycles = E.cycle_base + E.avr->cycle;
    if (s->stack_paint_enabled) {
        uint32_t a = s->stack_paint_boundary, top = E.avr->ramend;
        while (a <= top && E.avr->data[a] == PAINT_BYTE) a++;
        if (a > top) {
            s->stack_paint_lowest_modified = 0;
            s->stack_paint_hw_bytes = 0;
        } else {
            s->stack_paint_lowest_modified = a;
            s->stack_paint_hw_bytes = top + 1 - a;
        }
        s->stack_paint_below_sp_bytes = 0;
        if (s->stack_paint_hw_bytes && s->sp_min != 0xFFFFFFFFu && a < s->sp_min + 1)
            s->stack_paint_below_sp_bytes = s->sp_min + 1 - a;
    }
}

void rpu_avr_engine_set_write_watch_arm_pc(uint32_t byte_address) { E.ww_arm_pc = byte_address; }

int rpu_avr_engine_add_write_watch(uint32_t addr, uint32_t len, const char *name, uint32_t flags,
                                   const struct rpu_avr_pc_range *owners, uint32_t n_owners)
{
    if (E.ww_n >= MAX_WWATCH || !len || len > 256 || addr + len > 0x2200 || n_owners > 8 ||
        (n_owners && !owners))
        return -1;
    int i = E.ww_n++;
    memset(&E.ww[i], 0, sizeof E.ww[i]);
    snprintf(E.ww[i].name, sizeof E.ww[i].name, "%s", name ? name : "?");
    E.ww[i].addr = addr; E.ww[i].len = len; E.ww[i].flags = flags; E.ww[i].nown = n_owners;
    for (uint32_t k = 0; k < n_owners; k++) E.ww[i].own[k] = owners[k];
    memcpy(E.ww[i].rep.name, E.ww[i].name, sizeof E.ww[i].name);
    E.ww[i].rep.addr = addr; E.ww[i].rep.length = len; E.ww[i].rep.flags = flags;
    E.st.write_watch_count = (uint32_t)E.ww_n;
    if (g_inited && E.ww_armed) memcpy(E.ww[i].snap, &E.avr->data[addr], len);
    return i;
}

int rpu_avr_engine_get_write_watch(int i, struct rpu_avr_write_watch_report *r)
{
    if (i < 0 || i >= E.ww_n || !r) return -1;
    *r = E.ww[i].rep;
    return 0;
}

int rpu_avr_engine_format_safety_summary(char *buf, unsigned size)
{
    struct rpu_avr_engine_stats st;
    rpu_avr_engine_get_stats(&st);
    int n = snprintf(buf, size,
        "diner_rpu avr safety: sp_min 0x%04x ramend 0x%04x sp_used %u B | "
        "sp_below_static_end_steps %llu (0 = no observed SP/static overlap by SP value) | "
        "ramend_wrap_messages %llu (0 = no RAMEND wrapping observed; not an object bounds check) | ",
        (unsigned)st.sp_min, (unsigned)st.ramend,
        (unsigned)(st.sp_min <= st.ramend ? st.ramend - st.sp_min : 0),
        (unsigned long long)st.sp_below_static_end_steps, (unsigned long long)st.ramend_wrap_messages);
    if (n < 0 || (unsigned)n >= size) return n;
    if (st.stack_paint_enabled)
        n += snprintf(buf + n, size - n, "stack_paint hw %u B from 0x%04x (lowest modified 0x%04x, below_sp %u B) | ",
                      (unsigned)st.stack_paint_hw_bytes, (unsigned)st.stack_paint_boundary,
                      (unsigned)st.stack_paint_lowest_modified, (unsigned)st.stack_paint_below_sp_bytes);
    else
        n += snprintf(buf + n, size - n, "stack_paint OFF | ");
    if ((unsigned)n >= size) return n;
    n += snprintf(buf + n, size - n, "write_watch watches %u violations %llu (foreign %llu, owner %llu)%s",
                  (unsigned)st.write_watch_count, (unsigned long long)st.write_watch_violations,
                  (unsigned long long)st.write_watch_foreign_writes, (unsigned long long)st.write_watch_owner_writes,
                  st.write_watch_count ? "" : " [none configured]");
    if ((unsigned)n < size)
        n += snprintf(buf + n, size - n, " | sim_errors %llu crashed %u",
                      (unsigned long long)st.sim_errors, (unsigned)st.crashed);
    return n;
}

int rpu_avr_engine_watch_pc(uint32_t a)
{
    if (E.watch_n >= MAX_WATCH) return -1;
    E.watch_addr[E.watch_n] = a;
    return E.watch_n++;
}

int rpu_avr_engine_time_call(uint32_t a)
{
    int slot = rpu_avr_engine_watch_pc(a);
    if (slot >= 0) {
        E.watch_timed[slot] = 1;
        E.ct[slot].min_cycles = 0xFFFFFFFFu;
    }
    return slot;
}

int rpu_avr_engine_get_call_timing(int slot, struct rpu_avr_call_timing *out)
{
    if (slot < 0 || slot >= E.watch_n || !out) return -1;
    *out = E.ct[slot];
    return 0;
}

uint64_t rpu_avr_engine_pc_hits(int slot)
{
    return (slot >= 0 && slot < MAX_WATCH) ? E.watch_hits[slot] : 0;
}

int rpu_avr_engine_pin_event_count(void) { return E.ev_n; }

int rpu_avr_engine_get_pin_event(int i, struct rpu_avr_pin_event *ev)
{
    if (i < 0 || i >= E.ev_n || !ev) return -1;
    *ev = E.ev[i];
    return 0;
}

const char *rpu_avr_engine_firmware_id(void) { return E.fw_id; }

void rpu_avr_engine_shutdown(void)
{
    if (!g_inited) return;
    destroy_avr();
    g_inited = 0;
}
