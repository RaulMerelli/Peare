#ifndef PEARE_TINY_AES_H
#define PEARE_TINY_AES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TINY_AES_BLOCK_SIZE 16
#define TINY_AES128_EXPANDED_KEY_SIZE 176

typedef struct tiny_aes128_ctx {
    uint8_t round_key[TINY_AES128_EXPANDED_KEY_SIZE];
    uint8_t iv[TINY_AES_BLOCK_SIZE];
} tiny_aes128_ctx;

void tiny_aes128_init_cbc(tiny_aes128_ctx* ctx, const uint8_t key[16], const uint8_t iv[16]);
void tiny_aes128_cbc_decrypt(tiny_aes128_ctx* ctx, uint8_t* buffer, size_t length);

#ifdef __cplusplus
}
#endif

#endif
