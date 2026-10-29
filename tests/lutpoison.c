/*
 * Test-only, child-scoped malloc interposer for mpvqml_lut_guard_tests.
 *
 * libmpv 0.41.0 reinit_scaler() (video/out/gpu/video.c:1852) allocates its
 * 256-row scaler LUT with talloc_array() and fills only the kernel's taps per
 * 8-float row; the remaining cells keep heap contents and are uploaded as
 * texture data. This interposer makes that latent read deterministic: fresh
 * blocks of exactly the LUT's size (8192 bytes after libmpv's `ta` header, 48
 * bytes in NDEBUG builds and 80 with TA_MEMORY_DEBUGGING) receive a quiet NaN in
 * floats 6-7 of every row (the cells libmpv never writes for 6-tap kernels) and
 * zero elsewhere (cells libmpv overwrites). Correct code never observes it.
 *
 * Only an explicitly preloaded native child uses this library.
 */
#define _GNU_SOURCE
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

extern void *__libc_malloc(size_t size);

static atomic_long poisoned;

long mpvqml_lut_poison_count(void)
{
    return atomic_load(&poisoned);
}

void *malloc(size_t size)
{
    void *block = __libc_malloc(size);
    if (block && (size == 8192 + 48 || size == 8192 + 80)) {
        uint32_t *cells = (uint32_t *)((char *)block + (size - 8192));
        for (int i = 0; i < 2048; i++)
            cells[i] = (i % 8) >= 6 ? UINT32_C(0x7FC00000) : 0;
        atomic_fetch_add(&poisoned, 1);
    }
    return block;
}
