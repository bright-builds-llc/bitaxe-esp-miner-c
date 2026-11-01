#include <string.h>
#include <yyjson.h>

#include "bwg_test.h"
#include "bwg_v2_record.h"
#include "test_suites.h"

#define ADMIT_US 2000000ull
#define ATTEMPT "AAAAAAAAAAAAAAAAAAAAAA"

static bwg_v2_record_t s_record;
static uint8_t s_status[BWG_V2_STATUS_BYTES];

static bwg_v2_observation_t observation_at(uint64_t now_us)
{
    bwg_v2_observation_t observation = {
        .boot_ordinal = 2,
        .worker_generation = 7,
        .serial_transport_epoch = 4,
        .observed_at_us = now_us,
        .clock_valid = true,
        .wifi_connected = true,
    };
    strcpy(observation.station_ipv4, "192.168.1.2");
    return observation;
}

static yyjson_doc *status_doc(bwg_v2_scope_t scope, const bwg_v2_record_t *maybe_record, const bwg_v2_observation_t *observation)
{
    bwg_jw_t writer;
    bwg_jw_init_buffer(&writer, s_status, sizeof(s_status));
    bwg_v2_write_status(&writer, scope, observation, maybe_record);
    return bwg_jw_ok(&writer) ? yyjson_read((const char *)s_status, writer.len, 0) : NULL;
}

static void an_idle_status_is_exact(void)
{
    // Arrange
    const bwg_v2_observation_t observation = observation_at(ADMIT_US);

    // Act
    yyjson_doc *doc = status_doc(BWG_V2_SCOPE_SHARE, NULL, &observation);

    // Assert
    ASSERT_TRUE(doc != NULL);
    yyjson_val *root = yyjson_doc_get_root(doc);
    ASSERT_STREQ(yyjson_get_str(yyjson_obj_get(root, "schema")), "worker-stratum-v2-status-v1");
    ASSERT_STREQ(yyjson_get_str(yyjson_obj_get(root, "state")), "idle");
    ASSERT_TRUE(yyjson_is_null(yyjson_obj_get(root, "record")));
    ASSERT_TRUE(yyjson_is_null(yyjson_obj_get(root, "connection")));
    yyjson_val *seen = yyjson_obj_get(root, "observation");
    ASSERT_TRUE(yyjson_is_null(yyjson_obj_get(seen, "socket")));
    ASSERT_STREQ(yyjson_get_str(yyjson_obj_get(seen, "stationIpv4")), "192.168.1.2");
    yyjson_doc_free(doc);
}

// From the Rust repository's job-proof.mjs `workPayload`, for this test's job in slot 3.
static const uint8_t WORK_FIELDS_VECTOR[BWG_V2_DIGEST_BYTES] = {
    0x12, 0x75, 0xeb, 0x98, 0xa8, 0x64, 0x1a, 0xe7, 0xbc, 0xb8, 0x76, 0x95, 0xc7, 0xb7, 0x79, 0x99,
    0x49, 0x8e, 0x28, 0x5b, 0x45, 0x0e, 0xbc, 0xff, 0x4e, 0xf8, 0xf3, 0x20, 0x3a, 0x94, 0xdf, 0x41,
};

static void work_fields_hash_the_asic_payload_the_fixture_recomputes(void)
{
    // Arrange
    uint8_t merkle[32];
    uint8_t previous[32];
    for (size_t i = 0; i < 32; i++) {
        merkle[i] = (uint8_t)i;
        previous[i] = (uint8_t)(0x80 + i);
    }
    const bwg_v2_work_t work = {
        .version = 0x20000000u,
        .nbits = 0x207fffffu,
        .ntime = 0x6a000000u,
        .starting_nonce = 0,
        .merkle_root = merkle,
        .prev_hash = previous,
    };
    uint8_t digest[BWG_V2_DIGEST_BYTES];

    // Act
    bwg_v2_work_fields_digest(&work, 3, digest);

    // Assert
    ASSERT_MEMEQ(digest, WORK_FIELDS_VECTOR, sizeof(digest));
}

static void a_status_is_never_older_than_its_record(void)
{
    // Arrange
    const bwg_v2_observation_t admitted = observation_at(ADMIT_US);
    ASSERT_TRUE(bwg_v2_record_admit(&s_record, BWG_V2_SCOPE_CHANNEL, ATTEMPT, &admitted));
    const bwg_v2_observation_t earlier = observation_at(ADMIT_US - 200);

    // Act
    yyjson_doc *doc = status_doc(BWG_V2_SCOPE_CHANNEL, &s_record, &earlier);

    // Assert
    ASSERT_TRUE(doc != NULL);
    yyjson_val *seen = yyjson_obj_get(yyjson_doc_get_root(doc), "observation");
    ASSERT_EQ(yyjson_get_uint(yyjson_obj_get(seen, "observedAtUs")), ADMIT_US);
    yyjson_doc_free(doc);
}

static void a_record_of_another_scope_reads_as_idle(void)
{
    // Arrange
    const bwg_v2_observation_t admitted = observation_at(ADMIT_US);
    ASSERT_TRUE(bwg_v2_record_admit(&s_record, BWG_V2_SCOPE_CHANNEL, ATTEMPT, &admitted));
    const bwg_v2_observation_t observation = observation_at(ADMIT_US + 1);

    // Act
    yyjson_doc *doc = status_doc(BWG_V2_SCOPE_SHARE, &s_record, &observation);

    // Assert
    ASSERT_TRUE(doc != NULL);
    ASSERT_STREQ(yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc), "state")), "idle");
    yyjson_doc_free(doc);
}

void bwg_tests_v2_status(void)
{
    BWG_RUN(an_idle_status_is_exact);
    BWG_RUN(a_status_is_never_older_than_its_record);
    BWG_RUN(work_fields_hash_the_asic_payload_the_fixture_recomputes);
    BWG_RUN(a_record_of_another_scope_reads_as_idle);
}
