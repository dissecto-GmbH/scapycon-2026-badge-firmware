#include "utf8_name.h"

bool utf8_next(const char *s, uint32_t *cp, size_t *nbytes)
{
    if (!s || !cp || !nbytes) {
        return false;
    }

    const unsigned char *p = (const unsigned char *)s;
    unsigned char c0 = p[0];
    if (c0 == 0) {
        return false;
    }

    if (c0 < 0x80) {
        *cp = c0;
        *nbytes = 1;
        return true;
    }

    /* Only 2-byte form used for German letters (lead C3 for Latin-1 Supplement). */
    if ((c0 & 0xE0) == 0xC0) {
        unsigned char c1 = p[1];
        if (c1 < 0x80 || c1 > 0xBF) {
            return false;
        }
        /* Reject overlong encodings (lead C0/C1). */
        if (c0 < 0xC2) {
            return false;
        }
        *cp = ((uint32_t)(c0 & 0x1F) << 6) | (uint32_t)(c1 & 0x3F);
        *nbytes = 2;
        return true;
    }

    return false;
}

bool utf8_name_codepoint_allowed(uint32_t cp)
{
    if (cp >= 32 && cp < 127) {
        return true;
    }
    switch (cp) {
    case 0x00C4: /* Ä */
    case 0x00D6: /* Ö */
    case 0x00DC: /* Ü */
    case 0x00E4: /* ä */
    case 0x00F6: /* ö */
    case 0x00FC: /* ü */
    case 0x00DF: /* ß */
        return true;
    default:
        return false;
    }
}

size_t utf8_char_count(const char *s)
{
    if (!s) {
        return 0;
    }
    size_t n = 0;
    while (*s) {
        uint32_t cp;
        size_t nb;
        if (!utf8_next(s, &cp, &nb)) {
            break;
        }
        s += nb;
        n++;
    }
    return n;
}

unsigned utf8_codepoint_to_glyph(uint32_t cp)
{
    if (cp < 128) {
        return (unsigned)cp;
    }
    switch (cp) {
    case 0x00E4:
        return UTF8_GLYPH_AE;
    case 0x00F6:
        return UTF8_GLYPH_OE;
    case 0x00FC:
        return UTF8_GLYPH_UE;
    case 0x00C4:
        return UTF8_GLYPH_AE_UP;
    case 0x00D6:
        return UTF8_GLYPH_OE_UP;
    case 0x00DC:
        return UTF8_GLYPH_UE_UP;
    case 0x00DF:
        return UTF8_GLYPH_SS;
    default:
        return (unsigned)'?';
    }
}

size_t utf8_prev_char_len(const char *s, size_t len)
{
    if (!s || len == 0) {
        return 0;
    }

    /* ASCII or single trailing byte of a 2-byte sequence. */
    unsigned char last = (unsigned char)s[len - 1];
    if (last < 0x80) {
        return 1;
    }
    if (len >= 2 && (last & 0xC0) == 0x80) {
        unsigned char lead = (unsigned char)s[len - 2];
        if ((lead & 0xE0) == 0xC0) {
            return 2;
        }
    }
    /* Orphan continuation — drop one byte. */
    return 1;
}
