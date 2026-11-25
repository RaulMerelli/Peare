#include "TibxItemMetadata.h"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace peare {
namespace fs {
namespace {

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

class Input {
public:
    explicit Input(ByteStorePtr source) : source_(std::move(source)), capacity_(source_->capacity()),
        pos_(0), begin_(-1), size_(0) {}
    bool byte(std::uint8_t* out) {
        if (!out || pos_ >= capacity_ || source_->capacity() != capacity_) return false;
        if (begin_ < 0 || pos_ < begin_ || pos_ - begin_ >= size_) {
            begin_ = pos_;
            const int wanted = static_cast<int>(std::min<std::int64_t>(buffer_.size(), capacity_ - pos_));
            size_ = source_->read(pos_, buffer_.data(), wanted);
            if (size_ <= 0 || size_ > wanted) return false;
        }
        *out = buffer_[static_cast<std::size_t>(pos_++ - begin_)];
        return true;
    }
    std::int64_t remaining() const { return capacity_ - pos_; }
private:
    ByteStorePtr source_;
    std::int64_t capacity_, pos_, begin_;
    int size_;
    std::array<std::uint8_t, 4096> buffer_;
};

} // namespace

bool readTibxItemMetadata(const ByteStorePtr& source, TibxMetadataDomain domain,
                          std::vector<TibxMetadataField>* fields, std::string* error) {
    (void)domain;
    if (error) error->clear();
    if (!fields) return fail(error, "Missing TIBX metadata destination");
    fields->clear();
    if (!source || !source->validate() || source->capacity() < 0)
        return fail(error, "Invalid TIBX metadata source");
    try {
        Input input(source);
        std::vector<TibxMetadataField> result;
        bool haveTag = false;
        std::uint16_t previous = 0;
        while (input.remaining()) {
            std::uint8_t low = 0, high = 0, first = 0;
            if (!input.byte(&low) || !input.byte(&high) || !input.byte(&first))
                return fail(error, "Truncated TIBX metadata header");
            const std::uint16_t tag = std::uint16_t(low) | (std::uint16_t(high) << 8);
            if (haveTag && tag <= previous) return fail(error, "Unordered or duplicate TIBX metadata tag");
            previous = tag;
            haveTag = true;
            std::size_t length = first;
            if (first & 0x80U) {
                std::uint8_t second = 0;
                if (!input.byte(&second)) return fail(error, "Truncated TIBX metadata length");
                length = (std::size_t(first & 0x7fU) << 8) | second;
            }
            if (length > static_cast<std::uint64_t>(input.remaining()))
                return fail(error, "Truncated TIBX metadata value");
            TibxMetadataField field;
            field.tag = tag;
            field.bytes.resize(length);
            for (std::size_t i = 0; i < length; ++i)
                if (!input.byte(&field.bytes[i])) return fail(error, "Cannot read TIBX metadata value");
            result.push_back(std::move(field));
        }
        if (!source->validate()) return fail(error, "TIBX metadata source became invalid");
        fields->swap(result);
        return true;
    } catch (const std::bad_alloc&) { return fail(error, "TIBX metadata allocation failed"); }
      catch (const std::length_error&) { return fail(error, "TIBX metadata size is not representable"); }
}

bool getTibxItemMetadataField(const std::vector<TibxMetadataField>& fields,
                              std::uint16_t tag, std::size_t width,
                              std::vector<std::uint8_t>* value, bool* found,
                              std::string* error) {
    if (error) error->clear();
    if (!value || !found) return fail(error, "Missing TIBX metadata getter destination");
    *found = false;
    for (const TibxMetadataField& field : fields) {
        if (field.tag != tag) continue;
        if (field.bytes.size() > width) return fail(error, "TIBX metadata value exceeds field width");
        try {
            std::vector<std::uint8_t> result(field.bytes);
            result.resize(width, 0);
            value->swap(result);
            *found = true;
            return true;
        } catch (const std::bad_alloc&) { return fail(error, "TIBX metadata allocation failed"); }
          catch (const std::length_error&) { return fail(error, "TIBX metadata size is not representable"); }
    }
    value->clear();
    return true;
}

bool finishTibxItemMetadata(const std::vector<TibxMetadataField>& fields,
                            const std::vector<std::uint16_t>& handled,
                            std::string* error) {
    if (error) error->clear();
    for (const TibxMetadataField& field : fields) {
        if (!(field.tag & 0x8000U) &&
            std::find(handled.begin(), handled.end(), field.tag) == handled.end())
            return fail(error, "Unsupported required TIBX metadata tag");
    }
    return true;
}

} // namespace fs
} // namespace peare
