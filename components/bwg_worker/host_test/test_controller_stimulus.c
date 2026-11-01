#include <stdlib.h>
#include <string.h>

#include "bwg_review.h"
#include "bwg_test.h"
#include "test_controller_rig.h"
#include "test_suites.h"

#define REVIEW_PAYLOAD "{}"
#define STIMULUS_PAYLOAD "{\"requestNonce\":\"AAAAAAAAAAAAAAAAAAAAAA\"}"

static bool arm_stimulus(rig_t *rig, uint64_t now_ms)
{
    return rig_send(rig, "clock_discontinuity_stimulus", STIMULUS_PAYLOAD, now_ms) == BWG_CTRL_OK &&
           rig_confirm_at(rig, now_ms) == BWG_CTRL_OK;
}

static void stimulus_echoes_the_nonce_and_its_bounds(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 2000));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "clock_discontinuity_stimulus", STIMULUS_PAYLOAD, 2001);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/schema", "worker-clock-discontinuity-stimulus-v1"));
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/requestNonce", "AAAAAAAAAAAAAAAAAAAAAA"));
    ASSERT_TRUE(rig_reply_u64_is(&rig, "/result/offsetMilliseconds", 1000));
    ASSERT_TRUE(rig_reply_u64_is(&rig, "/result/armedForMilliseconds", 2000));
    rig_close(&rig);
}

static void stimulus_is_not_armed_until_its_reply_was_sent(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 2000));
    ASSERT_EQ(rig_send(&rig, "clock_discontinuity_stimulus", STIMULUS_PAYLOAD, 2001), BWG_CTRL_OK);

    // Act
    bwg_ctrl_error_t tick = bwg_controller_tick(&rig.controller, 2002);

    // Assert
    ASSERT_EQ(tick, BWG_CTRL_OK);
    ASSERT_TRUE(bwg_controller_has_active_lease(&rig.controller));
    ASSERT_EQ(rig.controller.stimulus.state, BWG_CLOCK_STIMULUS_IDLE);
    rig_close(&rig);
}

static void an_armed_stimulus_makes_one_tick_stop_the_lease(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 2000));
    ASSERT_TRUE(arm_stimulus(&rig, 2001));

    // Act
    bwg_ctrl_error_t tick = bwg_controller_tick(&rig.controller, 2002);

    // Assert
    ASSERT_EQ(tick, BWG_CTRL_MONOTONIC_RESET);
    ASSERT_FALSE(bwg_controller_has_active_lease(&rig.controller));
    ASSERT_EQ(rig.controller.stimulus.state, BWG_CLOCK_STIMULUS_CONSUMED);
    ASSERT_EQ(rig.controller.clock_discontinuities, 1u);
    ASSERT_EQ(rig.controller.restoration_reason, BWG_REASON_MONOTONIC_RESET);
    rig_close(&rig);
}

static void an_armed_stimulus_does_not_touch_frames(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 2000));
    ASSERT_TRUE(arm_stimulus(&rig, 2001));

    // Act
    bwg_ctrl_error_t status = rig_send(&rig, "status", NULL, 2002);

    // Assert
    ASSERT_EQ(status, BWG_CTRL_OK);
    ASSERT_TRUE(bwg_controller_has_active_lease(&rig.controller));
    ASSERT_EQ(rig.controller.stimulus.state, BWG_CLOCK_STIMULUS_ARMED);
    rig_close(&rig);
}

static void an_unconsumed_stimulus_expires_and_stays_spent(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 2000));
    ASSERT_TRUE(arm_stimulus(&rig, 2001));

    // Act
    bwg_ctrl_error_t tick = bwg_controller_tick(&rig.controller, 2001 + BWG_CLOCK_STIMULUS_ARMED_MS);
    bwg_ctrl_error_t again = rig_send(&rig, "clock_discontinuity_stimulus", STIMULUS_PAYLOAD, 4100);

    // Assert
    ASSERT_EQ(tick, BWG_CTRL_OK);
    ASSERT_EQ(rig.controller.stimulus.state, BWG_CLOCK_STIMULUS_EXPIRED);
    ASSERT_EQ(again, BWG_CTRL_INVALID_TRANSITION);
    rig_close(&rig);
}

static void a_safe_stop_ends_the_armed_window(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 2000));
    ASSERT_TRUE(arm_stimulus(&rig, 2001));

    // Act
    bwg_ctrl_error_t pause = rig_send(&rig, "pause", NULL, 2002);

    // Assert
    ASSERT_EQ(pause, BWG_CTRL_OK);
    ASSERT_EQ(rig.controller.stimulus.state, BWG_CLOCK_STIMULUS_EXPIRED);
    rig_close(&rig);
}

static void stimulus_needs_room_before_the_lease_ends(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 2000));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "clock_discontinuity_stimulus", STIMULUS_PAYLOAD, 2000 + 60000 - 4999);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    rig_close(&rig);
}

static void stimulus_needs_a_clock_that_can_go_back(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 500));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "clock_discontinuity_stimulus", STIMULUS_PAYLOAD, 501);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_INVALID_TRANSITION);
    rig_close(&rig);
}

static void a_confirmation_from_an_old_session_does_not_arm(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 2000));
    ASSERT_EQ(rig_send(&rig, "clock_discontinuity_stimulus", STIMULUS_PAYLOAD, 2001), BWG_CTRL_OK);
    const bwg_ctrl_effect_t effect = rig.outcome.effect;
    ASSERT_EQ(bwg_controller_disconnect(&rig.controller, 2002), BWG_CTRL_OK);

    // Act
    bwg_ctrl_error_t confirm = bwg_controller_confirm_sent(&rig.controller, &effect, 2003);

    // Assert
    ASSERT_EQ(confirm, BWG_CTRL_STALE_RESPONSE);
    ASSERT_EQ(rig.controller.stimulus.state, BWG_CLOCK_STIMULUS_IDLE);
    rig_close(&rig);
}

static void stimulus_review_reports_the_consumed_stimulus(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open_with_lease(&rig, 2000));
    ASSERT_TRUE(arm_stimulus(&rig, 2001));
    ASSERT_EQ(bwg_controller_tick(&rig.controller, 2002), BWG_CTRL_MONOTONIC_RESET);
    ASSERT_TRUE(rig_admit(&rig, 2003));

    // Act
    bwg_ctrl_error_t error = rig_send(&rig, "clock_discontinuity_stimulus_review", REVIEW_PAYLOAD, 2004);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/schema", "worker-clock-discontinuity-stimulus-review-v1"));
    ASSERT_TRUE(rig_reply_str_is(&rig, "/result/state", "consumed"));
    ASSERT_TRUE(rig_reply_u64_is(&rig, "/result/discontinuitiesDetected", 1));
    ASSERT_TRUE(rig_reply_u64_is(&rig, "/result/offsetMilliseconds", 1000));
    rig_close(&rig);
}

void bwg_tests_controller_stimulus(void)
{
    BWG_RUN(stimulus_echoes_the_nonce_and_its_bounds);
    BWG_RUN(stimulus_is_not_armed_until_its_reply_was_sent);
    BWG_RUN(an_armed_stimulus_makes_one_tick_stop_the_lease);
    BWG_RUN(an_armed_stimulus_does_not_touch_frames);
    BWG_RUN(an_unconsumed_stimulus_expires_and_stays_spent);
    BWG_RUN(a_safe_stop_ends_the_armed_window);
    BWG_RUN(stimulus_needs_room_before_the_lease_ends);
    BWG_RUN(stimulus_needs_a_clock_that_can_go_back);
    BWG_RUN(a_confirmation_from_an_old_session_does_not_arm);
    BWG_RUN(stimulus_review_reports_the_consumed_stimulus);
}
