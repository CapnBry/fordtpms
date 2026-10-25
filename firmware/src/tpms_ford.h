/*
 * Ford TPMS packet decoder (FSK, Manchester, 8 bytes, additive checksum).
 *
 * Port of rtl_433 src/devices/tpms_ford.c. Air format, as chips at 19.23 kchip/s
 * (52 us each), with the higher FSK tone taken as '1':
 *
 *     55 55 55 56 | 128 Manchester chips ('10' = 1, '01' = 0)
 *
 * The 0x55 run is the preamble (Manchester zeros; 28 to 61+ chips in the
 * rtl_433 captures). The final "0110" is a Manchester '1' start bit: its "11"
 * chip pair breaks the alternation, so 0x5556 marks the frame start exactly,
 * whereas 0x5555 would match anywhere in the preamble.
 *
 * which yields 8 bytes: II II II II PP TT FF CC
 *   I = sensor ID, P = pressure (PSI * 4, 9th bit in F & 0x20),
 *   T = temperature (C + 56, invalid when 0x80 set), F = flags,
 *   C = sum of bytes 0..6.
 *
 * This file is plain C with no ESP-IDF dependencies so it can be unit tested
 * on the host (see test/host).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TPMS_FORD_PAYLOAD_BITS   64
#define TPMS_FORD_PAYLOAD_BYTES  8
#define TPMS_FORD_CHIP_BYTES     16  /* 128 Manchester chips after the sync */

typedef enum {
    TPMS_FORD_OK = 0,
    TPMS_FORD_ERR_SHORT,      /* not enough chips after the preamble */
    TPMS_FORD_ERR_MANCHESTER, /* invalid chip pair before 64 bits were decoded */
    TPMS_FORD_ERR_CHECKSUM,
    TPMS_FORD_ERR_FLAGS,      /* checksum ok but flag bits rtl_433 rejects */
    TPMS_FORD_ERR_ZERO,       /* all-zero payload: '0101..' chips after a preamble, not a sensor */
    TPMS_FORD_ERR_NOSYNC,     /* preamble search found nothing */
} tpms_ford_status_t;

typedef enum {
    TPMS_MODE_REST = 0,
    TPMS_MODE_MOVING,
    TPMS_MODE_LEARN,
} tpms_ford_mode_t;

typedef struct {
    uint8_t  raw[TPMS_FORD_PAYLOAD_BYTES];
    uint32_t id;
    uint16_t psi_x4;        /* pressure in quarter PSI (0..511) */
    bool     temp_valid;
    int8_t   temp_c;
    tpms_ford_mode_t mode;
    uint32_t code;          /* bytes 4..6, as rtl_433 "code" */
    uint8_t  flags;         /* byte 6 */
    uint8_t  unknown_3;     /* flags & 0x03 */
} tpms_ford_t;

/* Decode from 8 already Manchester-decoded bytes (checks checksum + flags). */
tpms_ford_status_t tpms_ford_decode_bytes(const uint8_t bytes[8], tpms_ford_t *out);

/*
 * Manchester-decode 64 bits starting at chip index `bitpos` of `chips`
 * (MSB first), optionally with the chip polarity inverted, then decode.
 */
tpms_ford_status_t tpms_ford_decode_chips(const uint8_t *chips, size_t nbits, size_t bitpos,
                                          bool invert, tpms_ford_t *out);

/*
 * Invalid Manchester pairs ('00' or '11') among the 64 bits starting at chip
 * `bitpos`: bit i of the result is set if payload bit i (0 = first on air)
 * can't be decoded. 0 if the chips run short.
 */
uint64_t tpms_ford_bad_pairs(const uint8_t *chips, size_t nbits, size_t bitpos);

/*
 * Search a raw chip stream of either polarity for the 16 chip frame start
 * (0x5556 / 0xAAA9: preamble tail + start delimiter) and decode the first valid packet after it. Returns the
 * "best" error seen if nothing decoded.
 */
tpms_ford_status_t tpms_ford_search(const uint8_t *chips, size_t nbits, tpms_ford_t *out);

/* Builds a valid 8 byte payload (fills in the checksum). */
void tpms_ford_fix_checksum(uint8_t bytes[8]);

/*
 * Encodes 8 payload bytes as a complete air frame of chips, MSB first:
 * 55 55 55 56 (preamble + start delimiter) then 128 Manchester chips.
 * For transmit tests.
 */
#define TPMS_FORD_FRAME_BYTES    (4 + TPMS_FORD_CHIP_BYTES)
void tpms_ford_encode(const uint8_t bytes[8], uint8_t frame[TPMS_FORD_FRAME_BYTES]);

const char *tpms_ford_status_str(tpms_ford_status_t st);
const char *tpms_ford_mode_str(tpms_ford_mode_t m);
