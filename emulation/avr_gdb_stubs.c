/* simavr's GDB stub server is not used by the engine (it drags in sockets and
 * the hex/ELF loaders); these no-ops satisfy the references in sim_avr.c and
 * sim_core.c. */
#include "sim_avr.h"
#include "sim_gdb.h"

int avr_gdb_init(avr_t *avr) { (void)avr; return -1; }
void avr_deinit_gdb(avr_t *avr) { (void)avr; }
int avr_gdb_processor(avr_t *avr, int sleep) { (void)avr; (void)sleep; return 0; }
void avr_gdb_handle_watchpoints(avr_t *g, uint16_t addr, enum avr_gdb_watch_type type)
{ (void)g; (void)addr; (void)type; }
void avr_gdb_handle_break(avr_t *g) { (void)g; }
