#include "peare/lzx_frontends.h"
#include "wimlib/decompressor_ops.h"
#include "wimlib/lzx.h"
#include <stdlib.h>

struct peare_lzx_wim_decoder { void *context; };
struct peare_lzx_xex_decoder {
    size_t window_size;
    size_t expected_output_size;
    void *context;
};
struct peare_lzx_cab_decoder {
    size_t window_size;
    void *context;
};

static peare_lzx_status map_decoder_result(int value)
{
    return value == 0 ? PEARE_LZX_OK : PEARE_LZX_CORRUPT_STREAM;
}

peare_lzx_status peare_lzx_wim_create(size_t max_chunk_size,
                                      peare_lzx_wim_decoder **decoder)
{
    peare_lzx_wim_decoder *out;
    int result;
    if (!decoder || !lzx_window_size_valid(max_chunk_size))
        return PEARE_LZX_INVALID_ARGUMENT;
    *decoder = NULL;
    out = (peare_lzx_wim_decoder *)calloc(1, sizeof(*out));
    if (!out) return PEARE_LZX_OUT_OF_MEMORY;
    result = lzx_decompressor_ops.create_decompressor(max_chunk_size,
                                                       &out->context);
    if (result != 0) {
        free(out);
        return PEARE_LZX_OUT_OF_MEMORY;
    }
    *decoder = out;
    return PEARE_LZX_OK;
}

peare_lzx_status peare_lzx_wim_decompress(peare_lzx_wim_decoder *decoder,
                                          const void *compressed_data,
                                          size_t compressed_size,
                                          void *uncompressed_data,
                                          size_t uncompressed_size)
{
    if (!decoder || !compressed_data || !uncompressed_data ||
        compressed_size == 0 || uncompressed_size == 0)
        return PEARE_LZX_INVALID_ARGUMENT;
    return map_decoder_result(lzx_decompressor_ops.decompress(
        compressed_data, compressed_size, uncompressed_data,
        uncompressed_size, decoder->context));
}

void peare_lzx_wim_destroy(peare_lzx_wim_decoder *decoder)
{
    if (!decoder) return;
    if (decoder->context)
        lzx_decompressor_ops.free_decompressor(decoder->context);
    free(decoder);
}

peare_lzx_status peare_lzx_xex_create(size_t window_size,
                                      size_t expected_output_size,
                                      peare_lzx_xex_decoder **decoder)
{
    peare_lzx_xex_decoder *out;
    if (!decoder || !lzx_window_size_valid(window_size) ||
        expected_output_size == 0)
        return PEARE_LZX_INVALID_ARGUMENT;
    *decoder = NULL;
    out = (peare_lzx_xex_decoder *)calloc(1, sizeof(*out));
    if (!out) return PEARE_LZX_OUT_OF_MEMORY;
    out->window_size = window_size;
    out->expected_output_size = expected_output_size;
    if (lzx_decompressor_ops.create_decompressor(window_size, &out->context) != 0) {
        free(out);
        return PEARE_LZX_OUT_OF_MEMORY;
    }
    *decoder = out;
    return PEARE_LZX_OK;
}

peare_lzx_status peare_lzx_xex_decompress(peare_lzx_xex_decoder *decoder,
                                          const void *compressed_stream,
                                          size_t compressed_size,
                                          void *uncompressed_image,
                                          size_t image_size)
{
    if (!decoder || !compressed_stream || !uncompressed_image ||
        compressed_size == 0 || image_size == 0 ||
        image_size != decoder->expected_output_size)
        return PEARE_LZX_INVALID_ARGUMENT;
    return map_decoder_result(peare_lzx_decompress_contiguous(
        compressed_stream, compressed_size, uncompressed_image, image_size,
        decoder->context));
}

void peare_lzx_xex_destroy(peare_lzx_xex_decoder *decoder)
{
    if (!decoder) return;
    if (decoder->context)
        lzx_decompressor_ops.free_decompressor(decoder->context);
    free(decoder);
}

peare_lzx_status peare_lzx_cab_create(size_t window_size,
                                      peare_lzx_cab_decoder **decoder)
{
    peare_lzx_cab_decoder *out;
    if (!decoder || !lzx_window_size_valid(window_size))
        return PEARE_LZX_INVALID_ARGUMENT;
    *decoder = NULL;
    out = (peare_lzx_cab_decoder *)calloc(1, sizeof(*out));
    if (!out) return PEARE_LZX_OUT_OF_MEMORY;
    out->window_size = window_size;
    if (lzx_decompressor_ops.create_decompressor(window_size, &out->context) != 0) {
        free(out);
        return PEARE_LZX_OUT_OF_MEMORY;
    }
    *decoder = out;
    return PEARE_LZX_OK;
}

peare_lzx_status peare_lzx_cab_decompress(peare_lzx_cab_decoder *decoder,
                                          const void *compressed_stream,
                                          size_t compressed_size,
                                          void *uncompressed_data,
                                          size_t uncompressed_size)
{
    if (!decoder || !compressed_stream || !uncompressed_data ||
        compressed_size == 0 || uncompressed_size == 0)
        return PEARE_LZX_INVALID_ARGUMENT;
    return map_decoder_result(peare_lzx_decompress_contiguous(
        compressed_stream, compressed_size, uncompressed_data,
        uncompressed_size, decoder->context));
}

void peare_lzx_cab_destroy(peare_lzx_cab_decoder *decoder)
{
    if (!decoder) return;
    if (decoder->context)
        lzx_decompressor_ops.free_decompressor(decoder->context);
    free(decoder);
}
