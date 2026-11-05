/* Small driver: runs the RPU firmware image against a minimal S11C PIA bus.
 *   rpu_avr_cli <image.hex> [--seconds N] [--eeprom file] [--mpu-clock]
 *               [--heap-start ADDR] [--switch-closed] [--stack-paint]
 *               [--arm-pc ADDR] [--watch NAME:ADDR:LEN[:guard][:OWNLO-OWNHI[,OWNLO-OWNHI...]]]...
 * --stack-paint needs --heap-start (paints [heap-start, RAMEND] with 0xA5 and reports the stack
 * high-water mark from what was actually written). --watch adds a write watch (see
 * rpu_avr_engine.h): with "guard" any change is a violation, otherwise a change by a PC outside
 * the owner ranges is. Watches are armed when the PC first reaches --arm-pc (e.g. main).
 * Prints serial output and engine statistics; exits 0 when boot probes passed
 * (PIA activity seen, no protocol violations). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "rpu_avr_engine.h"

struct pia { uint8_t ora, orb, ddra, ddrb, cra, crb; };
static const uint16_t bases[7] = {0x2100, 0xFFFF, 0x2400, 0x2800, 0x2C00, 0x3000, 0x3400};
static struct pia pias[7];
/* $2200-$2203 is a write-only latch (U28); reads return 0x3C like the host test. */
static uint64_t forbidden, reads_total, writes_total;
static uint8_t last_bus = 0xFF;   /* open bus = last byte on the data bus */

static int find(uint16_t a)
{
    for (int i = 0; i < 7; i++)
        if (a >= bases[i] && a < bases[i] + 4) return i;
    return -1;
}

static uint8_t bus_read_(uint16_t a);
static uint8_t bus_read(void *c, uint16_t a)
{
    (void)c; uint8_t v = bus_read_(a); last_bus = v; return v;
}
static uint8_t bus_read_(uint16_t a)
{
    reads_total++;
    if (a < 0x800 || a >= 0x4000) forbidden++;
    if (a >= 0x2200 && a < 0x2300) return last_bus;
    int i = find(a);
    if (i < 0) return 0xFF;
    struct pia *p = &pias[i];
    switch (a & 3) {
    case 0: return (p->cra & 4) ? p->ora : p->ddra;
    case 1: return p->cra;
    case 2: return (p->crb & 4) ? p->orb : p->ddrb;
    default: return p->crb;
    }
}

static void bus_write(void *c, uint16_t a, uint8_t d)
{
    (void)c; writes_total++; last_bus = d;
    if (a < 0x800 || a >= 0x4000) forbidden++;
    int i = find(a);
    if (i < 0) return;
    struct pia *p = &pias[i];
    switch (a & 3) {
    case 0: if (p->cra & 4) p->ora = d; else p->ddra = d; break;
    case 1: p->cra = d & 0x3F; break;
    case 2: if (p->crb & 4) p->orb = d; else p->ddrb = d; break;
    default: p->crb = d & 0x3F; break;
    }
}

static void logf_(void *c, const char *t) { (void)c; printf("serial: %s\n", t); }

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s image.hex [--seconds N] ...\n", argv[0]); return 2; }
    double seconds = 3.0;
    const char *eepfile = NULL;
    struct rpu_avr_engine_config cfg;
    memset(&cfg, 0, sizeof cfg);
    uint32_t arm_pc = 0;
    struct { char name[32]; uint32_t a, n, flags, no; struct rpu_avr_pc_range o[8]; } wl[16];
    int nwl = 0;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--eeprom") && i + 1 < argc) eepfile = argv[++i];
        else if (!strcmp(argv[i], "--mpu-clock")) cfg.clock_mode = RPU_AVR_CLOCK_FROM_MPU;
        else if (!strcmp(argv[i], "--heap-start") && i + 1 < argc) cfg.heap_start = (uint32_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--switch-closed")) cfg.rpu_switch_closed = 1;
        else if (!strcmp(argv[i], "--stack-paint")) cfg.stack_paint = 1;
        else if (!strcmp(argv[i], "--arm-pc") && i + 1 < argc) arm_pc = (uint32_t)strtoul(argv[++i], 0, 0);
        else if (!strcmp(argv[i], "--watch") && i + 1 < argc && nwl < 16) {
            char *s = strdup(argv[++i]), *save = NULL, *tok = strtok_r(s, ":", &save);
            int k = 0;
            memset(&wl[nwl], 0, sizeof wl[nwl]);
            for (; tok; tok = strtok_r(NULL, ":", &save), k++) {
                if (k == 0) snprintf(wl[nwl].name, sizeof wl[nwl].name, "%s", tok);
                else if (k == 1) wl[nwl].a = (uint32_t)strtoul(tok, 0, 0);
                else if (k == 2) wl[nwl].n = (uint32_t)strtoul(tok, 0, 0);
                else if (!strcmp(tok, "guard")) wl[nwl].flags |= RPU_AVR_WATCH_GUARD;
                else {
                    char *sv2 = NULL;
                    for (char *r = strtok_r(tok, ",", &sv2); r && wl[nwl].no < 8; r = strtok_r(NULL, ",", &sv2)) {
                        char *dash = strchr(r, '-');
                        if (!dash) continue;
                        *dash = 0;
                        wl[nwl].o[wl[nwl].no].lo = (uint32_t)strtoul(r, 0, 0);
                        wl[nwl].o[wl[nwl].no].hi = (uint32_t)strtoul(dash + 1, 0, 0);
                        wl[nwl].no++;
                    }
                }
            }
            free(s);
            nwl++;
        }
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("hex"); return 2; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *text = malloc(n + 1); if (fread(text, 1, n, f) != (size_t)n) return 2; fclose(f);
    static uint8_t flash[262144]; uint32_t size = 0;
    if (rpu_avr_engine_parse_ihex(text, (uint32_t)n, flash, sizeof flash, &size)) { fprintf(stderr, "bad hex\n"); return 2; }
    static uint8_t eeprom[4096]; memset(eeprom, 0xFF, sizeof eeprom);
    if (eepfile && (f = fopen(eepfile, "rb"))) { if (fread(eeprom, 1, 4096, f)) {} fclose(f); }
    cfg.bus_read = bus_read; cfg.bus_write = bus_write; cfg.log = logf_;
    cfg.flash_image = flash; cfg.flash_size = size; cfg.eeprom = eeprom; cfg.eeprom_size = 4096;
    if (rpu_avr_engine_init(&cfg)) { fprintf(stderr, "init failed\n"); return 2; }
    rpu_avr_engine_set_write_watch_arm_pc(arm_pc);
    for (int i = 0; i < nwl; i++)
        if (rpu_avr_engine_add_write_watch(wl[i].a, wl[i].n, wl[i].name, wl[i].flags, wl[i].o, wl[i].no) < 0)
            fprintf(stderr, "bad --watch %s\n", wl[i].name);
    printf("firmware_id %s (%u bytes)\n", rpu_avr_engine_firmware_id(), size);
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    int total = (int)(seconds * 1e6);
    for (int done = 0; done < total; done += 1000) rpu_avr_engine_run(1000);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double wall = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    struct rpu_avr_engine_stats st; rpu_avr_engine_get_stats(&st);
    printf("virtual %.3f s, wall %.3f s, %.1f AVR MHz (%.2fx real time)\n", seconds, wall,
           st.avr_cycles / wall / 1e6, seconds / wall);
    printf("reads %llu writes %llu isr %llu eeprom_writes %llu forbidden %llu viol %llu\n",
           (unsigned long long)st.bus_reads, (unsigned long long)st.bus_writes,
           (unsigned long long)st.timer1_isr_entries, (unsigned long long)st.eeprom_writes,
           (unsigned long long)forbidden, (unsigned long long)st.protocol_violations);
    printf("sp_min 0x%x ramend 0x%x ramend_wrap_messages %llu sim_errors %llu hash %016llx\n", st.sp_min,
           st.ramend, (unsigned long long)st.ramend_wrap_messages, (unsigned long long)st.sim_errors,
           (unsigned long long)st.trace_hash);
    char sum[900];
    rpu_avr_engine_format_safety_summary(sum, sizeof sum);
    printf("%s\n", sum);
    for (int i = 0; i < nwl; i++) {
        struct rpu_avr_write_watch_report r;
        if (!rpu_avr_engine_get_write_watch(i, &r))
            printf("watch %s [0x%x+%u]%s changes %llu owner %llu foreign %llu%s\n", r.name, r.addr, r.length,
                   (r.flags & RPU_AVR_WATCH_GUARD) ? " guard" : "", (unsigned long long)r.changes,
                   (unsigned long long)r.owner_writes, (unsigned long long)r.foreign_writes,
                   r.violated ? "  VIOLATION" : "");
    }
    rpu_avr_engine_shutdown();
    return (reads_total && writes_total && !st.protocol_violations) ? 0 : 1;
}
