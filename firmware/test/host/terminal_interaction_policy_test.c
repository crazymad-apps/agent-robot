#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "terminal_interaction_policy.h"

static terminal_interaction_policy_state_t update(
    terminal_interaction_policy_t *policy,
    uint64_t now_ms,
    bool pressed)
{
    return terminal_interaction_policy_update(policy, now_ms, pressed);
}

static void verify_released_boot_cancels(void)
{
    terminal_interaction_policy_t policy;
    terminal_interaction_policy_initialize(&policy, 100U, false);
    assert(update(&policy, 129U, false) == TERMINAL_INTERACTION_POLICY_EVALUATING);
    assert(update(&policy, 130U, false) == TERMINAL_INTERACTION_POLICY_CANCELLED);
}

static void verify_short_press_cancels_after_debounce(void)
{
    terminal_interaction_policy_t policy;
    terminal_interaction_policy_initialize(&policy, 0U, true);
    assert(update(&policy, 30U, true) == TERMINAL_INTERACTION_POLICY_HOLDING);
    assert(update(&policy, 5000U, false) == TERMINAL_INTERACTION_POLICY_HOLDING);
    assert(update(&policy, 5029U, false) == TERMINAL_INTERACTION_POLICY_HOLDING);
    assert(update(&policy, 5030U, false) == TERMINAL_INTERACTION_POLICY_CANCELLED);
}

static void verify_release_bounce_does_not_cancel(void)
{
    terminal_interaction_policy_t policy;
    terminal_interaction_policy_initialize(&policy, 0U, true);
    assert(update(&policy, 30U, true) == TERMINAL_INTERACTION_POLICY_HOLDING);
    assert(update(&policy, 4000U, false) == TERMINAL_INTERACTION_POLICY_HOLDING);
    assert(update(&policy, 4010U, true) == TERMINAL_INTERACTION_POLICY_HOLDING);
    assert(update(&policy, 8000U, true) == TERMINAL_INTERACTION_POLICY_ARMED);
}

static void verify_release_before_threshold_cancels(void)
{
    terminal_interaction_policy_t policy;
    terminal_interaction_policy_initialize(&policy, 0U, true);
    assert(update(&policy, 30U, true) == TERMINAL_INTERACTION_POLICY_HOLDING);
    assert(update(&policy, 7990U, false) == TERMINAL_INTERACTION_POLICY_HOLDING);
    assert(update(&policy, 8000U, false) == TERMINAL_INTERACTION_POLICY_HOLDING);
    assert(update(&policy, 8020U, false) == TERMINAL_INTERACTION_POLICY_CANCELLED);
}

static void verify_armed_requires_release(void)
{
    terminal_interaction_policy_t policy;
    terminal_interaction_policy_initialize(&policy, 0U, true);
    assert(update(&policy, 30U, true) == TERMINAL_INTERACTION_POLICY_HOLDING);
    assert(update(&policy, 7999U, true) == TERMINAL_INTERACTION_POLICY_HOLDING);
    assert(update(&policy, 8000U, true) == TERMINAL_INTERACTION_POLICY_ARMED);
    assert(update(&policy, 30000U, true) == TERMINAL_INTERACTION_POLICY_ARMED);
    assert(update(&policy, 30001U, false) == TERMINAL_INTERACTION_POLICY_ARMED);
    assert(update(&policy, 30030U, false) == TERMINAL_INTERACTION_POLICY_ARMED);
    assert(update(&policy, 30031U, false) == TERMINAL_INTERACTION_POLICY_CONFIRMED);
}

static void verify_non_monotonic_time_is_ignored(void)
{
    terminal_interaction_policy_t policy;
    terminal_interaction_policy_initialize(&policy, 100U, true);
    assert(update(&policy, 90U, false) == TERMINAL_INTERACTION_POLICY_EVALUATING);
    assert(update(&policy, 130U, true) == TERMINAL_INTERACTION_POLICY_HOLDING);
}

int main(void)
{
    verify_released_boot_cancels();
    verify_short_press_cancels_after_debounce();
    verify_release_bounce_does_not_cancel();
    verify_release_before_threshold_cancels();
    verify_armed_requires_release();
    verify_non_monotonic_time_is_ignored();
    puts("terminal interaction boot policy tests passed");
    return 0;
}
