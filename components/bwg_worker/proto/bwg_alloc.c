#include "bwg_alloc.h"

#include <sodium.h>

void *bwg_alloc(const bwg_allocator_t *allocator, size_t size)
{
    if (size == 0) {
        return NULL;
    }
    return allocator->alloc(allocator->ctx, size);
}

void bwg_free_secret(const bwg_allocator_t *allocator, void *ptr, size_t size)
{
    if (ptr == NULL) {
        return;
    }
    sodium_memzero(ptr, size);
    allocator->free(allocator->ctx, ptr);
}

bool bwg_buffer_alloc(const bwg_allocator_t *allocator, bwg_buffer_t *buffer, size_t capacity)
{
    buffer->data = bwg_alloc(allocator, capacity);
    buffer->len = 0;
    buffer->capacity = buffer->data == NULL ? 0 : capacity;
    return buffer->data != NULL;
}

void bwg_buffer_release(const bwg_allocator_t *allocator, bwg_buffer_t *buffer)
{
    bwg_free_secret(allocator, buffer->data, buffer->capacity);
    buffer->data = NULL;
    buffer->len = 0;
    buffer->capacity = 0;
}
