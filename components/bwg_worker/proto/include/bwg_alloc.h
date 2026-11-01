#ifndef BWG_ALLOC_H
#define BWG_ALLOC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * Where the core puts its big buffers. The device points this at PSRAM; tests count and fail
 * allocations through it. `free` accepts only what `alloc` returned and never receives NULL.
 */
typedef struct {
    void *ctx;
    void *(*alloc)(void *ctx, size_t size);
    void (*free)(void *ctx, void *ptr);
} bwg_allocator_t;

/** An owned byte buffer. `data` is NULL when it owns nothing. */
typedef struct {
    uint8_t *data;
    size_t len;
    size_t capacity;
} bwg_buffer_t;

/** Returns NULL when `size` is zero or the allocator refuses. */
void *bwg_alloc(const bwg_allocator_t *allocator, size_t size);

/** Zeroizes `size` bytes and frees them. A NULL `ptr` does nothing. */
void bwg_free_secret(const bwg_allocator_t *allocator, void *ptr, size_t size);

/** Returns false, leaving `buffer` empty, when the allocation fails. */
bool bwg_buffer_alloc(const bwg_allocator_t *allocator, bwg_buffer_t *buffer, size_t capacity);

/** Zeroizes and frees the whole capacity, leaving `buffer` empty. An empty buffer is a no-op. */
void bwg_buffer_release(const bwg_allocator_t *allocator, bwg_buffer_t *buffer);

#endif // BWG_ALLOC_H
