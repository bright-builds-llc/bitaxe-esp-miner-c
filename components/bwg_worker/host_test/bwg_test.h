#ifndef BWG_TEST_H
#define BWG_TEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <yyjson.h>

#include "bwg_alloc.h"

/** A test allocator: counts outstanding blocks, can fail on command, and spots secrets on free. */
extern const bwg_allocator_t bwg_test_allocator;

/** Fail the allocation `count` allocations from now (0 is the next). -1 turns failure off. */
void bwg_test_alloc_fail_after(long count);
/** Fail every allocation of at least `bytes`. 0 turns this off. */
void bwg_test_alloc_fail_size(size_t bytes);
size_t bwg_test_alloc_outstanding(void);
/** Record a failure if any later freed block still holds these bytes. NULL turns it off. */
void bwg_test_alloc_watch_secret(const char *needle);

void bwg_test_fail(const char *file, int line, const char *message);
void bwg_test_run(const char *name, void (*test)(void));
int bwg_test_summary(void);

/** Reads a fixture relative to host_test/fixtures. Fails the running test and returns NULL if absent. */
char *bwg_test_read_fixture(const char *relative_path, size_t *len);
void bwg_test_free_fixture(char *text);
/** Parses a fixture with yyjson's default allocator, for the harness side only. */
yyjson_doc *bwg_test_fixture_doc(const char *relative_path);

/** Writes the value as compact JSON and a trailing LF into malloc'ed memory, for use as a frame. */
char *bwg_test_json_line(yyjson_val *value, size_t *len);

#define BWG_RUN(test) bwg_test_run(#test, test)

#define ASSERT_TRUE(cond)                                     \
    do {                                                      \
        if (!(cond)) {                                        \
            bwg_test_fail(__FILE__, __LINE__, #cond);         \
            return;                                           \
        }                                                     \
    } while (0)

#define ASSERT_FALSE(cond) ASSERT_TRUE(!(cond))

#define ASSERT_EQ(actual, expected) ASSERT_TRUE((actual) == (expected))

#define ASSERT_STREQ(actual, expected) ASSERT_TRUE(strcmp((actual), (expected)) == 0)

#define ASSERT_MEMEQ(actual, expected, len) ASSERT_TRUE(memcmp((actual), (expected), (len)) == 0)

#endif // BWG_TEST_H
