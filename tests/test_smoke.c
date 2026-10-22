/* Smoke test: proves the build, the strict flags and CTest are wired up. */
#include "hal.h"
#include "test_assert.h"

static void test_hal_status_ok_is_zero(void)
{
    CHECK_EQ(HAL_OK, 0);
}

int main(void)
{
    RUN_TEST(test_hal_status_ok_is_zero);
    return 0;
}
