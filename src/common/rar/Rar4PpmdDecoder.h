#ifndef PEARE_RAR4_PPMD_DECODER_H
#define PEARE_RAR4_PPMD_DECODER_H

#include <stdint.h>

#include "../../opener/compression/third_party/lzma/Ppmd7.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*PeareRar4PpmdReadByte)(void *context, uint8_t *value);

typedef struct PeareRar4PpmdDecoder {
    uint32_t low;
    uint32_t range;
    uint32_t code;
    PeareRar4PpmdReadByte read_byte;
    void *read_context;
    int status;
} PeareRar4PpmdDecoder;

enum {
    PEARE_RAR4_PPMD_END = -1,
    PEARE_RAR4_PPMD_ERROR = -2
};

int peare_rar4_ppmd_init(PeareRar4PpmdDecoder *decoder,
                         PeareRar4PpmdReadByte read_byte,
                         void *read_context);
int peare_rar4_ppmd_decode_symbol(PeareRar4PpmdDecoder *decoder,
                                  CPpmd7 *model);

#ifdef __cplusplus
}
#endif

#endif
