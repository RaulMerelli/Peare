#pragma once

#include "DiscStore.h"

#include <QFile>
#include <QString>
#include <mutex>

namespace peare {
namespace fs {

class QFileByteStore final : public IByteStore {
public:
    explicit QFileByteStore(const QString& path) : file_(path) {
        if (file_.open(QIODevice::ReadOnly)) size_ = file_.size();
    }

    std::int64_t capacity() const override { return size_; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (size_ < 0 || pos < 0 || count <= 0 || pos >= size_ || !dst) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!file_.seek(pos)) return 0;
        const qint64 request = std::min<std::int64_t>(count, size_ - pos);
        const qint64 got = file_.read(reinterpret_cast<char*>(dst), request);
        return got > 0 ? static_cast<int>(got) : 0;
    }

    bool validate() const override { return file_.isOpen() && size_ >= 0; }

private:
    mutable QFile file_;
    std::int64_t size_ = -1;
    mutable std::mutex mutex_;
};

}  // namespace fs
}  // namespace peare
