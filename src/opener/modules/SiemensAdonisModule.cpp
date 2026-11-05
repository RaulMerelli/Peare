#include "SiemensAdonisModule.h"
#include "Compat.h"

#include <QFile>
#include <QtEndian>

#include <stdexcept>

namespace peare {
namespace {

constexpr quint32 kRuntimeBase = 0x20100000U;

quint32 read32(const QByteArray& data, qsizetype off, bool bigEndian)
{
    if (off < 0 || off + 4 > data.size()) return 0;
    const uchar* p = reinterpret_cast<const uchar*>(data.constData() + off);
    return bigEndian ? qFromBigEndian<quint32>(p) : qFromLittleEndian<quint32>(p);
}

bool looksLikeVectorWord(quint32 word)
{
    // ARM LDR pc,[pc,#imm12] / LDR pc,[pc,-#imm12]. Both Basic Panel
    // generations start with a dense vector veneer of this form.
    return (word & 0xFFFFF000U) == 0xE59FF000U ||
           (word & 0xFFFFF000U) == 0xE51FF000U;
}

bool detectTraits(const QByteArray& data)
{
    if (data.size() < 0x100) return false;

    for (int endian = 0; endian < 2; ++endian) {
        const bool be = endian != 0;
        int vectorWords = 0;
        for (int off = 0; off < 0x20; off += 4)
            if (looksLikeVectorWord(read32(data, off, be))) ++vectorWords;
        if (vectorWords < 5) continue;

        int basePointers = 0;
        const int limit = qMin(data.size(), 0x400);
        for (int off = 0; off + 4 <= limit; off += 4) {
            const quint32 value = read32(data, off, be);
            if (value >= kRuntimeBase && value < 0x28000000U)
                ++basePointers;
        }
        if (basePointers < 4) continue;

        return true;
    }
    return false;
}

bool containsAdonisMarker(const QByteArray& data)
{
    return data.indexOf(QByteArrayLiteral("ADONIS")) >= 0 ||
           data.indexOf(QByteArrayLiteral("SMART_OS")) >= 0;
}

QByteArray readAll(const QString& path, QString& error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { error = file.errorString(); return {}; }
    return file.readAll();
}

} // namespace

bool SiemensAdonisModule::isImage(const QByteArray& data) noexcept
{
    try {
        return detectTraits(data) && containsAdonisMarker(data);
    } catch (...) {
        return false;
    }
}

ModulePtr SiemensAdonisModule::open(const QString& filePath)
{
    QString error;
    const QByteArray bytes = readAll(filePath, error);
    if (!error.isEmpty()) {
        auto module = peare::makeUnique<SiemensAdonisModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::SIEMENS_ADONIS;
        module->info_.description = QStringLiteral("Siemens ADONIS / SMART_OS flat ARM image");
        module->info_.error = error;
        return module;
    }
    return open(bytes, filePath);
}

ModulePtr SiemensAdonisModule::open(const QByteArray& data, const QString& logicalName)
{
    auto module = peare::makeUnique<SiemensAdonisModule>();
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::SIEMENS_ADONIS;
    module->info_.description = QStringLiteral("Siemens ADONIS / SMART_OS flat ARM image");

    try {
        if (!detectTraits(data))
            throw std::runtime_error("not a recognised Siemens ADONIS flat ARM image");
        if (!containsAdonisMarker(data))
            throw std::runtime_error("ARM image lacks ADONIS/SMART_OS markers");

        ResourceEntry entry;
        entry.type = QStringLiteral("SIEMENS_ADONIS_DATA");
        entry.name = QStringLiteral("firmware.bin");
        entry.dataSize = quint64(data.size());
        entry.hierarchyPath = QStringList() << QStringLiteral("Image") << entry.name;
        entry.format = ModuleFormat::SIEMENS_ADONIS;
        entry.data = data;
        module->resources_.push_back(std::move(entry));
    } catch (const std::exception& ex) {
        module->info_.error = QString::fromLocal8Bit(ex.what());
    }
    return module;
}

} // namespace peare
