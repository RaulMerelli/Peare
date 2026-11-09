#include "UnixCompressModule.h"
#include "Compat.h"
#include "../../common/compression/UnixCompress.h"
#include "../fs/QFileByteStore.h"

#include <QIODevice>
#include <QFileInfo>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace peare {
namespace {

class VectorDevice final : public QIODevice {
public:
    explicit VectorDevice(const std::vector<std::uint8_t>* input)
        : input_(input) {}
    explicit VectorDevice(std::vector<std::uint8_t>* output)
        : output_(output) {}

    bool isSequential() const override { return true; }

protected:
    qint64 readData(char* data, qint64 maxSize) override
    {
        if (!input_ || maxSize <= 0) return -1;
        const std::size_t available = input_->size() - position_;
        const std::size_t count = std::min<std::size_t>(
            available, std::size_t(maxSize));
        if (!count) return 0;
        std::memcpy(data, input_->data() + position_, count);
        position_ += count;
        return qint64(count);
    }

    qint64 writeData(const char* data, qint64 size) override
    {
        if (!output_ || size < 0 ||
            std::uint64_t(size) > output_->max_size() - output_->size()) return -1;
        output_->insert(output_->end(), data, data + size);
        return size;
    }

private:
    const std::vector<std::uint8_t>* input_ = nullptr;
    std::vector<std::uint8_t>* output_ = nullptr;
    std::size_t position_ = 0;
};

QString payloadName(const QString& logicalName)
{
    QString name = QFileInfo(logicalName).fileName();
    if (name.endsWith(QStringLiteral(".Z"), Qt::CaseInsensitive)) name.chop(2);
    return name.isEmpty() ? QStringLiteral("compress_payload") : name;
}

}  // namespace

ModulePtr UnixCompressModule::open(const QString& filePath)
{
    auto file = std::make_shared<fs::QFileByteStore>(filePath);
    if (!file->validate()) {
        auto module = peare::makeUnique<UnixCompressModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::UNIX_COMPRESS;
        module->info_.error = QStringLiteral("Could not read Unix compress input");
        return ModulePtr(std::move(module));
    }
    return open(file, filePath);
}

ModulePtr UnixCompressModule::open(const QByteArray& data, const QString& logicalName)
{
    return open(std::vector<std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(data.constData()),
        reinterpret_cast<const std::uint8_t*>(data.constData()) + data.size()), logicalName);
}

ModulePtr UnixCompressModule::open(std::vector<std::uint8_t> data,
                                   const QString& logicalName)
{
    auto module = peare::makeUnique<UnixCompressModule>();
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::UNIX_COMPRESS;
    module->info_.description = QStringLiteral("Unix compress (.Z) stream");

    const std::vector<std::uint8_t>* source = &data;
    VectorDevice sourceDevice(source);
    sourceDevice.open(QIODevice::ReadOnly);
    std::vector<std::uint8_t> decoded;
    VectorDevice outputDevice(&decoded);
    outputDevice.open(QIODevice::WriteOnly);
    if (!compression::decompressUnixCompress(&sourceDevice, &outputDevice)) {
        module->info_.error = QStringLiteral("Invalid or truncated Unix compress stream");
        return ModulePtr(std::move(module));
    }

    ResourceEntry entry;
    entry.type = QStringLiteral("UNIX_COMPRESS_FILE");
    entry.name = payloadName(logicalName);
    entry.language = QStringLiteral("neutral");
    entry.dataSize = quint64(decoded.size());
    entry.format = ModuleFormat::UNIX_COMPRESS;
    entry.hierarchyPath = QStringList() << entry.name;
    entry.isEmbeddedFile = true;
    entry.content = std::make_shared<fs::MemoryStore>(std::move(decoded));
    module->resources_.push_back(std::move(entry));
    return ModulePtr(std::move(module));
}

ModulePtr UnixCompressModule::open(const fs::ByteStorePtr& file, const QString& logicalName)
{
    if (!file || file->capacity() < 0 ||
        std::uint64_t(file->capacity()) > std::numeric_limits<std::size_t>::max()) {
        auto module = peare::makeUnique<UnixCompressModule>();
        module->info_.filePath = logicalName;
        module->info_.format = ModuleFormat::UNIX_COMPRESS;
        module->info_.error = QStringLiteral("Invalid Unix compress byte source");
        return ModulePtr(std::move(module));
    }
    const std::vector<std::uint8_t> bytes = file->readAll();
    return open(std::move(bytes), logicalName);
}

}  // namespace peare
