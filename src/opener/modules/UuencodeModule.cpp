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

bool parseHeader(const QByteArray& line, QByteArray* name, bool* base64,
                 bool* encodedName)
{
    if (!name || !base64 || !encodedName) return false;
    qsizetype modeStart = 0;
    if (line.startsWith("begin-base64-encoded ")) {
        modeStart = 21;
        *base64 = true;
        *encodedName = true;
    } else if (line.startsWith("begin-base64 ")) {
        modeStart = 13;
        *base64 = true;
        *encodedName = false;
    } else if (line.startsWith("begin-encoded ")) {
        modeStart = 14;
        *base64 = false;
        *encodedName = true;
    } else if (line.startsWith("begin ")) {
        modeStart = 6;
        *base64 = false;
        *encodedName = false;
    } else {
        return false;
    }
    const qsizetype modeEnd = line.indexOf(' ', modeStart);
    if (modeEnd - modeStart < 3 || modeEnd - modeStart > 4 || modeEnd + 1 >= line.size())
        return false;
    for (qsizetype i = modeStart; i < modeEnd; ++i)
        if (line.at(i) < '0' || line.at(i) > '7') return false;
    *name = line.mid(modeEnd + 1).trimmed();
    if (*encodedName && name->contains(' ')) return false;
    return !name->isEmpty();
}

bool readDeviceLine(QIODevice& input, QByteArray* line);

bool findHeader(QIODevice& input, QByteArray* name, bool* base64, bool* encodedName)
{
    QByteArray line;
    while (readDeviceLine(input, &line))
        if (parseHeader(line, name, base64, encodedName)) return true;
    return false;
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

int base64Value(unsigned char value)
{
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '+') return 62;
    if (value == '/') return 63;
    return -1;
}

bool decodeBase64Line(const QByteArray& line, std::array<std::uint8_t, 57>* output,
                      int* outputSize, bool* hasPadding)
{
    if (!output || !outputSize || !hasPadding || line.isEmpty() || line.size() % 4 != 0 ||
        line.size() > 76)
        return false;
    int decoded = 0;
    for (qsizetype i = 0; i < line.size(); i += 4) {
        const int a = base64Value(static_cast<unsigned char>(line.at(i)));
        const int b = base64Value(static_cast<unsigned char>(line.at(i + 1)));
        const char c = line.at(i + 2);
        const char d = line.at(i + 3);
        if (a < 0 || b < 0 || *hasPadding) return false;
        (*output)[std::size_t(decoded++)] = std::uint8_t((a << 2) | (b >> 4));
        if (c == '=') {
            if (d != '=' || i + 4 != line.size() || (b & 0x0f) != 0) return false;
            *hasPadding = true;
            continue;
        }
        const int cv = base64Value(static_cast<unsigned char>(c));
        if (cv < 0) return false;
        (*output)[std::size_t(decoded++)] = std::uint8_t((b << 4) | (cv >> 2));
        if (d == '=') {
            if (i + 4 != line.size() || (cv & 0x03) != 0) return false;
            *hasPadding = true;
            continue;
        }
        const int dv = base64Value(static_cast<unsigned char>(d));
        if (dv < 0) return false;
        (*output)[std::size_t(decoded++)] = std::uint8_t((cv << 6) | dv);
    }
    *outputSize = decoded;
    return true;
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

bool decodeEncodedName(QByteArray* name)
{
    if (!name || name->isEmpty() || name->size() % 4 != 0) return false;
    QByteArray decoded;
    decoded.reserve(name->size() / 4 * 3);
    bool padded = false;
    for (qsizetype i = 0; i < name->size(); i += 4) {
        const int a = base64Value(static_cast<unsigned char>(name->at(i)));
        const int b = base64Value(static_cast<unsigned char>(name->at(i + 1)));
        const char c = name->at(i + 2);
        const char d = name->at(i + 3);
        if (a < 0 || b < 0 || padded) return false;
        decoded.append(char((a << 2) | (b >> 4)));
        if (c == '=') {
            if (d != '=' || i + 4 != name->size() || (b & 0x0f) != 0) return false;
            padded = true;
            continue;
        }
        const int cv = base64Value(static_cast<unsigned char>(c));
        if (cv < 0) return false;
        decoded.append(char((b << 4) | (cv >> 2)));
        if (d == '=') {
            if (i + 4 != name->size() || (cv & 0x03) != 0) return false;
            padded = true;
            continue;
        }
        const int dv = base64Value(static_cast<unsigned char>(d));
        if (dv < 0) return false;
        decoded.append(char((cv << 6) | dv));
    }
    *name = std::move(decoded);
    return !name->isEmpty();
}

}  // namespace

bool UuencodeModule::hasHeader(const QByteArray& data)
{
    qsizetype position = 0;
    QByteArray header;
    QByteArray body;
    QByteArray name;
    bool base64 = false;
    bool encodedName = false;
    bool found = false;
    while (readLine(data, &position, &header)) {
        if (parseHeader(header, &name, &base64, &encodedName)) {
            found = true;
            break;
        }
    }
    if (!found || !readLine(data, &position, &body)) return false;
    if ((!base64 && body == "end") || (base64 && body == "====")) return true;
    if (base64) {
        std::array<std::uint8_t, 57> decoded{};
        int decodedSize = 0;
        bool hasPadding = false;
        return decodeBase64Line(body, &decoded, &decodedSize, &hasPadding);
    }
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

    QByteArray encodedName;
    bool base64 = false;
    bool hasEncodedName = false;
    if (!findHeader(input, &encodedName, &base64, &hasEncodedName)) {
        module->info_.error = QStringLiteral("Invalid UUencode begin header");
        return module;
    }
    if (hasEncodedName && !decodeEncodedName(&encodedName)) {
        module->info_.error = QStringLiteral("Invalid UUencode encoded filename");
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
    bool hasBase64Padding = false;
    QByteArray line;
    std::array<std::uint8_t, 45> decodedLine{};
    std::array<std::uint8_t, 57> decodedBase64Line{};
    while (readDeviceLine(input, &line)) {
        if ((!base64 && line == "end") || (base64 && line == "====")) {
            sawEnd = true;
            break;
        }
        int lineSize = 0;
        const std::uint8_t* decodedData = nullptr;
        if (base64) {
            if (line.isEmpty() || !decodeBase64Line(
                    line, &decodedBase64Line, &lineSize, &hasBase64Padding)) {
                module->info_.error = QStringLiteral("Invalid UUencode Base64 data line");
                return module;
            }
            decodedData = decodedBase64Line.data();
        } else {
            if (sawZeroLengthLine || !decodeLine(line, &decodedLine, &lineSize)) {
                module->info_.error = QStringLiteral("Invalid UUencode data line");
                return module;
            }
            if (line.at(0) == ' ' || line.at(0) == '`') sawZeroLengthLine = true;
            decodedData = decodedLine.data();
        }
        if (decodedSize > std::numeric_limits<std::int64_t>::max() - lineSize ||
            !writeAll(*decodedFile, decodedData, lineSize)) {
            module->info_.error = QStringLiteral("Cannot store decoded UUencode payload");
            return module;
        }
        decodedSize += lineSize;
    }
    if (!sawEnd) {
        module->info_.error = QStringLiteral("UUencode end marker is missing");
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
