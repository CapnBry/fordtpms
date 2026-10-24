/* Host test: decode chip strings extracted from rtl_433_tests Ford captures.
 * Build/run: make -C test/host */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tpms_ford.h"

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static size_t pack(const char *s, uint8_t *out, size_t max_bytes, int invert)
{
    size_t n = 0;
    memset(out, 0, max_bytes);
    for (; *s == '0' || *s == '1'; s++, n++) {
        if (n / 8 >= max_bytes) break;
        int bit = (*s == '1') ^ invert;
        out[n / 8] |= (uint8_t)(bit << (7 - n % 8));
    }
    return n;
}

/* Emulates the radio's packet engine: sync on 0x5556, then the next 16 bytes land in the FIFO. */
static int decode_like_radio(const uint8_t *chips, size_t nbits, tpms_ford_t *out)
{
    int found = 0;
    uint16_t win = 0;
    for (size_t pos = 0; pos + 128 < nbits; pos++) {
        win = (uint16_t)(win << 1 | ((chips[pos / 8] >> (7 - pos % 8)) & 1));
        if (pos >= 15 && win == 0x5556) {
            uint8_t fifo[16] = {0};
            for (int i = 0; i < 128; i++) {
                size_t p = pos + 1 + i;
                fifo[i / 8] |= (uint8_t)(((chips[p / 8] >> (7 - p % 8)) & 1) << (7 - i % 8));
            }
            if (tpms_ford_decode_chips(fifo, 128, 0, false, &out[found]) == TPMS_FORD_OK) {
                found++;
            }
        }
    }
    return found;
}

static void run_vector(const char *chips_path, unsigned exp_id, unsigned exp_psi_x4, unsigned exp_code)
{
    FILE *f = fopen(chips_path, "r");
    if (!f) { CHECK(0, "open %s", chips_path); return; }
    static char line[65536];
    int total = 0, total_inv = 0;
    while (fgets(line, sizeof(line), f)) {
        uint8_t buf[8192];
        tpms_ford_t pk[64];
        size_t n = pack(line, buf, sizeof(buf), 0);
        int got = decode_like_radio(buf, n, pk);
        for (int i = 0; i < got; i++) {
            printf("  %s: id=%08x psi=%.2f temp=%s%d mode=%s code=%06x u3=%x\n", chips_path,
                   (unsigned)pk[i].id, pk[i].psi_x4 / 4.0, pk[i].temp_valid ? "" : "(invalid)",
                   pk[i].temp_c, tpms_ford_mode_str(pk[i].mode), (unsigned)pk[i].code, pk[i].unknown_3);
            CHECK(pk[i].id == exp_id, "id %08x", (unsigned)pk[i].id);
            CHECK(pk[i].psi_x4 == exp_psi_x4, "psi_x4 %u", pk[i].psi_x4);
            CHECK(pk[i].code == exp_code, "code %06x", (unsigned)pk[i].code);
        }
        total += got;

        /* Polarity-agnostic search must also work on the inverted stream. */
        tpms_ford_t t;
        pack(line, buf, sizeof(buf), 1);
        if (tpms_ford_search(buf, n, &t) == TPMS_FORD_OK && t.id == exp_id) total_inv++;
    }
    fclose(f);
    CHECK(total >= 1, "%s: no packets decoded", chips_path);
    CHECK(total_inv >= 1, "%s: inverted search failed", chips_path);
    printf("%s: %d packet(s) via radio path, inverted search ok=%d\n", chips_path, total, total_inv);
}

int main(void)
{
    /* Expected values from rtl_433_tests/tests/Ford_TPMS/ *.json */
    run_vector("vectors/gfile059.chips", 0x45bb320f, 106, 0x6ad446);
    run_vector("vectors/gfile124.chips", 0x45d30b69, 138, 0x8ac146);
    /* gfile082: read expected id from json at runtime is overkill; checked below */
    {
        FILE *f = fopen("vectors/gfile082.json", "r");
        char js[1024] = {0};
        if (f) { fread(js, 1, sizeof(js) - 1, f); fclose(f); }
        unsigned id = 0, code = 0; double psi = 0;
        char *p = strstr(js, "\"id\" : \"");
        if (p) sscanf(p + 8, "%x", &id);
        p = strstr(js, "\"pressure_PSI\" : ");
        if (p) sscanf(p + 17, "%lf", &psi);
        p = strstr(js, "\"code\" : \"");
        if (p) sscanf(p + 10, "%x", &code);
        run_vector("vectors/gfile082.chips", id, (unsigned)(psi * 4 + 0.5), code);
    }

    /* Byte-level checks: checksum, flags, temperature, 9th pressure bit. */
    uint8_t b[8] = {0x45, 0xbb, 0x32, 0x0f, 0x6a, 0x48, 0x46, 0};
    tpms_ford_fix_checksum(b);
    tpms_ford_t t;
    CHECK(tpms_ford_decode_bytes(b, &t) == TPMS_FORD_OK, "decode bytes");
    CHECK(t.temp_valid && t.temp_c == 0x48 - 56, "temp %d", t.temp_c);
    CHECK(t.mode == TPMS_MODE_MOVING, "mode");
    b[7]++;
    CHECK(tpms_ford_decode_bytes(b, &t) == TPMS_FORD_ERR_CHECKSUM, "checksum detect");
    b[6] = 0x26; /* 0x20 = 9th pressure bit, 0x04 = rest */
    tpms_ford_fix_checksum(b);
    CHECK(tpms_ford_decode_bytes(b, &t) == TPMS_FORD_OK && t.psi_x4 == 0x16a && t.mode == TPMS_MODE_REST, "9th bit");
    b[6] = 0x90 | 0x04;
    tpms_ford_fix_checksum(b);
    CHECK(tpms_ford_decode_bytes(b, &t) == TPMS_FORD_ERR_FLAGS, "flags reject");
    b[5] = 0x85; b[6] = 0x08;
    tpms_ford_fix_checksum(b);
    CHECK(tpms_ford_decode_bytes(b, &t) == TPMS_FORD_OK && !t.temp_valid && t.mode == TPMS_MODE_LEARN, "learn/no temp");

    /* Encoder round trip, through the same path as the radio (sync 0x5556 + 16 bytes). */
    {
        uint8_t p[8] = {0xfe, 0xed, 0xf0, 0x0d, 0x98, 0x55, 0x06, 0};
        tpms_ford_fix_checksum(p);
        uint8_t frame[TPMS_FORD_FRAME_BYTES];
        tpms_ford_encode(p, frame);
        tpms_ford_t out[4];
        CHECK(decode_like_radio(frame, sizeof(frame) * 8 + 1, out) == 1 && out[0].id == 0xfeedf00d &&
              out[0].psi_x4 == 0x98, "encode -> radio decode");
        CHECK(tpms_ford_search(frame, sizeof(frame) * 8, &t) == TPMS_FORD_OK && t.id == 0xfeedf00d, "encode -> search");
    }

    memset(b, 0, sizeof(b));
    CHECK(tpms_ford_decode_bytes(b, &t) == TPMS_FORD_ERR_ZERO, "zero payload reject");

    printf(failures ? "\n%d FAILURE(S)\n" : "\nALL TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
