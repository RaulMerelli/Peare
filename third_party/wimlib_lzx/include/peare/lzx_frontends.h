#ifndef PEARE_LZX_FRONTENDS_H
#define PEARE_LZX_FRONTENDS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum peare_lzx_status {
    PEARE_LZX_OK = 0,
    PEARE_LZX_INVALID_ARGUMENT = 1,
    PEARE_LZX_OUT_OF_MEMORY = 2,
    PEARE_LZX_CORRUPT_STREAM = 3,
    PEARE_LZX_UNSUPPORTED = 4
} peare_lzx_status;

typedef struct peare_lzx_wim_decoder peare_lzx_wim_decoder;
typedef struct peare_lzx_xex_decoder peare_lzx_xex_decoder;
typedef struct peare_lzx_cab_decoder peare_lzx_cab_decoder;

/* WIM chunks are independent and remain handled by the original wimlib
 * decoder entry point. */
peare_lzx_status peare_lzx_wim_create(size_t max_chunk_size,
                                      peare_lzx_wim_decoder **decoder);
peare_lzx_status peare_lzx_wim_decompress(peare_lzx_wim_decoder *decoder,
                                          const void *compressed_data,
                                          size_t compressed_size,
                                          void *uncompressed_data,
                                          size_t uncompressed_size);
void peare_lzx_wim_destroy(peare_lzx_wim_decoder *decoder);

/* XEX uses one continuous LZX state across the de-blocked stream and may emit
 * more bytes than the physical window.  This is deliberately a distinct API;
 * it must never be implemented by resetting the WIM decoder per XEX chunk. */
peare_lzx_status peare_lzx_xex_create(size_t window_size,
                                      size_t expected_output_size,
                                      peare_lzx_xex_decoder **decoder);
peare_lzx_status peare_lzx_xex_decompress(peare_lzx_xex_decoder *decoder,
                                          const void *compressed_stream,
                                          size_t compressed_size,
                                          void *uncompressed_image,
                                          size_t image_size);
void peare_lzx_xex_destroy(peare_lzx_xex_decoder *decoder);

/* Reserved now so CAB support can be added without changing the public engine
 * boundary.  CAB framing/state is not implemented in this batch. */
peare_lzx_status peare_lzx_cab_create(size_t window_size,
                                      peare_lzx_cab_decoder **decoder);
void peare_lzx_cab_destroy(peare_lzx_cab_decoder *decoder);

#ifdef __cplusplus
}
#endif
#endif
