#ifndef RPU_ALPHA_SCAN_H
#define RPU_ALPHA_SCAN_H
#include <stdint.h>
// System 11C raw alpha display scan.  The LA-4 alpha displays are driven by the stock ROM with
// a four-byte, two-row sequence per strobe (upper row words on $2C00/$2C02, lower row words on
// $2802/$3400, strobe and shadow bits on $2800), with the segment data inverted (a segment is on
// when its bit is 0).  The library's own scan does not use that sequence, so RPU_SetAlphaRawFrame()
// hands the library finished words and the display ISR replays the stock sequence.
namespace RPUAlpha {
// Storage is owned by foreground callers and the non-nested display ISR only.
// Foreground must serialize calls; reserve/publish/cancel need atomic sections.
static const uint8_t None = 0xff;
static const uint8_t Release = 0xfe;
struct State {
  volatile uint16_t frames[3][2][16];
  volatile uint8_t active;
  volatile uint8_t pending;
  uint8_t reserve() const {
    for (uint8_t i = 0; i < 3; ++i)
      if (i != active && i != pending) return i;
    return None;
  }
  void boundary() {
    const uint8_t next = pending;
    if (next != None) {
      active = next == Release ? None : next;
      pending = None;
    }
  }
  void cancel() { active = None; pending = None; }
};
struct Ports { uint8_t upperLo, upperHi, lowerLo, lowerHi; };
inline Ports encode(uint16_t upper, uint16_t lower, uint8_t digit) {
  const uint8_t special = digit == 15 ? 0x80 : 0;
  const Ports p = {
    static_cast<uint8_t>(static_cast<uint8_t>(~upper) | special),
    static_cast<uint8_t>(static_cast<uint8_t>(~(upper >> 8)) | special),
    static_cast<uint8_t>(static_cast<uint8_t>(~lower) | special),
    static_cast<uint8_t>(static_cast<uint8_t>(~(lower >> 8)) | special)
  };
  return p;
}
// This is the exact nine-write display subsequence at stock $D28B..$D2E2.
// Call once per digit 0..15. Timing/frequency remains the caller's policy.
template<class Writer>
inline void write(uint16_t upper, uint16_t lower, uint8_t digit, uint8_t shadow, Writer writer) {
  const Ports p = encode(upper, lower, digit);
  writer(0x2c02, 0xff);
  writer(0x2c00, 0xff);
  writer(0x2802, 0xff);
  writer(0x3400, 0xff);
  writer(0x2800, static_cast<uint8_t>((shadow & 0xf0u) | digit));
  writer(0x2c02, p.upperLo);
  writer(0x2c00, p.upperHi);
  writer(0x2802, p.lowerLo);
  writer(0x3400, p.lowerHi);
}
inline uint16_t fromPacked14(uint16_t glyph) {
  return static_cast<uint16_t>((glyph & 0x007fu) | ((glyph & 0x3f80u) << 1));
}
}
#endif
