#include "ParallelsDisk.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>
namespace peare {
namespace fs {
namespace {
static std::uint32_t le32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) |
           (std::uint32_t(p[3]) << 24);
}
static std::uint64_t le64(const std::uint8_t* p) {
    return std::uint64_t(le32(p)) | (std::uint64_t(le32(p + 4)) << 32);
}
class ParallelsStore final : public IByteStore {
public:
    ParallelsStore(ByteStorePtr f, std::uint64_t sectors, std::uint32_t tracks,
                   std::vector<std::uint32_t> bat, bool ext)
        : f_(std::move(f)), size_(sectors * 512ULL), cluster_(std::uint64_t(tracks) * 512ULL),
          bat_(std::move(bat)), ext_(ext) {}
    std::int64_t capacity() const override {
        return size_ > std::uint64_t(std::numeric_limits<std::int64_t>::max())
                   ? 0
                   : std::int64_t(size_);
    }
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!f_ || pos < 0 || count <= 0 || std::uint64_t(pos) >= size_) return 0;
        int want = int(std::min<std::uint64_t>(count, size_ - std::uint64_t(pos))), done = 0;
        while (done < want) {
            std::uint64_t at = std::uint64_t(pos) + done, idx = at / cluster_, in = at % cluster_;
            int n = int(std::min<std::uint64_t>(want - done, cluster_ - in));
            if (idx >= bat_.size() || bat_[idx] == 0)
                std::memset(dst + done, 0, n);
            else {
                std::uint64_t base =
                    ext_ ? std::uint64_t(bat_[idx]) * cluster_ : std::uint64_t(bat_[idx]) * 512ULL;
                int got = f_->read(std::int64_t(base + in), dst + done, n);
                if (got < n) std::memset(dst + done + std::max(got, 0), 0, n - std::max(got, 0));
            }
            done += n;
        }
        return done;
    }

private:
    ByteStorePtr f_;
    std::uint64_t size_, cluster_;
    std::vector<std::uint32_t> bat_;
    bool ext_;
};
} // namespace
ByteStorePtr openParallelsDisk(const ByteStorePtr& file, std::string* error) {
    if (!file || file->capacity() < 64) {
        if (error) *error = "Parallels header is truncated";
        return {};
    }
    auto h = file->readRange(0, 64);
    const bool classic = std::memcmp(h.data(), "WithoutFreeSpace", 16) == 0,
               ext = std::memcmp(h.data(), "WithouFreSpacExt", 16) == 0;
    if (!classic && !ext) {
        if (error) *error = "Not a Parallels expandable image";
        return {};
    }
    const std::uint32_t version = le32(h.data() + 16), tracks = le32(h.data() + 28),
                        entries = le32(h.data() + 32);
    const std::uint64_t sectors = le64(h.data() + 36);
    if (version != 2 || tracks == 0 || entries == 0 || sectors == 0 || entries > 0x40000000U) {
        if (error) *error = "Unsupported or invalid Parallels header";
        return {};
    }
    const std::uint64_t batBytes = std::uint64_t(entries) * 4;
    if (64 + batBytes > std::uint64_t(file->capacity())) {
        if (error) *error = "Parallels BAT is truncated";
        return {};
    }
    auto raw = file->readRange(64, batBytes);
    std::vector<std::uint32_t> bat(entries);
    for (std::uint32_t i = 0; i < entries; ++i) bat[i] = le32(raw.data() + std::size_t(i) * 4);
    return std::make_shared<ParallelsStore>(file, sectors, tracks, std::move(bat), ext);
}
} // namespace fs
} // namespace peare
