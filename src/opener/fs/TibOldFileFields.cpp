#include "TibOldFileFields.h"

#include "../../common/compression/DeflateDecoder.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>

namespace peare {
namespace fs {
namespace {

struct Volume {
    ByteStorePtr store;
    std::uint64_t start;
    std::uint64_t size;
};

std::uint32_t readLe32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}

std::uint64_t readLe64(const std::uint8_t* p) {
    return std::uint64_t(readLe32(p)) | (std::uint64_t(readLe32(p + 4)) << 32);
}

bool readExact(const IByteStore& store, std::int64_t offset,
               std::uint8_t* data, int size) {
    int done = 0;
    while (done < size) {
        const int n = store.read(offset + done, data + done, size - done);
        if (n <= 0 || n > size - done) return false;
        done += n;
    }
    return true;
}

class Input final : public compression::InflateInput {
public:
    Input(const std::vector<Volume>& volumes, std::uint64_t length)
        : volumes_(volumes), length_(length), position_(0), cacheStart_(0),
          cacheSize_(0), streamStart_(0), streamActive_(false),
          streamLimitHit_(false), eofHit_(false), failed_(false) {}

    bool readByte(std::uint8_t* value) override {
        if (!value || failed_) return false;
        if (position_ >= length_) {
            if (streamActive_) eofHit_ = true;
            return false;
        }
        if (streamActive_ && position_ - streamStart_ >=
                std::numeric_limits<std::size_t>::max()) {
            streamLimitHit_ = true;
            return false;
        }
        if (position_ < cacheStart_ || position_ - cacheStart_ >= cacheSize_) {
            const std::size_t index = volumeAt(position_);
            const Volume& volume = volumes_[index];
            const std::uint64_t within = position_ - volume.start;
            const std::uint64_t available = volume.size - within;
            const int count = static_cast<int>(std::min<std::uint64_t>(available, sizeof(cache_)));
            const int got = volume.store->read(static_cast<std::int64_t>(within + 12), cache_, count);
            if (got <= 0 || got > count) {
                failed_ = true;
                return false;
            }
            cacheStart_ = position_;
            cacheSize_ = static_cast<std::size_t>(got);
        }
        *value = cache_[static_cast<std::size_t>(position_++ - cacheStart_)];
        return true;
    }

    std::size_t consumed() const override {
        return static_cast<std::size_t>(position_ - streamStart_);
    }

    bool rewind(std::size_t count) override {
        if (count > position_ || (streamActive_ && count > position_ - streamStart_))
            return false;
        position_ -= count;
        return true;
    }

    bool failed() const { return failed_; }
    bool atEnd() const { return position_ == length_; }
    std::uint64_t offset() const { return position_; }
    void beginStream() {
        streamStart_ = position_;
        streamActive_ = true;
        streamLimitHit_ = false;
        eofHit_ = false;
    }
    void endStream() { streamActive_ = false; }
    bool streamLimitHit() const { return streamLimitHit_; }
    bool eofHit() const { return eofHit_; }

    bool seek(std::size_t volumeIndex, std::uint64_t volumeOffset) {
        if (volumeIndex >= volumes_.size() || volumeOffset < 12 ||
            volumeOffset - 12 >= volumes_[volumeIndex].size) return false;
        position_ = volumes_[volumeIndex].start + (volumeOffset - 12);
        cacheSize_ = 0;
        return true;
    }

    bool position(std::uint64_t sectionStart, std::uint64_t sectionBase,
                  TibOldFieldPosition* result) const {
        const std::uint64_t relative = position_ - sectionStart;
        if (relative > std::numeric_limits<std::uint64_t>::max() - sectionBase) return false;
        result->logicalOffset = sectionBase + relative;
        if (position_ == length_) {
            result->volumeIndex = volumes_.size() - 1;
            result->volumeOffset = volumes_.back().size + 12;
        } else {
            result->volumeIndex = volumeAt(position_);
            result->volumeOffset = position_ - volumes_[result->volumeIndex].start + 12;
        }
        return true;
    }

    bool nextSection(TibOldSkippedRange* skipped) {
        const std::size_t lastVolume = volumeAt(position_ - 1);
        const Volume& volume = volumes_[lastVolume];
        skipped->volumeIndex = lastVolume;
        skipped->volumeOffset = position_ - volume.start + 12;
        skipped->length = volume.size - (position_ - volume.start);
        if (lastVolume + 1 == volumes_.size()) {
            position_ = length_;
            return false;
        }
        position_ = volumes_[lastVolume + 1].start;
        return true;
    }

private:
    std::size_t volumeAt(std::uint64_t position) const {
        std::size_t lo = 0;
        std::size_t hi = volumes_.size();
        while (lo < hi) {
            const std::size_t mid = lo + (hi - lo) / 2;
            if (volumes_[mid].start <= position) lo = mid + 1;
            else hi = mid;
        }
        return lo - 1;
    }

    const std::vector<Volume>& volumes_;
    std::uint64_t length_;
    std::uint64_t position_;
    std::uint64_t cacheStart_;
    std::size_t cacheSize_;
    std::uint64_t streamStart_;
    bool streamActive_;
    bool streamLimitHit_;
    bool eofHit_;
    bool failed_;
    std::uint8_t cache_[4096];
};

class FieldOutput {
public:
    FieldOutput(TibOldFieldSink& sink, std::uint64_t maximum)
        : sink_(sink), maximum_(maximum), size_(0), used_(0), a_(1), b_(0),
          crc_(0xffffffffU), limitHit_(false), stopped_(false) {}

    bool put(std::uint8_t value) {
        if (size_ == maximum_) {
            limitHit_ = true;
            return false;
        }
        ++size_;
        a_ = (a_ + value) % 65521U;
        b_ = (b_ + a_) % 65521U;
        crc_ ^= value;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc_ = (crc_ >> 1) ^ (0xedb88320U & (0U - (crc_ & 1U)));
        buffer_[used_++] = value;
        return used_ != sizeof(buffer_) || flush();
    }

    bool flush() {
        if (used_ && !sink_.fieldData(buffer_, used_)) {
            stopped_ = true;
            return false;
        }
        used_ = 0;
        return true;
    }

    bool matches(std::uint32_t expected) const {
        return ((b_ << 16) | a_) == expected || (crc_ ^ 0xffffffffU) == expected;
    }
    bool limitHit() const { return limitHit_; }
    bool stopped() const { return stopped_; }
    std::uint64_t size() const { return size_; }

private:
    TibOldFieldSink& sink_;
    std::uint64_t maximum_;
    std::uint64_t size_;
    std::size_t used_;
    std::uint32_t a_, b_, crc_;
    bool limitHit_, stopped_;
    std::uint8_t buffer_[4096];
};

class FieldGuard {
public:
    FieldGuard(TibOldFieldSink& sink, TibOldField& field)
        : sink_(sink), field_(field), verified_(false) {}
    ~FieldGuard() { sink_.endField(field_, verified_); }
    void verified() { verified_ = true; }
private:
    TibOldFieldSink& sink_;
    TibOldField& field_;
    bool verified_;
};

TibOldFieldStatus prepareVolumes(const std::vector<ByteStorePtr>& stores,
                                 const TibOldFieldLimits& limits,
                                 std::vector<Volume>* volumes,
                                 std::uint64_t* length) {
    if (stores.empty()) return TibOldFieldStatus::InvalidArgument;
    if (stores.size() > limits.maxVolumes) return TibOldFieldStatus::LimitExceeded;
    const std::uint8_t magic[4] = {0xb4, 0x6e, 0x68, 0x44};
    volumes->reserve(stores.size());
    *length = 0;
    std::uint32_t previous = 0;
    std::uint32_t archiveId = 0;
    for (std::size_t i = 0; i < stores.size(); ++i) {
        if (!stores[i] || stores[i]->capacity() < 12 || !stores[i]->validate())
            return TibOldFieldStatus::InvalidVolume;
        std::uint8_t header[12];
        if (!readExact(*stores[i], 0, header, sizeof(header))) return TibOldFieldStatus::ReadError;
        if (!std::equal(magic, magic + sizeof(magic), header)) return TibOldFieldStatus::InvalidVolume;
        const std::uint32_t currentId = readLe32(header + 4);
        if (i == 0) archiveId = currentId;
        else if (currentId != archiveId) return TibOldFieldStatus::InvalidVolume;
        const std::uint32_t sequence = readLe32(header + 8);
        if ((i == 0 && sequence != 1) ||
            (i != 0 && (previous == std::numeric_limits<std::uint32_t>::max() ||
                        sequence != previous + 1))) return TibOldFieldStatus::InvalidSequence;
        previous = sequence;
        const std::uint64_t size = static_cast<std::uint64_t>(stores[i]->capacity() - 12);
        Volume volume = {stores[i], *length, size};
        volumes->push_back(volume);
    }
    const ByteStorePtr& last = stores.back();
    const std::int64_t physicalSize = last->capacity();
    std::uint8_t marker[4] = {};
    if (physicalSize >= 16 && !readExact(*last, physicalSize - 4, marker, sizeof(marker)))
        return TibOldFieldStatus::ReadError;
    const std::uint8_t footerMagic[4] = {0xb4, 0x31, 0x96, 0x17};
    if (physicalSize >= 16 && std::equal(footerMagic, footerMagic + sizeof(footerMagic), marker)) {
        if (stores.size() > 1000000 || stores.size() >
            static_cast<std::uint64_t>(physicalSize - 16) / 8)
            return TibOldFieldStatus::InvalidVolume;
        const std::int64_t tableStart = physicalSize - 4 - static_cast<std::int64_t>(stores.size() * 8);
        for (std::size_t i = 0; i < stores.size(); ++i) {
            std::uint8_t entry[8];
            if (!readExact(*last, tableStart + static_cast<std::int64_t>(i * 8), entry, sizeof(entry)))
                return TibOldFieldStatus::ReadError;
            const std::uint64_t size = readLe64(entry);
            const std::int64_t end = i + 1 == stores.size() ? tableStart : stores[i]->capacity();
            if (end < 12 || size > static_cast<std::uint64_t>(end - 12))
                return TibOldFieldStatus::InvalidVolume;
            (*volumes)[i].size = size;
        }
    }
    for (Volume& volume : *volumes) {
        if (*length > limits.maxInputBytes || volume.size > limits.maxInputBytes - *length ||
            volume.size > std::numeric_limits<std::uint64_t>::max() - *length)
            return TibOldFieldStatus::LimitExceeded;
        volume.start = *length;
        *length += volume.size;
    }
    return TibOldFieldStatus::Ok;
}

TibOldFieldStatus readField(Input& input, std::uint64_t sectionStart,
                           std::uint64_t sectionBase, std::uint64_t maximum,
                           TibOldFieldSink& sink, TibOldField* result) {
    TibOldField field;
    if (!input.position(sectionStart, sectionBase, &field.start))
        return TibOldFieldStatus::LimitExceeded;
    if (!input.readByte(&field.tag)) return TibOldFieldStatus::ReadError;
    if (!sink.beginField(field)) return TibOldFieldStatus::CallbackStopped;
    FieldGuard guard(sink, field);
    const std::uint64_t compressedStart = input.offset();
    input.beginStream();
    FieldOutput output(sink, maximum);
    std::string inflateError;
    std::size_t decodedSize = 0;
    if (!compression::inflateRawStreaming(input, [&output](std::uint8_t value) {
            return output.put(value);
        }, &decodedSize, 32768, &inflateError)) {
        if (input.failed()) return TibOldFieldStatus::ReadError;
        if (input.streamLimitHit()) return TibOldFieldStatus::LimitExceeded;
        if (output.limitHit()) return TibOldFieldStatus::LimitExceeded;
        if (output.stopped()) return TibOldFieldStatus::CallbackStopped;
        if (inflateError == "Deflate output exceeds limit" ||
            inflateError == "Deflate output size overflow")
            return TibOldFieldStatus::LimitExceeded;
        if (inflateError == "Deflate allocation failed")
            return TibOldFieldStatus::AllocationFailure;
        if (input.eofHit() && (inflateError == "truncated Deflate stream" ||
                               inflateError == "truncated Deflate stored block"))
            return TibOldFieldStatus::TruncatedField;
        return TibOldFieldStatus::InvalidDeflate;
    }
    input.endStream();
    if (input.failed()) return TibOldFieldStatus::ReadError;
    if (!output.flush()) return TibOldFieldStatus::CallbackStopped;
    field.compressedBytes = input.offset() - compressedStart;
    field.decodedBytes = decodedSize;
    std::uint8_t checksum[4];
    for (std::size_t i = 0; i < sizeof(checksum); ++i) {
        if (!input.readByte(&checksum[i]))
            return input.failed() ? TibOldFieldStatus::ReadError : TibOldFieldStatus::TruncatedChecksum;
    }
    if (!output.matches(readLe32(checksum)))
        return TibOldFieldStatus::ChecksumMismatch;
    if (!input.position(sectionStart, sectionBase, &field.end))
        return TibOldFieldStatus::LimitExceeded;
    *result = field;
    guard.verified();
    return TibOldFieldStatus::Ok;
}

TibOldFieldStatus scan(const std::vector<ByteStorePtr>& stores,
                       const TibOldFieldLimits& limits,
                       TibOldFieldSink& sink) {
    std::vector<Volume> volumes;
    std::uint64_t length = 0;
    const TibOldFieldStatus prepared = prepareVolumes(stores, limits, &volumes, &length);
    if (prepared != TibOldFieldStatus::Ok) return prepared;
    Input input(volumes, length);
    std::uint64_t total = 0;
    std::size_t fieldCount = 0;
    std::uint64_t sectionStart = 0;
    std::uint64_t sectionBase = 0;
    bool skippedData = false;
    while (!input.atEnd()) {
        if (fieldCount >= limits.maxFields) return TibOldFieldStatus::LimitExceeded;
        TibOldField field;
        const TibOldFieldStatus status = readField(input, sectionStart, sectionBase,
            std::min(limits.maxFieldBytes, limits.maxTotalBytes - total), sink, &field);
        if (status != TibOldFieldStatus::Ok) return status;
        total += field.decodedBytes;
        ++fieldCount;
        if (field.tag == 0x68) {
            TibOldSkippedRange skipped;
            const bool hasNextSection = input.nextSection(&skipped);
            if (skipped.length != 0) {
                skippedData = true;
                sink.skippedSectionTail(skipped);
            }
            if (hasNextSection) {
                if (field.end.logicalOffset > std::numeric_limits<std::uint64_t>::max() - 12)
                    return TibOldFieldStatus::LimitExceeded;
                sectionBase = field.end.logicalOffset + 12;
                sectionStart = input.offset();
            }
        }
    }
    return skippedData ? TibOldFieldStatus::OkWithSkippedData : TibOldFieldStatus::Ok;
}

} // namespace

TibOldFieldStatus scanTibOldFileFields(const std::vector<ByteStorePtr>& volumes,
                                       const TibOldFieldLimits& limits,
                                       TibOldFieldSink* sink) {
    if (!sink) return TibOldFieldStatus::InvalidArgument;
    try {
        return scan(volumes, limits, *sink);
    } catch (const std::bad_alloc&) {
        return TibOldFieldStatus::AllocationFailure;
    } catch (const std::length_error&) {
        return TibOldFieldStatus::LimitExceeded;
    }
}

TibOldFieldStatus readTibOldFileFieldAt(const std::vector<ByteStorePtr>& stores,
                                        const TibOldFieldPosition& position,
                                        const TibOldFieldLimits& limits,
                                        TibOldFieldSink* sink) {
    if (!sink) return TibOldFieldStatus::InvalidArgument;
    try {
        std::vector<Volume> volumes;
        std::uint64_t length = 0;
        const TibOldFieldStatus prepared = prepareVolumes(stores, limits, &volumes, &length);
        if (prepared != TibOldFieldStatus::Ok) return prepared;
        if (limits.maxFields == 0) return TibOldFieldStatus::LimitExceeded;
        Input input(volumes, length);
        if (!input.seek(position.volumeIndex, position.volumeOffset))
            return TibOldFieldStatus::InvalidArgument;
        TibOldField field;
        return readField(input, input.offset(), position.logicalOffset,
                         std::min(limits.maxFieldBytes, limits.maxTotalBytes), *sink, &field);
    } catch (const std::bad_alloc&) {
        return TibOldFieldStatus::AllocationFailure;
    } catch (const std::length_error&) {
        return TibOldFieldStatus::LimitExceeded;
    }
}

} // namespace fs
} // namespace peare
