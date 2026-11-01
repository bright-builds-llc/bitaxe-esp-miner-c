#ifndef TEST_LINK_SUPPORT_H
#define TEST_LINK_SUPPORT_H

#include <stdio.h>
#include <stdlib.h>

#include "bwg_codec.h"
#include "bwg_json.h"
#include "bwg_link.h"
#include "bwg_test.h"
#include "test_suites.h"

#define HOST_NONCE "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"

// The stub's entropy is all 0x11; these are its session id and nonce.
#define SESSION_ID "EREREREREREREREREREREQ"

typedef struct {
    bool refuse_generation;
    bool refuse_session;
    bool refuse_control;
    bool refuse_ack;
    bool refuse_random;
    int generations_begun;
    int generations_closed;
    int heartbeats;
    int acks;
    int sessions;
    int controls;
    int failure_clears;
    bwg_link_failure_t last_failure;
    int failures;
    uint8_t ack[BWG_HELLO_ACK_MAX_BYTES];
    size_t ack_len;
    bwg_buffer_t control;
    uint32_t control_sequence;
    uint32_t last_ack_epoch;
} stub_t;

typedef struct {
    stub_t stub;
    bwg_link_shared_t shared;
    bwg_link_t link;
} fixture_t;

void link_fixture_init(fixture_t *fixture);
void link_fixture_deinit(fixture_t *fixture);
size_t link_feed_record(fixture_t *fixture, const char *kind, const char *maybe_session, uint32_t sequence,
                          const char *payload, uint64_t now_ms);
void link_feed_hello(fixture_t *fixture, uint64_t now_ms);
uint32_t link_published_credit(fixture_t *fixture);

#endif // TEST_LINK_SUPPORT_H
