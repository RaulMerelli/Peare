#include <stdlib.h>
#if defined(_WIN32)
#include <malloc.h>
void *wimlib_aligned_malloc(size_t size, size_t alignment) { return _aligned_malloc(size, alignment); }
void wimlib_aligned_free(void *p) { _aligned_free(p); }
#else
void *wimlib_aligned_malloc(size_t size, size_t alignment) {
    void *p = NULL;
    return posix_memalign(&p, alignment, size) == 0 ? p : NULL;
}
void wimlib_aligned_free(void *p) { free(p); }
#endif
