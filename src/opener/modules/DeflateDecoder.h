#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace peare {
namespace compression {

// Sequential byte source used by the raw Deflate decoder. It permits callers
// such as ZIP to decode directly from a positioned store without first copying
// the whole compressed member into memory.
class InflateInput {
public:
    virtual ~InflateInput() {}
    virtual bool readByte(std::uint8_t* value) = 0;
    virtual std::size_t consumed() const = 0;
};

class MemoryInflateInput final : public InflateInput {
public:
    MemoryInflateInput(const std::uint8_t* data, std::size_t size);

    bool readByte(std::uint8_t* value) override;
    std::size_t consumed() const override;

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t position_;
};

// Decode enough of a raw Deflate stream to produce exactly `wanted` bytes.
// The stream may remain unfinished. This is intended for lazy ZIP prefix reads.
bool inflateRawPrefix(InflateInput& input, std::size_t wanted,
                      std::vector<std::uint8_t>* output,
                      std::string* error = nullptr);

bool inflateRawPrefix(const std::uint8_t* data, std::size_t size,
                      std::size_t wanted, std::vector<std::uint8_t>* output,
                      std::string* error = nullptr);

// Decode a complete raw Deflate stream and require exactly `expected` output
// bytes. `dictionary` supplies an optional prior 32 KiB history, as required by
// CAB MSZIP blocks after the first block.
bool inflateRawExact(InflateInput& input, std::size_t expected,
                     const std::vector<std::uint8_t>* dictionary,
                     std::vector<std::uint8_t>* output,
                     std::string* error = nullptr);

bool inflateRawExact(const std::uint8_t* data, std::size_t size,
                     std::size_t expected,
                     const std::vector<std::uint8_t>* dictionary,
                     std::vector<std::uint8_t>* output,
                     std::string* error = nullptr);

// Decode a complete zlib stream, verify its header and Adler-32 trailer, and
// reject output larger than `maximum`.
bool inflateZlib(const std::uint8_t* data, std::size_t size,
                 std::size_t maximum, std::vector<std::uint8_t>* output,
                 std::string* error = nullptr);

bool inflateZlib(const std::vector<std::uint8_t>& input,
                 std::size_t maximum, std::vector<std::uint8_t>* output,
                 std::string* error = nullptr);

bool inflateZlibExact(const std::uint8_t* data, std::size_t size,
                      std::size_t expected, std::vector<std::uint8_t>* output,
                      std::string* error = nullptr);

bool inflateZlibExact(const std::vector<std::uint8_t>& input,
                      std::size_t expected, std::vector<std::uint8_t>* output,
                      std::string* error = nullptr);

}  // namespace compression
}  // namespace peare
