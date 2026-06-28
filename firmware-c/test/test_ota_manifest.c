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

    ASSERT_TRUE(ota_is_newer(43, 42));
    ASSERT_TRUE(!ota_is_newer(42, 42));
    ASSERT_TRUE(!ota_is_newer(41, 42));
    printf("test_ota_manifest OK\n");
    return 0;
}
