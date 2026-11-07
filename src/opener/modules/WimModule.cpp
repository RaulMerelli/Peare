#include "WimModule.h"
#include "Compat.h"
#include "FsLevel.h"

#include "../fs/WimReader.h"
#include "../fs/QFileByteStore.h"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

#include <cstring>
#include <utility>

namespace peare {
namespace {

std::uint16_t readLe16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + 2 > bytes.size()) return 0;
    return std::uint16_t(bytes[offset]) | (std::uint16_t(bytes[offset + 1]) << 8);
}

bool splitWimPaths(const QString& filePath, QStringList* paths, QString* error) {
    auto first = std::make_shared<fs::QFileByteStore>(filePath);
    if (!first->validate()) {
        *error = QStringLiteral("Could not open WIM byte source");
        return false;
    }
    const std::vector<std::uint8_t> header = first->readRange(0, 44);
    if (header.size() < 44 || std::memcmp(header.data(), "MSWIM\0\0\0", 8) != 0) {
        paths->append(filePath);
        return true;
    }
    const std::uint16_t partNumber = readLe16(header, 40);
    const std::uint16_t totalParts = readLe16(header, 42);
    if (totalParts <= 1) {
        paths->append(filePath);
        return true;
    }
    if (partNumber == 0 || partNumber > totalParts || totalParts > 4096) {
        *error = QStringLiteral("The split WIM header has an invalid part count");
        return false;
    }

    QFileInfo selected(filePath);
    QString firstBase = selected.completeBaseName();
    if (partNumber > 1) {
        int digitStart = firstBase.size();
        while (digitStart > 0 && firstBase.at(digitStart - 1).isDigit()) --digitStart;
        bool suffixOk = false;
        const uint suffix = firstBase.mid(digitStart).toUInt(&suffixOk);
        if (digitStart == firstBase.size() || !suffixOk || suffix != partNumber) {
            *error = QStringLiteral("Open the first part of the split WIM set");
            return false;
        }
        firstBase.truncate(digitStart);
    }

    const QString suffix = selected.completeSuffix();
    const QDir directory = selected.absoluteDir();
    for (std::uint16_t number = 1; number <= totalParts; ++number) {
        const QString name = firstBase + (number == 1 ? QString() : QString::number(number)) +
                             QLatin1Char('.') + suffix;
        const QString partPath = directory.filePath(name);
        if (!QFileInfo::exists(partPath)) {
            *error = QStringLiteral("Missing split WIM part %1 of %2: %3")
                         .arg(number).arg(totalParts).arg(partPath);
            return false;
        }
        paths->append(partPath);
    }
    return true;
}

}  // namespace

ModulePtr WimModule::open(const fs::ByteStorePtr& disc, const QString& sourceName,
                          const QString& subPath) {
    auto module = peare::makeUnique<WimModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::WIM;
    module->info_.description = QStringLiteral("Windows Imaging (WIM) image");

    auto reader = std::make_shared<fs::WimReader>(disc);
    if (!reader->valid()) {
        module->info_.error = QString::fromStdString(reader->error());
        return ModulePtr(std::move(module));
    }
    module->fs_ = reader;
    module->info_.description = QString::fromStdString(reader->friendlyName());
    buildFsLevel(*reader, disc, subPath.toStdString(), ModuleFormat::WIM,
                 QStringLiteral("WIM_FILE"), QStringLiteral("WIM_DIR"), module->resources_);
    return ModulePtr(std::move(module));
}

ModulePtr WimModule::open(const std::vector<fs::ByteStorePtr>& parts,
                          const QString& sourceName) {
    auto module = peare::makeUnique<WimModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::WIM;
    module->info_.description = QStringLiteral("Windows Imaging (WIM) image");

    auto reader = std::make_shared<fs::WimReader>(parts);
    if (!reader->valid()) {
        module->info_.error = QString::fromStdString(reader->error());
        return ModulePtr(std::move(module));
    }
    module->fs_ = reader;
    module->info_.description = QString::fromStdString(reader->friendlyName());
    buildFsLevel(*reader, parts.front(), std::string(), ModuleFormat::WIM,
                 QStringLiteral("WIM_FILE"), QStringLiteral("WIM_DIR"), module->resources_);
    return ModulePtr(std::move(module));
}

ModulePtr WimModule::open(const QString& filePath) {
    QStringList paths;
    QString error;
    if (!splitWimPaths(filePath, &paths, &error)) {
        auto module = peare::makeUnique<WimModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::WIM;
        module->info_.error = error;
        return ModulePtr(std::move(module));
    }
    std::vector<fs::ByteStorePtr> parts;
    parts.reserve(std::size_t(paths.size()));
    for (const QString& path : paths) {
        auto source = std::make_shared<fs::QFileByteStore>(path);
        if (!source->validate()) {
            auto module = peare::makeUnique<WimModule>();
            module->info_.filePath = filePath;
            module->info_.format = ModuleFormat::WIM;
            module->info_.error = QStringLiteral("Could not read WIM part: %1").arg(path);
            return ModulePtr(std::move(module));
        }
        parts.push_back(std::move(source));
    }
    return open(parts, filePath);
}

}  // namespace peare
