#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

bool decompressLhaLk7(const char* method,
                      const uint8_t* input,
                      size_t inputSize,
                      size_t outputSize,
                      std::vector<uint8_t>* output);
