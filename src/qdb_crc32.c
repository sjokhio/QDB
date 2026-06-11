/*
 * qdb_crc32.c — CRC-32/ISO-HDLC
 *
 * Polynomial : 0xEDB88320 (reflected)
 * Initial    : 0xFFFFFFFF
 * Final XOR  : 0xFFFFFFFF
 *
 * This is the same CRC used by zlib, PNG, and Ethernet.
 *
 * SPDX-License-Identifier: MIT
 */

#include "qdb_internal.h"

/* -------------------------------------------------------------------------
 * Lookup table — computed once at first use
 * ---------------------------------------------------------------------- */

static uint32_t s_table[256];
static int      s_table_ready = 0;

static void table_init(void)
{
    uint32_t i;
    for (i = 0; i < 256u; i++) {
        uint32_t v = i;
        int      b;
        for (b = 0; b < 8; b++) {
            v = (v & 1u) ? ((v >> 1) ^ 0xEDB88320u) : (v >> 1);
        }
        s_table[i] = v;
    }
    s_table_ready = 1;
}

/* -------------------------------------------------------------------------
 * Public API
 * ---------------------------------------------------------------------- */

uint32_t qdb__crc32_begin(void)
{
    if (!s_table_ready) {
        table_init();
    }
    return 0xFFFFFFFFu;
}

uint32_t qdb__crc32_update(uint32_t state, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    size_t         i;
    if (!p || len == 0) {
        return state;
    }
    for (i = 0; i < len; i++) {
        state = (state >> 8) ^ s_table[(state ^ (uint32_t)p[i]) & 0xFFu];
    }
    return state;
}

uint32_t qdb__crc32_end(uint32_t state)
{
    return state ^ 0xFFFFFFFFu;
}

uint32_t qdb__crc32(const void *data, size_t len)
{
    return qdb__crc32_end(qdb__crc32_update(qdb__crc32_begin(), data, len));
}
