#include <stdio.h>
#include <stdbool.h>
#include "test_assert.h"

/* Function under test lives in ../src/config_choose.c — compiled alongside. */
int config_choose_source(bool partition_valid, bool legacy_valid);

int main(void) {
    ASSERT_EQ(config_choose_source(true,  true),  0);
    ASSERT_EQ(config_choose_source(true,  false), 0);
    ASSERT_EQ(config_choose_source(false, true),  1);
    ASSERT_EQ(config_choose_source(false, false), 2);
    printf("test_config_choose OK\n");
    return 0;
}
