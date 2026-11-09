#include "LzmaModule.h"
#include "Compat.h"

#include "../compression/LzmaDecoder.h"
#include "../fs/DiscStore.h"
#include "../fs/QFileByteStore.h"

#include <QFileInfo>

#include <algorithm>
#include <limits>
#include <vector>

namespace peare {
namespace {

const std::size_t kHeaderSize = 13;

std::uint64_t readLe64(const std::uint8_t* data)
{
    std::uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i)
        value |= std::uint64_t(data[i]) << (i * 8);
    return value;
}

QString payloadName(const QString& logicalName)
{
    QString name = QFileInfo(logicalName).fileName();
    if (name.endsWith(QStringLiteral(".lzma"), Qt::CaseInsensitive)) name.chop(5);
    else if (name.endsWith(QStringLiteral(".tlz"), Qt::CaseInsensitive)) name.chop(4);
    return name.isEmpty() ? QStringLiteral("lzma_payload") : name;
}

bool readAll(const fs::ByteStorePtr& source, std::vector<std::uint8_t>* data)
{
    if (!source || source->capacity() < 0 ||
        std::uint64_t(source->capacity()) > data->max_size()) return false;
    try {
        data->resize(std::size_t(source->capacity()));
    } catch (...) {
        return false;
    }
    std::size_t done = 0;
    while (done < data->size()) {
        const int request = int(std::min<std::size_t>(data->size() - done, 4 * 1024 * 1024));
        const int count = source->read(std::int64_t(done), data->data() + done, request);
        if (count <= 0 || count > request) return false;
        done += std::size_t(count);
    }
    return true;
}

} // namespace

bool LzmaModule::hasExtension(const QString& fileName)
{
    const QString suffix = QFileInfo(fileName).suffix();
    return suffix.compare(QLatin1String("lzma"), Qt::CaseInsensitive) == 0 ||
           suffix.compare(QLatin1String("tlz"), Qt::CaseInsensitive) == 0;
}

ModulePtr LzmaModule::open(const QString& filePath)
{
    return open(std::make_shared<fs::QFileByteStore>(filePath), filePath);
}

ModulePtr LzmaModule::open(const QByteArray& data, const QString& logicalName)
{
    const fs::ByteStorePtr source = std::make_shared<fs::MemoryStore>(
        reinterpret_cast<const std::uint8_t*>(data.constData()), std::size_t(data.size()));
    return open(source, logicalName);
}

ModulePtr LzmaModule::open(const fs::ByteStorePtr& source, const QString& logicalName)
{
    std::unique_ptr<LzmaModule> module(new LzmaModule());
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::LZMA;
    module->info_.description = QStringLiteral("LZMA-alone stream");
    const auto fail = [&module](const QString& error) -> ModulePtr {
        module->info_.error = error;
        return ModulePtr(module.release());
    };
    if (!source || source->capacity() < std::int64_t(kHeaderSize))
        return fail(QStringLiteral("Truncated LZMA-alone header"));

    std::vector<std::uint8_t> input;
    if (!readAll(source, &input)) return fail(QStringLiteral("Could not read LZMA-alone input"));
    const std::uint64_t declaredSize = readLe64(input.data() + 5);
    const std::uint8_t* packed = input.data() + kHeaderSize;
    const std::size_t packedSize = input.size() - kHeaderSize;
    std::vector<std::uint8_t> decoded;
    bool valid = false;
    if (declaredSize == std::numeric_limits<std::uint64_t>::max()) {
        valid = compression::decodeLzmaRawUntilEndMarker(
            packed, packedSize, input.data(), 5, &decoded);
    } else if (declaredSize <= std::numeric_limits<std::size_t>::max() &&
               declaredSize <= decoded.max_size()) {
        const std::size_t size = std::size_t(declaredSize);
        valid = compression::decodeLzmaRaw(
            packed, packedSize, input.data(), 5, size, &decoded);
        if (!valid && size == 0)
            valid = compression::decodeLzmaRawWithEndMarker(
                packed, packedSize, input.data(), 5, size, &decoded);
    }
    if (!valid) return fail(QStringLiteral("Invalid or unsupported LZMA-alone stream"));

    ResourceEntry entry;
    entry.type = QStringLiteral("LZMA_FILE");
    entry.name = payloadName(logicalName);
    entry.language = QStringLiteral("neutral");
    entry.dataOffset = kHeaderSize;
    entry.dataSize = quint64(decoded.size());
    entry.format = ModuleFormat::LZMA;
    entry.isEmbeddedFile = true;
    entry.content = std::make_shared<fs::MemoryStore>(std::move(decoded));
    module->resources_.push_back(std::move(entry));
    return ModulePtr(module.release());
}

} // namespace peare
