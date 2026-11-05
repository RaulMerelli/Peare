#include "ext4_casefold.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "ext4_casefold_data.h"

#define EXT4_CASEFOLD_MAX_NAME 255
#define EXT4_CASEFOLD_MAX_CODEPOINTS 4096

static const struct ext4_unicode_mapping *find_mapping(
    const struct ext4_unicode_mapping *table, size_t count, uint32_t codepoint)
{
    size_t low = 0;
    size_t high = count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (table[middle].codepoint < codepoint)
            low = middle + 1;
        else
            high = middle;
    }
    return low < count && table[low].codepoint == codepoint
        ? &table[low] : NULL;
}

static uint8_t combining_class(uint32_t codepoint)
{
    size_t low = 0;
    size_t high = EXT4_UNICODE_CCC_COUNT;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        if (ext4_unicode_ccc_table[middle].codepoint < codepoint)
            low = middle + 1;
        else
            high = middle;
    }
    if (low < EXT4_UNICODE_CCC_COUNT &&
        ext4_unicode_ccc_table[low].codepoint == codepoint)
        return ext4_unicode_ccc_table[low].value;
    return 0;
}

static int append_codepoint(uint32_t *output, size_t *length, uint32_t codepoint)
{
    if (*length == EXT4_CASEFOLD_MAX_CODEPOINTS)
        return ENAMETOOLONG;
    output[(*length)++] = codepoint;
    return 0;
}

static int append_decomposed(uint32_t *output, size_t *length,
                             uint32_t codepoint, unsigned int depth)
{
    const uint32_t s_base = 0xac00;
    const uint32_t l_base = 0x1100;
    const uint32_t v_base = 0x1161;
    const uint32_t t_base = 0x11a7;
    const uint32_t l_count = 19;
    const uint32_t v_count = 21;
    const uint32_t t_count = 28;
    const uint32_t n_count = v_count * t_count;
    const uint32_t s_count = l_count * n_count;
    if (depth > 8)
        return EINVAL;
    if (codepoint >= s_base && codepoint < s_base + s_count) {
        uint32_t index = codepoint - s_base;
        int result = append_codepoint(output, length,
                                      l_base + index / n_count);
        if (result)
            return result;
        result = append_codepoint(output, length,
                                  v_base + (index % n_count) / t_count);
        if (result)
            return result;
        if (index % t_count)
            return append_codepoint(output, length,
                                    t_base + index % t_count);
        return 0;
    }
    const struct ext4_unicode_mapping *mapping = find_mapping(
        ext4_unicode_decomposition, EXT4_UNICODE_DECOMPOSITION_COUNT,
        codepoint);
    if (!mapping)
        return append_codepoint(output, length, codepoint);
    for (uint8_t i = 0; i < mapping->length; ++i) {
        int result = append_decomposed(output, length,
            ext4_unicode_decomposition_values[mapping->offset + i], depth + 1);
        if (result)
            return result;
    }
    return 0;
}

static int decode_utf8(const unsigned char *input, size_t length,
                       uint32_t *output, size_t *output_length)
{
    size_t index = 0;
    *output_length = 0;
    while (index < length) {
        uint32_t value;
        unsigned char first = input[index++];
        if (first < 0x80) {
            value = first;
        } else if (first >= 0xc2 && first <= 0xdf && index < length &&
                   (input[index] & 0xc0) == 0x80) {
            value = ((uint32_t)(first & 0x1f) << 6) |
                    (input[index++] & 0x3f);
        } else if (first >= 0xe0 && first <= 0xef && index + 1 < length &&
                   (input[index] & 0xc0) == 0x80 &&
                   (input[index + 1] & 0xc0) == 0x80 &&
                   !(first == 0xe0 && input[index] < 0xa0) &&
                   !(first == 0xed && input[index] >= 0xa0)) {
            value = ((uint32_t)(first & 0x0f) << 12) |
                    ((uint32_t)(input[index] & 0x3f) << 6) |
                    (input[index + 1] & 0x3f);
            index += 2;
        } else if (first >= 0xf0 && first <= 0xf4 && index + 2 < length &&
                   (input[index] & 0xc0) == 0x80 &&
                   (input[index + 1] & 0xc0) == 0x80 &&
                   (input[index + 2] & 0xc0) == 0x80 &&
                   !(first == 0xf0 && input[index] < 0x90) &&
                   !(first == 0xf4 && input[index] >= 0x90)) {
            value = ((uint32_t)(first & 0x07) << 18) |
                    ((uint32_t)(input[index] & 0x3f) << 12) |
                    ((uint32_t)(input[index + 1] & 0x3f) << 6) |
                    (input[index + 2] & 0x3f);
            index += 3;
        } else {
            return EILSEQ;
        }
        int result = append_codepoint(output, output_length, value);
        if (result)
            return result;
    }
    return 0;
}

static void canonical_order(uint32_t *values, size_t length)
{
    for (size_t i = 1; i < length; ++i) {
        uint8_t current_class = combining_class(values[i]);
        if (!current_class)
            continue;
        size_t position = i;
        while (position > 0) {
            uint8_t previous_class = combining_class(values[position - 1]);
            if (!previous_class || previous_class <= current_class)
                break;
            uint32_t value = values[position];
            values[position] = values[position - 1];
            values[position - 1] = value;
            --position;
        }
    }
}

static int encode_utf8(uint32_t codepoint, char *output, size_t capacity,
                       size_t *length)
{
    size_t needed = codepoint < 0x80 ? 1 : codepoint < 0x800 ? 2 :
                    codepoint < 0x10000 ? 3 : 4;
    if (needed > capacity - *length)
        return ENAMETOOLONG;
    if (needed == 1) {
        output[(*length)++] = (char)codepoint;
    } else if (needed == 2) {
        output[(*length)++] = (char)(0xc0 | (codepoint >> 6));
        output[(*length)++] = (char)(0x80 | (codepoint & 0x3f));
    } else if (needed == 3) {
        output[(*length)++] = (char)(0xe0 | (codepoint >> 12));
        output[(*length)++] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        output[(*length)++] = (char)(0x80 | (codepoint & 0x3f));
    } else {
        output[(*length)++] = (char)(0xf0 | (codepoint >> 18));
        output[(*length)++] = (char)(0x80 | ((codepoint >> 12) & 0x3f));
        output[(*length)++] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        output[(*length)++] = (char)(0x80 | (codepoint & 0x3f));
    }
    return 0;
}

int ext4_casefold_fold(const char *name, size_t length, char *output,
                       size_t output_capacity, size_t *output_length)
{
    uint32_t decoded[EXT4_CASEFOLD_MAX_NAME];
    uint32_t decomposed[EXT4_CASEFOLD_MAX_CODEPOINTS];
    uint32_t folded[EXT4_CASEFOLD_MAX_CODEPOINTS];
    size_t decoded_length = 0;
    size_t decomposed_length = 0;
    size_t folded_length = 0;
    size_t encoded_length = 0;
    int result;
    if (!name || !output || !output_length || length > EXT4_CASEFOLD_MAX_NAME)
        return EINVAL;
    result = decode_utf8((const unsigned char *)name, length, decoded,
                         &decoded_length);
    if (result == EILSEQ) {
        if (length > output_capacity)
            return ENAMETOOLONG;
        memcpy(output, name, length);
        *output_length = length;
        return 0;
    }
    if (result)
        return result;
    for (size_t i = 0; i < decoded_length; ++i) {
        result = append_decomposed(decomposed, &decomposed_length,
                                   decoded[i], 0);
        if (result)
            return result;
    }
    canonical_order(decomposed, decomposed_length);
    for (size_t i = 0; i < decomposed_length; ++i) {
        const struct ext4_unicode_mapping *mapping = find_mapping(
            ext4_unicode_casefold, EXT4_UNICODE_CASEFOLD_COUNT,
            decomposed[i]);
        if (!mapping) {
            result = append_codepoint(folded, &folded_length, decomposed[i]);
            if (result)
                return result;
            continue;
        }
        for (uint8_t j = 0; j < mapping->length; ++j) {
            result = append_codepoint(folded, &folded_length,
                ext4_unicode_casefold_values[mapping->offset + j]);
            if (result)
                return result;
        }
    }
    decomposed_length = 0;
    for (size_t i = 0; i < folded_length; ++i) {
        result = append_decomposed(decomposed, &decomposed_length,
                                   folded[i], 0);
        if (result)
            return result;
    }
    canonical_order(decomposed, decomposed_length);
    for (size_t i = 0; i < decomposed_length; ++i) {
        result = encode_utf8(decomposed[i], output, output_capacity,
                             &encoded_length);
        if (result)
            return result;
    }
    *output_length = encoded_length;
    return 0;
}

int ext4_casefold_equal(const char *left, size_t left_length,
                        const char *right, size_t right_length, int *equal)
{
    char folded_left[EXT4_CASEFOLD_MAX_BYTES];
    char folded_right[EXT4_CASEFOLD_MAX_BYTES];
    size_t folded_left_length = 0;
    size_t folded_right_length = 0;
    int result;
    if (!equal)
        return EINVAL;
    *equal = 0;
    result = ext4_casefold_fold(left, left_length, folded_left,
                                sizeof(folded_left), &folded_left_length);
    if (result)
        return result;
    result = ext4_casefold_fold(right, right_length, folded_right,
                                sizeof(folded_right), &folded_right_length);
    if (result)
        return result;
    *equal = folded_left_length == folded_right_length &&
             !memcmp(folded_left, folded_right, folded_left_length);
    return 0;
}
