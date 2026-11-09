#include "WinZipJpegEncoder.h"

extern "C" {
#include "../../opener/compression/third_party/winzipjpeg/JPEG.h"
#include "../../opener/compression/third_party/winzipjpeg/ArithmeticDecoder.h"
extern std::uint16_t winzipjpeg_alogtbl[1024];
}

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <numeric>

namespace peare {
namespace writer {
namespace {

struct JpegComponentBlocks {
    unsigned h = 0;
    unsigned v = 0;
    unsigned columns = 0;
    unsigned rows = 0;
    std::vector<WinZipJPEGBlock> blocks;
};

struct JpegInput {
    WinZipJPEGMetadata metadata{};
    std::vector<std::uint8_t> markers;
    std::vector<JpegComponentBlocks> components;
    std::size_t scanOffset = 0;
    std::size_t scanEndOffset = 0;
};

constexpr std::uint16_t kLogP[49] = {
    1024,895,795,706,628,559,493,437,379,331,287,247,212,186,158,143,127,
    110,98,84,72,65,59,53,48,45,42,40,37,35,33,30,28,26,23,21,19,17,15,
    13,11,9,7,5,4,3,2,1,1024};
constexpr std::uint16_t kLqp[49] = {
    0,272,502,726,941,1150,1371,1578,1819,2044,2278,2521,2765,2971,3227,
    3382,3566,3788,3965,4200,4435,4590,4737,4899,5050,5147,5250,5325,5441,
    5527,5617,5758,5863,5976,6157,6295,6447,6616,6806,7024,7278,7585,7972,
    8495,8884,9309,10065,11689,0};
constexpr std::uint16_t kNmaxlp[49] = {
    16384,16110,15105,14826,14444,13975,13804,13547,13265,13240,12915,12844,
    12720,12648,12482,12441,12319,12320,12250,12180,12168,12155,12154,12084,
    12096,12105,12096,12080,12062,12075,12078,12060,12068,12090,12075,12075,
    12103,12121,12150,12181,12221,12294,12411,12615,13120,13113,14574,21860,0};
constexpr std::uint8_t kHalfi[49] = {
    8,8,7,7,7,6,6,6,6,6,6,6,6,6,6,6,6,6,6,8,9,10,10,10,10,10,10,9,9,
    8,8,7,7,6,6,6,5,5,4,4,3,3,3,3,2,2,1,0};
constexpr std::uint8_t kDbli[49] = {
    0,1,2,3,4,5,6,7,8,7,7,6,6,6,6,6,6,6,6,6,6,6,6,6,6,7,8,8,9,9,
    10,10,10,10,10,10,9,8,7,6,6,5,4,3,3,3,2,1,0};

std::uint32_t antilog(std::int32_t value) {
    const unsigned fraction = static_cast<unsigned>(value) & 0x3ffu;
    const int whole = value >> 10;
    const auto base = static_cast<std::uint32_t>(winzipjpeg_alogtbl[fraction]);
    const int shift = 7 - whole;
    return shift >= 0 ? base << shift : base >> -shift;
}

struct Models {
    std::array<WinZipJPEGContext, 4 * 13 * 63> eob{};
    std::array<WinZipJPEGContext, 4 * 62 * 3 * 6> zero{};
    std::array<WinZipJPEGContext, 4 * 63 * 5 * 7> pivot{};
    std::array<WinZipJPEGContext, 4 * 3 * 9 * 9 * 9> acMagnitude{};
    std::array<WinZipJPEGContext, 4 * 3 * 7 * 13> acRemainder{};
    std::array<WinZipJPEGContext, 4 * 27 * 3 * 2> acSign{};
    std::array<WinZipJPEGContext, 4 * 13 * 10> dcMagnitude{};
    std::array<WinZipJPEGContext, 4 * 13 * 14> dcRemainder{};
    std::array<WinZipJPEGContext, 4 * 2 * 2 * 2> dcSign{};
    WinZipJPEGContext fixed{};

    Models() {
        initialize(eob);
        initialize(zero);
        initialize(pivot);
        initialize(acMagnitude);
        initialize(acRemainder);
        initialize(acSign);
        initialize(dcMagnitude);
        initialize(dcRemainder);
        initialize(dcSign);
        init(fixed);
        fixed.i = 48;
    }

    static void init(WinZipJPEGContext& c) {
        c = {0, kNmaxlp[0], 0, 0};
    }

    template <std::size_t N>
    static void initialize(std::array<WinZipJPEGContext, N>& values) {
        for (auto& value : values) init(value);
    }

    WinZipJPEGContext& Eob(unsigned c, unsigned e, unsigned node) {
        return eob[(c * 13 + e) * 63 + node];
    }
    WinZipJPEGContext& Zero(unsigned c, unsigned k, unsigned a, unsigned b) {
        return zero[((c * 62 + k) * 3 + a) * 6 + b];
    }
    WinZipJPEGContext& Pivot(unsigned c, unsigned k, unsigned a, unsigned b) {
        return pivot[((c * 63 + k) * 5 + a) * 7 + b];
    }
    WinZipJPEGContext& AcMagnitude(unsigned c, unsigned n, unsigned a, unsigned b,
                                  unsigned x) {
        return acMagnitude[(((c * 3 + n) * 9 + a) * 9 + b) * 9 + x];
    }
    WinZipJPEGContext& AcRemainder(unsigned c, unsigned n, unsigned x, unsigned bit) {
        return acRemainder[((c * 3 + n) * 7 + x) * 13 + bit];
    }
    WinZipJPEGContext& AcSign(unsigned c, unsigned n, unsigned a, unsigned b) {
        return acSign[((c * 27 + n) * 3 + a) * 2 + b];
    }
    WinZipJPEGContext& DcMagnitude(unsigned c, unsigned a, unsigned bit) {
        return dcMagnitude[(c * 13 + a) * 10 + bit];
    }
    WinZipJPEGContext& DcRemainder(unsigned c, unsigned a, unsigned bit) {
        return dcRemainder[(c * 13 + a) * 14 + bit];
    }
    WinZipJPEGContext& DcSign(unsigned c, unsigned a, unsigned b, unsigned p) {
        return dcSign[((c * 2 + a) * 2 + b) * 2 + p];
    }
};

class LogEncoder {
public:
    bool bit(WinZipJPEGContext* context, unsigned value) {
        if (!context || context->i < 0 || context->i > 48 || value > 1) return false;
        std::int32_t limit = lr_ + context->dlrm;
        if (limit > 0x7ff) renormRange(&limit);
        lr_ += kLogP[context->i];
        if (value == context->mps) {
            if (lr_ >= limit) updateMps(context, &limit);
        } else {
            ++context->k;
            renormRange(&limit);
            const std::uint32_t delta = antilog(lr_);
            const std::uint64_t sum = std::uint64_t(x_) + (std::uint64_t(delta) << shift_);
            x_ = static_cast<std::uint32_t>(sum);
            if (sum >> 32) carry(sum >> 32);
            updateLps(context, &limit);
        }
        context->dlrm = limit - lr_;
        return true;
    }

    void flush() {
        if (flushed_) return;
        std::int32_t limit = lr_;
        renormRange(&limit);
        lr_ += 0x8000;
        limit = lr_;
        renormRange(&limit);
        if (output_.size() >= 2) output_.erase(output_.begin(), output_.begin() + 2);
        serialized_.clear();
        serialized_.reserve(output_.size() + output_.size() / 64);
        for (std::uint8_t byte : output_) {
            serialized_.push_back(byte);
            const std::size_t size = serialized_.size();
            if (size >= 2 && serialized_[size - 2] == 0xff &&
                serialized_[size - 1] == 0xff) serialized_.push_back(0);
        }
        if (serialized_.size() >= 2) serialized_.resize(serialized_.size() - 2);
        flushed_ = true;
    }

    const std::vector<std::uint8_t>& bytes() const { return serialized_; }

private:
    static std::uint32_t antilog(std::int32_t value) {
        return writer::antilog(value);
    }

    void writeByte(std::uint8_t byte) {
        output_.push_back(byte);
    }

    void carry(std::uint64_t value) {
        for (std::size_t i = output_.size(); i > 0; --i) {
            value += output_[i - 1];
            output_[i - 1] = static_cast<std::uint8_t>(value);
            value >>= 8;
            if (!value) break;
        }
    }

    void renormRange(std::int32_t* limit) {
        while (lr_ >= 0x2000) {
            writeByte(static_cast<std::uint8_t>(x_ >> 24));
            x_ <<= 8;
            lr_ -= 0x2000;
            *limit -= 0x2000;
        }
    }

    void updateMps(WinZipJPEGContext* c, std::int32_t* limit) {
        if (c->k <= 5 && c->i < 47) {
            ++c->i;
            if (c->k <= 1) {
                c->i += kHalfi[c->i];
                if (c->k == 0) c->i += kHalfi[c->i];
            }
        }
        c->k = 0;
        *limit = lr_ + kNmaxlp[c->i];
        renormRange(limit);
    }

    void updateLps(WinZipJPEGContext* c, std::int32_t* limit) {
        lr_ += kLqp[c->i];
        *limit += kLqp[c->i];
        if (c->k >= 11) {
            if (c->i >= 48) {
                c->k = 0;
                *limit = lr_ + kNmaxlp[c->i];
                return;
            }
            int saved = 0;
            std::int32_t delta = *limit - lr_;
            if (delta >= kNmaxlp[c->i] / 2) {
                delta = kNmaxlp[c->i] - delta;
                if (delta <= kNmaxlp[c->i] / 4) doubleIndex(&c->i, &saved);
                doubleIndex(&c->i, &saved);
            } else {
                if (delta >= kNmaxlp[c->i] / 4) incrementIndex(&c->i, &saved);
                incrementIndex(&c->i, &saved);
            }
            if (c->i <= 0) {
                c->i = saved;
                c->mps ^= 1;
            }
            c->k = 0;
            *limit = lr_ + kNmaxlp[c->i];
        } else if (*limit < lr_) {
            *limit = lr_;
        }
    }

    static void incrementIndex(int* i, int* saved) {
        if (*i > 0) --*i;
        else ++*saved;
    }

    static void doubleIndex(int* i, int* saved) {
        if (*i > 0) *i -= kDbli[*i];
        else *saved += kDbli[*i];
    }

    std::vector<std::uint8_t> output_{0, 0};
    std::vector<std::uint8_t> serialized_;
    std::uint32_t x_ = 0;
    std::int32_t lr_ = 0x1001;
    enum { shift_ = 16 };
    bool flushed_ = false;
};

unsigned row(unsigned k) {
    static constexpr std::uint8_t values[64] = {
        0,0,1,2,1,0,0,1,2,3,4,3,2,1,0,0,1,2,3,4,5,6,5,4,3,2,1,0,0,1,2,3,
        4,5,6,7,7,6,5,4,3,2,1,2,3,4,5,6,7,7,6,5,4,3,4,5,6,7,7,6,5,6,7,7};
    return values[k];
}

unsigned column(unsigned k) {
    static constexpr std::uint8_t values[64] = {
        0,1,0,0,1,2,3,2,1,0,0,1,2,3,4,5,4,3,2,1,0,0,1,2,3,4,5,6,7,6,5,4,
        3,2,1,0,1,2,3,4,5,6,7,7,6,5,4,3,2,3,4,5,6,7,7,6,5,4,5,6,7,7,6,7};
    return values[k];
}

unsigned category(std::uint32_t value) {
    unsigned result = 0;
    while (value) {
        ++result;
        value >>= 1;
    }
    return result;
}

unsigned magnitude(int value) {
    return static_cast<unsigned>(value < 0 ? -std::int64_t(value) : value);
}

int sum(unsigned k, const WinZipJPEGBlock& block) {
    int result = 0;
    for (unsigned i = 0; i < 64; ++i)
        if (i != k && row(i) >= row(k) && column(i) >= column(k))
            result += static_cast<int>(magnitude(block.c[i]));
    return result;
}

unsigned zigzag(unsigned r, unsigned c) {
    static constexpr std::uint8_t table[8][8] = {
        {0,1,5,6,14,15,27,28},{2,4,7,13,16,26,29,42},
        {3,8,12,17,25,30,41,43},{9,11,18,24,31,40,44,53},
        {10,19,23,32,39,45,52,54},{20,22,33,38,46,51,55,60},
        {21,34,37,47,50,56,59,61},{35,36,48,49,57,58,62,63}};
    return r < 8 && c < 8 ? table[r][c] : 0;
}

int average(unsigned k, const WinZipJPEGBlock& north, const WinZipJPEGBlock& west,
             const WinZipJPEGQuantizationTable& q) {
    const unsigned r = row(k), c = column(k);
    if (k <= 2) return (magnitude(north.c[k]) + magnitude(west.c[k]) + 1) / 2;
    std::uint64_t total = 0;
    unsigned count = 1;
    auto add = [&](unsigned i) {
        total += (std::uint64_t(magnitude(north.c[i])) + magnitude(west.c[i])) *
                 q.c[i] / q.c[k];
        ++count;
    };
    if (r) add(zigzag(r - 1, c));
    if (c) add(zigzag(r, c - 1));
    if (r && c && k != 4) add(zigzag(r - 1, c - 1));
    total += magnitude(north.c[k]) + magnitude(west.c[k]);
    return static_cast<int>((total + count) / (2 * count));
}

int bdr(unsigned k, const WinZipJPEGBlock& current, const WinZipJPEGBlock& north,
         const WinZipJPEGBlock& west, const WinZipJPEGQuantizationTable& q) {
    if (row(k) == 0) {
        const unsigned below = zigzag(1, column(k));
        return north.c[k] - (north.c[below] + current.c[below]) * q.c[below] / q.c[k];
    }
    const unsigned right = zigzag(row(k), 1);
    return west.c[k] - (west.c[right] + current.c[right]) * q.c[right] / q.c[k];
}

bool encodeBinarization(LogEncoder* coder, WinZipJPEGContext* magnitudeContexts,
                        WinZipJPEGContext* remainderContexts, unsigned value,
                        unsigned maxBits, unsigned cap) {
    unsigned ones = value ? category(value) : 0;
    if (ones > maxBits) return false;
    for (unsigned i = 0; i < ones; ++i)
        if (!coder->bit(&magnitudeContexts[std::min(i, cap - 1)], 1)) return false;
    if (ones < maxBits &&
        !coder->bit(&magnitudeContexts[std::min(ones, cap - 1)], 0)) return false;
    if (ones > 1) {
        unsigned remainder = value - (1u << (ones - 1));
        for (int i = int(ones) - 2; i >= 0; --i)
            if (!coder->bit(&remainderContexts[i], (remainder >> i) & 1u)) return false;
    }
    return true;
}

bool encodeBlock(LogEncoder* coder, Models* models, unsigned component,
                 const WinZipJPEGBlock& current, const WinZipJPEGBlock* northBlock,
                 const WinZipJPEGBlock* westBlock,
                 const WinZipJPEGQuantizationTable& q) {
    static const WinZipJPEGBlock zeroBlock{};
    const WinZipJPEGBlock& north = northBlock ? *northBlock : zeroBlock;
    const WinZipJPEGBlock& west = westBlock ? *westBlock : zeroBlock;
    const unsigned eob = current.eob;
    const int eobAverage = !northBlock && !westBlock ? 0 :
        !northBlock ? sum(0, west) : !westBlock ? sum(0, north) :
        (sum(0, north) + sum(0, west) + 1) / 2;
    const unsigned eobContext = std::min(category(eobAverage), 12u);
    unsigned node = 0;
    for (int shift = 5; shift >= 0; --shift) {
        const unsigned bit = (eob >> shift) & 1u;
        if (!coder->bit(&models->Eob(component, eobContext, node), bit)) return false;
        node = (node << 1) + 1u + bit;
    }

    static const std::uint8_t signIndex[64] = {
        0,0,1,2,3,4,5,6,7,8,9,10,0,11,12,13,14,0,0,15,16,17,18,0,0,0,19,20,
        21,22,0,0,0,0,23,24,25,0,0,0,0,0,26,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,0};
    for (int k = int(eob); k >= 1; --k) {
        const int val = current.c[k];
        const int value1 = row(unsigned(k)) == 0 || column(unsigned(k)) == 0
            ? std::abs(bdr(unsigned(k), current, north, west, q))
            : average(unsigned(k), north, west, q);
        const int value2 = sum(unsigned(k), current);
        const unsigned context1 = std::min(category(unsigned(value1)), 2u);
        const unsigned context2 = std::min(category(unsigned(value2)), 5u);
        if (unsigned(k) != eob &&
            !coder->bit(&models->Zero(component, unsigned(k - 1), context1, context2),
                        val != 0)) return false;
        if (!val) continue;

        const unsigned absval = magnitude(val);
        const unsigned pivot1 = std::min(category(unsigned(value1)), 4u);
        const unsigned pivot2 = std::min(category(unsigned(value2)), 6u);
        auto& pivotContext = models->Pivot(component, unsigned(k - 1), pivot1, pivot2);
        if (!coder->bit(&pivotContext,
                        absval >= 2)) return false;

        if (absval >= 2) {
            unsigned n, value3;
            if (row(unsigned(k)) == 0) { n = 0; value3 = column(unsigned(k)) - 1; }
            else if (column(unsigned(k)) == 0) { n = 1; value3 = row(unsigned(k)) - 1; }
            else { n = 2; value3 = category(unsigned(k - 4)); }
            const unsigned mag1 = std::min(category(unsigned(value1)), 8u);
            const unsigned mag2 = std::min(category(unsigned(value2)), 8u);
            if (!encodeBinarization(coder, &models->AcMagnitude(component, n, mag1, mag2, 0),
                    &models->AcRemainder(component, n, value3, 0), absval - 2, 14, 9))
                return false;
        }

        bool fixedSign = true;
        unsigned predictedSign = 0;
        if (row(unsigned(k)) == 0 || column(unsigned(k)) == 0) {
            const int prediction = bdr(unsigned(k), current, north, west, q);
            if (prediction) { fixedSign = false; predictedSign = prediction < 0; }
        } else if (unsigned(k) == 4) {
            const int signs = (north.c[k] > 0) - (north.c[k] < 0) +
                              (west.c[k] > 0) - (west.c[k] < 0);
            if (signs) { fixedSign = false; predictedSign = signs < 0; }
        } else if (row(unsigned(k)) == 1) {
            if (north.c[k]) { fixedSign = false; predictedSign = north.c[k] < 0; }
        } else if (column(unsigned(k)) == 1) {
            if (west.c[k]) { fixedSign = false; predictedSign = west.c[k] < 0; }
        }
        const unsigned sign = val < 0;
        if (fixedSign) {
            if (!coder->bit(&models->fixed, sign)) return false;
        } else {
            const unsigned signContext = std::min(category(absval) / 2, 2u);
            if (!coder->bit(&models->AcSign(component, signIndex[k], signContext,
                                             predictedSign), sign)) return false;
        }
    }

    int predicted = 0;
    auto dcPredict = [&](const WinZipJPEGBlock& neighbor, unsigned ac) {
        const int64_t t = int64_t(neighbor.c[0]) * 10000 -
            int64_t(11038) * q.c[ac] * (int(neighbor.c[ac]) + int(current.c[ac])) / q.c[0];
        return static_cast<int>((t < 0 ? t - 5000 : t + 5000) / 10000);
    };
    if (northBlock && westBlock) {
        const int p0 = dcPredict(north, 2);
        const int p1 = dcPredict(west, 1);
        int d0 = 0, d1 = 0;
        for (unsigned i = 1; i < 8; ++i) {
            const unsigned ni = zigzag(i, 0), wi = zigzag(0, i);
            d0 += std::abs(int(north.c[ni]) - int(current.c[ni]));
            d1 += std::abs(int(west.c[wi]) - int(current.c[wi]));
        }
        if (d0 > d1) {
            const std::int64_t w = std::int64_t(1) << std::min(d0 - d1, 31);
            predicted = static_cast<int>((w * p1 + p0) / (1 + w));
        } else {
            const std::int64_t w = std::int64_t(1) << std::min(d1 - d0, 31);
            predicted = static_cast<int>((w * p0 + p1) / (1 + w));
        }
    } else if (northBlock) predicted = dcPredict(north, 2);
    else if (westBlock) predicted = dcPredict(west, 1);

    const int residual = int(current.c[0]) - predicted;
    const unsigned absResidual = magnitude(residual);
    const unsigned dcContext = std::min(category(unsigned(sum(0, current))), 12u);
    if (!encodeBinarization(coder, &models->DcMagnitude(component, dcContext, 0),
            &models->DcRemainder(component, dcContext, 0), absResidual, 15, 10)) return false;
    if (absResidual) {
        if (!coder->bit(&models->DcSign(component, north.c[0] < predicted,
                                        west.c[0] < predicted, predicted < 0),
                        residual < 0)) return false;
    }
    return true;
}

class EntropyReader {
public:
    EntropyReader(const std::uint8_t* bytes, std::size_t size, std::size_t offset)
        : bytes_(bytes), size_(size), offset_(offset) {}

    bool bit(unsigned* value) {
        if (!bits_) {
            if (!byte(&current_)) return false;
            bits_ = 8;
        }
        *value = (current_ >> (--bits_)) & 1u;
        return true;
    }

    bool bits(unsigned count, unsigned* value) {
        unsigned result = 0;
        for (unsigned i = 0; i < count; ++i) {
            unsigned next = 0;
            if (!bit(&next)) return false;
            result = (result << 1) | next;
        }
        *value = result;
        return true;
    }

    bool restart(unsigned expected) {
        bits_ = 0;
        if (offset_ + 2 > size_ || bytes_[offset_] != 0xff ||
            bytes_[offset_ + 1] != 0xd0 + (expected & 7u)) return false;
        offset_ += 2;
        return true;
    }

    std::size_t offset() const { return offset_; }
    void align() { bits_ = 0; }

private:
    bool byte(std::uint8_t* value) {
        if (offset_ >= size_) return false;
        const std::uint8_t first = bytes_[offset_++];
        if (first != 0xff) {
            *value = first;
            return true;
        }
        if (offset_ >= size_ || bytes_[offset_++] != 0x00) return false;
        *value = 0xff;
        return true;
    }

    const std::uint8_t* bytes_;
    std::size_t size_;
    std::size_t offset_;
    std::uint8_t current_ = 0;
    unsigned bits_ = 0;
};

bool findScanHeader(const std::uint8_t* data, std::size_t size,
                    std::size_t* imageStart, std::size_t* scanOffset) {
    const std::size_t searchLimit = std::min<std::size_t>(size, 128);
    std::size_t soi = 0;
    while (soi + 1 < searchLimit &&
           !(data[soi] == 0xff && data[soi + 1] == 0xd8)) ++soi;
    if (soi + 1 >= searchLimit) return false;

    std::size_t offset = soi + 2;
    while (offset < size) {
        if (data[offset++] != 0xff) return false;
        while (offset < size && data[offset] == 0xff) ++offset;
        if (offset >= size) return false;
        const std::uint8_t marker = data[offset++];
        if (marker == 0xd9 || marker == 0xd8 ||
            (marker >= 0xd0 && marker <= 0xd7)) return false;
        if (offset + 2 > size) return false;
        const std::size_t length = (std::size_t(data[offset]) << 8) | data[offset + 1];
        if (length < 2 || length > size - offset) return false;
        offset += length;
        if (marker == 0xda) {
            *imageStart = soi;
            *scanOffset = offset;
            return true;
        }
    }
    return false;
}

int receiveExtend(unsigned size, unsigned value) {
    if (!size) return 0;
    const unsigned threshold = 1u << (size - 1);
    return value < threshold ? int(value) - int((1u << size) - 1u) : int(value);
}

bool readHuffman(EntropyReader* reader, const WinZipJPEGHuffmanTable* table,
                 unsigned* symbol) {
    unsigned code = 0;
    for (unsigned length = 1; length <= 16; ++length) {
        unsigned next = 0;
        if (!reader->bit(&next)) return false;
        code = (code << 1) | next;
        for (unsigned candidate = 0; candidate < 256; ++candidate) {
            const auto& entry = table->codes[candidate];
            if (entry.length == length && entry.code == code) {
                *symbol = candidate;
                return true;
            }
        }
    }
    return false;
}

bool readBlock(EntropyReader* reader, const WinZipJPEGScanComponent& scan,
               int* predictor, WinZipJPEGBlock* block) {
    unsigned category = 0;
    unsigned value = 0;
    if (!readHuffman(reader, scan.dctable, &category) || category > 16 ||
        !reader->bits(category, &value)) return false;
    *predictor += receiveExtend(category, value);
    block->c[0] = static_cast<std::int16_t>(*predictor);
    block->eob = 0;
    unsigned k = 1;
    while (k < 64) {
        unsigned runSize = 0;
        if (!readHuffman(reader, scan.actable, &runSize)) return false;
        const unsigned run = runSize >> 4;
        const unsigned size = runSize & 15u;
        if (!size) {
            if (!run) break;
            if (run != 15 || k + 16 > 64) return false;
            k += 16;
            continue;
        }
        if (size > 16 || k + run >= 64) return false;
        k += run;
        if (!reader->bits(size, &value)) return false;
        const int coefficient = receiveExtend(size, value);
        block->c[k] = static_cast<std::int16_t>(coefficient);
        block->eob = static_cast<std::uint8_t>(k++);
    }
    return true;
}

bool parseJpeg(const std::uint8_t* data, std::size_t size, JpegInput* jpeg) {
    if (!data || !size || !jpeg) return false;
    std::size_t imageStart = 0;
    if (!findScanHeader(data, size, &imageStart, &jpeg->scanOffset)) return false;
    if (ParseWinZipJPEGMetadata(&jpeg->metadata, data + imageStart,
                                jpeg->scanOffset - imageStart) !=
        WinZipJPEGMetadataFoundStartOfScan || jpeg->metadata.bits > 12) return false;
    jpeg->markers.assign(data, data + jpeg->scanOffset);
    jpeg->components.resize(jpeg->metadata.numcomponents);
    for (unsigned i = 0; i < jpeg->metadata.numcomponents; ++i) {
        auto& component = jpeg->components[i];
        component.h = jpeg->metadata.components[i].horizontalfactor;
        component.v = jpeg->metadata.components[i].verticalfactor;
        component.columns = jpeg->metadata.horizontalmcus * component.h;
        component.rows = jpeg->metadata.verticalmcus * component.v;
        const std::size_t count = std::size_t(component.columns) * component.rows;
        if (!component.columns || count > std::numeric_limits<std::size_t>::max() /
                                           sizeof(WinZipJPEGBlock)) return false;
        component.blocks.resize(count);
    }

    EntropyReader reader(data, size, jpeg->scanOffset);
    std::array<int, 4> predictors{};
    unsigned restartIndex = 0;
    unsigned mcu = 0;
    const unsigned columns = jpeg->metadata.horizontalmcus;
    const unsigned rows = jpeg->metadata.verticalmcus;
    const bool interleaved = jpeg->metadata.numscancomponents > 1;
    const unsigned scanColumns = interleaved ? columns :
        (jpeg->metadata.width * jpeg->metadata.scancomponents[0].component->horizontalfactor +
         8 * jpeg->metadata.maxhorizontalfactor - 1) /
            (8 * jpeg->metadata.maxhorizontalfactor);
    const unsigned scanRows = interleaved ? rows :
        (jpeg->metadata.height * jpeg->metadata.scancomponents[0].component->verticalfactor +
         8 * jpeg->metadata.maxverticalfactor - 1) /
            (8 * jpeg->metadata.maxverticalfactor);

    for (unsigned my = 0; my < scanRows; ++my) {
        for (unsigned mx = 0; mx < scanColumns; ++mx) {
            if (jpeg->metadata.restartinterval && mcu &&
                mcu % jpeg->metadata.restartinterval == 0) {
                if (!reader.restart(restartIndex++)) return false;
                predictors.fill(0);
            }
            for (unsigned si = 0; si < jpeg->metadata.numscancomponents; ++si) {
                const auto& scan = jpeg->metadata.scancomponents[si];
                const unsigned ci = unsigned(scan.component - jpeg->metadata.components);
                auto& component = jpeg->components[ci];
                const unsigned hcount = interleaved ? component.h : 1;
                const unsigned vcount = interleaved ? component.v : 1;
                for (unsigned vy = 0; vy < vcount; ++vy) {
                    for (unsigned hx = 0; hx < hcount; ++hx) {
                        const unsigned bx = interleaved ? mx * component.h + hx : mx;
                        const unsigned by = interleaved ? my * component.v + vy : my;
                        if (bx >= component.columns || by >= component.rows ||
                            !readBlock(&reader, scan, &predictors[ci],
                                &component.blocks[bx + std::size_t(by) * component.columns]))
                            return false;
                    }
                }
            }
            ++mcu;
        }
    }
    reader.align();
    if (jpeg->metadata.restartinterval && reader.offset() < size &&
        data[reader.offset()] == 0xff) {
        if (!reader.restart(restartIndex)) return false;
    }
    jpeg->scanEndOffset = reader.offset();
    return true;
}

void append16(std::vector<std::uint8_t>* output, std::uint16_t value) {
    output->push_back(static_cast<std::uint8_t>(value));
    output->push_back(static_cast<std::uint8_t>(value >> 8));
}

void append32(std::vector<std::uint8_t>* output, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        output->push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

bool appendMetadataBundle(std::vector<std::uint8_t>* output,
                          const std::uint8_t* data, std::size_t size) {
    if (!size || size > 16u * 1024u * 1024u) return false;
    if (size >= 0xffffu) {
        append16(output, 0xffffu);
        append16(output, 0xffffu);
        append32(output, static_cast<std::uint32_t>(size));
        append32(output, 0);
    } else {
        append16(output, static_cast<std::uint16_t>(size));
        append16(output, 0);
    }
    output->insert(output->end(), data, data + size);
    return true;
}

bool encodeScan(const JpegInput& jpeg, std::vector<std::uint8_t>* output) {
    Models models;
    for (unsigned si = 0; si < jpeg.metadata.numscancomponents; ++si) {
        const unsigned ci = unsigned(jpeg.metadata.scancomponents[si].component -
                                     jpeg.metadata.components);
        const auto& component = jpeg.components[ci];
        const auto* quantization = jpeg.metadata.components[ci].quantizationtable;
        if (!quantization) return false;
        LogEncoder coder;
        for (unsigned y = 0; y < component.rows; ++y) {
            for (unsigned x = 0; x < component.columns; ++x) {
                const auto& block = component.blocks[x + std::size_t(y) * component.columns];
                const WinZipJPEGBlock* north = y ?
                    &component.blocks[x + std::size_t(y - 1) * component.columns] : nullptr;
                const WinZipJPEGBlock* west = x ?
                    &component.blocks[x - 1 + std::size_t(y) * component.columns] : nullptr;
                if (!encodeBlock(&coder, &models, ci, block, north, west, *quantization))
                    return false;
            }
        }
        coder.flush();
        const auto& bytes = coder.bytes();
        output->insert(output->end(), bytes.begin(), bytes.end());
    }
    return true;
}

}  // namespace

bool encodeWinZipJpeg(const std::uint8_t* input, std::size_t inputSize,
                      std::vector<std::uint8_t>* output) {
    if (!input || !inputSize || !output) return false;
    JpegInput jpeg;
    if (!parseJpeg(input, inputSize, &jpeg) || jpeg.metadata.bits > 12 ||
        jpeg.scanEndOffset > inputSize) return false;

    output->clear();
    output->reserve(inputSize);
    output->insert(output->end(), {4, 0x10, 1, 8});
    if (!appendMetadataBundle(output, jpeg.markers.data(), jpeg.markers.size()) ||
        !encodeScan(jpeg, output)) return false;

    if (jpeg.scanEndOffset == inputSize) {
        static constexpr std::uint8_t eoi[] = {0xff, 0xd9};
        return appendMetadataBundle(output, eoi, sizeof(eoi));
    }
    return appendMetadataBundle(output, input + jpeg.scanEndOffset,
                                inputSize - jpeg.scanEndOffset);
}

}  // namespace writer
}  // namespace peare
