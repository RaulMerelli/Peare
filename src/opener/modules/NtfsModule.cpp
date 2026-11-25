#include "NtfsModule.h"
#include "Compat.h"
#include "FsLevel.h"

#include "../fs/NtfsReader.h"

#include <QFile>

#include <utility>

namespace peare {
namespace {
class SynchronizedNtfsFile final : public fs::IByteStore {
public:
    explicit SynchronizedNtfsFile(fs::ByteStorePtr source) : source_(std::move(source)) {}
    std::int64_t capacity() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return source_->capacity();
    }
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (failed_) return -1;
        const int result = source_->read(pos, dst, count);
        if (result < 0 || result > count ||
            (result == 0 && pos >= 0 && count > 0 && pos < source_->capacity())) {
            failed_ = true;
            return -1;
        }
        return result;
    }
    bool validate() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return !failed_ && source_->validate();
    }
    bool cheapRandomAccess() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return source_->cheapRandomAccess();
    }
private:
    fs::ByteStorePtr source_;
    mutable std::mutex mutex_;
    mutable bool failed_ = false;
};
}

ModulePtr NtfsModule::open(const fs::ByteStorePtr& disc, const QString& sourceName,
                           const QString& subPath) {
    auto module = peare::makeUnique<NtfsModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::NTFS;
    module->info_.description = QStringLiteral("NTFS volume");

    auto reader = std::make_shared<fs::NtfsReader>(disc);
    if (!reader->valid()) {
        module->info_.error = QString::fromStdString(reader->error());
        return ModulePtr(std::move(module));
    }
    std::vector<fs::DiscEntry> entries;
    if (!reader->listChecked(subPath.toStdString(), &entries)) {
        module->info_.error = QStringLiteral("Cannot read NTFS directory");
        return ModulePtr(std::move(module));
    }
    module->fs_ = reader;
    module->info_.description = QString::fromStdString(reader->friendlyName()) + QStringLiteral(" volume");
    buildFsLevel(*reader, disc, subPath.toStdString(), ModuleFormat::NTFS,
                 QStringLiteral("NTFS_FILE"), QStringLiteral("NTFS_DIR"), module->resources_,
                 [reader](const std::string& path) -> fs::ByteStorePtr {
                     fs::ByteStorePtr content = reader->openFile(path);
                     if (!content) return fs::ByteStorePtr();
                     return std::make_shared<SynchronizedNtfsFile>(std::move(content));
                 }, &entries);
    return ModulePtr(std::move(module));
}

ModulePtr NtfsModule::open(const QString& filePath) {
    auto holder = std::make_shared<QFile>(filePath);
    if (!holder->open(QIODevice::ReadOnly)) {
        auto module = peare::makeUnique<NtfsModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::NTFS;
        module->info_.error = holder->errorString();
        return ModulePtr(std::move(module));
    }
    const qint64 size = holder->size();
    fs::ByteStorePtr disc;
    uchar* mapped = size > 0 ? holder->map(0, size) : nullptr;
    if (mapped) {
        disc = std::make_shared<fs::ExternalStore>(
            reinterpret_cast<const std::uint8_t*>(mapped), std::int64_t(size),
            std::static_pointer_cast<void>(holder));
    } else {
        const QByteArray bytes = holder->readAll();
        disc = std::make_shared<fs::MemoryStore>(
            reinterpret_cast<const std::uint8_t*>(bytes.constData()), std::size_t(bytes.size()));
    }
    return open(disc, filePath);
}

}  // namespace peare
