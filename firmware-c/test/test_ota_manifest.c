#include <string.h>
#include <stdio.h>
#include "../src/ota_manifest.h"
#include "test_assert.h"

int main(void) {
    const char *ok = "42 418234\n"
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n"
        "/firmware/image.bin\n";
    ota_manifest_t m;
    ASSERT_TRUE(ota_manifest_parse(ok, strlen(ok), &m));
    ASSERT_EQ(m.version, 42);
    ASSERT_EQ(m.size, 418234);
    ASSERT_STR_EQ(m.sha256, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    ASSERT_STR_EQ(m.path, "/firmware/image.bin");

    // Reject short sha
    const char *bad = "42 418234\nabc\n/firmware/image.bin\n";
    ASSERT_TRUE(!ota_manifest_parse(bad, strlen(bad), &m));

    // Reject zero size
    const char *zero = "42 0\nba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n/firmware/image.bin\n";
    ASSERT_TRUE(!ota_manifest_parse(zero, strlen(zero), &m));

    // Path exactly 79 chars (valid '/' + 78 more) -> ACCEPT
    char p79[256];
    {
        char path79[80];
        path79[0] = '/';
        memset(path79 + 1, 'a', 78);
        path79[79] = '\0';
        snprintf(p79, sizeof(p79),
            "42 418234\n"
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n"
            "%s\n", path79);
        ASSERT_TRUE(ota_manifest_parse(p79, strlen(p79), &m));
        ASSERT_EQ(strlen(m.path), 79);
    }

    // Path exactly 80 chars -> REJECT
    char p80[256];
    {
        char path80[81];
        path80[0] = '/';
        memset(path80 + 1, 'a', 79);
        path80[80] = '\0';
        snprintf(p80, sizeof(p80),
            "42 418234\n"
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n"
            "%s\n", path80);
        ASSERT_TRUE(!ota_manifest_parse(p80, strlen(p80), &m));
    }

    // Size just over the cap (OTA_MAX_IMAGE_BYTES + 1 == 2039809) -> REJECT
    const char *over = "42 2039809\n"
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n"
        "/firmware/image.bin\n";
    ASSERT_TRUE(!ota_manifest_parse(over, strlen(over), &m));

    // Negative size line -> REJECT
    const char *neg = "42 -1\n"
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\n"
        "/firmware/image.bin\n";
    ASSERT_TRUE(!ota_manifest_parse(neg, strlen(neg), &m));

    ASSERT_TRUE(ota_is_newer(43, 42));
    ASSERT_TRUE(!ota_is_newer(42, 42));
    ASSERT_TRUE(!ota_is_newer(41, 42));
    printf("test_ota_manifest OK\n");
    return 0;
}
