// ======================================================================================
// CP437 <-> UTF-8 conversion for the Remote Config web tool.
//
// The OLED font (Adafruit GFX's built-in font with cp437(true)) uses the IBM Code Page 437
// character set, so the on-device trivia CSVs store accented letters as single CP437 bytes
// (e.g. 0x82 = e-acute). Browsers speak UTF-8. Everything that crosses the web boundary is
// therefore converted: CP437 -> UTF-8 when sending file contents to the page, UTF-8 -> CP437
// when saving what the page sends back. Without this, a single load+save in the web editor
// replaced every accent with U+FFFD, permanently.
//
// Plain C (no Arduino types) so it can be unit-tested on a desktop compiler.
// ======================================================================================
#pragma once
#include <stdint.h>
#include <stddef.h>

// Unicode code point for each CP437 byte 0x80..0xFF.
static const uint16_t CP437_HIGH_TO_UNICODE[128] = {
  0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
  0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
  0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
  0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
  0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
  0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
  0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
  0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

// ASCII stand-ins for Latin-1 letters U+00C0..U+00FF that CP437 has no glyph for (mostly
// accented capitals like A-grave, E-circumflex). Only used after an exact CP437 match fails.
static const char LATIN1_ASCII_FALLBACK[65] =
  "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";

// Encodes one code point as UTF-8 into out (needs 4 bytes). Returns the byte count.
static inline int tt_utf8Encode(uint32_t cp, char* out) {
  if (cp < 0x80) { out[0] = (char)cp; return 1; }
  if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

// Decodes one UTF-8 sequence starting at s[i]. Writes the code point to *cp and returns how many
// bytes it used (always >= 1). A malformed sequence consumes 1 byte and yields U+FFFD.
static inline size_t tt_utf8Decode(const uint8_t* s, size_t len, size_t i, uint32_t* cp) {
  uint8_t b = s[i];
  size_t need;
  uint32_t v;
  if (b < 0x80) { *cp = b; return 1; }
  else if ((b & 0xE0) == 0xC0) { need = 1; v = b & 0x1F; }
  else if ((b & 0xF0) == 0xE0) { need = 2; v = b & 0x0F; }
  else if ((b & 0xF8) == 0xF0) { need = 3; v = b & 0x07; }
  else { *cp = 0xFFFD; return 1; }
  if (i + need >= len) { *cp = 0xFFFD; return 1; } // sequence runs past the end of the input
  for (size_t k = 1; k <= need; k++) {
    uint8_t c = s[i + k];
    if ((c & 0xC0) != 0x80) { *cp = 0xFFFD; return 1; }
    v = (v << 6) | (c & 0x3F);
  }
  *cp = v;
  return need + 1;
}

// Converts one code point to the CP437 byte(s) the OLED can draw. Writes 1-3 bytes into out and
// returns the count. Exact CP437 glyphs first, then readable ASCII stand-ins for the characters
// phones commonly insert (smart quotes, dashes, ellipsis, oe-ligature, accented capitals), then '?'.
static inline int tt_unicodeToCp437(uint32_t cp, char* out) {
  if (cp < 0x80) { out[0] = (char)cp; return 1; }
  for (int k = 0; k < 128; k++) {
    if (CP437_HIGH_TO_UNICODE[k] == cp) { out[0] = (char)(0x80 + k); return 1; }
  }
  switch (cp) {
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: case 0x00B4: out[0] = '\''; return 1;
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: out[0] = '"'; return 1;
    case 0x2013: case 0x2014: case 0x2212: out[0] = '-'; return 1;
    case 0x2026: out[0] = '.'; out[1] = '.'; out[2] = '.'; return 3;
    case 0x0152: out[0] = 'O'; out[1] = 'E'; return 2;
    case 0x0153: out[0] = 'o'; out[1] = 'e'; return 2;
    case 0x202F: case 0x2009: case 0x2002: case 0x2003: out[0] = ' '; return 1;
  }
  if (cp >= 0xC0 && cp <= 0xFF) { out[0] = LATIN1_ASCII_FALLBACK[cp - 0xC0]; return 1; }
  out[0] = '?';
  return 1;
}
