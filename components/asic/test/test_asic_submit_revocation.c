#include "unity.h"
#include "bwg_revocation.h"
#include "job_pipeline_test_harness.h"

TEST_CASE("job submission sends nothing once authority is revoked", "[asic-job][bwg-revocation]")
{
    // Arrange
    asic_job_t source = {.job_id = "send", .extranonce2 = "aabbcc"};
    job_pipeline_harness_result_t result;
    bwg_revocation_revoke(BWG_STOP_REASON_LINK_CLOSED);

    // Act
    job_pipeline_harness_send_job(&source, &result);

    // Assert
    TEST_ASSERT_EQUAL_UINT32(0, result.job_count);
    job_pipeline_harness_result_free(&result);
    bwg_revocation_note_stopped();
    TEST_ASSERT_TRUE(bwg_revocation_release_to_local());
}
