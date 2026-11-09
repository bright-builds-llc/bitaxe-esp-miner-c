#include "bwg_test.h"
#include "test_suites.h"

int main(void)
{
    bwg_tests_codec();
    bwg_tests_json();
    bwg_tests_frame();
    bwg_tests_link();
    bwg_tests_writer();
    bwg_tests_possession();
    bwg_tests_trust();
    bwg_tests_lease();
    bwg_tests_lease_auth();
    bwg_tests_controller_session();
    bwg_tests_controller_lease();
    bwg_tests_controller_wire();
    bwg_tests_controller_gate();
    bwg_tests_link_session();
    bwg_tests_writer_output();
    bwg_tests_writer_close();
    bwg_tests_crash_streak();
    bwg_tests_progress();
    bwg_tests_ledger();
    bwg_tests_budget();
    bwg_tests_controller_budget();
    bwg_tests_controller_evidence();
    bwg_tests_trace();
    bwg_tests_cadence();
    bwg_tests_v2_record();
    bwg_tests_v2_status();
    bwg_tests_controller_v2();
    bwg_tests_noise_record();
    bwg_tests_controller_noise();
    bwg_tests_lease_auth_shape();
    bwg_tests_frame_stream();
    bwg_tests_controller_stop();
    bwg_tests_controller_probe();
    bwg_tests_controller_trust();
    bwg_tests_seq_doc();
    bwg_tests_controller_reviews();
    bwg_tests_controller_stimulus();
    bwg_tests_cutoff();
    bwg_tests_restart_guard();
    bwg_tests_core_dump_receipt();
    bwg_tests_preparation_receipt();
    bwg_tests_mining_rules();
    bwg_tests_ctrl_lifecycle();
    bwg_tests_settings_hmac();
    bwg_tests_onboarding();
    bwg_tests_boot_check();
    bwg_tests_controller_onboarding();
    return bwg_test_summary();
}
