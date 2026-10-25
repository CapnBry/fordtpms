#include "tpms_ford.h"

#include <string.h>

static inline int chip_at(const uint8_t *b, size_t pos)
{
    return (b[pos >> 3] >> (7 - (pos & 7))) & 1;
}

void tpms_ford_fix_checksum(uint8_t b[8])
{
    b[7] = (uint8_t)(b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6]);
}

void tpms_ford_encode(const uint8_t b[8], uint8_t frame[TPMS_FORD_FRAME_BYTES])
{
    static const uint8_t hdr[4] = {0x55, 0x55, 0x55, 0x56};
    memcpy(frame, hdr, sizeof(hdr));
    for (int i = 0; i < TPMS_FORD_PAYLOAD_BYTES; i++) {
        uint16_t chips = 0;
        for (int k = 7; k >= 0; k--) {
            chips = (uint16_t)(chips << 2 | (((b[i] >> k) & 1) ? 0x2 : 0x1));  /* '10' = 1, '01' = 0 */
        }
        frame[4 + 2 * i] = (uint8_t)(chips >> 8);
        frame[5 + 2 * i] = (uint8_t)chips;
    }
}

tpms_ford_status_t tpms_ford_decode_bytes(const uint8_t b[8], tpms_ford_t *out)
{
    if ((uint8_t)(b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6]) != b[7]) {
        return TPMS_FORD_ERR_CHECKSUM;
    }
    /* An all-zero payload sums to zero and its flags pass, but it is just an
     * alternating chip run (e.g. a long preamble from another device). */
    if ((b[0] | b[1] | b[2] | b[3] | b[4] | b[5] | b[6]) == 0) {
        return TPMS_FORD_ERR_ZERO;
    }

    tpms_ford_t t = {0};
    memcpy(t.raw, b, sizeof(t.raw));
    t.id     = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
    t.code   = (uint32_t)b[4] << 16 | (uint32_t)b[5] << 8 | b[6];
    t.flags  = b[6];
    t.psi_x4 = (uint16_t)(((b[6] & 0x20) << 3) | b[4]);

    if ((b[5] & 0x80) == 0) {
        t.temp_valid = true;
        t.temp_c     = (int8_t)((b[5] & 0x7f) - 56);
    }

    /* Same acceptance rules as rtl_433: unknown flag combinations are rejected. */
    uint8_t unknown = 0;
    switch (b[6] & 0x4c) {
    case 0x08: t.mode = TPMS_MODE_LEARN; break;
    case 0x04: t.mode = TPMS_MODE_REST; break;
    case 0x44: t.mode = TPMS_MODE_MOVING; break;
    default:   unknown = b[6] & 0x4c; break;
    }
    unknown |= b[6] & 0x90;
    if (unknown) {
        return TPMS_FORD_ERR_FLAGS;
    }
    t.unknown_3 = b[6] & 0x03;

    if (out) {
        *out = t;
    }
    return TPMS_FORD_OK;
}

tpms_ford_status_t tpms_ford_decode_chips(const uint8_t *chips, size_t nbits, size_t bitpos,
                                          bool invert, tpms_ford_t *out)
{
    if (bitpos + 2 * TPMS_FORD_PAYLOAD_BITS > nbits) {
        return TPMS_FORD_ERR_SHORT;
    }
    uint8_t bytes[8] = {0};
    for (int i = 0; i < TPMS_FORD_PAYLOAD_BITS; i++) {
        int c1 = chip_at(chips, bitpos + 2 * i);
        int c2 = chip_at(chips, bitpos + 2 * i + 1);
        if (c1 == c2) {
            return TPMS_FORD_ERR_MANCHESTER;
        }
        /* rtl_433 inverts the stream and keeps the 2nd chip: i.e. the 1st raw chip. */
        int bit = invert ? !c1 : c1;
        bytes[i >> 3] |= (uint8_t)(bit << (7 - (i & 7)));
    }
    return tpms_ford_decode_bytes(bytes, out);
}

uint64_t tpms_ford_bad_pairs(const uint8_t *chips, size_t nbits, size_t bitpos)
{
    if (bitpos + 2 * TPMS_FORD_PAYLOAD_BITS > nbits) {
        return 0;
    }
    uint64_t bad = 0;
    for (int i = 0; i < TPMS_FORD_PAYLOAD_BITS; i++) {
        if (chip_at(chips, bitpos + 2 * i) == chip_at(chips, bitpos + 2 * i + 1)) {
            bad |= 1ull << i;
        }
    }
    return bad;
}

tpms_ford_status_t tpms_ford_search(const uint8_t *chips, size_t nbits, tpms_ford_t *out)
{
    tpms_ford_status_t best = TPMS_FORD_ERR_NOSYNC;
    uint16_t win = 0;
    for (size_t pos = 0; pos < nbits; pos++) {
        win = (uint16_t)(win << 1 | chip_at(chips, pos));
        if (pos < 15) {
            continue;
        }
        bool invert;
        if (win == 0x5556) {
            invert = false;
        } else if (win == 0xAAA9) {
            invert = true;
        } else {
            continue;
        }
        tpms_ford_status_t st = tpms_ford_decode_chips(chips, nbits, pos + 1, invert, out);
        if (st == TPMS_FORD_OK) {
            return st;
        }
        /* Prefer reporting the error that got furthest into the packet. */
        if (best == TPMS_FORD_ERR_NOSYNC || st > best) {
            best = st;
        }
    }
    return best;
}

const char *tpms_ford_status_str(tpms_ford_status_t st)
{
    switch (st) {
    case TPMS_FORD_OK: return "ok";
    case TPMS_FORD_ERR_SHORT: return "short";
    case TPMS_FORD_ERR_MANCHESTER: return "manchester";
    case TPMS_FORD_ERR_CHECKSUM: return "checksum";
    case TPMS_FORD_ERR_FLAGS: return "flags";
    case TPMS_FORD_ERR_ZERO: return "zero";
    case TPMS_FORD_ERR_NOSYNC: return "nosync";
    }
    return "?";
}

const char *tpms_ford_mode_str(tpms_ford_mode_t m)
{
    switch (m) {
    case TPMS_MODE_REST: return "REST";
    case TPMS_MODE_MOVING: return "MOVING";
    case TPMS_MODE_LEARN: return "LEARN";
    }
    return "?";
}
