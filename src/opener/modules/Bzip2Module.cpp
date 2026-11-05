#include "Bzip2Module.h"
#include "Compat.h"

#include "../compression/peare/Bzip2Decoder.h"
#include "../fs/DiscStore.h"
#include "../fs/QFileByteStore.h"

#include <QFile>
#include <QFileInfo>

#include <utility>
#include <vector>

namespace peare {
namespace {

QString fallbackName(const QString& logicalName)
{
    QString name = QFileInfo(logicalName).fileName();
    if (name.endsWith(QStringLiteral(".bzip2"), Qt::CaseInsensitive))
        name.chop(6);
    else if (name.endsWith(QStringLiteral(".bz2"), Qt::CaseInsensitive))
        name.chop(4);
    return name.isEmpty() ? QStringLiteral("bzip2_payload") : name;
}

} // namespace

std::unique_ptr<Bzip2Module> Bzip2Module::open(const QString& filePath)
{
    auto source = std::make_shared<fs::QFileByteStore>(filePath);
    if (!source->validate()) {
        auto module = peare::makeUnique<Bzip2Module>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::BZIP2;
        module->info_.error = QStringLiteral("Could not read BZIP2 input");
        return module;
    }
    return open(source, filePath);
}

std::unique_ptr<Bzip2Module> Bzip2Module::open(const fs::ByteStorePtr& source,
                                               const QString& logicalName)
{
    if (!source || source->capacity() < 0 ||
        std::uint64_t(source->capacity()) > std::numeric_limits<std::size_t>::max()) {
        auto module = peare::makeUnique<Bzip2Module>();
        module->info_.filePath = logicalName;
        module->info_.format = ModuleFormat::BZIP2;
        module->info_.error = QStringLiteral("Invalid BZIP2 byte source");
        return module;
    }
    return open(source->readAll(), logicalName);
}

std::unique_ptr<Bzip2Module> Bzip2Module::open(const QByteArray& data,
                                               const QString& logicalName)
{
    return open(std::vector<std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(data.constData()),
        reinterpret_cast<const std::uint8_t*>(data.constData()) + data.size()), logicalName);
}

std::unique_ptr<Bzip2Module> Bzip2Module::open(std::vector<std::uint8_t> data,
                                               const QString& logicalName)
{
    auto module = peare::makeUnique<Bzip2Module>();
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::BZIP2;
    module->info_.description = QStringLiteral("BZIP2 compressed stream");

    std::vector<std::uint8_t> decoded;
    if (!compression::decompressBzip2(data.data(), data.size(), decoded.max_size(), true,
                                      &decoded)) {
        module->info_.error = QStringLiteral("Invalid or truncated BZIP2 stream");
        return module;
    }

    ResourceEntry entry;
    entry.type = QStringLiteral("BZIP2_FILE");
    entry.name = fallbackName(logicalName);
    entry.language = QStringLiteral("neutral");
    entry.dataSize = quint64(decoded.size());
    entry.format = ModuleFormat::BZIP2;
    entry.hierarchyPath = QStringList() << entry.name;
    entry.isEmbeddedFile = true;
    entry.content = std::make_shared<fs::MemoryStore>(std::move(decoded));
    module->resources_.push_back(std::move(entry));
    return module;
}

} // namespace peare
