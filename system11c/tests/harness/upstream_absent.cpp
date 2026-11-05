// Linked ONLY when the tests run against upstream RPU.  These functions do not exist there, so the
// tests that call them would not build.  Each stub records that it was called and does nothing, which
// is exactly what upstream can do for the caller: nothing.  The test then fails on its own assertions
// and says "not in upstream" in the reason.
#include <stdint.h>
typedef bool boolean;
typedef uint8_t byte;
extern int g_absent_calls;
extern const char* g_absent_names[8];
static void absent(const char* n) {
  for (int i = 0; i < g_absent_calls && i < 8; i++) if (g_absent_names[i] == n) return;
  if (g_absent_calls < 8) g_absent_names[g_absent_calls++] = n;
}
void RPU_SetContinuousSolenoidMax(boolean, byte, unsigned short) { absent("RPU_SetContinuousSolenoidMax"); }
void RPU_SetSolenoidHoldPwm(byte, byte, byte) { absent("RPU_SetSolenoidHoldPwm"); }
boolean RPU_SolenoidStackBusyShared() { absent("RPU_SolenoidStackBusyShared"); return false; }
boolean RPU_SetAlphaRawFrame(const uint16_t[2][16]) { absent("RPU_SetAlphaRawFrame"); return false; }
