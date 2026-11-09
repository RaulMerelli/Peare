#include "UnixCompress.h"

#include <QIODevice>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace peare {
namespace compression {
namespace {

class CodeReader {
public:
    explicit CodeReader(QIODevice* input) : input_(input) {}

    bool read(int width, int* code) {
        while (availableBits_ < width) {
            char byte = 0;
            if (!input_ || !input_->getChar(&byte)) return false;
            bitBuffer_ |= std::uint32_t(std::uint8_t(byte)) << availableBits_;
            availableBits_ += 8;
            ++bytesInSection_;
        }
        const std::uint32_t mask = (std::uint32_t(1) << width) - 1;
        *code = int(bitBuffer_ & mask);
        bitBuffer_ >>= width;
        availableBits_ -= width;
        return true;
    }

    bool resetSection(int width) {
        bitBuffer_ = 0;
        availableBits_ = 0;
        return skipPadding(width);
    }

    bool beginNewWidthSection(int width) {
        bitBuffer_ = 0;
        availableBits_ = 0;
        return skipPadding(width);
    }

private:
    bool skipPadding(int width) {
        const std::uint64_t remainder = bytesInSection_ % std::uint64_t(width);
        const std::uint64_t skip = (std::uint64_t(width) - remainder) % width;
        for (std::uint64_t i = 0; i < skip; ++i) {
            char ignored = 0;
            if (!input_ || !input_->getChar(&ignored)) return false;
        }
        bytesInSection_ = 0;
        return true;
    }

    QIODevice* input_;
    std::uint32_t bitBuffer_ = 0;
    int availableBits_ = 0;
    std::uint64_t bytesInSection_ = 0;
};

bool writeAll(QIODevice* output, const char* data, qint64 size) {
    while (size > 0) {
        if (!output) return false;
        const qint64 written = output->write(data, size);
        if (written <= 0) return false;
        data += written;
        size -= written;
    }
    return true;
}

}

bool decompressUnixCompress(QIODevice* input, QIODevice* output) {
    if (!input || !output || !input->isOpen() || !output->isOpen()) return false;
    char header[3] = {};
    if (input->read(header, sizeof(header)) != qint64(sizeof(header)) ||
        std::uint8_t(header[0]) != 0x1f || std::uint8_t(header[1]) != 0x9d)
        return false;

    const std::uint8_t flags = std::uint8_t(header[2]);
    const int maxBits = flags & 0x1f;
    const bool blockMode = (flags & 0x80) != 0;
    if (maxBits < 9 || maxBits > 16 || (flags & 0x60) != 0) return false;

    const int maxCodes = 1 << maxBits;
    int codeWidth = 9;
    int sectionEnd = (1 << codeWidth) - 1;
    int nextCode = blockMode ? 257 : 256;
    int previousCode = -1;
    std::uint8_t firstByte = 0;
    std::vector<std::uint16_t> prefixes(std::size_t(maxCodes), 0);
    std::vector<std::uint8_t> suffixes(std::size_t(maxCodes), 0);
    std::vector<std::uint8_t> stack(std::size_t(maxCodes) + 1, 0);
    std::vector<char> outputBuffer(65536);
    std::size_t outputSize = 0;
    CodeReader reader(input);
    for (int code = 0; code < 256; ++code)
        suffixes[std::size_t(code)] = std::uint8_t(code);

    const auto flush = [&]() {
        if (!outputSize) return true;
        const bool ok = writeAll(output, outputBuffer.data(), qint64(outputSize));
        outputSize = 0;
        return ok;
    };
    const auto append = [&](std::uint8_t value) {
        outputBuffer[outputSize++] = char(value);
        return outputSize != outputBuffer.size() || flush();
    };

    for (;;) {
        int code = 0;
        if (!reader.read(codeWidth, &code)) break;
        if (blockMode && code == 256) {
            if (!reader.resetSection(codeWidth)) return false;
            codeWidth = 9;
            sectionEnd = (1 << codeWidth) - 1;
            nextCode = 257;
            previousCode = -1;
            continue;
        }
        if (code > nextCode || (code == nextCode && previousCode < 0)) return false;

        const int inputCode = code;
        std::size_t stackSize = 0;
        if (code == nextCode) {
            if (previousCode < 0 || stackSize >= stack.size()) return false;
            stack[stackSize++] = firstByte;
            code = previousCode;
        }
        while (code >= 256) {
            if (code >= nextCode || stackSize >= stack.size()) return false;
            stack[stackSize++] = suffixes[std::size_t(code)];
            code = prefixes[std::size_t(code)];
        }
        if (code < 0 || code > 255 || stackSize >= stack.size()) return false;
        firstByte = std::uint8_t(code);
        stack[stackSize++] = firstByte;
        std::reverse(stack.begin(), stack.begin() + stackSize);
        for (std::size_t i = 0; i < stackSize; ++i)
            if (!append(stack[i])) return false;

        if (previousCode >= 0 && nextCode < maxCodes) {
            prefixes[std::size_t(nextCode)] = std::uint16_t(previousCode);
            suffixes[std::size_t(nextCode)] = firstByte;
            ++nextCode;
            if (nextCode > sectionEnd && codeWidth < maxBits) {
                if (!reader.beginNewWidthSection(codeWidth)) return false;
                ++codeWidth;
                sectionEnd = codeWidth == maxBits ? maxCodes : (1 << codeWidth) - 1;
            }
        }
        previousCode = inputCode;
    }
    return flush();
}

}
}
