#ifndef TEST_WRITER_SUPPORT_H
#define TEST_WRITER_SUPPORT_H

#include <stdio.h>
#include <stdlib.h>

#include "bwg_frame.h"
#include "bwg_test.h"
#include "bwg_writer.h"
#include "test_suites.h"

#define SESSION_ID "AAAAAAAAAAAAAAAAAAAAAA"

#define ACK "{\"op\":\"hello_ack\"}"

typedef struct {
    uint8_t *bytes;
    size_t len;
    size_t capacity;
    int writes;
    size_t largest_chunk;
    /** Accept at most this many bytes per write; 0 blocks, -1 fails. */
    int32_t accept_limit;
    /** Total bytes accepted before the sink behaves as full. */
    size_t budget;
    bool hold_flush;
    /** Revokes this epoch just after the first accepted write. */
    bwg_link_shared_t *maybe_revoke_shared;
    uint32_t revoke_epoch;
} sink_state_t;

typedef struct {
    bwg_link_shared_t shared;
    bwg_writer_t writer;
    sink_state_t state;
    bwg_sink_t sink;
} fixture_t;

void writer_fixture_init(fixture_t *fixture);
void writer_fixture_deinit(fixture_t *fixture);
void writer_open_session(fixture_t *fixture, uint32_t epoch, uint64_t now_ms);
void writer_drain(fixture_t *fixture, uint64_t now_ms);
size_t writer_parse_output(const fixture_t *fixture, bwg_record_t *records, size_t capacity);

#endif // TEST_WRITER_SUPPORT_H
