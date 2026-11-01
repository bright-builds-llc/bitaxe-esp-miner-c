#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bwg_test.h"
#include "test_controller_rig.h"
#include "test_suites.h"

// ---- Controller scenarios: the simulator's steps on the real state machine, with fixture authentication.

/** A clock above every scripted reset, since `reset_monotonic` only counts as a reset if the clock falls. */
#define SCENARIO_BASE_MS 10000u

typedef struct {
    rig_t rig;
    yyjson_doc *fixture;
    uint64_t now_ms;
    bwg_ctrl_error_t last_error;
} scenario_t;

/** Applies the input's overrides to its base object and returns compact JSON text. Caller frees it. */
static char *input_json(yyjson_val *root, const char *input_name)
{
    yyjson_val *entry = yyjson_obj_get(yyjson_obj_get(root, "inputs"), input_name);
    yyjson_val *base = yyjson_obj_get(root, yyjson_get_str(yyjson_obj_get(entry, "base")));
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *merged = yyjson_val_mut_copy(doc, base);
    yyjson_mut_doc_set_root(doc, merged);
    size_t idx = 0;
    size_t max = 0;
    yyjson_val *key = NULL;
    yyjson_val *value = NULL;
    yyjson_obj_foreach(yyjson_obj_get(entry, "overrides"), idx, max, key, value)
    {
        yyjson_mut_obj_put(merged, yyjson_val_mut_copy(doc, key), yyjson_val_mut_copy(doc, value));
    }
    char *text = yyjson_mut_write(doc, 0, NULL);
    yyjson_mut_doc_free(doc);
    return text;
}

static bwg_ctrl_error_t ensure_admitted(scenario_t *scenario)
{
    rig_t *rig = &scenario->rig;
    if (rig->controller.boot_clear_required) {
        // A real host reads the baseline first, which is what lets a reboot be acknowledged.
        bwg_session_binding_t session = rig_fixture_session();
        bwg_ctrl_error_t begun = bwg_controller_begin_session(&rig->controller, &session);
        if (begun != BWG_CTRL_OK) {
            return begun;
        }
        bwg_ctrl_error_t status = rig_send(rig, "status", NULL, scenario->now_ms);
        if (status != BWG_CTRL_OK) {
            return status;
        }
        bwg_ctrl_error_t confirm = rig_confirm(rig);
        if (confirm != BWG_CTRL_OK) {
            return confirm;
        }
        return rig_reprove(rig, scenario->now_ms) ? BWG_CTRL_OK : BWG_CTRL_ADMISSION_REQUIRED;
    }
    return rig_admit(rig, scenario->now_ms) ? BWG_CTRL_OK : BWG_CTRL_ADMISSION_REQUIRED;
}

static bwg_ctrl_error_t run_payload_step(scenario_t *scenario, const char *command, yyjson_val *step)
{
    char *payload = input_json(yyjson_doc_get_root(scenario->fixture), yyjson_get_str(yyjson_obj_get(step, "input")));
    bwg_ctrl_error_t error = rig_send(&scenario->rig, command, payload, scenario->now_ms);
    free(payload);
    return error;
}

static bwg_ctrl_error_t run_step(scenario_t *scenario, yyjson_val *step)
{
    const char *operation = yyjson_get_str(yyjson_obj_get(step, "operation"));
    yyjson_val *value = yyjson_obj_get(step, "value");
    rig_t *rig = &scenario->rig;
    if (strcmp(operation, "discover") == 0) {
        return rig_send(rig, "discover", NULL, scenario->now_ms);
    }
    if (strcmp(operation, "start") == 0) {
        bwg_ctrl_error_t admitted = ensure_admitted(scenario);
        return admitted != BWG_CTRL_OK ? admitted : run_payload_step(scenario, "start_lease", step);
    }
    if (strcmp(operation, "renew") == 0) {
        return run_payload_step(scenario, "renew_lease", step);
    }
    if (strcmp(operation, "status") == 0 || strcmp(operation, "pause") == 0 || strcmp(operation, "cancel") == 0) {
        return rig_send(rig, operation, NULL, scenario->now_ms);
    }
    if (strcmp(operation, "advance_monotonic") == 0) {
        scenario->now_ms += yyjson_get_uint(value);
        return bwg_controller_tick(&rig->controller, scenario->now_ms);
    }
    if (strcmp(operation, "lose_continuity") == 0) {
        // The device has no continuity channel of its own: the host reports the loss through `restore`.
        return rig_send(rig, "restore", "{\"reason\":\"lost_continuity\"}", scenario->now_ms);
    }
    if (strcmp(operation, "reset_monotonic") == 0) {
        scenario->now_ms = yyjson_get_uint(value);
        bwg_ctrl_error_t error = bwg_controller_tick(&rig->controller, scenario->now_ms);
        return error == BWG_CTRL_MONOTONIC_RESET ? BWG_CTRL_OK : BWG_CTRL_SESSION_FAILED;
    }
    if (strcmp(operation, "reboot") == 0) {
        return rig_reboot(rig) ? BWG_CTRL_OK : BWG_CTRL_SESSION_FAILED;
    }
    return BWG_CTRL_INVALID_REQUEST;
}

/**
 * The simulator's error text in this protocol's closed categories. "Work Lease is not active" maps
 * to admission_required, not invalid_transition: expiry clears admission, and that is checked first.
 */
static bwg_ctrl_error_t expected_category(const char *text)
{
    if (strcmp(text, "lease duration is outside the contract") == 0) {
        return BWG_CTRL_INVALID_REQUEST;
    }
    if (strcmp(text, "Work Lease authentication failed") == 0) {
        return BWG_CTRL_AUTHENTICATION_FAILED;
    }
    if (strcmp(text, "Work Lease is not active") == 0) {
        return BWG_CTRL_ADMISSION_REQUIRED;
    }
    return BWG_CTRL_OK;
}

static yyjson_val *find_scenario(yyjson_val *root, const char *id)
{
    size_t idx = 0;
    size_t max = 0;
    yyjson_val *scenario = NULL;
    yyjson_arr_foreach(yyjson_obj_get(root, "scenarios"), idx, max, scenario)
    {
        if (strcmp(yyjson_get_str(yyjson_obj_get(scenario, "id")), id) == 0) {
            return scenario;
        }
    }
    return NULL;
}

static void check_final_state(scenario_t *scenario, yyjson_val *expected)
{
    rig_t *rig = &scenario->rig;
    yyjson_val *state = yyjson_obj_get(expected, "state");
    if (state == NULL) {
        return;
    }
    scenario->now_ms++;
    ASSERT_EQ(rig_send(rig, "status", NULL, scenario->now_ms), BWG_CTRL_OK);
    ASSERT_TRUE(rig_reply_str_is(rig, "/result/state", yyjson_get_str(state)));
    yyjson_val *reason = yyjson_obj_get(expected, "reason");
    if (reason != NULL) {
        ASSERT_TRUE(rig_reply_str_is(rig, "/result/restoration/reason", yyjson_get_str(reason)));
    }
    if (yyjson_get_bool(yyjson_obj_get(expected, "redacted"))) {
        const char *reply = rig_reply_text(rig);
        ASSERT_TRUE(strstr(reply, "authorization") == NULL);
        ASSERT_TRUE(strstr(reply, "username") == NULL);
        ASSERT_TRUE(strstr(reply, "password") == NULL);
        ASSERT_TRUE(strstr(reply, RIG_PASSWORD) == NULL);
    }
}

static void run_scenario(const char *id)
{
    // Arrange
    static scenario_t scenario;
    memset(&scenario, 0, sizeof(scenario));
    ASSERT_TRUE(rig_open(&scenario.rig));
    scenario.fixture = bwg_test_fixture_doc("controller/fixtures.json");
    scenario.now_ms = SCENARIO_BASE_MS;
    yyjson_val *root = yyjson_doc_get_root(scenario.fixture);
    yyjson_val *entry = find_scenario(root, id);
    ASSERT_TRUE(entry != NULL);
    yyjson_val *expected = yyjson_obj_get(entry, "expected");
    yyjson_val *steps = yyjson_obj_get(entry, "steps");
    size_t step_count = yyjson_arr_size(steps);

    // Act
    bwg_ctrl_error_t error = BWG_CTRL_OK;
    for (size_t i = 0; i < step_count; i++) {
        error = run_step(&scenario, yyjson_arr_get(steps, i));
        if (i + 1 < step_count) {
            ASSERT_EQ(error, BWG_CTRL_OK);
        }
    }

    // Assert
    yyjson_val *expected_error = yyjson_obj_get(expected, "error");
    if (expected_error != NULL) {
        bwg_ctrl_error_t category = expected_category(yyjson_get_str(expected_error));
        ASSERT_TRUE(category != BWG_CTRL_OK);
        ASSERT_EQ(error, category);
        ASSERT_TRUE(rig_reply_is_rejection(&scenario.rig, category));
    } else {
        ASSERT_EQ(error, BWG_CTRL_OK);
    }
    if (yyjson_get_bool(yyjson_obj_get(expected, "capabilities"))) {
        yyjson_doc *reply = yyjson_read(rig_reply_text(&scenario.rig), scenario.rig.outcome.response.len, 0);
        ASSERT_TRUE(yyjson_equals(yyjson_doc_ptr_get(reply, "/result"), yyjson_obj_get(root, "capabilities")));
        yyjson_doc_free(reply);
    }
    check_final_state(&scenario, expected);
    yyjson_doc_free(scenario.fixture);
    rig_close(&scenario.rig);
}

#define SCENARIO_TEST(name)                 \
    static void scenario_##name(void)       \
    {                                       \
        run_scenario(#name);                \
    }

SCENARIO_TEST(discover)
SCENARIO_TEST(start)
SCENARIO_TEST(renew)
SCENARIO_TEST(status_after_renew_hint)
SCENARIO_TEST(pause_restore)
SCENARIO_TEST(cancel_restore)
SCENARIO_TEST(expiry_restore)
SCENARIO_TEST(lost_continuity_restore)
SCENARIO_TEST(monotonic_reset_restore)
SCENARIO_TEST(reboot_restore)
SCENARIO_TEST(fresh_lease_after_reboot)
SCENARIO_TEST(reject_oversized_lease)
SCENARIO_TEST(reject_invalid_authorization)
SCENARIO_TEST(reject_stale_renewal)
SCENARIO_TEST(reject_wrong_lease_renewal)
SCENARIO_TEST(redacted_diagnostics)

static void scenario_list_is_the_one_tested_here(void)
{
    // Arrange
    yyjson_doc *fixture = bwg_test_fixture_doc("controller/fixtures.json");

    // Act
    size_t count = yyjson_arr_size(yyjson_obj_get(yyjson_doc_get_root(fixture), "scenarios"));

    // Assert
    ASSERT_EQ(count, 16);
    yyjson_doc_free(fixture);
}

// ---- USB vectors.

static yyjson_val *usb_vector(yyjson_val *root, const char *id)
{
    size_t idx = 0;
    size_t max = 0;
    yyjson_val *vector = NULL;
    yyjson_arr_foreach(yyjson_obj_get(root, "usbVectors"), idx, max, vector)
    {
        if (strcmp(yyjson_get_str(yyjson_obj_get(vector, "id")), id) == 0) {
            return vector;
        }
    }
    return NULL;
}

static bwg_ctrl_error_t send_vector_request(rig_t *rig, yyjson_val *vector, uint64_t now_ms)
{
    size_t len = 0;
    char *frame = bwg_test_json_line(yyjson_obj_get(vector, "request"), &len);
    bwg_ctrl_error_t error = rig_send_raw(rig, frame, len, now_ms);
    free(frame);
    return error;
}

static bool reply_equals(const rig_t *rig, yyjson_val *expected)
{
    yyjson_doc *reply = yyjson_read(rig_reply_text(rig), rig->outcome.response.len, 0);
    bool same = reply != NULL && yyjson_equals(yyjson_doc_get_root(reply), expected);
    yyjson_doc_free(reply);
    return same;
}

static void usb_vector_discover_matches(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    yyjson_doc *fixture = bwg_test_fixture_doc("controller/fixtures.json");
    yyjson_val *vector = usb_vector(yyjson_doc_get_root(fixture), "serial_discover");

    // Act
    bwg_ctrl_error_t error = send_vector_request(&rig, vector, 0);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_TRUE(reply_equals(&rig, yyjson_obj_get(vector, "response")));
    yyjson_doc_free(fixture);
    rig_close(&rig);
}

static void usb_vector_start_matches(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 0));
    yyjson_doc *fixture = bwg_test_fixture_doc("controller/fixtures.json");
    yyjson_val *vector = usb_vector(yyjson_doc_get_root(fixture), "serial_start");

    // Act
    bwg_ctrl_error_t error = send_vector_request(&rig, vector, 0);

    // Assert
    ASSERT_EQ(error, BWG_CTRL_OK);
    ASSERT_TRUE(reply_equals(&rig, yyjson_obj_get(vector, "response")));
    yyjson_doc_free(fixture);
    rig_close(&rig);
}

static void usb_vector_oversized_gets_a_closed_category_not_the_fixture_text(void)
{
    // Arrange
    rig_t rig;
    ASSERT_TRUE(rig_open(&rig));
    ASSERT_TRUE(rig_admit(&rig, 0));
    yyjson_doc *fixture = bwg_test_fixture_doc("controller/fixtures.json");
    yyjson_val *vector = usb_vector(yyjson_doc_get_root(fixture), "serial_reject_oversized");
    yyjson_val *response = yyjson_obj_get(vector, "response");

    // Act
    bwg_ctrl_error_t error = send_vector_request(&rig, vector, 0);

    // Assert: everything agrees except `message`, whose fixture text the live host parser refuses.
    ASSERT_EQ(error, BWG_CTRL_INVALID_REQUEST);
    ASSERT_TRUE(rig_reply_str_is(&rig, "/requestId", yyjson_get_str(yyjson_obj_get(response, "requestId"))));
    ASSERT_TRUE(rig_reply_str_is(&rig, "/error/code", yyjson_get_str(yyjson_ptr_get(response, "/error/code"))));
    ASSERT_TRUE(rig_reply_str_is(&rig, "/error/message", "invalid_request"));
    ASSERT_STREQ(yyjson_get_str(yyjson_ptr_get(response, "/error/message")), "Worker Controller command was rejected");
    yyjson_doc_free(fixture);
    rig_close(&rig);
}

void bwg_tests_controller_gate(void)
{
    BWG_RUN(scenario_list_is_the_one_tested_here);
    BWG_RUN(scenario_discover);
    BWG_RUN(scenario_start);
    BWG_RUN(scenario_renew);
    BWG_RUN(scenario_status_after_renew_hint);
    BWG_RUN(scenario_pause_restore);
    BWG_RUN(scenario_cancel_restore);
    BWG_RUN(scenario_expiry_restore);
    BWG_RUN(scenario_lost_continuity_restore);
    BWG_RUN(scenario_monotonic_reset_restore);
    BWG_RUN(scenario_reboot_restore);
    BWG_RUN(scenario_fresh_lease_after_reboot);
    BWG_RUN(scenario_reject_oversized_lease);
    BWG_RUN(scenario_reject_invalid_authorization);
    BWG_RUN(scenario_reject_stale_renewal);
    BWG_RUN(scenario_reject_wrong_lease_renewal);
    BWG_RUN(scenario_redacted_diagnostics);
    BWG_RUN(usb_vector_discover_matches);
    BWG_RUN(usb_vector_start_matches);
    BWG_RUN(usb_vector_oversized_gets_a_closed_category_not_the_fixture_text);
}
