#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int peare_mtx_decompress(const uint8_t* input, uint32_t input_size, int encrypted,
                         uint8_t** output, uint32_t* output_size);
void peare_mtx_free(uint8_t* output);

#ifdef __cplusplus
}
#endif
