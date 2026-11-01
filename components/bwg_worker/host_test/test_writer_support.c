#include "test_writer_support.h"

#include <stdio.h>
#include <stdlib.h>

#include "bwg_frame.h"
#include "bwg_test.h"
#include "bwg_writer.h"
#include "test_suites.h"

static int32_t sink_write(void *ctx, const uint8_t *bytes, size_t len)
{
    sink_state_t *state = ctx;
    if (state->accept_limit < 0) {
        return -1;
    }
    size_t accepted = len < (size_t) state->accept_limit ? len : (size_t) state->accept_limit;
    accepted = accepted < state->budget ? accepted : state->budget;
    state->budget -= accepted;
    if (accepted == 0) {
        return 0;
    }
    memcpy(state->bytes + state->len, bytes, accepted);
    state->len += accepted;
    state->writes++;
    if (len > state->largest_chunk) {
        state->largest_chunk = len;
    }
    if (state->maybe_revoke_shared != NULL) {
        bwg_link_shared_revoke(state->maybe_revoke_shared, state->revoke_epoch);
        state->maybe_revoke_shared = NULL;
    }
    return (int32_t) accepted;
}

static bool sink_flushed(void *ctx)
{
    return !((sink_state_t *) ctx)->hold_flush;
}

void writer_fixture_init(fixture_t *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    bwg_link_shared_init(&fixture->shared);
    bwg_writer_init(&fixture->writer, &bwg_test_allocator, &fixture->shared);
    fixture->state.capacity = 256 * 1024;
    fixture->state.bytes = malloc(fixture->state.capacity);
    fixture->state.accept_limit = 1 << 20;
    fixture->state.budget = SIZE_MAX;
    fixture->sink = (bwg_sink_t) {.ctx = &fixture->state, .write = sink_write, .flushed = sink_flushed};
}

void writer_fixture_deinit(fixture_t *fixture)
{
    bwg_writer_deinit(&fixture->writer);
    free(fixture->state.bytes);
}

/** Starts epoch 1 and writes its acknowledgement at `now_ms`. */
void writer_open_session(fixture_t *fixture, uint32_t epoch, uint64_t now_ms)
{
    bwg_credit_begin(&fixture->shared, epoch);
    atomic_store(&fixture->shared.current_epoch, epoch);
    bwg_writer_begin_session(&fixture->writer, epoch, SESSION_ID, (const uint8_t *) ACK, strlen(ACK));
    while (bwg_writer_next(&fixture->writer, &fixture->sink, now_ms).status == BWG_WRITER_WROTE) {
    }
}

/** Runs the writer until it stops making progress. */
void writer_drain(fixture_t *fixture, uint64_t now_ms)
{
    while (bwg_writer_next(&fixture->writer, &fixture->sink, now_ms).status == BWG_WRITER_WROTE) {
    }
}

/** Parses the output into records, returning how many, with sequences and kinds. */
size_t writer_parse_output(const fixture_t *fixture, bwg_record_t *records, size_t capacity)
{
    size_t count = 0;
    const uint8_t *at = fixture->state.bytes;
    const uint8_t *end = fixture->state.bytes + fixture->state.len;
    while (at < end && count < capacity) {
        const uint8_t *lf = memchr(at, '\n', (size_t) (end - at));
        if (lf == NULL) {
            break;
        }
        size_t len = (size_t) (lf - at) + 1;
        if (bwg_record_parse(at, len, &records[count]) == BWG_FRAME_OK) {
            count++;
        }
        at += len;
    }
    return count;
}
