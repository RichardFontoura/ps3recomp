#include "../../../include/ps3emu/log_first.h"
#include <limits.h>
#include <stdio.h>

static int failures;

static void check(int cond, const char* what)
{
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", what);
        failures++;
    }
}

int main(void)
{
    int n = 0, fired = 0;
    for (int i = 0; i < 100; i++)
        if (PS3_LOG_FIRST(n, 8)) fired++;
    check(fired == 8, "fires exactly limit times");
    check(n == 8, "counter stops at the limit");

    int near = INT_MAX - 1, again = 0;
    for (int i = 0; i < 1000; i++)
        if (PS3_LOG_FIRST(near, INT_MAX)) again++;
    check(again == 1 && near == INT_MAX, "no signed overflow at INT_MAX");

    int zero = 0, never = 0;
    for (int i = 0; i < 10; i++)
        if (PS3_LOG_FIRST(zero, 0)) never++;
    check(never == 0 && zero == 0, "limit 0 never fires nor writes");

    int other = 0, side = 0;
    if (PS3_LOG_FIRST(other, 2) || side) side = 1;
    check(side == 1 && other == 1, "usable inside a larger condition");

    if (failures) return 1;
    printf("PS3_LOG_FIRST checks passed\n");
    return 0;
}
