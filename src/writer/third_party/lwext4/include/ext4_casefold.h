#ifndef EXT4_CASEFOLD_H_
#define EXT4_CASEFOLD_H_

#include <stddef.h>

#define EXT4_CASEFOLD_MAX_BYTES (4096 * 4)

int ext4_casefold_fold(const char *name, size_t length, char *output,
                       size_t output_capacity, size_t *output_length);
int ext4_casefold_equal(const char *left, size_t left_length,
                        const char *right, size_t right_length, int *equal);

#endif
