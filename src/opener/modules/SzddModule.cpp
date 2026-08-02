#include "SzddModule.h"
#include "Compat.h"

#include <QFile>
#include <QFileInfo>
#include <array>
#include <utility>

namespace peare {
namespace {

constexpr qsizetype kHeaderSize = 14;
constexpr quint32 kMaxDecodedSize = 512u * 1024u * 1024u;
const QByteArray kSignature("SZDD\x88\xF0\x27\x33", 8);

quint32 le32(const QByteArray& data, qsizetype offset)
{
    if (offset < 0 || offset + 4 > data.size()) return 0;
    const auto* p = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint32(p[0]) | (quint32(p[1]) << 8) |
           (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

bool decompress(const QByteArray& input, quint32 expectedSize, QByteArray* output, QString* error)
{
    if (expectedSize > kMaxDecodedSize) {
        *error = QStringLiteral("SZDD declared size exceeds safety limit");
        return false;
    }

    std::array<uchar, 4096> dictionary{};
    dictionary.fill(0x20);
    int dictionaryPos = 4096 - 16;
    qsizetype inputPos = kHeaderSize;
    quint16 flags = 0;

    output->clear();
    output->reserve(int(expectedSize));

    while (quint32(output->size()) < expectedSize) {
        flags >>= 1;
        if ((flags & 0x0100u) == 0) {
            if (inputPos >= input.size()) {
                *error = QStringLiteral("Truncated SZDD control stream");
                return false;
            }
            flags = quint16(uchar(input.at(inputPos++))) | 0xFF00u;
        }

        if ((flags & 1u) != 0) {
            if (inputPos >= input.size()) {
                *error = QStringLiteral("Truncated SZDD literal");
                return false;
            }
            const uchar value = uchar(input.at(inputPos++));
            output->append(char(value));
            dictionary[dictionaryPos] = value;
            dictionaryPos = (dictionaryPos + 1) & 0x0FFF;
        } else {
            if (inputPos + 2 > input.size()) {
                *error = QStringLiteral("Truncated SZDD dictionary reference");
                return false;
            }
            const uchar low = uchar(input.at(inputPos++));
            const uchar highLength = uchar(input.at(inputPos++));
            int sourcePos = int(low) | ((int(highLength) & 0xF0) << 4);
            const int length = (int(highLength) & 0x0F) + 3;
            for (int i = 0; i < length && quint32(output->size()) < expectedSize; ++i) {
                const uchar value = dictionary[sourcePos];
                sourcePos = (sourcePos + 1) & 0x0FFF;
                output->append(char(value));
                dictionary[dictionaryPos] = value;
                dictionaryPos = (dictionaryPos + 1) & 0x0FFF;
            }
        }
    }
    return true;
}

} // namespace

std::unique_ptr<SzddModule> SzddModule::open(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        auto module = peare::makeUnique<SzddModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::SZDD;
        module->info_.error = file.errorString();
        return module;
    }
    return open(file.readAll(), filePath);
}

std::unique_ptr<SzddModule> SzddModule::open(const QByteArray& data, const QString& logicalName)
{
    auto module = peare::makeUnique<SzddModule>();
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::SZDD;
    module->info_.description = QStringLiteral("Microsoft Compress SZDD archive");

    if (data.size() < kHeaderSize || data.left(8) != kSignature) {
        module->info_.error = QStringLiteral("Invalid SZDD signature or truncated header");
        return module;
    }
    if (uchar(data.at(8)) != uchar('A')) {
        module->info_.error = QStringLiteral("Unsupported SZDD compression mode");
        return module;
    }

    const quint32 originalSize = le32(data, 10);
    QByteArray decoded;
    QString error;
    if (!decompress(data, originalSize, &decoded, &error)) {
        module->info_.error = error;
        return module;
    }

    ResourceEntry entry;
    entry.type = QStringLiteral("SZDD_FILE");
    entry.isEmbeddedFile = true;
    QString payloadName = QFileInfo(logicalName).fileName();
    if (payloadName.isEmpty())
        payloadName = QStringLiteral("SZDD");
    entry.name = payloadName + QLatin1Char('~');
    entry.language = QStringLiteral("neutral");
    entry.dataOffset = kHeaderSize;
    entry.dataSize = quint64(decoded.size());
    entry.format = ModuleFormat::SZDD;
    entry.hierarchyPath = QStringList() << entry.name;
    entry.data = std::move(decoded);
    module->resources_.push_back(std::move(entry));
    return module;
}

} // namespace peare
