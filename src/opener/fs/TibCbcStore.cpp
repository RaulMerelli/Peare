#include "TibCbcStore.h"

#include "../../common/compression/AesInitialization.h"
#include "../compression/third_party/lzma/Aes.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <new>
#include <utility>

namespace peare {
namespace fs {
namespace {

bool readExact(const ByteStorePtr& source, std::int64_t pos, std::uint8_t* dst, int count) {
    int done = 0;
    while (done < count) {
        const int n = source->read(pos + done, dst + done, count - done);
        if (n <= 0 || n > count - done) return false;
        done += n;
    }
    return true;
}

struct AesReadState {
    alignas(16) UInt32 schedule[AES_NUM_IVMRK_WORDS] = {};
    alignas(16) std::array<std::uint8_t, 4096> data = {};
    ~AesReadState() {
        std::memset(schedule, 0, sizeof(schedule));
        std::fill(data.begin(), data.end(), 0);
    }
};

class TibCbcStore final : public IByteStore {
public:
    TibCbcStore(ByteStorePtr source, const std::vector<std::uint8_t>& key)
        : source_(std::move(source)), size_(source_->capacity()), keySize_(key.size()), key_{} {
        std::copy(key.begin(), key.end(), key_.begin());
    }

    ~TibCbcStore() override { std::fill(key_.begin(), key_.end(), 0); }

    std::int64_t capacity() const override { return size_; }
    bool validate() const override { return source_->validate() && source_->capacity() == size_; }
    bool cheapRandomAccess() const override { return source_->cheapRandomAccess(); }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!dst || pos < 0 || count <= 0 || pos >= size_) return 0;
        if (pos < AES_BLOCK_SIZE) return -1;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!validate()) return -1;
        const int amount = static_cast<int>(std::min<std::int64_t>(count, size_ - pos));
        const std::int64_t end = pos + amount;
        const std::int64_t lastBlock = (end - 1) - (end - 1) % AES_BLOCK_SIZE;
        if (lastBlock > size_ - AES_BLOCK_SIZE) return -1;
        std::int64_t block = pos - pos % AES_BLOCK_SIZE;
        std::uint8_t iv[AES_BLOCK_SIZE];
        if (!readExact(source_, block - AES_BLOCK_SIZE, iv, AES_BLOCK_SIZE)) return -1;
        compression::initializeAesTables();
        AesReadState state;
        Aes_SetKey_Dec(state.schedule + 4, key_.data(), static_cast<unsigned>(keySize_));
        AesCbc_Init(state.schedule, iv);
        int produced = 0;
        while (block < end) {
            const std::int64_t remaining = end - block;
            const std::int64_t blocks = remaining / AES_BLOCK_SIZE +
                                        (remaining % AES_BLOCK_SIZE != 0 ? 1 : 0);
            const int chunk = static_cast<int>(std::min<std::int64_t>(
                blocks, static_cast<std::int64_t>(state.data.size() / AES_BLOCK_SIZE)) * AES_BLOCK_SIZE);
            if (!readExact(source_, block, state.data.data(), chunk)) return -1;
            AesCbc_Decode(state.schedule, state.data.data(), std::size_t(chunk / AES_BLOCK_SIZE));
            const std::int64_t start = std::max(pos, block);
            const std::int64_t stop = std::min(end, block + chunk);
            const int size = static_cast<int>(stop - start);
            std::memcpy(dst + produced, state.data.data() + (start - block), size);
            produced += size;
            block += chunk;
        }
        return produced;
    }

private:
    ByteStorePtr source_;
    std::int64_t size_;
    std::size_t keySize_;
    std::array<std::uint8_t, 32> key_;
    mutable std::mutex mutex_;
};

} // namespace

ByteStorePtr openTibCbcStore(const ByteStorePtr& source,
                              const std::vector<std::uint8_t>& key,
                              std::string* error) {
    if (error) error->clear();
    if (!source || !source->validate() || source->capacity() < AES_BLOCK_SIZE) {
        if (error) *error = "Invalid TIB CBC source";
        return ByteStorePtr();
    }
    if (key.size() != 16 && key.size() != 24 && key.size() != 32) {
        if (error) *error = "Invalid TIB CBC key size";
        return ByteStorePtr();
    }
    try {
        return std::make_shared<TibCbcStore>(source, key);
    } catch (const std::bad_alloc&) {
        if (error) *error = "TIB CBC store allocation failed";
        return ByteStorePtr();
    }
}

} // namespace fs
} // namespace peare
