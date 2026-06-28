#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "../src/ota_sink.h"
#include "test_assert.h"

#define MAXSEC 4
static uint8_t g_sectors[MAXSEC][4096];
static uint32_t g_count;

static bool capture(void *ctx, uint32_t idx, const uint8_t buf[4096]) {
    (void)ctx;
    ASSERT_TRUE(idx < MAXSEC);
    memcpy(g_sectors[idx], buf, 4096);
    g_count = idx + 1;
    return true;
}

// Counting callback for the exact-boundary scenario: just tally invocations.
static uint32_t g_calls;
static bool count_capture(void *ctx, uint32_t idx, const uint8_t buf[4096]) {
    (void)ctx; (void)idx; (void)buf;
    g_calls++;
    return true;
}

// Failing callback: returns false on sector 0, tallies every invocation so we
// can prove the latch prevents any further writes after the first failure.
static uint32_t g_fail_calls;
static bool fail_capture(void *ctx, uint32_t idx, const uint8_t buf[4096]) {
    (void)ctx; (void)buf;
    g_fail_calls++;
    return idx != 0;  // fail on the very first sector
}

int main(void) {
    g_count = 0;
    ota_sink_t s;
    ota_sink_init(&s, capture, NULL);

    // Push 4096 + 10 bytes: expect 1 full sector during push, 1 partial on finish
    uint8_t *data = malloc(4106);
    for (int i = 0; i < 4106; i++) data[i] = (uint8_t)(i & 0xff);
    ASSERT_TRUE(ota_sink_push(&s, data, 4106));
    ASSERT_EQ(g_count, 1);                       // only the full sector flushed so far
    ASSERT_TRUE(ota_sink_finish(&s));
    ASSERT_EQ(g_count, 2);                        // partial sector now flushed

    // First sector matches first 4096 bytes
    for (int i = 0; i < 4096; i++) ASSERT_EQ(g_sectors[0][i], (uint8_t)(i & 0xff));
    // Second sector: 10 real bytes then 0xFF padding
    for (int i = 0; i < 10; i++) ASSERT_EQ(g_sectors[1][i], (uint8_t)((4096 + i) & 0xff));
    for (int i = 10; i < 4096; i++) ASSERT_EQ(g_sectors[1][i], 0xFF);

    free(data);

    // --- Exact-4096 boundary: a full sector must flush during push, finish does nothing ---
    {
        g_calls = 0;
        ota_sink_t e;
        ota_sink_init(&e, count_capture, NULL);
        uint8_t *exact = malloc(OTA_SECTOR);
        memset(exact, 0xA5, OTA_SECTOR);
        ASSERT_TRUE(ota_sink_push(&e, exact, OTA_SECTOR));
        ASSERT_EQ(g_calls, 1);                   // flushed during push, not deferred to finish
        ASSERT_TRUE(ota_sink_finish(&e));
        ASSERT_EQ(g_calls, 1);                   // nothing left over -> finish writes nothing
        free(exact);
    }

    // --- Write-failure latch: first failure latches, no further writes attempted ---
    {
        g_fail_calls = 0;
        ota_sink_t f;
        ota_sink_init(&f, fail_capture, NULL);
        uint8_t *fdata = malloc(OTA_SECTOR);
        memset(fdata, 0x5A, OTA_SECTOR);
        ASSERT_TRUE(!ota_sink_push(&f, fdata, OTA_SECTOR));  // sector 0 write fails
        ASSERT_EQ(g_fail_calls, 1);                          // callback invoked exactly once
        // Latched: further push/finish short-circuit without touching the callback.
        ASSERT_TRUE(!ota_sink_push(&f, fdata, OTA_SECTOR));
        ASSERT_EQ(g_fail_calls, 1);
        ASSERT_TRUE(!ota_sink_finish(&f));
        ASSERT_EQ(g_fail_calls, 1);
        free(fdata);
    }

    printf("test_ota_sink OK\n");
    return 0;
}
