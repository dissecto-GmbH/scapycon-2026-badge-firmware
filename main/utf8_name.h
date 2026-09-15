#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Glyph indices for German letters in font8x8_basic[128..134]. */
#define UTF8_GLYPH_AE     128  /* ä */
#define UTF8_GLYPH_OE     129  /* ö */
#define UTF8_GLYPH_UE     130  /* ü */
#define UTF8_GLYPH_AE_UP  131  /* Ä */
#define UTF8_GLYPH_OE_UP  132  /* Ö */
#define UTF8_GLYPH_UE_UP  133  /* Ü */
#define UTF8_GLYPH_SS     134  /* ß */
#define UTF8_GLYPH_COUNT  135

/**
 * Decode one UTF-8 character at s.
 * Accepts ASCII or 2-byte sequences in U+0080..U+07FF (C2/C3 lead).
 * Returns false on NUL, invalid, or truncated input.
 */
bool utf8_next(const char *s, uint32_t *cp, size_t *nbytes);

/** True if printable ASCII or ÄÖÜäöüß. */
bool utf8_name_codepoint_allowed(uint32_t cp);

/** Number of Unicode characters (not bytes) in a valid sanitized name. */
size_t utf8_char_count(const char *s);

/**
 * Map codepoint to font glyph index (0..134).
 * Unknown codepoints map to '?'.
 */
unsigned utf8_codepoint_to_glyph(uint32_t cp);

/** Byte length of the last UTF-8 character in s[0..len). 0 if empty/invalid. */
size_t utf8_prev_char_len(const char *s, size_t len);
