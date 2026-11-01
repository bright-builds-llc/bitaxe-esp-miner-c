#include "bwg_test.h"

#include <stdio.h>
#include <stdlib.h>

#ifndef BWG_FIXTURE_DIR
#error "BWG_FIXTURE_DIR must name host_test/fixtures"
#endif

typedef struct {
    size_t size;
    size_t padding;
} block_header_t;

static size_t outstanding;
static long fail_countdown = -1;
static size_t fail_size;
static const char *watched_secret;
static int tests_run;
static int tests_failed;
static bool current_failed;

static void *test_alloc(void *ctx, size_t size)
{
    (void) ctx;
    if (fail_size != 0 && size >= fail_size) {
        return NULL;
    }
    if (fail_countdown == 0) {
        fail_countdown = -1;
        return NULL;
    }
    if (fail_countdown > 0) {
        fail_countdown--;
    }
    block_header_t *header = malloc(sizeof(*header) + size);
    if (header == NULL) {
        return NULL;
    }
    header->size = size;
    outstanding++;
    return header + 1;
}

static void test_free(void *ctx, void *ptr)
{
    (void) ctx;
    block_header_t *header = (block_header_t *) ptr - 1;
    if (watched_secret != NULL && header->size >= strlen(watched_secret) &&
        memmem(ptr, header->size, watched_secret, strlen(watched_secret)) != NULL) {
        bwg_test_fail(__FILE__, __LINE__, "secret still present in a freed block");
    }
    outstanding--;
    free(header);
}

const bwg_allocator_t bwg_test_allocator = {.ctx = NULL, .alloc = test_alloc, .free = test_free};

void bwg_test_alloc_fail_after(long count)
{
    fail_countdown = count;
}

void bwg_test_alloc_fail_size(size_t bytes)
{
    fail_size = bytes;
}

size_t bwg_test_alloc_outstanding(void)
{
    return outstanding;
}

void bwg_test_alloc_watch_secret(const char *needle)
{
    watched_secret = needle;
}

void bwg_test_fail(const char *file, int line, const char *message)
{
    current_failed = true;
    fprintf(stderr, "    %s:%d: %s\n", file, line, message);
}

void bwg_test_run(const char *name, void (*test)(void))
{
    current_failed = false;
    bwg_test_alloc_fail_after(-1);
    bwg_test_alloc_fail_size(0);
    bwg_test_alloc_watch_secret(NULL);
    size_t before = outstanding;
    test();
    if (outstanding != before) {
        bwg_test_fail(__FILE__, __LINE__, "the test leaked core allocations");
    }
    bwg_test_alloc_watch_secret(NULL);
    tests_run++;
    if (current_failed) {
        tests_failed++;
        fprintf(stderr, "FAIL %s\n", name);
    }
}

int bwg_test_summary(void)
{
    printf("bwg host tests: %d run, %d passed, %d failed\n", tests_run, tests_run - tests_failed, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}

char *bwg_test_read_fixture(const char *relative_path, size_t *len)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", BWG_FIXTURE_DIR, relative_path);
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        bwg_test_fail(path, 0, "fixture is missing");
        return NULL;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *text = malloc((size_t) size + 1);
    size_t got = fread(text, 1, (size_t) size, file);
    fclose(file);
    text[got] = '\0';
    if (len != NULL) {
        *len = got;
    }
    return text;
}

void bwg_test_free_fixture(char *text)
{
    free(text);
}

yyjson_doc *bwg_test_fixture_doc(const char *relative_path)
{
    size_t len = 0;
    char *text = bwg_test_read_fixture(relative_path, &len);
    if (text == NULL) {
        return NULL;
    }
    yyjson_doc *doc = yyjson_read(text, len, 0);
    free(text);
    if (doc == NULL) {
        bwg_test_fail(relative_path, 0, "fixture is not JSON");
    }
    return doc;
}

char *bwg_test_json_line(yyjson_val *value, size_t *len)
{
    size_t written = 0;
    char *json = yyjson_val_write(value, 0, &written);
    char *line = malloc(written + 2);
    memcpy(line, json, written);
    line[written] = '\n';
    line[written + 1] = '\0';
    free(json);
    *len = written + 1;
    return line;
}
