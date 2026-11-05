#include "UuencodeModule.h"
#include "Compat.h"
#include "../fs/DiscStore.h"

#include <QBuffer>
#include <QFile>
#include <QIODevice>
#include <QTemporaryFile>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <mutex>
#include <memory>

namespace peare {
namespace {

constexpr qsizetype kMaxLineLength = 64 * 1024;

bool readLine(const QByteArray& data, qsizetype* position, QByteArray* line)
{
    if (*position >= data.size()) return false;
    const qsizetype start = *position;
    const qsizetype end = data.indexOf('\n', start);
    *position = end < 0 ? data.size() : end + 1;
    const qsizetype length = (end < 0 ? data.size() : end) - start;
    *line = data.mid(start, length);
    if (line->endsWith('\r')) line->chop(1);
    return true;
}

bool parseHeader(const QByteArray& line, QByteArray* name)
{
    if (!line.startsWith("begin ")) return false;
    const qsizetype modeStart = 6;
    const qsizetype modeEnd = line.indexOf(' ', modeStart);
    if (modeEnd - modeStart < 3 || modeEnd - modeStart > 4 || modeEnd + 1 >= line.size())
        return false;
    for (qsizetype i = modeStart; i < modeEnd; ++i)
        if (line.at(i) < '0' || line.at(i) > '7') return false;
    *name = line.mid(modeEnd + 1).trimmed();
    return !name->isEmpty();
}

int decodeCharacter(char value)
{
    const unsigned char byte = static_cast<unsigned char>(value);
    if (byte < 0x20 || byte > 0x60) return -1;
    return (byte - 0x20) & 0x3F;
}

bool decodeLine(const QByteArray& line, std::array<std::uint8_t, 45>* output,
                int* outputSize)
{
    if (!output || !outputSize || line.isEmpty()) return false;
    const int count = decodeCharacter(line.at(0));
    if (count < 0 || count > 45) return false;
    const qsizetype encodedLength = 1 + 4 * ((count + 2) / 3);
    if (line.size() != encodedLength) return false;

    int decoded = 0;
    for (qsizetype i = 1; i < line.size(); i += 4) {
        int values[4];
        for (int j = 0; j < 4; ++j) {
            values[j] = decodeCharacter(line.at(i + j));
            if (values[j] < 0) return false;
        }
        const std::uint8_t bytes[3] = {
            std::uint8_t((values[0] << 2) | (values[1] >> 4)),
            std::uint8_t((values[1] << 4) | (values[2] >> 2)),
            std::uint8_t((values[2] << 6) | values[3])
        };
        const int take = std::min(3, count - decoded);
        std::copy(bytes, bytes + take, output->begin() + decoded);
        decoded += take;
    }
    *outputSize = decoded;
    return decoded == count;
}

bool readDeviceLine(QIODevice& input, QByteArray* line)
{
    if (!line) return false;
    *line = input.readLine(kMaxLineLength + 1);
    if (line->isEmpty() || line->size() > kMaxLineLength) return false;
    if (line->endsWith('\n')) line->chop(1);
    if (line->endsWith('\r')) line->chop(1);
    return true;
}

bool writeAll(QIODevice& output, const std::uint8_t* data, qint64 size)
{
    qint64 written = 0;
    while (written < size) {
        const qint64 count = output.write(
            reinterpret_cast<const char*>(data + written), size - written);
        if (count <= 0) return false;
        written += count;
    }
    return true;
}

bool trailingWhitespace(QIODevice& input)
{
    std::array<char, 64 * 1024> buffer{};
    while (!input.atEnd()) {
        const qint64 count = input.read(buffer.data(), qint64(buffer.size()));
        if (count <= 0) return false;
        for (qint64 i = 0; i < count; ++i) {
            const char value = buffer[std::size_t(i)];
            if (value != '\r' && value != '\n' && value != ' ' && value != '\t')
                return false;
        }
    }
    return true;
}

class UuencodeFileStore final : public fs::IByteStore {
public:
    UuencodeFileStore(std::shared_ptr<QTemporaryFile> file, std::int64_t size)
        : file_(std::move(file)), size_(size) {}

    std::int64_t capacity() const override { return size_; }

    int read(std::int64_t pos, std::uint8_t* destination, int count) const override
    {
        if (!file_ || !destination || pos < 0 || count <= 0 || pos >= size_) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!file_->seek(pos)) return 0;
        const qint64 request = std::min<std::int64_t>(count, size_ - pos);
        const qint64 received = file_->read(reinterpret_cast<char*>(destination), request);
        return received > 0 ? int(received) : 0;
    }

    bool validate() const override
    {
        return file_ && file_->isOpen() && file_->size() == size_;
    }

private:
    std::shared_ptr<QTemporaryFile> file_;
    std::int64_t size_ = 0;
    mutable std::mutex mutex_;
};

QString payloadName(QByteArray name)
{
    const qsizetype slash = std::max(name.lastIndexOf('/'), name.lastIndexOf('\\'));
    name = name.mid(slash + 1).trimmed();
    QString decoded = QString::fromLocal8Bit(name);
    const QString invalid = QStringLiteral("<>:\"|?*");
    for (QChar& character : decoded) {
        if (character.unicode() < 0x20 || invalid.contains(character)) character = QLatin1Char('_');
    }
    return decoded.isEmpty() || decoded == QStringLiteral(".") || decoded == QStringLiteral("..")
        ? QStringLiteral("uue_payload") : decoded;
}

}  // namespace

bool UuencodeModule::hasHeader(const QByteArray& data)
{
    qsizetype position = 0;
    QByteArray header;
    QByteArray body;
    QByteArray name;
    if (!readLine(data, &position, &header) || !parseHeader(header, &name) ||
        !readLine(data, &position, &body))
        return false;
    if (body == "end") return true;
    std::array<std::uint8_t, 45> decoded{};
    int decodedSize = 0;
    return decodeLine(body, &decoded, &decodedSize);
}

ModulePtr UuencodeModule::open(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        auto module = peare::makeUnique<UuencodeModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::UUE;
        module->info_.error = file.errorString();
        return module;
    }
    return open(file, filePath);
}

ModulePtr UuencodeModule::open(const QByteArray& data, const QString& logicalName)
{
    QBuffer input;
    input.setData(data);
    if (!input.open(QIODevice::ReadOnly)) {
        auto module = peare::makeUnique<UuencodeModule>();
        module->info_.filePath = logicalName;
        module->info_.format = ModuleFormat::UUE;
        module->info_.error = input.errorString();
        return module;
    }
    return open(input, logicalName);
}

ModulePtr UuencodeModule::open(QIODevice& input, const QString& logicalName)
{
    auto module = peare::makeUnique<UuencodeModule>();
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::UUE;
    module->info_.description = QStringLiteral("UUencoded file");

    QByteArray header;
    QByteArray encodedName;
    if (!readDeviceLine(input, &header) || !parseHeader(header, &encodedName)) {
        module->info_.error = QStringLiteral("Invalid UUencode begin header");
        return module;
    }

    auto decodedFile = std::make_shared<QTemporaryFile>();
    if (!decodedFile->open()) {
        module->info_.error = decodedFile->errorString();
        return module;
    }
    std::int64_t decodedSize = 0;
    bool sawZeroLengthLine = false;
    bool sawEnd = false;
    QByteArray line;
    std::array<std::uint8_t, 45> decodedLine{};
    while (readDeviceLine(input, &line)) {
        if (line == "end") {
            sawEnd = true;
            break;
        }
        int lineSize = 0;
        if (sawZeroLengthLine || !decodeLine(line, &decodedLine, &lineSize)) {
            module->info_.error = QStringLiteral("Invalid UUencode data line");
            return module;
        }
        if (line.at(0) == ' ' || line.at(0) == '`') sawZeroLengthLine = true;
        if (decodedSize > std::numeric_limits<std::int64_t>::max() - lineSize ||
            !writeAll(*decodedFile, decodedLine.data(), lineSize)) {
            module->info_.error = QStringLiteral("Cannot store decoded UUencode payload");
            return module;
        }
        decodedSize += lineSize;
    }
    if (!sawEnd) {
        module->info_.error = QStringLiteral("UUencode end marker is missing");
        return module;
    }
    if (!trailingWhitespace(input)) {
        module->info_.error = QStringLiteral("Unexpected data after UUencode end marker");
        return module;
    }
    if (!decodedFile->flush() || !decodedFile->seek(0)) {
        module->info_.error = decodedFile->errorString();
        return module;
    }

    ResourceEntry entry;
    entry.name = payloadName(encodedName);
    entry.type = QStringLiteral("UUE_FILE");
    entry.format = ModuleFormat::UUE;
    entry.isEmbeddedFile = true;
    entry.dataSize = quint64(decodedSize);
    entry.content = std::make_shared<UuencodeFileStore>(decodedFile, decodedSize);
    entry.hierarchyPath = QStringList() << entry.name;
    module->resources_.push_back(std::move(entry));
    return module;
}

}  // namespace peare
