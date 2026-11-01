#include "test_link_support.h"

#include <stdio.h>
#include <stdlib.h>

#include "bwg_codec.h"
#include "bwg_json.h"
#include "bwg_link.h"
#include "bwg_test.h"
#include "test_suites.h"

static bool stub_random(void *ctx, uint8_t *bytes, size_t len)
{
    stub_t *stub = ctx;
    if (stub->refuse_random) {
        return false;
    }
    memset(bytes, 0x11, len);
    return true;
}

static bool stub_begin(void *ctx, uint64_t now_ms, uint32_t *generation)
{
    (void) now_ms;
    stub_t *stub = ctx;
    stub->generations_begun++;
    *generation = 100 + (uint32_t) stub->generations_begun;
    return !stub->refuse_generation;
}

static void stub_heartbeat(void *ctx, uint32_t generation, uint64_t now_ms)
{
    (void) generation;
    (void) now_ms;
    ((stub_t *) ctx)->heartbeats++;
}

static void stub_close(void *ctx, uint32_t generation, uint64_t now_ms)
{
    (void) generation;
    (void) now_ms;
    ((stub_t *) ctx)->generations_closed++;
}

static bool stub_session(void *ctx, uint32_t epoch, const bwg_session_binding_t *binding, uint32_t generation)
{
    (void) epoch;
    (void) binding;
    (void) generation;
    stub_t *stub = ctx;
    stub->sessions++;
    return !stub->refuse_session;
}

static bool stub_control(void *ctx, uint32_t epoch, uint32_t sequence, bwg_buffer_t frame)
{
    (void) epoch;
    stub_t *stub = ctx;
    if (stub->refuse_control) {
        return false;
    }
    stub->controls++;
    bwg_buffer_release(&bwg_test_allocator, &stub->control);
    stub->control = frame;
    stub->control_sequence = sequence;
    return true;
}

static bool stub_ack(void *ctx, uint32_t epoch, const char *session_id, const uint8_t *payload, size_t len)
{
    (void) session_id;
    stub_t *stub = ctx;
    if (stub->refuse_ack) {
        return false;
    }
    stub->acks++;
    stub->last_ack_epoch = epoch;
    memcpy(stub->ack, payload, len);
    stub->ack_len = len;
    return true;
}

static void stub_failure(void *ctx, bwg_link_failure_t failure, size_t observed)
{
    (void) observed;
    stub_t *stub = ctx;
    stub->last_failure = failure;
    stub->failures++;
}

static void stub_failure_cleared(void *ctx)
{
    ((stub_t *) ctx)->failure_clears++;
}

void link_fixture_init(fixture_t *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    bwg_link_shared_init(&fixture->shared);
    const bwg_link_ops_t ops = {
        .ctx = &fixture->stub,
        .fill_random = stub_random,
        .begin_generation = stub_begin,
        .heartbeat_generation = stub_heartbeat,
        .close_generation = stub_close,
        .enqueue_session = stub_session,
        .enqueue_control = stub_control,
        .send_hello_ack = stub_ack,
        .on_failure = stub_failure,
        .on_failure_cleared = stub_failure_cleared,
    };
    char commit[41];
    char elf[65];
    memset(commit, 'a', 40);
    commit[40] = '\0';
    memset(elf, 'b', 64);
    elf[64] = '\0';
    bwg_link_init(&fixture->link, &bwg_test_allocator, &fixture->shared, &ops, commit, elf);
}

void link_fixture_deinit(fixture_t *fixture)
{
    bwg_buffer_release(&bwg_test_allocator, &fixture->stub.control);
    bwg_link_deinit(&fixture->link, 0);
}

/** Builds and feeds one integrity-valid line. Returns its length. */
size_t link_feed_record(fixture_t *fixture, const char *kind, const char *maybe_session, uint32_t sequence,
                          const char *payload, uint64_t now_ms)
{
    char digest[BWG_DIGEST_TEXT_SIZE];
    bwg_sha256_text((const uint8_t *) payload, strlen(payload), digest);
    size_t capacity = strlen(payload) + 512;
    char *line = malloc(capacity);
    char session[64];
    if (maybe_session == NULL) {
        snprintf(session, sizeof(session), "null");
    } else {
        snprintf(session, sizeof(session), "\"%s\"", maybe_session);
    }
    size_t len = (size_t) snprintf(line, capacity,
                                   "{\"profile\":\"bwg-worker-serial/0.2\",\"kind\":\"%s\",\"sessionId\":%s,"
                                   "\"sequence\":%u,\"payloadBytes\":%zu,\"payloadSha256\":\"%s\",\"payload\":%s}\n",
                                   kind, session, sequence, strlen(payload), digest, payload);
    bwg_link_feed(&fixture->link, (const uint8_t *) line, len, now_ms);
    free(line);
    return len;
}

void link_feed_hello(fixture_t *fixture, uint64_t now_ms)
{
    link_feed_record(fixture, "session", NULL, 0, "{\"op\":\"hello\",\"hostNonce\":\"" HOST_NONCE "\"}", now_ms);
}

uint32_t link_published_credit(fixture_t *fixture)
{
    uint32_t received = 0;
    bwg_credit_received(&fixture->shared, bwg_link_epoch(&fixture->link), &received);
    return received;
}
