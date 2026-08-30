#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "terminal_button_policy.h"

static void verify_press_and_release(void)
{
    terminal_button_policy_t policy;
    terminal_button_policy_initialize(&policy, 100U, false);
    bool pressed = false;
    assert(!terminal_button_policy_update(&policy, 200U, true, &pressed));
    assert(!terminal_button_policy_update(&policy, 229U, true, &pressed));
    assert(terminal_button_policy_update(&policy, 230U, true, &pressed));
    assert(pressed);
    assert(!terminal_button_policy_update(&policy, 400U, false, &pressed));
    assert(terminal_button_policy_update(&policy, 430U, false, &pressed));
    assert(!pressed);
}

static void verify_bounce_is_rejected(void)
{
    terminal_button_policy_t policy;
    terminal_button_policy_initialize(&policy, 0U, false);
    bool pressed = false;
    assert(!terminal_button_policy_update(&policy, 100U, true, &pressed));
    assert(!terminal_button_policy_update(&policy, 110U, false, &pressed));
    assert(!terminal_button_policy_update(&policy, 120U, true, &pressed));
    assert(!terminal_button_policy_update(&policy, 149U, true, &pressed));
    assert(terminal_button_policy_update(&policy, 150U, true, &pressed));
    assert(pressed);
}

static void verify_non_monotonic_time_is_ignored(void)
{
    terminal_button_policy_t policy;
    terminal_button_policy_initialize(&policy, 100U, false);
    bool pressed = false;
    assert(!terminal_button_policy_update(&policy, 90U, true, &pressed));
    assert(!terminal_button_policy_update(&policy, 100U, true, &pressed));
    assert(terminal_button_policy_update(&policy, 130U, true, &pressed));
    assert(pressed);
}

int main(void)
{
    verify_press_and_release();
    verify_bounce_is_rejected();
    verify_non_monotonic_time_is_ignored();
    puts("terminal button runtime policy tests passed");
    return 0;
}
