#include <string.h>
#include <stdio.h>
#include "../src/sha256.h"
#include "test_assert.h"

int main(void) {
    // NIST: sha256("abc")
    sha256_ctx c;
    uint8_t d[32];
    char hex[65];
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)"abc", 3);
    sha256_final(&c, d);
    sha256_hex(d, hex);
    ASSERT_STR_EQ(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // Streaming in two chunks must equal one-shot
    sha256_init(&c);
    sha256_update(&c, (const uint8_t *)"ab", 2);
    sha256_update(&c, (const uint8_t *)"c", 1);
    sha256_final(&c, d);
    sha256_hex(d, hex);
    ASSERT_STR_EQ(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // Empty input
    sha256_init(&c);
    sha256_final(&c, d);
    sha256_hex(d, hex);
    ASSERT_STR_EQ(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    printf("test_sha256 OK\n");
    return 0;
}
