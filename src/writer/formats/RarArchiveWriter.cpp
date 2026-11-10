#include "RarArchiveWriter.h"
#include "../../common/rar/Rar4LzDecoder.h"
#include "../../common/rar/Rar4Crypto.h"
#include "../../common/rar/Rar5Crypto.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryFile>
#include <QVector>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>

namespace peare {
namespace writer {
namespace {

const std::uint8_t kRar4Marker[] = {0x52, 0x61, 0x72, 0x21, 0x1a, 0x07, 0x00};
const std::uint8_t kRar5Marker[] = {0x52, 0x61, 0x72, 0x21, 0x1a, 0x07, 0x01, 0x00};
const qint64 kCopyChunk = 1024 * 1024;

quint32 crc32(const QByteArray& bytes)
{
    quint32 crc = 0xffffffffU;
    for (char raw : bytes) {
        crc ^= static_cast<unsigned char>(raw);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xedb88320U : 0U);
    }
    return crc ^ 0xffffffffU;
}

quint32 crc32(const char* data, qint64 size)
{
    quint32 crc = 0xffffffffU;
    for (qint64 i = 0; i < size; ++i) {
        crc ^= static_cast<unsigned char>(data[i]);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xedb88320U : 0U);
    }
    return crc ^ 0xffffffffU;
}

quint32 rar4RecoveryCrc32(const QByteArray& data)
{
    quint32 crc = 0xf0000000U ^ 0xffffffffU;
    for (char raw : data) {
        crc ^= static_cast<unsigned char>(raw);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xedb88320U : 0U);
    }
    return crc ^ 0xffffffffU;
}

quint32 crc32Update(quint32 crc, const char* data, qint64 size)
{
    for (qint64 i = 0; i < size; ++i) {
        crc ^= static_cast<unsigned char>(data[i]);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xedb88320U : 0U);
    }
    return crc;
}

bool crc32Range(QFile* file, quint64 offset, quint64 size, quint32* result)
{
    if (!file || !result || offset > quint64(std::numeric_limits<qint64>::max()) ||
        size > quint64(std::numeric_limits<qint64>::max()) - offset ||
        !file->seek(qint64(offset))) return false;
    QByteArray buffer(64 * 1024, Qt::Uninitialized);
    quint32 crc = 0xffffffffU;
    quint64 copied = 0;
    while (copied < size) {
        const qint64 request = qint64(std::min<quint64>(
            quint64(buffer.size()), size - copied));
        if (file->read(buffer.data(), request) != request) return false;
        crc = crc32Update(crc, buffer.constData(), request);
        copied += quint64(request);
    }
    *result = crc ^ 0xffffffffU;
    return true;
}

quint64 rar5Crc64Update(quint64 crc, const char* data, qint64 size)
{
    for (qint64 i = 0; i < size; ++i) {
        crc ^= static_cast<unsigned char>(data[i]);
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U)
                ? 0xc96c5795d7870f42ULL : 0ULL);
    }
    return crc;
}

quint64 rar5Crc64(const char* data, qint64 size)
{
    return rar5Crc64Update(0xffffffffffffffffULL, data, size) ^
        0xffffffffffffffffULL;
}

quint16 rar5GfMultiply(quint16 left, quint16 right)
{
    quint32 a = left;
    quint32 b = right;
    quint32 result = 0;
    while (b) {
        if (b & 1U) result ^= a;
        b >>= 1;
        a <<= 1;
        if (a & 0x10000U) a ^= 0x1100bU;
    }
    return quint16(result);
}

quint16 rar5GfInverse(quint16 value)
{
    if (!value) return 0;
    quint16 result = 1;
    quint16 base = value;
    unsigned exponent = 65534U;
    while (exponent) {
        if (exponent & 1U) result = rar5GfMultiply(result, base);
        base = rar5GfMultiply(base, base);
        exponent >>= 1;
    }
    return result;
}

quint32 rar4DosTime(const QFileInfo& info)
{
    const QDateTime modified = info.lastModified().toLocalTime();
    if (!modified.isValid()) return 0;
    const QDate date = modified.date();
    const QTime time = modified.time();
    const quint32 year = quint32(std::max(1980, std::min(2107, date.year())) - 1980);
    const quint32 dosDate = (year << 9) | (quint32(date.month()) << 5) |
        quint32(date.day());
    const quint32 dosTime = (quint32(time.hour()) << 11) |
        (quint32(time.minute()) << 5) | quint32(time.second() / 2);
    return (dosDate << 16) | dosTime;
}

void append16(QByteArray* output, quint16 value)
{
    output->append(char(value & 0xffU));
    output->append(char((value >> 8) & 0xffU));
}

void append32(QByteArray* output, quint32 value)
{
    append16(output, quint16(value));
    append16(output, quint16(value >> 16));
}

void append64(QByteArray* output, quint64 value)
{
    append32(output, quint32(value));
    append32(output, quint32(value >> 32));
}

quint16 read16(const char* data)
{
    return quint16(static_cast<unsigned char>(data[0])) |
        quint16(static_cast<unsigned char>(data[1]) << 8);
}

QString nextRar4VolumePath(const QString& path)
{
    const int part = path.lastIndexOf(QLatin1String(".part"), -1,
                                      Qt::CaseInsensitive);
    const int rar = path.lastIndexOf(QLatin1String(".rar"), -1,
                                     Qt::CaseInsensitive);
    if (part >= 0 && rar > part + 5) {
        bool ok = false;
        const int number = path.mid(part + 5, rar - part - 5).toInt(&ok);
        if (!ok || number < 1) return QString();
        return path.left(part + 5) + QString::number(number + 1).rightJustified(
            rar - part - 5, QLatin1Char('0')) + path.mid(rar);
    }
    const QString lower = path.toLower();
    const int extension = lower.lastIndexOf(QLatin1Char('.'));
    if (extension < 0) return QString();
    const QString suffix = lower.mid(extension + 1);
    if (suffix == QLatin1String("rar"))
        return path.left(extension) + QLatin1String(".r00");
    if (suffix.size() == 3 && suffix.at(0) == QLatin1Char('r') &&
        suffix.at(1).isDigit() && suffix.at(2).isDigit()) {
        bool ok = false;
        const int number = suffix.mid(1).toInt(&ok);
        if (!ok || number >= 99) return QString();
        return path.left(extension + 2) + QString::number(number + 1)
            .rightJustified(2, QLatin1Char('0'));
    }
    return QString();
}

QString rar4VolumePath(const QString& firstPath, int index)
{
    const int part = firstPath.lastIndexOf(QLatin1String(".part"), -1,
                                           Qt::CaseInsensitive);
    const int rar = firstPath.lastIndexOf(QLatin1String(".rar"), -1,
                                          Qt::CaseInsensitive);
    if (part >= 0 && rar > part + 5) {
        bool ok = false;
        const int first = firstPath.mid(part + 5, rar - part - 5).toInt(&ok);
        if (!ok || first < 1) return QString();
        const int digits = rar - part - 5;
        return firstPath.left(part + 5) + QString::number(first + index)
            .rightJustified(digits, QLatin1Char('0')) + firstPath.mid(rar);
    }
    if (index == 0) return firstPath;
    const QString lower = firstPath.toLower();
    const int extension = lower.lastIndexOf(QLatin1Char('.'));
    if (extension < 0) return QString();
    if (lower.mid(extension + 1) == QLatin1String("rar"))
        return firstPath.left(extension) + QLatin1String(".r") +
            QString::number(index - 1).rightJustified(2, QLatin1Char('0'));
    return QString();
}

QByteArray rar4WithPackedSize(QByteArray header, quint64 size,
                              bool splitBefore, bool splitAfter)
{
    quint16 flags = read16(header.constData() + 3);
    flags &= quint16(~0x0003U);
    if (splitBefore) flags |= 0x0001U;
    if (splitAfter) flags |= 0x0002U;
    header[3] = char(flags & 0xffU);
    header[4] = char(flags >> 8);
    for (int i = 0; i < 4; ++i) header[7 + i] = char((size >> (i * 8)) & 0xffU);
    if (flags & 0x0100U)
        for (int i = 0; i < 4; ++i) header[32 + i] = char((size >> (32 + i * 8)) & 0xffU);
    const quint16 checksum = quint16(crc32(header.constData() + 2,
                                           header.size() - 2));
    header[0] = char(checksum & 0xffU);
    header[1] = char(checksum >> 8);
    return header;
}

QByteArray rar4EndHeader(bool hasNext)
{
    QByteArray header;
    append16(&header, 0);
    header.append(char(0x7b));
    append16(&header, hasNext ? 1 : 0);
    append16(&header, 7);
    const quint16 checksum = quint16(crc32(header.mid(2)) & 0xffffU);
    header[0] = char(checksum & 0xffU);
    header[1] = char(checksum >> 8);
    return header;
}

bool makeRar4RecoveryBlock(QFile* archive, quint64 protectedSize,
                          quint32 recoverySectors, QByteArray* header,
                          QByteArray* payload, QString* error)
{
    if (!archive || !header || !payload || !recoverySectors ||
        protectedSize > quint64(std::numeric_limits<qint64>::max())) {
        if (error) *error = QStringLiteral("The RAR4 recovery parameters are invalid.");
        return false;
    }
    const quint64 dataSectors = (protectedSize + 511U) / 512U;
    const quint64 parityBytes = quint64(recoverySectors) * 512U;
    if (dataSectors > std::numeric_limits<quint32>::max() ||
        parityBytes > std::numeric_limits<quint32>::max() ||
        dataSectors > (std::numeric_limits<quint32>::max() - parityBytes) / 2U) {
        if (error) *error = QStringLiteral("The RAR4 recovery record is too large.");
        return false;
    }
    const quint64 payloadSize = dataSectors * 2U + quint64(recoverySectors) * 512U;
    if (payloadSize > quint64(std::numeric_limits<int>::max())) {
        if (error) *error = QStringLiteral("The RAR4 recovery record exceeds the staging limit.");
        return false;
    }
    payload->clear();
    payload->reserve(int(payloadSize));
    std::vector<std::array<unsigned char, 512> > parity;
    try {
        parity.resize(recoverySectors);
        for (auto& sector : parity) sector.fill(0);
    } catch (...) {
        if (error) *error = QStringLiteral("Could not allocate RAR4 recovery sectors.");
        return false;
    }
    QByteArray sector(512, char(0));
    for (quint64 index = 0; index < dataSectors; ++index) {
        sector.fill(char(0));
        const quint64 offset = index * 512U;
        const qint64 amount = qint64(std::min<quint64>(512U,
            protectedSize - offset));
        if (!archive->seek(qint64(offset)) || archive->read(sector.data(), amount) != amount) {
            if (error) *error = archive->errorString().isEmpty()
                ? QStringLiteral("Could not read protected RAR4 archive data.")
                : archive->errorString();
            return false;
        }
        const quint16 tag = quint16(~crc32(sector.constData(), 512));
        append16(payload, tag);
        auto& target = parity[std::size_t(index % recoverySectors)];
        for (int byte = 0; byte < 512; ++byte)
            target[std::size_t(byte)] ^= static_cast<unsigned char>(sector.at(byte));
    }
    for (const auto& sectorData : parity)
        payload->append(reinterpret_cast<const char*>(sectorData.data()), 512);

    QByteArray block;
    append16(&block, 0);
    block.append(char(0x7a));
    append16(&block, 0xc000U);
    append16(&block, 54);
    append32(&block, quint32(payload->size()));
    append32(&block, quint32(payload->size()));
    block.append(char(2));
    append32(&block, rar4RecoveryCrc32(*payload));
    append32(&block, rar4DosTime(QFileInfo(archive->fileName())));
    block.append(char(29));
    block.append(char(0x30));
    append16(&block, 2);
    append32(&block, 0);
    block.append("RRProtect+", 10);
    append32(&block, recoverySectors);
    append32(&block, quint32(dataSectors));
    append32(&block, 0);
    const quint16 checksum = quint16(crc32(block.constData() + 2,
                                           block.size() - 2));
    block[0] = char(checksum & 0xffU);
    block[1] = char(checksum >> 8);
    *header = block;
    return true;
}

quint32 read32(const char* data)
{
    return quint32(read16(data)) |
        (quint32(read16(data + 2)) << 16);
}

std::uint32_t readLe32(const std::uint8_t* data)
{
    return std::uint32_t(data[0]) | (std::uint32_t(data[1]) << 8) |
        (std::uint32_t(data[2]) << 16) | (std::uint32_t(data[3]) << 24);
}

#include "../../common/rar/Rar5Decoder.inl"

bool readRar5Vint(const QByteArray& bytes, int* offset, quint64* value);

struct Rar5EncryptionParameters {
    bool present = false;
    quint8 flags = 0;
    quint8 kdfCount = 0;
    std::array<std::uint8_t, 16> salt = {};
    std::array<std::uint8_t, 16> iv = {};
};

bool parseRar5EncryptionRecord(const QByteArray& extra,
                               Rar5EncryptionParameters* parameters)
{
    if (!parameters) return false;
    *parameters = Rar5EncryptionParameters();
    int offset = 0;
    while (offset < extra.size()) {
        quint64 recordSize = 0;
        if (!readRar5Vint(extra, &offset, &recordSize) || !recordSize ||
            recordSize > quint64(extra.size() - offset)) return false;
        const int recordEnd = offset + int(recordSize);
        quint64 recordType = 0;
        if (!readRar5Vint(extra, &offset, &recordType) || offset > recordEnd)
            return false;
        if (recordType == 1) {
            quint64 version = 0;
            quint64 flags = 0;
            if (parameters->present ||
                !readRar5Vint(extra, &offset, &version) || version != 0 ||
                !readRar5Vint(extra, &offset, &flags) || (flags & ~3ULL) ||
                offset >= recordEnd) return false;
            parameters->kdfCount = quint8(extra.at(offset++));
            if (offset > recordEnd - 32) return false;
            std::memcpy(parameters->salt.data(), extra.constData() + offset, 16);
            offset += 16;
            std::memcpy(parameters->iv.data(), extra.constData() + offset, 16);
            offset += 16;
            if (flags & 1U) {
                if (offset > recordEnd - 12) return false;
                offset += 12;
            }
            parameters->flags = quint8(flags);
            parameters->present = true;
        }
        if (offset > recordEnd) return false;
        offset = recordEnd;
    }
    return true;
}

bool readExact(QFile* input, qint64 offset, qint64 size, QByteArray* output)
{
    if (!input || !output || offset < 0 || size < 0 || !input->seek(offset))
        return false;
    output->resize(int(size));
    qint64 done = 0;
    while (done < size) {
        const qint64 count = input->read(output->data() + done, size - done);
        if (count <= 0) return false;
        done += count;
    }
    return true;
}

bool readFileVint(QFile* input, qint64* position, quint64* value,
                  QByteArray* encoded)
{
    if (!input || !position || !value) return false;
    QByteArray bytes;
    quint64 result = 0;
    unsigned shift = 0;
    for (unsigned count = 0; count < 10; ++count) {
        QByteArray byte;
        if (!readExact(input, *position, 1, &byte)) return false;
        ++*position;
        bytes.append(byte);
        const quint8 current = quint8(byte.at(0));
        if (shift == 63 && (current & 0x7eU)) return false;
        result |= quint64(current & 0x7fU) << shift;
        if (!(current & 0x80U)) {
            *value = result;
            if (encoded) *encoded = bytes;
            return true;
        }
        shift += 7;
    }
    return false;
}

bool readRar5Vint(const QByteArray& bytes, int* offset, quint64* value)
{
    if (!offset || !value) return false;
    quint64 result = 0;
    unsigned shift = 0;
    for (unsigned count = 0; count < 10; ++count) {
        if (*offset < 0 || *offset >= bytes.size() || shift >= 64) return false;
        const quint8 byte = quint8(bytes.at((*offset)++));
        if (shift == 63 && (byte & 0x7eU)) return false;
        result |= quint64(byte & 0x7fU) << shift;
        if (!(byte & 0x80U)) {
            *value = result;
            return true;
        }
        shift += 7;
    }
    return false;
}

void appendVint(QByteArray* output, quint64 value)
{
    do {
        quint8 byte = quint8(value & 0x7fU);
        value >>= 7;
        if (value) byte |= 0x80U;
        output->append(char(byte));
    } while (value);
}

bool decodeRar4UnicodeName(const QByteArray& encoded, QByteArray* utf8Name)
{
    if (!utf8Name) return false;
    const int separator = encoded.indexOf('\0');
    if (separator < 0) {
        *utf8Name = encoded;
        return true;
    }
    if (separator + 1 >= encoded.size()) return false;

    const QByteArray fallback = encoded.left(separator);
    const quint8 highByte = quint8(encoded.at(separator + 1));
    int source = separator + 2;
    quint8 flagByte = 0;
    unsigned flagBits = 0;
    QVector<ushort> codeUnits;
    codeUnits.reserve(encoded.size());
    while (source < encoded.size()) {
        if (!flagBits) {
            flagByte = quint8(encoded.at(source++));
            flagBits = 8;
        }
        flagBits -= 2;
        const unsigned mode = (flagByte >> flagBits) & 3U;
        if (mode < 2U) {
            if (source >= encoded.size()) return false;
            const ushort lowByte = quint8(encoded.at(source++));
            codeUnits.append(ushort((mode ? highByte : 0U) << 8) | lowByte);
        } else if (mode == 2U) {
            if (source > encoded.size() - 2) return false;
            const ushort lowByte = quint8(encoded.at(source++));
            const ushort high = quint8(encoded.at(source++));
            codeUnits.append(ushort((high << 8) | lowByte));
        } else {
            if (source >= encoded.size()) return false;
            const quint8 runByte = quint8(encoded.at(source++));
            const bool corrected = (runByte & 0x80U) != 0;
            quint8 correction = 0;
            if (corrected) {
                if (source >= encoded.size()) return false;
                correction = quint8(encoded.at(source++));
            }
            const unsigned run = unsigned(runByte & 0x7fU) + 2U;
            if (!corrected) {
                const unsigned uncorrectedRun = unsigned(runByte) + 2U;
                if (uncorrectedRun > unsigned(fallback.size()) -
                        std::min<unsigned>(unsigned(codeUnits.size()),
                                           unsigned(fallback.size())))
                    return false;
                for (unsigned index = 0; index < uncorrectedRun; ++index)
                    codeUnits.append(ushort(quint8(fallback.at(codeUnits.size()))));
            } else {
                if (run > unsigned(fallback.size()) -
                        std::min<unsigned>(unsigned(codeUnits.size()),
                                           unsigned(fallback.size())))
                    return false;
                for (unsigned index = 0; index < run; ++index) {
                    const quint8 lowByte = quint8(fallback.at(codeUnits.size())) +
                                           correction;
                    codeUnits.append(ushort((highByte << 8) | lowByte));
                }
            }
        }
        if (codeUnits.size() > 65535) return false;
    }
    if (codeUnits.isEmpty()) return false;
    const QString decoded = QString::fromUtf16(codeUnits.constData(), codeUnits.size());
    if (decoded.contains(QChar(0))) return false;
    *utf8Name = decoded.toUtf8();
    return !utf8Name->isEmpty();
}

class Rar5BitWriter final {
public:
    void write(unsigned count, quint32 value)
    {
        for (unsigned index = 0; index < count; ++index) {
            const unsigned shift = count - index - 1;
            current_ = quint8((current_ << 1) | ((value >> shift) & 1U));
            if (++used_ == 8) {
                bytes_.append(char(current_));
                current_ = 0;
                used_ = 0;
            }
        }
    }

    QByteArray finish()
    {
        const unsigned remaining = used_ ? 8U - used_ : 0U;
        if (used_) bytes_.append(char(current_ << remaining));
        used_ = 0;
        return bytes_;
    }

    unsigned usedBitsInFinalByte() const { return used_ ? used_ : 8U; }

private:
    QByteArray bytes_;
    quint8 current_ = 0;
    unsigned used_ = 0;
};

bool writeAll(QIODevice* output, const char* data, qint64 size)
{
    qint64 offset = 0;
    while (offset < size) {
        const qint64 written = output->write(data + offset, size - offset);
        if (written <= 0) return false;
        offset += written;
    }
    return true;
}

class Rar4BitWriter final {
public:
    explicit Rar4BitWriter(QIODevice* output) : output_(output) {}

    bool write(unsigned count, quint32 value)
    {
        if (!output_ || count > 32) return false;
        for (unsigned index = 0; index < count; ++index) {
            const unsigned shift = count - index - 1;
            current_ = quint8((current_ << 1) | ((value >> shift) & 1U));
            if (++used_ == 8) {
                if (!writeAll(output_, reinterpret_cast<const char*>(&current_), 1))
                    return false;
                current_ = 0;
                used_ = 0;
            }
        }
        return true;
    }

    bool finish()
    {
        if (!used_) return true;
        current_ = quint8(current_ << (8U - used_));
        const bool result = writeAll(output_,
            reinterpret_cast<const char*>(&current_), 1);
        current_ = 0;
        used_ = 0;
        return result;
    }

private:
    QIODevice* output_;
    quint8 current_ = 0;
    unsigned used_ = 0;
};

bool copyFile(QFile* input, QIODevice* output)
{
    if (!input || !output) return false;
    QByteArray buffer(int(kCopyChunk), Qt::Uninitialized);
    while (true) {
        const qint64 count = input->read(buffer.data(), buffer.size());
        if (count < 0) return false;
        if (!count) return input->error() == QFileDevice::NoError;
        if (!writeAll(output, buffer.constData(), count)) return false;
    }
}

bool copyRange(QFile* input, QIODevice* output, qint64 offset, quint64 size)
{
    if (!input || !output || offset < 0 ||
        size > quint64(std::numeric_limits<qint64>::max()) || !input->seek(offset))
        return false;
    QByteArray buffer(int(kCopyChunk), Qt::Uninitialized);
    quint64 copied = 0;
    while (copied < size) {
        const qint64 request = qint64(std::min<quint64>(
            quint64(buffer.size()), size - copied));
        const qint64 count = input->read(buffer.data(), request);
        if (count != request || !writeAll(output, buffer.constData(), count))
            return false;
        copied += quint64(count);
    }
    return true;
}

bool copyPath(const QString& source, const QString& destination)
{
    QFile input(source);
    QFile output(destination);
    if (!input.open(QIODevice::ReadOnly) ||
        !output.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        !copyFile(&input, &output) || !output.flush())
        return false;
    return output.setFileTime(input.fileTime(QFileDevice::FileModificationTime),
                              QFileDevice::FileModificationTime);
}

template <typename Entries>
bool conflictsWithEntry(const Entries& entries, const QByteArray& name,
                        bool directory)
{
    const QByteArray prefix = name + '/';
    for (const auto& entry : entries) {
        if (entry.name == name) return true;
        if (!directory && entry.name.startsWith(prefix)) return true;
        const QByteArray entryPrefix = entry.name + '/';
        if (!entry.directory && name.startsWith(entryPrefix)) return true;
    }
    return false;
}

QByteArray rar5Block(const QByteArray& body)
{
    QByteArray encoded;
    appendVint(&encoded, quint64(body.size()));
    encoded.append(body);
    QByteArray block;
    append32(&block, crc32(encoded));
    block.append(encoded);
    return block;
}

QByteArray rar5EndHeader(bool hasNext)
{
    QByteArray body;
    appendVint(&body, 5);
    appendVint(&body, 0);
    appendVint(&body, hasNext ? 1 : 0);
    return rar5Block(body);
}

QByteArray rar5VolumeMainHeader(int volumeIndex, bool recovery,
                                quint64 recoveryOffset = 0)
{
    QByteArray body;
    appendVint(&body, 1);
    QByteArray extra;
    if (recovery) {
        QByteArray locator;
        appendVint(&locator, 1);
        appendVint(&locator, 2);
        appendVint(&locator, recoveryOffset);
        appendVint(&extra, quint64(locator.size()));
        extra.append(locator);
    }
    appendVint(&body, recovery ? 5 : 0);
    if (recovery) appendVint(&body, quint64(extra.size()));
    appendVint(&body, quint64(1U | (recovery ? 8U : 0U) |
                              (volumeIndex ? 2U : 0U)));
    if (volumeIndex) appendVint(&body, quint64(volumeIndex));
    body.append(extra);
    return rar5Block(body);
}

bool rewriteRar5VolumeRecoveryLocator(const QString& path, int volumeIndex,
                                      QString* error)
{
    QFile source(path);
    const QByteArray marker(reinterpret_cast<const char*>(kRar5Marker),
                            int(sizeof(kRar5Marker)));
    QByteArray actualMarker;
    qint64 headerPosition = marker.size();
    QByteArray headerCrc;
    quint64 headerBodySize = 0;
    QByteArray encodedSize;
    if (!source.open(QIODevice::ReadOnly) ||
        !readExact(&source, 0, marker.size(), &actualMarker) ||
        actualMarker != marker ||
        !readExact(&source, headerPosition, 4, &headerCrc)) {
        if (error) *error = source.errorString().isEmpty()
            ? QStringLiteral("The staged RAR5 volume header is invalid.")
            : source.errorString();
        return false;
    }
    headerPosition += 4;
    if (!readFileVint(&source, &headerPosition, &headerBodySize, &encodedSize) ||
        headerBodySize > quint64(source.size() - headerPosition)) {
        if (error) *error = QStringLiteral("The staged RAR5 volume header is truncated.");
        return false;
    }
    const qint64 oldHeaderEnd = headerPosition + qint64(headerBodySize);
    const quint64 tailSize = quint64(source.size() - oldHeaderEnd);
    quint64 recoveryOffset = 0;
    QByteArray replacementHeader;
    for (int attempt = 0; attempt < 4; ++attempt) {
        replacementHeader = rar5VolumeMainHeader(volumeIndex, true,
                                                  recoveryOffset);
        const quint64 nextOffset = quint64(replacementHeader.size()) + tailSize;
        if (nextOffset == recoveryOffset) break;
        recoveryOffset = nextOffset;
    }
    replacementHeader = rar5VolumeMainHeader(volumeIndex, true,
                                              recoveryOffset);
    if (quint64(replacementHeader.size()) + tailSize != recoveryOffset) {
        if (error) *error = QStringLiteral("The RAR5 recovery locator offset did not stabilize.");
        return false;
    }

    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly) ||
        !writeAll(&output, marker.constData(), marker.size()) ||
        !writeAll(&output, replacementHeader.constData(), replacementHeader.size()) ||
        !source.seek(oldHeaderEnd) || !copyFile(&source, &output)) {
        if (error) *error = output.errorString().isEmpty()
            ? source.errorString() : output.errorString();
        output.cancelWriting();
        return false;
    }
    source.close();
    if (!output.commit()) {
        if (error) *error = output.errorString();
        return false;
    }
    return true;
}

QByteArray rar5MainPrefix(const QByteArray& marker, bool recovery)
{
    QByteArray body;
    appendVint(&body, 1);
    appendVint(&body, recovery ? 1 : 0);
    QByteArray extra;
    if (recovery) {
        QByteArray locator;
        appendVint(&locator, 1);
        appendVint(&locator, 2);
        appendVint(&locator, 0);
        appendVint(&extra, quint64(locator.size()));
        extra.append(locator);
    }
    if (recovery) appendVint(&body, quint64(extra.size()));
    appendVint(&body, recovery ? 8 : 0);
    body.append(extra);
    return marker + rar5Block(body);
}

QByteArray rar5RecoveryHeader(quint64 payloadSize, quint8 recoveryPercent)
{
    QByteArray extra;
    appendVint(&extra, 2);
    appendVint(&extra, 7);
    appendVint(&extra, recoveryPercent);
    QByteArray body;
    appendVint(&body, 3);
    appendVint(&body, 7);
    appendVint(&body, quint64(extra.size()));
    appendVint(&body, payloadSize);
    appendVint(&body, 0);
    appendVint(&body, payloadSize);
    appendVint(&body, 0);
    appendVint(&body, 0);
    appendVint(&body, 0);
    appendVint(&body, 2);
    body.append("RR", 2);
    body.append(extra);
    return rar5Block(body);
}

quint64 rar5RecoveryPayloadSize(quint64 archiveSize, quint8 recoveryPercent)
{
    if (!archiveSize || !recoveryPercent || recoveryPercent > 100) return 0;
    const quint64 dataShards = archiveSize >= 200U * 1024U ? 200U :
        std::max<quint64>(1U, (archiveSize + 1023U) / 1024U);
    quint64 recoveryShards = (quint64(recoveryPercent) * dataShards) / 100U;
    if (!recoveryShards && archiveSize < 200U * 1024U) recoveryShards = 1;
    recoveryShards = std::min(recoveryShards, dataShards);
    quint64 groupSize = (archiveSize + dataShards - 1U) / dataShards;
    if (groupSize & 1U) ++groupSize;
    const quint64 shardSize = dataShards * 8U + 72U + groupSize;
    if (recoveryShards > std::numeric_limits<quint64>::max() / shardSize)
        return 0;
    return recoveryShards * shardSize;
}

bool writeRar5RecoveryPayload(QFile* archive, quint64 archiveSize,
                             quint8 recoveryPercent, QIODevice* output,
                             QString* error)
{
    if (!archive || !output || !archiveSize || !recoveryPercent ||
        recoveryPercent > 100 || archiveSize > quint64(archive->size())) {
        if (error) *error = QStringLiteral("The RAR5 recovery parameters are invalid.");
        return false;
    }
    const quint64 dataShards = archiveSize >= 200U * 1024U ? 200U :
        std::max<quint64>(1U, (archiveSize + 1023U) / 1024U);
    quint64 recoveryShards = (quint64(recoveryPercent) * dataShards) / 100U;
    if (!recoveryShards && archiveSize < 200U * 1024U) recoveryShards = 1;
    recoveryShards = std::min(recoveryShards, dataShards);
    quint64 groupSize = (archiveSize + dataShards - 1U) / dataShards;
    if (groupSize & 1U) ++groupSize;
    if (groupSize > quint64(std::numeric_limits<qint64>::max()) ||
        groupSize > quint64(std::numeric_limits<int>::max())) {
        if (error) *error = QStringLiteral("The RAR5 recovery shard exceeds the current buffer limit.");
        return false;
    }
    const quint64 headerSize = dataShards * 8U + 72U;
    const quint64 shardSize = headerSize + groupSize;
    if (shardSize > std::numeric_limits<quint32>::max() ||
        recoveryShards > std::numeric_limits<quint64>::max() / shardSize) {
        if (error) *error = QStringLiteral("The RAR5 recovery record is too large.");
        return false;
    }
    const quint64 payloadSize = recoveryShards * shardSize;
    std::vector<quint64> shardStates(std::size_t(dataShards), 0);
    QByteArray buffer(64 * 1024, Qt::Uninitialized);
    for (quint64 shard = 0; shard < dataShards; ++shard) {
        const quint64 start = shard * groupSize;
        const quint64 count = start < archiveSize
            ? std::min(groupSize, archiveSize - start) : 0;
        quint64 done = 0;
        while (done < count) {
            const qint64 request = qint64(std::min<quint64>(
                quint64(buffer.size()), count - done));
            if (!archive->seek(qint64(start + done)) ||
                archive->read(buffer.data(), request) != request) {
                if (error) *error = archive->errorString();
                return false;
            }
            shardStates[std::size_t(shard)] = rar5Crc64Update(
                shardStates[std::size_t(shard)], buffer.constData(), request);
            done += quint64(request);
        }
    }
    const quint64 lastShardStart = (dataShards - 1U) * groupSize;
    const quint32 lastShardExtent = lastShardStart < archiveSize
        ? quint32(archiveSize - lastShardStart) : 0U;
    QTemporaryFile parityFile;
    if (!parityFile.open()) {
        if (error) *error = parityFile.errorString();
        return false;
    }
    const quint64 chunkLimit = 64U * 1024U;
    for (quint64 parityIndex = 0; parityIndex < recoveryShards; ++parityIndex) {
        if (!parityFile.resize(0) || !parityFile.seek(0)) {
            if (error) *error = parityFile.errorString();
            return false;
        }
        for (quint64 offset = 0; offset < groupSize;) {
            quint64 amount = std::min(chunkLimit, groupSize - offset);
            if (amount & 1U) --amount;
            QByteArray parity(int(amount), char(0));
            for (quint64 dataIndex = 0; dataIndex < dataShards; ++dataIndex) {
                const quint16 coefficient = rar5GfInverse(quint16(
                    (parityIndex + dataShards) ^ dataIndex));
                const quint64 sourceOffset = dataIndex * groupSize + offset;
                const quint64 available = sourceOffset < archiveSize
                    ? std::min(amount, archiveSize - sourceOffset) : 0;
                QByteArray data(int(amount), char(0));
                if (available && (!archive->seek(qint64(sourceOffset)) ||
                    archive->read(data.data(), qint64(available)) != qint64(available))) {
                    if (error) *error = archive->errorString();
                    return false;
                }
                for (quint64 wordOffset = 0; wordOffset < amount; wordOffset += 2U) {
                    const quint16 word = quint16(static_cast<unsigned char>(
                        data.at(int(wordOffset)))) |
                        quint16(static_cast<unsigned char>(data.at(int(wordOffset + 1U))) << 8);
                    const quint16 encoded = rar5GfMultiply(coefficient, word);
                    parity[int(wordOffset)] = char(
                        static_cast<unsigned char>(parity.at(int(wordOffset))) ^
                        (encoded & 0xffU));
                    parity[int(wordOffset + 1U)] = char(
                        static_cast<unsigned char>(parity.at(int(wordOffset + 1U))) ^
                        (encoded >> 8));
                }
            }
            if (!writeAll(&parityFile, parity.constData(), parity.size())) {
                if (error) *error = parityFile.errorString();
                return false;
            }
            offset += amount;
        }

        QByteArray fields;
        fields.append(char(1));
        fields.append(char(1));
        append64(&fields, 0);
        append32(&fields, lastShardExtent);
        append64(&fields, archiveSize);
        append64(&fields, groupSize);
        append64(&fields, shardSize);
        append16(&fields, quint16(dataShards));
        append16(&fields, quint16(recoveryShards));
        append16(&fields, quint16(parityIndex));
        for (quint64 state : shardStates) append64(&fields, state);
        append64(&fields, 0);
        if (fields.size() != int(headerSize - 20U) ||
            quint64(parityFile.size()) != groupSize || !parityFile.seek(0)) {
            if (error) *error = QStringLiteral("The RAR5 recovery shard layout is inconsistent.");
            return false;
        }
        QByteArray checksumInput;
        append32(&checksumInput, quint32(shardSize));
        append32(&checksumInput, quint32(headerSize));
        checksumInput.append(fields);
        quint64 checksum = rar5Crc64Update(0xffffffffffffffffULL,
            checksumInput.constData(), checksumInput.size());
        quint64 copied = 0;
        while (copied < groupSize) {
            const qint64 request = qint64(std::min<quint64>(
                quint64(buffer.size()), groupSize - copied));
            if (parityFile.read(buffer.data(), request) != request) {
                if (error) *error = parityFile.errorString();
                return false;
            }
            checksum = rar5Crc64Update(checksum, buffer.constData(), request);
            copied += quint64(request);
        }
        checksum ^= 0xffffffffffffffffULL;
        QByteArray chunk;
        chunk.append("{RB}", 4);
        append64(&chunk, checksum);
        append32(&chunk, quint32(shardSize));
        append32(&chunk, quint32(headerSize));
        chunk.append(fields);
        if (!writeAll(output, chunk.constData(), chunk.size()) || !parityFile.seek(0)) {
            if (error) *error = output->errorString().isEmpty()
                ? parityFile.errorString() : output->errorString();
            return false;
        }
        copied = 0;
        while (copied < groupSize) {
            const qint64 request = qint64(std::min<quint64>(
                quint64(buffer.size()), groupSize - copied));
            if (parityFile.read(buffer.data(), request) != request ||
                !writeAll(output, buffer.constData(), request)) {
                if (error) *error = output->errorString().isEmpty()
                    ? parityFile.errorString() : output->errorString();
                return false;
            }
            copied += quint64(request);
        }
    }
    return true;
}

QByteArray rar5WithPackedSize(const QByteArray& header, quint64 packedSize,
                              bool splitBefore, bool splitAfter,
                              bool replaceDataCrc, quint32 dataCrc)
{
    if (header.size() < 6) return QByteArray();
    int bodyOffset = 4;
    quint64 bodySize = 0;
    if (!readRar5Vint(header, &bodyOffset, &bodySize) ||
        bodySize != quint64(header.size() - bodyOffset)) return QByteArray();
    const QByteArray body = header.mid(bodyOffset);
    int cursor = 0;
    quint64 type = 0;
    quint64 flags = 0;
    quint64 extraSize = 0;
    quint64 oldPackedSize = 0;
    if (!readRar5Vint(body, &cursor, &type) || type != 2) return QByteArray();
    const int flagsOffset = cursor;
    if (!readRar5Vint(body, &cursor, &flags) || !(flags & 2U))
        return QByteArray();
    const int fieldsOffset = cursor;
    if ((flags & 1U) && !readRar5Vint(body, &cursor, &extraSize))
        return QByteArray();
    const int dataSizeOffset = cursor;
    if (!readRar5Vint(body, &cursor, &oldPackedSize)) return QByteArray();
    const int dataSizeEnd = cursor;
    flags &= ~quint64(0x18U);
    if (splitBefore) flags |= 0x08U;
    if (splitAfter) flags |= 0x10U;
    QByteArray updatedBody = body.left(flagsOffset);
    appendVint(&updatedBody, flags);
    updatedBody.append(body.mid(fieldsOffset, dataSizeOffset - fieldsOffset));
    appendVint(&updatedBody, packedSize);
    updatedBody.append(body.mid(dataSizeEnd));
    if (replaceDataCrc) {
        int fileCursor = 0;
        quint64 ignored = 0;
        if (!readRar5Vint(updatedBody, &fileCursor, &ignored) ||
            !readRar5Vint(updatedBody, &fileCursor, &ignored) ||
            ((flags & 1U) && !readRar5Vint(updatedBody, &fileCursor, &ignored)) ||
            !readRar5Vint(updatedBody, &fileCursor, &ignored))
            return QByteArray();
        quint64 fileFlags = 0;
        quint64 unpackedSize = 0;
        quint64 attributes = 0;
        if (!readRar5Vint(updatedBody, &fileCursor, &fileFlags) ||
            !readRar5Vint(updatedBody, &fileCursor, &unpackedSize) ||
            !readRar5Vint(updatedBody, &fileCursor, &attributes))
            return QByteArray();
        if (fileFlags & 2U) fileCursor += 4;
        if (!(fileFlags & 4U) || fileCursor < 0 ||
            fileCursor > updatedBody.size() - 4) return QByteArray();
        for (int index = 0; index < 4; ++index)
            updatedBody[fileCursor + index] = char((dataCrc >> (index * 8)) & 0xffU);
        Q_UNUSED(unpackedSize);
        Q_UNUSED(attributes);
    }
    Q_UNUSED(extraSize);
    Q_UNUSED(oldPackedSize);
    return rar5Block(updatedBody);
}

bool writeRar5LiteralBlock(const QByteArray& plain, bool extendedDistances,
                           bool lastBlock, QIODevice* output)
{
    if (!output) return false;
    const std::size_t distanceCount = extendedDistances ? 80U : 64U;
    const std::size_t tableSize = 306U + distanceCount + 16U + 44U;
    Rar5BitWriter bits;
    for (unsigned symbol = 0; symbol < 20; ++symbol)
        bits.write(4, 5U);
    bits.write(5, 8);
    bits.write(5, 17);
    bits.write(7, 127);
    bits.write(5, 17);
    bits.write(7, 106);
    std::size_t zeroCount = tableSize - 256U;
    while (zeroCount) {
        const std::size_t run = std::min<std::size_t>(zeroCount, 138U);
        if (run < 11U) return false;
        bits.write(5, 19);
        bits.write(7, unsigned(run - 11U));
        zeroCount -= run;
    }
    for (unsigned char byte : plain) bits.write(8, byte);
    const unsigned tailBits = bits.usedBitsInFinalByte();
    const QByteArray packed = bits.finish();
    const quint32 blockSize = quint32(packed.size());
    if (blockSize > 0x00ffffffU) return false;
    const quint8 flags = quint8(0x80U | (lastBlock ? 0x40U : 0U) |
                                0x10U | (tailBits - 1U));
    const quint8 checksum = quint8(0x5aU ^ flags ^ blockSize ^
        (blockSize >> 8) ^ (blockSize >> 16));
    char header[5] = {
        char(flags), char(checksum), char(blockSize & 0xffU),
        char((blockSize >> 8) & 0xffU), char((blockSize >> 16) & 0xffU)
    };
    return writeAll(output, header, sizeof(header)) &&
        writeAll(output, packed.constData(), packed.size());
}

bool encodeRar5LiteralFile(QFile* input, QFile* output, quint64 expectedSize,
                           bool extendedDistances, quint32* dataCrc,
                           quint64* packedSize)
{
    if (!input || !output || !dataCrc || !packedSize) return false;
    quint32 crc = 0xffffffffU;
    quint64 totalRead = 0;
    quint64 totalWritten = 0;
    while (totalRead < expectedSize) {
        const qint64 request = qint64(std::min<quint64>(
            quint64(kCopyChunk), expectedSize - totalRead));
        QByteArray plain(int(request), Qt::Uninitialized);
        if (input->read(plain.data(), request) != request) return false;
        crc = crc32Update(crc, plain.constData(), request);
        totalRead += quint64(request);
        const bool lastBlock = totalRead == expectedSize;
        const qint64 before = output->pos();
        if (!writeRar5LiteralBlock(plain, extendedDistances, lastBlock, output))
            return false;
        const qint64 after = output->pos();
        if (before < 0 || after < before) return false;
        totalWritten += quint64(after - before);
    }
    if (!expectedSize && !writeRar5LiteralBlock(QByteArray(), extendedDistances,
                                                 true, output)) return false;
    if (!input->atEnd()) return false;
    *dataCrc = crc ^ 0xffffffffU;
    *packedSize = totalWritten;
    if (!expectedSize) {
        const qint64 finalPosition = output->pos();
        if (finalPosition < 0) return false;
        *packedSize = quint64(finalPosition);
    }
    return true;
}

bool writeRar5Entry(QIODevice* output, const QString& stagedPath,
                    const QString& packedPath, const QByteArray& name,
                    bool directory, unsigned algorithmVersion,
                    unsigned method, QString* error)
{
    QFile input(stagedPath);
    if (!directory && (!input.open(QIODevice::ReadOnly) || input.size() < 0)) {
        if (error) *error = input.errorString();
        return false;
    }
    const quint64 size = directory ? 0 : quint64(input.size());
    quint64 packedSize = size;
    quint32 dataCrc = 0xffffffffU;
    QFile encoded(packedPath);
    if (!directory) {
        if (method == 0) {
            QByteArray buffer(int(kCopyChunk), Qt::Uninitialized);
            while (true) {
                const qint64 count = input.read(buffer.data(), buffer.size());
                if (count < 0) {
                    if (error) *error = input.errorString();
                    return false;
                }
                if (!count) break;
                dataCrc = crc32Update(dataCrc, buffer.constData(), count);
            }
            dataCrc ^= 0xffffffffU;
            if (!input.seek(0)) {
                if (error) *error = input.errorString();
                return false;
            }
        } else {
            const bool encodedOk = encoded.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                encodeRar5LiteralFile(&input, &encoded, size,
                    algorithmVersion == 1, &dataCrc, &packedSize) && encoded.flush();
            encoded.close();
            if (!encodedOk || !encoded.open(QIODevice::ReadOnly)) {
                if (error) *error = encoded.errorString();
                return false;
            }
        }
    }

    const QFileInfo info(stagedPath);
    const quint32 mtime = directory ? 0 : quint32(std::max<qint64>(0,
        info.lastModified().toSecsSinceEpoch()));
    const quint64 attributes = directory ? 0x10U : 0x20U;
    QByteArray body;
    appendVint(&body, 2);
    appendVint(&body, 2);
    appendVint(&body, packedSize);
    appendVint(&body, directory ? 3U : 6U);
    appendVint(&body, size);
    appendVint(&body, attributes);
    append32(&body, mtime);
    if (!directory) append32(&body, dataCrc);
    appendVint(&body, (quint64(method) << 7) | algorithmVersion);
    appendVint(&body, 0);
    appendVint(&body, quint64(name.size()));
    body.append(name);
    const QByteArray header = rar5Block(body);
    if (!writeAll(output, header.constData(), header.size())) {
        if (error) *error = output->errorString();
        return false;
    }
    if (directory) return true;
    QFile* payload = method == 0 ? &input : &encoded;
    return copyFile(payload, output);
}

bool encodeRar4LiteralFile(QFile* input, QFile* output, quint64 expectedSize,
                           QString* error)
{
    if (!input || !output) return false;
    Rar4BitWriter bits(output);
    if (!bits.write(1, 0) || !bits.write(1, 0)) return false;
    for (unsigned index = 0; index < 20; ++index)
        if (!bits.write(4, 5)) return false;
    for (unsigned index = 0; index < 257; ++index)
        if (!bits.write(5, 9)) return false;
    if (!bits.write(5, 19) || !bits.write(7, 127) ||
        !bits.write(5, 18) || !bits.write(3, 6)) return false;
    QByteArray buffer(int(kCopyChunk), Qt::Uninitialized);
    quint64 total = 0;
    while (total < expectedSize) {
        const qint64 request = qint64(std::min<quint64>(
            quint64(buffer.size()), expectedSize - total));
        const qint64 count = input->read(buffer.data(), request);
        if (count != request) {
            if (error) *error = input->errorString();
            return false;
        }
        for (qint64 index = 0; index < count; ++index)
            if (!bits.write(9, static_cast<unsigned char>(buffer.at(int(index)))))
                return false;
        total += quint64(count);
    }
    if (!bits.write(9, 256) || !bits.write(1, 0) || !bits.write(1, 0) ||
        !bits.finish() || !output->flush()) {
        if (error) *error = QStringLiteral("Could not finish the RAR4 LZ stream.");
        return false;
    }
    return true;
}

bool writeRar4Entry(QIODevice* output, const QString& stagedPath,
                    const QString& packedPath, const QByteArray& name,
                    bool directory, unsigned method, QString* error)
{
    QFile input(stagedPath);
    if (!directory && (!input.open(QIODevice::ReadOnly) || input.size() < 0)) {
        if (error) *error = input.errorString();
        return false;
    }
    const quint64 size = directory ? 0 : quint64(input.size());
    if (name.isEmpty() || name.size() > int(std::numeric_limits<quint16>::max()) - 40) {
        if (error) *error = QStringLiteral("The RAR4 member name is too long.");
        return false;
    }
    quint32 dataCrc = 0xffffffffU;
    quint64 packedSize = size;
    QFile packed(packedPath);
    if (!directory) {
        QByteArray buffer(int(kCopyChunk), Qt::Uninitialized);
        while (true) {
            const qint64 count = input.read(buffer.data(), buffer.size());
            if (count < 0) {
                if (error) *error = input.errorString();
                return false;
            }
            if (!count) break;
            dataCrc = crc32Update(dataCrc, buffer.constData(), count);
        }
        dataCrc ^= 0xffffffffU;
        if (!input.seek(0)) {
            if (error) *error = input.errorString();
            return false;
        }
        if (method != 0) {
            if (!packed.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
                !encodeRar4LiteralFile(&input, &packed, size, error)) {
                if (error && error->isEmpty()) *error = packed.errorString();
                return false;
            }
            packedSize = quint64(packed.size());
            packed.close();
            if (!packed.open(QIODevice::ReadOnly)) {
                *error = packed.errorString();
                return false;
            }
        }
    }

    QByteArray header;
    append16(&header, 0);
    header.append(char(0x74));
    const bool unicodeName = std::any_of(name.begin(), name.end(),
        [](char value) { return (static_cast<unsigned char>(value) & 0x80U) != 0; });
    quint16 flags = 0x8000U | (unicodeName ? 0x0200U : 0U);
    const bool large = packedSize > std::numeric_limits<quint32>::max() ||
                       size > std::numeric_limits<quint32>::max();
    if (large) flags |= 0x0100U;
    if (directory) flags |= 0x00e0U;
    append16(&header, flags);
    append16(&header, quint16((large ? 40 : 32) + name.size()));
    append32(&header, quint32(packedSize));
    append32(&header, quint32(size));
    header.append(char(2));
    append32(&header, directory ? 0U : dataCrc);
    append32(&header, directory ? 0U : rar4DosTime(QFileInfo(stagedPath)));
    header.append(char(method ? 29 : 20));
    header.append(char(0x30U + method));
    append16(&header, quint16(name.size()));
    append32(&header, directory ? 0x10U : 0x20U);
    if (large) {
        append32(&header, quint32(packedSize >> 32));
        append32(&header, quint32(size >> 32));
    }
    header.append(name);
    const quint16 headerCrc = quint16(crc32(header.mid(2)) & 0xffffU);
    header[0] = char(headerCrc & 0xffU);
    header[1] = char((headerCrc >> 8) & 0xffU);
    QFile* payload = method == 0 ? &input : &packed;
    if (!writeAll(output, header.constData(), header.size()) ||
        (!directory && !copyFile(payload, output))) {
        if (error) *error = output->errorString();
        return false;
    }
    return true;
}

} // namespace

RarArchiveWriter::RarArchiveWriter() = default;
RarArchiveWriter::~RarArchiveWriter() = default;

bool RarArchiveWriter::canUpdate(const QString& archivePath)
{
    RarArchiveWriter probe;
    probe.capabilityProbe_ = true;
    probe.updateSourcePath_ = archivePath;
    QString error;
    return probe.parseUpdateArchive(&error);
}

bool RarArchiveWriter::beginUpdate(const QString& archivePath,
                                   const QString& password, QString* error)
{
    if (active_ || archivePath.isEmpty() || !error) return false;
    password_ = password;
    updateSourcePath_ = QFileInfo(archivePath).absoluteFilePath();
    outputPath_ = updateSourcePath_;
    entries_.clear();
    archivePrefix_.clear();
    archiveEnd_.clear();
    sourceVolumePaths_.clear();
    volumeSizeLimit_ = 0;
    archiveMultiVolume_ = false;
    archiveRecovery_ = false;
    recoverySectors_ = 0;
    recoveryPercent_ = 0;
    writingRecoveryArchive_ = false;
    archiveSolid_ = false;
    archiveNeedsRewrite_ = false;
    if (!parseUpdateArchive(error)) {
        entries_.clear();
        updateSourcePath_.clear();
        outputPath_.clear();
        password_.fill(QChar(0));
        password_.clear();
        return false;
    }
    if (version_ != RarArchiveVersion::Rar4 && archiveSolid_ &&
        !clearRar5SolidFlag(error)) {
        entries_.clear();
        updateSourcePath_.clear();
        outputPath_.clear();
        return false;
    }
    const QFileInfo info(updateSourcePath_);
    const QString stageTemplate = info.absoluteDir().filePath(
        QStringLiteral(".peare-rar-XXXXXX"));
    staging_.reset(new (std::nothrow) QTemporaryDir(stageTemplate));
    if (!staging_ || !staging_->isValid()) {
        staging_.reset();
        entries_.clear();
        updateSourcePath_.clear();
        outputPath_.clear();
        *error = QStringLiteral("Could not create RAR update staging beside the archive.");
        return false;
    }
    if ((archiveSolid_ || archiveNeedsRewrite_) && !clearSolidFlag(error)) {
        staging_.reset();
        entries_.clear();
        updateSourcePath_.clear();
        outputPath_.clear();
        return false;
    }
    const bool requiresMaterialization = archiveSolid_ ||
        (version_ == RarArchiveVersion::Rar4 &&
         std::any_of(entries_.begin(), entries_.end(), [](const Entry& entry) {
             return entry.rawChunks.size() > 1;
         }));
    if (requiresMaterialization && !materializeSolidMembers(error)) {
        staging_.reset();
        entries_.clear();
        updateSourcePath_.clear();
        outputPath_.clear();
        archivePrefix_.clear();
        archiveEnd_.clear();
        archiveSolid_ = false;
        return false;
    }
    compressionMethod_ = 0;
    nextStageIndex_ = 0;
    updateMode_ = true;
    active_ = true;
    return true;
}

bool RarArchiveWriter::parseUpdateArchive(QString* error)
{
    QFile input(updateSourcePath_);
    if (!input.open(QIODevice::ReadOnly) || input.size() < qint64(sizeof(kRar4Marker)) + 13) {
        if (error) *error = input.errorString().isEmpty()
            ? QStringLiteral("The RAR archive is truncated.") : input.errorString();
        return false;
    }
    QByteArray marker;
    if (!readExact(&input, 0, sizeof(kRar5Marker), &marker)) return false;
    const QByteArray rar4Marker(reinterpret_cast<const char*>(kRar4Marker),
                                int(sizeof(kRar4Marker)));
    const bool isRar4 = marker.left(int(sizeof(kRar4Marker))) == rar4Marker;
    const bool isRar5 = marker == QByteArray(
        reinterpret_cast<const char*>(kRar5Marker), int(sizeof(kRar5Marker)));
    if (!isRar4 && !isRar5) {
        if (error) *error = QStringLiteral("The file is not a supported RAR archive.");
        return false;
    }
    if (isRar5) {
        version_ = RarArchiveVersion::Rar5;
        qint64 position = qint64(sizeof(kRar5Marker));
        qint64 fileSize = input.size();
        QString currentVolumePath = updateSourcePath_;
        sourceVolumePaths_.push_back(currentVolumePath);
        volumeSizeLimit_ = quint64(fileSize);
        bool sawMain = false;
        bool expectVolumeMain = false;
        bool encryptedHeaders = false;
        QByteArray headerKey;
        std::array<std::uint8_t, 16> headerSalt = {};
        std::uint8_t headerKdfCount = 0;
        QByteArray canonicalMainPrefix;
        std::size_t pendingSplitIndex = std::numeric_limits<std::size_t>::max();
        while (position < fileSize) {
            const qint64 headerOffset = position;
            QByteArray crcBytes;
            QByteArray sizeBytes;
            quint64 headerSize = 0;
            QByteArray body;
            qint64 physicalHeaderSize = 0;
            if (encryptedHeaders) {
                QByteArray ivAndBlock;
                if (!readExact(&input, position, 32, &ivAndBlock)) break;
                std::vector<std::uint8_t> firstBlock(16);
                std::memcpy(firstBlock.data(), ivAndBlock.constData() + 16, 16);
                if (!peare::compression::decryptRar5AesCbc(headerKey,
                        reinterpret_cast<const std::uint8_t*>(ivAndBlock.constData()),
                        &firstBlock)) break;
                int firstSizeOffset = 0;
                if (!readRar5Vint(QByteArray(
                        reinterpret_cast<const char*>(firstBlock.data() + 4), 12),
                        &firstSizeOffset, &headerSize) || !headerSize ||
                    headerSize > 16U * 1024U * 1024U) break;
                const int encodedSizeLength = firstSizeOffset;
                const quint64 plainSize = 4U + quint64(encodedSizeLength) + headerSize;
                const quint64 cipherSize = (plainSize + 15U) & ~15ULL;
                if (cipherSize < 16 || cipherSize > 16U * 1024U * 1024U + 32U ||
                    quint64(position) + 16U + cipherSize > quint64(fileSize)) break;
                QByteArray encrypted(int(cipherSize), char(0));
                if (!readExact(&input, position + 16, encrypted.size(), &encrypted)) break;
                std::vector<std::uint8_t> plaintext(
                    reinterpret_cast<const std::uint8_t*>(encrypted.constData()),
                    reinterpret_cast<const std::uint8_t*>(encrypted.constData()) + encrypted.size());
                if (!peare::compression::decryptRar5AesCbc(headerKey,
                        reinterpret_cast<const std::uint8_t*>(ivAndBlock.constData()),
                        &plaintext)) break;
                crcBytes = QByteArray(reinterpret_cast<const char*>(plaintext.data()), 4);
                sizeBytes = QByteArray(reinterpret_cast<const char*>(plaintext.data() + 4),
                                       encodedSizeLength);
                body = QByteArray(reinterpret_cast<const char*>(plaintext.data() + 4 + encodedSizeLength),
                                  int(headerSize));
                physicalHeaderSize = 16 + qint64(cipherSize);
            } else {
                if (!readExact(&input, position, 4, &crcBytes)) break;
                position += 4;
                if (!readFileVint(&input, &position, &headerSize, &sizeBytes) ||
                    !headerSize || headerSize > 16U * 1024U * 1024U ||
                    headerSize > quint64(fileSize - position) ||
                    !readExact(&input, position, qint64(headerSize), &body)) break;
                physicalHeaderSize = 4 + sizeBytes.size() + qint64(headerSize);
            }
            QByteArray checksumBytes = sizeBytes + body;
            if (crc32(checksumBytes) != read32(crcBytes.constData())) {
                if (error) *error = QStringLiteral("The RAR5 block header CRC is invalid.");
                return false;
            }
            const qint64 fullHeaderOffset = headerOffset;
            const qint64 fullHeaderSize = physicalHeaderSize;
            position = headerOffset + physicalHeaderSize;
            int cursor = 0;
            quint64 type = 0;
            quint64 flags = 0;
            quint64 extraSize = 0;
            quint64 dataSize = 0;
            if (!readRar5Vint(body, &cursor, &type) ||
                !readRar5Vint(body, &cursor, &flags) ||
                ((flags & 1U) && !readRar5Vint(body, &cursor, &extraSize)) ||
                ((flags & 2U) && !readRar5Vint(body, &cursor, &dataSize)) ||
                dataSize > quint64(fileSize - position)) {
                if (error) *error = QStringLiteral("The RAR5 block header is malformed.");
                return false;
            }
            if (type == 5) {
                if (!sawMain) {
                    if (error) *error = QStringLiteral("The RAR5 main header is missing.");
                    return false;
                }
                quint64 endFlags = 0;
                if (!readRar5Vint(body, &cursor, &endFlags)) {
                    if (error) *error = QStringLiteral(
                        "The RAR5 end-of-archive header is malformed.");
                    return false;
                }
                if (endFlags & 1U) {
                    const QString nextPath = nextRar4VolumePath(currentVolumePath);
                    if (nextPath.isEmpty() || !QFileInfo::exists(nextPath)) {
                        if (error) *error = QStringLiteral(
                            "The next RAR5 volume is missing.");
                        return false;
                    }
                    currentVolumePath = nextPath;
                    sourceVolumePaths_.push_back(currentVolumePath);
                    input.close();
                    input.setFileName(currentVolumePath);
                    if (!input.open(QIODevice::ReadOnly) ||
                        input.size() < qint64(sizeof(kRar5Marker)) + 6) {
                        if (error) *error = QStringLiteral(
                            "The next RAR5 volume is truncated.");
                        return false;
                    }
                    fileSize = input.size();
                    volumeSizeLimit_ = std::max(volumeSizeLimit_, quint64(fileSize));
                    QByteArray nextMarker;
                    if (!readExact(&input, 0, qint64(sizeof(kRar5Marker)),
                                   &nextMarker) || nextMarker != marker) {
                        if (error) *error = QStringLiteral(
                            "The next RAR5 volume marker is invalid.");
                        return false;
                    }
                    archiveMultiVolume_ = true;
                    expectVolumeMain = true;
                    sawMain = false;
                    position = qint64(sizeof(kRar5Marker));
                    continue;
                }
                if (encryptedHeaders) {
                    archiveEnd_ = rar5Block(body);
                } else if (!readExact(&input, fullHeaderOffset,
                                      fileSize - fullHeaderOffset, &archiveEnd_)) return false;
                if (pendingSplitIndex != std::numeric_limits<std::size_t>::max()) {
                    if (error) *error = QStringLiteral("A RAR5 split member is incomplete.");
                    return false;
                }
                return true;
            }
            if (type == 1) {
                if ((sawMain && !expectVolumeMain) ||
                    (!sawMain && !expectVolumeMain && sourceVolumePaths_.size() > 1U)) {
                    if (error) *error = QStringLiteral("The RAR5 main header is malformed.");
                    return false;
                }
                quint64 archiveFlags = 0;
                quint64 volumeNumber = 0;
                if (!readRar5Vint(body, &cursor, &archiveFlags) ||
                    ((archiveFlags & 2U) && !readRar5Vint(body, &cursor, &volumeNumber)) ||
                    (archiveFlags & ~quint64(15U)) ||
                    extraSize > quint64(body.size())) {
                    if (error) *error = QStringLiteral(
                        "The RAR5 main header uses unsupported archive flags.");
                    return false;
                }
                if (expectVolumeMain && !(archiveFlags & 1U)) {
                    if (error) *error = QStringLiteral(
                        "A RAR5 continuation volume has no volume flag.");
                    return false;
                }
                archiveSolid_ = archiveSolid_ || (archiveFlags & 4U) != 0;
                archiveMultiVolume_ = archiveMultiVolume_ ||
                    (archiveFlags & 1U) != 0;
                archiveRecovery_ = archiveRecovery_ || (archiveFlags & 8U) != 0;
                archiveNeedsRewrite_ = archiveNeedsRewrite_ ||
                    (archiveFlags & 9U) != 0;
                if ((archiveFlags & 2U) &&
                    volumeNumber != quint64(sourceVolumePaths_.size() - 1U)) {
                    if (error) *error = QStringLiteral(
                        "The RAR5 volume number is not sequential.");
                    return false;
                }
                if (archiveNeedsRewrite_) {
                    archivePrefix_ = rar5MainPrefix(marker, archiveRecovery_);
                    sawMain = true;
                    expectVolumeMain = false;
                    position += qint64(dataSize);
                    continue;
                }
                QByteArray cleanMainBody;
                appendVint(&cleanMainBody, 1);
                appendVint(&cleanMainBody, 0);
                appendVint(&cleanMainBody, 0);
                canonicalMainPrefix = marker + rar5Block(cleanMainBody);
                if (extraSize) {
                    int extraOffset = cursor;
                    const int extraEnd = cursor + int(extraSize);
                    QByteArray rewrittenExtra;
                    bool rewriteLocator = false;
                    while (extraOffset < extraEnd) {
                        const int recordStart = extraOffset;
                        quint64 recordSize = 0;
                        if (!readRar5Vint(body, &extraOffset, &recordSize) ||
                            !recordSize || recordSize > quint64(extraEnd - extraOffset)) {
                            if (error) *error = QStringLiteral(
                                "The RAR5 main-header extra area is malformed.");
                            return false;
                        }
                        const int recordEnd = extraOffset + int(recordSize);
                        quint64 recordType = 0;
                        if (!readRar5Vint(body, &extraOffset, &recordType)) {
                            if (error) *error = QStringLiteral(
                                "The RAR5 main-header extra record is malformed.");
                            return false;
                        }
                        if (recordType == 1) {
                            quint64 locatorFlags = 0;
                            quint64 quickOpenOffset = 0;
                            quint64 recoveryOffset = 0;
                            if (!readRar5Vint(body, &extraOffset, &locatorFlags) ||
                                (locatorFlags & ~quint64(3U)) ||
                                ((locatorFlags & 1U) &&
                                 !readRar5Vint(body, &extraOffset, &quickOpenOffset)) ||
                                ((locatorFlags & 2U) &&
                                 !readRar5Vint(body, &extraOffset, &recoveryOffset))) {
                                if (error) *error = QStringLiteral(
                                    "The RAR5 locator record is malformed or unsupported.");
                                return false;
                            }
                            if (extraOffset != recordEnd) {
                                if (error) *error = QStringLiteral(
                                    "The RAR5 locator record has unexpected data.");
                                return false;
                            }
                            if (locatorFlags & 2U) {
                                archiveRecovery_ = true;
                                archiveNeedsRewrite_ = true;
                                archivePrefix_ = rar5MainPrefix(marker, true);
                            } else if (locatorFlags & 1U) {
                                rewriteLocator = true;
                            } else {
                                rewrittenExtra.append(body.mid(
                                    recordStart, recordEnd - recordStart));
                            }
                        } else {
                            rewrittenExtra.append(body.mid(
                                recordStart, recordEnd - recordStart));
                        }
                        if (extraOffset > recordEnd) {
                            if (error) *error = QStringLiteral(
                                "The RAR5 main-header extra record is malformed.");
                            return false;
                        }
                        extraOffset = recordEnd;
                    }
                    if (rewriteLocator) {
                        int prefixOffset = 0;
                        quint64 mainType = 0;
                        quint64 mainFlags = 0;
                        quint64 originalExtraSize = 0;
                        if (!readRar5Vint(body, &prefixOffset, &mainType) ||
                            !readRar5Vint(body, &prefixOffset, &mainFlags) ||
                            !(mainFlags & 1U)) {
                            if (error) *error = QStringLiteral(
                                "The RAR5 main-header extra area cannot be rewritten.");
                            return false;
                        }
                        const int extraSizeFieldOffset = prefixOffset;
                        if (!readRar5Vint(body, &prefixOffset, &originalExtraSize) ||
                            originalExtraSize != extraSize || prefixOffset > cursor) {
                            if (error) *error = QStringLiteral(
                                "The RAR5 main-header extra area cannot be rewritten.");
                            return false;
                        }
                        Q_UNUSED(mainType);
                        QByteArray mainBody = body.left(extraSizeFieldOffset);
                        appendVint(&mainBody, quint64(rewrittenExtra.size()));
                        mainBody.append(body.mid(prefixOffset, cursor - prefixOffset));
                        mainBody.append(rewrittenExtra);
                        archivePrefix_.append(marker);
                        archivePrefix_.append(rar5Block(mainBody));
                    }
                }
                sawMain = true;
                if (archivePrefix_.isEmpty() &&
                    !readExact(&input, 0, position, &archivePrefix_)) return false;
                position += qint64(dataSize);
                continue;
            }
            if (type == 4) {
                quint64 encryptionVersion = 0;
                quint64 encryptionFlags = 0;
                if (encryptedHeaders || !readRar5Vint(body, &cursor, &encryptionVersion) ||
                    encryptionVersion != 0 || !readRar5Vint(body, &cursor, &encryptionFlags) ||
                    (encryptionFlags & ~1ULL) || cursor >= body.size()) {
                    if (error) *error = QStringLiteral(
                        "The RAR5 encrypted-header record is malformed or unsupported.");
                    return false;
                }
                headerKdfCount = quint8(body.at(cursor++));
                if (cursor > body.size() - int(headerSalt.size())) {
                    if (error) *error = QStringLiteral(
                        "The RAR5 encrypted-header salt is truncated.");
                    return false;
                }
                std::memcpy(headerSalt.data(), body.constData() + cursor,
                            headerSalt.size());
                cursor += int(headerSalt.size());
                if (encryptionFlags & 1U) {
                    if (cursor > body.size() - 12) {
                        if (error) *error = QStringLiteral(
                            "The RAR5 encrypted-header password check is truncated.");
                        return false;
                    }
                    cursor += 12;
                }
                if (password_.isNull()) {
                    if (capabilityProbe_) return true;
                    if (error) *error = QStringLiteral(
                        "RAR5 encrypted headers require a password.");
                    return false;
                }
                QByteArray hashKey;
                if (!peare::compression::deriveRar5Keys(password_.toUtf8(),
                        headerSalt.data(), headerKdfCount, &headerKey, &hashKey)) {
                    if (error) *error = QStringLiteral(
                        "The RAR5 encrypted-header key parameters are invalid.");
                    return false;
                }
                encryptedHeaders = true;
                archiveNeedsRewrite_ = true;
                archivePrefix_ = canonicalMainPrefix;
                position += qint64(dataSize);
                continue;
            }
            if (!sawMain || position > fileSize - qint64(dataSize)) {
                if (error) *error = QStringLiteral("The RAR5 block exceeds the archive bounds.");
                return false;
            }
            bool isRecoveryRecord = false;
            quint8 parsedRecoveryPercent = 0;
            if (type == 3) {
                int serviceCursor = cursor;
                quint64 serviceFlags = 0;
                quint64 unpackedSize = 0;
                quint64 attributes = 0;
                quint64 compressionInfo = 0;
                quint64 hostOs = 0;
                quint64 nameSize = 0;
                if (readRar5Vint(body, &serviceCursor, &serviceFlags) &&
                    readRar5Vint(body, &serviceCursor, &unpackedSize) &&
                    readRar5Vint(body, &serviceCursor, &attributes) &&
                    (!(serviceFlags & 2U) || serviceCursor <= body.size() - 4) &&
                    (!(serviceFlags & 2U) || (serviceCursor += 4, true)) &&
                    (!(serviceFlags & 4U) || serviceCursor <= body.size() - 4) &&
                    (!(serviceFlags & 4U) || (serviceCursor += 4, true)) &&
                    readRar5Vint(body, &serviceCursor, &compressionInfo) &&
                    readRar5Vint(body, &serviceCursor, &hostOs) &&
                    readRar5Vint(body, &serviceCursor, &nameSize) &&
                    nameSize <= 65535 && nameSize <= quint64(body.size() - serviceCursor)) {
                    const QByteArray serviceName = body.mid(serviceCursor, int(nameSize));
                    serviceCursor += int(nameSize);
                    if (serviceName == QByteArray("RR", 2) &&
                        extraSize <= quint64(body.size() - serviceCursor)) {
                        const int extraEnd = serviceCursor + int(extraSize);
                        while (serviceCursor < extraEnd) {
                            quint64 recordSize = 0;
                            if (!readRar5Vint(body, &serviceCursor, &recordSize) ||
                                !recordSize || recordSize > quint64(extraEnd - serviceCursor))
                                break;
                            const int recordEnd = serviceCursor + int(recordSize);
                            quint64 recordType = 0;
                            if (!readRar5Vint(body, &serviceCursor, &recordType)) break;
                            if (recordType == 7 && serviceCursor < recordEnd) {
                                quint64 percent = 0;
                                if (readRar5Vint(body, &serviceCursor, &percent) &&
                                    percent > 0 && percent <= 100 &&
                                    serviceCursor == recordEnd) {
                                    isRecoveryRecord = true;
                                    parsedRecoveryPercent = quint8(percent);
                                }
                            }
                            serviceCursor = recordEnd;
                        }
                    }
                }
                Q_UNUSED(unpackedSize);
                Q_UNUSED(attributes);
                Q_UNUSED(compressionInfo);
                Q_UNUSED(hostOs);
            }
            Entry entry;
            entry.raw = true;
            entry.rawHeader = crcBytes + sizeBytes + body;
            entry.rawDataOffset = position;
            entry.rawDataSize = dataSize;
            entry.editable = false;
            entry.recoveryRecord = isRecoveryRecord;
            if (type == 2) {
                const bool splitBefore = (flags & 0x08U) != 0;
                const bool splitAfter = (flags & 0x10U) != 0;
                if (splitBefore || splitAfter) {
                    archiveMultiVolume_ = true;
                    archiveNeedsRewrite_ = true;
                }
                quint64 fileFlags = 0;
                quint64 unpackedSize = 0;
                quint64 attributes = 0;
                quint64 compressionInfo = 0;
                quint64 hostOs = 0;
                quint64 nameSize = 0;
                if (!readRar5Vint(body, &cursor, &fileFlags) ||
                    !readRar5Vint(body, &cursor, &unpackedSize) ||
                    !readRar5Vint(body, &cursor, &attributes)) {
                    if (error) *error = QStringLiteral("The RAR5 file header is malformed.");
                    return false;
                }
                if (fileFlags & 2U) cursor += 4;
                quint32 unpackedCrc = 0;
                if (fileFlags & 4U) {
                    if (cursor > body.size() - 4) {
                        if (error) *error = QStringLiteral("The RAR5 file CRC is truncated.");
                        return false;
                    }
                    unpackedCrc = read32(body.constData() + cursor);
                    cursor += 4;
                }
                if (cursor > body.size() ||
                    !readRar5Vint(body, &cursor, &compressionInfo) ||
                    !readRar5Vint(body, &cursor, &hostOs)) {
                    if (error) *error = QStringLiteral("The RAR5 file header is truncated.");
                    return false;
                }
                const int nameLengthOffset = cursor;
                if (!readRar5Vint(body, &cursor, &nameSize) || nameSize > 65535 ||
                    nameSize > quint64(body.size() - cursor) ||
                    extraSize != quint64(body.size() - cursor) - nameSize) {
                    if (error) *error = QStringLiteral("The RAR5 member name is malformed.");
                    return false;
                }
                Q_UNUSED(attributes);
                Q_UNUSED(hostOs);
                const quint64 algorithmVersion = compressionInfo & 0x3fU;
                if ((compressionInfo & 0x3fU) == 1U)
                    version_ = RarArchiveVersion::Rar7;
                entry.name = body.mid(cursor, int(nameSize));
                if (entry.name.isEmpty()) {
                    if (error) *error = QStringLiteral("The RAR5 member name is empty.");
                    return false;
                }
                entry.directory = (fileFlags & 1U) != 0;
                entry.editable = true;
                entry.unpackedSize = unpackedSize;
                entry.unpackedCrc = unpackedCrc;
                entry.solid = (compressionInfo & 0x40U) != 0;
                entry.packedMethod = unsigned((compressionInfo >> 7) & 7U);
                entry.algorithmVersion = unsigned(algorithmVersion);
                const unsigned dictionaryBits = unsigned((compressionInfo >> 10) & 0x1fU);
                const unsigned dictionaryFraction = unsigned((compressionInfo >> 15) & 0x1fU);
                if (entry.algorithmVersion > 1 || entry.packedMethod > 5 ||
                    dictionaryBits > 23 || dictionaryFraction > 31) {
                    if (error) *error = QStringLiteral(
                        "The RAR5 member uses unsupported compression parameters.");
                    return false;
                }
                entry.dictionarySize = (quint64(128) * 1024U) << dictionaryBits;
                if (entry.algorithmVersion == 1)
                    entry.dictionarySize += (entry.dictionarySize / 32U) * dictionaryFraction;
                entry.extendedDistances = entry.algorithmVersion == 1 &&
                    (compressionInfo & 0x100000U) == 0;
                Rar5EncryptionParameters encryption;
                if (extraSize && !parseRar5EncryptionRecord(
                        body.mid(cursor + int(nameSize), int(extraSize)), &encryption)) {
                    if (error) *error = QStringLiteral(
                        "The RAR5 member encryption record is malformed.");
                    return false;
                }
                entry.encrypted = encryption.present;
                entry.encryptionFlags = encryption.flags;
                entry.kdfCount = encryption.kdfCount;
                entry.rar5Salt = encryption.salt;
                entry.rar5Iv = encryption.iv;
                if (entry.solid) archiveSolid_ = true;
                entry.rawDataSize = dataSize;
                entry.rawDataOffset = position;
                entry.rawChunks.push_back({currentVolumePath, position, dataSize});
                entry.rawHeaderSizeLength = sizeBytes.size();
                entry.rawNameLengthOffset = 4 + sizeBytes.size() + nameLengthOffset;
                entry.rawNameLengthBytes = cursor - nameLengthOffset;
                entry.rawNameOffset = 4 + sizeBytes.size() + cursor;
                if (entry.directory && entry.name.endsWith('/')) entry.name.chop(1);
                if (splitBefore) {
                    if (pendingSplitIndex == std::numeric_limits<std::size_t>::max() ||
                        pendingSplitIndex >= entries_.size()) {
                        if (error) *error = QStringLiteral(
                            "The RAR5 split member has no initial volume part.");
                        return false;
                    }
                    Entry& pending = entries_[pendingSplitIndex];
                    if (pending.name != entry.name ||
                        pending.unpackedSize != entry.unpackedSize ||
                        pending.packedMethod != entry.packedMethod ||
                        pending.algorithmVersion != entry.algorithmVersion ||
                        pending.dictionarySize != entry.dictionarySize ||
                        pending.encrypted != entry.encrypted ||
                        pending.encryptionFlags != entry.encryptionFlags ||
                        pending.kdfCount != entry.kdfCount ||
                        pending.rar5Salt != entry.rar5Salt ||
                        pending.rar5Iv != entry.rar5Iv ||
                        pending.rawDataSize > std::numeric_limits<quint64>::max() - dataSize) {
                        if (error) *error = QStringLiteral(
                            "The RAR5 split member continuation does not match.");
                        return false;
                    }
                    pending.rawDataSize += dataSize;
                    pending.rawChunks.push_back({currentVolumePath, position, dataSize});
                    pending.unpackedCrc = entry.unpackedCrc;
                    pending.solid = entry.solid;
                    if (!splitAfter)
                        pendingSplitIndex = std::numeric_limits<std::size_t>::max();
                    archiveNeedsRewrite_ = true;
                } else if (!entry.directory && conflictsWithEntry(entries_, entry.name, false)) {
                    if (error) *error = QStringLiteral("RAR5 contains conflicting member names.");
                    return false;
                } else {
                    if (splitAfter) {
                        archiveNeedsRewrite_ = true;
                        pendingSplitIndex = entries_.size();
                    }
                    entries_.push_back(std::move(entry));
                }
                if (splitBefore) {
                    position += qint64(dataSize);
                    Q_UNUSED(fullHeaderSize);
                    continue;
                }
            } else {
                if (isRecoveryRecord) {
                    archiveRecovery_ = true;
                    archiveNeedsRewrite_ = true;
                    recoveryPercent_ = parsedRecoveryPercent;
                    archivePrefix_ = rar5MainPrefix(marker, true);
                }
                entry.rawChunks.push_back({currentVolumePath, position, dataSize});
                entries_.push_back(std::move(entry));
            }
            position += qint64(dataSize);
            Q_UNUSED(fullHeaderSize);
        }
        if (error) *error = QStringLiteral("The RAR5 end-of-archive header is missing or invalid.");
        return false;
    }
    version_ = RarArchiveVersion::Rar4;
    qint64 fileSize = input.size();
    QString currentVolumePath = updateSourcePath_;
    sourceVolumePaths_.push_back(currentVolumePath);
    volumeSizeLimit_ = quint64(fileSize);
    QByteArray mainPrefix;
    if (!readExact(&input, qint64(sizeof(kRar4Marker)), 7, &mainPrefix) ||
        quint8(mainPrefix.at(2)) != 0x73) {
        if (error) *error = QStringLiteral("The RAR4 main header is missing.");
        return false;
    }
    const quint16 mainFlags = read16(mainPrefix.constData() + 3);
    const quint16 mainSize = read16(mainPrefix.constData() + 5);
    if (mainSize < 13 || qint64(sizeof(kRar4Marker)) + mainSize > fileSize ||
        ((mainFlags & 0x0080U) && password_.isNull() && !capabilityProbe_)) {
        if (error) *error = QStringLiteral(
            "RAR4 encrypted headers require a password for updates.");
        return false;
    }
    QByteArray mainHeader;
    if (!readExact(&input, qint64(sizeof(kRar4Marker)), mainSize, &mainHeader) ||
        quint16(crc32(mainHeader.constData() + 2, mainHeader.size() - 2)) !=
            read16(mainHeader.constData())) {
        if (error) *error = QStringLiteral("The RAR4 main header is corrupt.");
        return false;
    }
    archiveMultiVolume_ = (mainFlags & 0x0001U) != 0;
    archiveRecovery_ = (mainFlags & 0x0040U) != 0;
    const bool encryptedHeaders = (mainFlags & 0x0080U) != 0;
    if (capabilityProbe_ && encryptedHeaders) return true;
    if (encryptedHeaders) {
        quint16 flags = read16(mainHeader.constData() + 3) & quint16(~0x0080U);
        mainHeader[3] = char(flags & 0xffU);
        mainHeader[4] = char(flags >> 8);
        const quint16 checksum = quint16(crc32(mainHeader.constData() + 2,
                                               mainHeader.size() - 2) & 0xffffU);
        mainHeader[0] = char(checksum & 0xffU);
        mainHeader[1] = char(checksum >> 8);
    }
    archivePrefix_ = rar4Marker + mainHeader;
    archiveSolid_ = (mainFlags & 0x0008U) != 0;
    archiveNeedsRewrite_ = (mainFlags & 0x0001U) != 0;
    qint64 position = qint64(sizeof(kRar4Marker)) + mainSize;
    bool foundEnd = false;
    std::size_t pendingSplitIndex = std::numeric_limits<std::size_t>::max();
    while (position <= fileSize - 7) {
        QByteArray header;
        qint64 physicalHeaderSize = 0;
        quint16 headerSize = 0;
        if (encryptedHeaders) {
            QByteArray saltAndBlock;
            if (!readExact(&input, position, 24, &saltAndBlock)) break;
            std::vector<std::uint8_t> firstPlain;
            if (!peare::compression::decryptRar4Data(password_,
                    reinterpret_cast<const std::uint8_t*>(saltAndBlock.constData()),
                    reinterpret_cast<const std::uint8_t*>(saltAndBlock.constData() + 8),
                    16, &firstPlain) || firstPlain.size() < 7) break;
            headerSize = read16(reinterpret_cast<const char*>(firstPlain.data() + 5));
            if (headerSize < 7 || headerSize > 1024U * 1024U) break;
            const qint64 cipherSize = (qint64(headerSize) + 15) & ~qint64(15);
            QByteArray encrypted;
            if (!readExact(&input, position + 8, cipherSize, &encrypted)) break;
            std::vector<std::uint8_t> plain;
            if (!peare::compression::decryptRar4Data(password_,
                    reinterpret_cast<const std::uint8_t*>(saltAndBlock.constData()),
                    reinterpret_cast<const std::uint8_t*>(encrypted.constData()),
                    std::size_t(encrypted.size()), &plain) ||
                plain.size() < std::size_t(headerSize)) break;
            header = QByteArray(reinterpret_cast<const char*>(plain.data()), headerSize);
            physicalHeaderSize = 8 + cipherSize;
        } else {
            QByteArray fixed;
            if (!readExact(&input, position, 7, &fixed)) break;
            headerSize = read16(fixed.constData() + 5);
            if (headerSize < 7 || position > fileSize - headerSize ||
                !readExact(&input, position, headerSize, &header)) break;
            physicalHeaderSize = headerSize;
        }
        const quint16 flags = read16(header.constData() + 3);
        if (quint16(crc32(header.constData() + 2, header.size() - 2)) !=
            read16(header.constData())) break;
        const quint8 type = quint8(header.at(2));
        bool recoveryBlock = false;
        if (type == 0x7a) {
            if (header.size() < 32) {
                if (error) *error = QStringLiteral(
                    "The RAR4 service header is truncated.");
                return false;
            }
            const quint16 nameSize = read16(header.constData() + 26);
            if (32 + nameSize > header.size()) {
                if (error) *error = QStringLiteral("The RAR4 service name is truncated.");
                return false;
            }
            if (header.mid(32, nameSize) == QByteArray("RR", 2)) {
                if (header.size() < 54 ||
                    header.mid(32, 10) != QByteArray("RRProtect+", 10)) {
                    if (error) *error = QStringLiteral(
                        "The RAR4 recovery record header is malformed.");
                    return false;
                }
                const quint32 sectors = read32(header.constData() + 42);
                if (!sectors || sectors > 524288U) {
                    if (error) *error = QStringLiteral(
                        "The RAR4 recovery sector count is invalid.");
                    return false;
                }
                recoverySectors_ = sectors;
                archiveRecovery_ = true;
                recoveryBlock = true;
            }
        }
        if (type == 0x7b) {
            if (flags & 0x0001U) {
                const QString nextPath = nextRar4VolumePath(currentVolumePath);
                if (nextPath.isEmpty() || !QFileInfo::exists(nextPath)) {
                    if (error) *error = QStringLiteral("The next RAR4 volume is missing.");
                    return false;
                }
                currentVolumePath = nextPath;
                sourceVolumePaths_.push_back(currentVolumePath);
                input.close();
                input.setFileName(currentVolumePath);
                if (!input.open(QIODevice::ReadOnly) ||
                    input.size() < qint64(sizeof(kRar4Marker)) + 13) {
                    if (error) *error = QStringLiteral("The next RAR4 volume is truncated.");
                    return false;
                }
                fileSize = input.size();
                volumeSizeLimit_ = std::max(volumeSizeLimit_, quint64(fileSize));
                QByteArray nextMarker;
                QByteArray nextMainPrefix;
                if (!readExact(&input, 0, sizeof(kRar4Marker), &nextMarker) ||
                    nextMarker != rar4Marker ||
                    !readExact(&input, qint64(sizeof(kRar4Marker)), 7,
                               &nextMainPrefix) || quint8(nextMainPrefix.at(2)) != 0x73) {
                    if (error) *error = QStringLiteral("The next RAR4 volume header is invalid.");
                    return false;
                }
                const quint16 nextMainFlags = read16(nextMainPrefix.constData() + 3);
                const quint16 nextMainSize = read16(nextMainPrefix.constData() + 5);
                QByteArray nextMainHeader;
                if (nextMainSize < 13 ||
                    qint64(sizeof(kRar4Marker)) + nextMainSize > fileSize ||
                    !(nextMainFlags & 0x0001U) || (nextMainFlags & 0x0080U) ||
                    !readExact(&input, qint64(sizeof(kRar4Marker)), nextMainSize,
                               &nextMainHeader) ||
                    quint16(crc32(nextMainHeader.constData() + 2,
                                  nextMainHeader.size() - 2)) !=
                        read16(nextMainHeader.constData())) {
                    if (error) *error = QStringLiteral("The next RAR4 main header is corrupt.");
                    return false;
                }
                archiveSolid_ = archiveSolid_ || (nextMainFlags & 0x0008U);
                archiveMultiVolume_ = true;
                position = qint64(sizeof(kRar4Marker)) + nextMainSize;
                continue;
            }
            if (pendingSplitIndex != std::numeric_limits<std::size_t>::max()) {
                if (error) *error = QStringLiteral("A RAR4 split member is incomplete.");
                return false;
            }
            archiveEnd_ = header;
            foundEnd = true;
            break;
        }
        quint64 dataSize = 0;
        if (flags & 0x8000U) {
            if (headerSize < 11) break;
            dataSize = read32(header.constData() + 7);
        }
        if (type == 0x74 && (flags & 0x0100U)) {
            if (headerSize < 40) break;
            dataSize |= quint64(read32(header.constData() + 32)) << 32;
        }
        if (dataSize > quint64(fileSize) ||
            position + physicalHeaderSize > fileSize - qint64(dataSize)) break;
        Entry entry;
        entry.raw = true;
        entry.rawHeader = header;
        entry.rawDataOffset = position + physicalHeaderSize;
        entry.rawDataSize = dataSize;
        entry.rawChunks.push_back({currentVolumePath,
            position + physicalHeaderSize, dataSize});
        entry.editable = false;
        if (type == 0x74) {
            if (headerSize < 32) break;
            const bool large = (flags & 0x0100U) != 0;
            const int nameOffset = large ? 40 : 32;
            if (large && headerSize < 40) break;
            const quint16 nameSize = read16(header.constData() + 26);
            if (nameOffset + nameSize > headerSize) {
                if (error) *error = QStringLiteral(
                    "The RAR4 member name is malformed.");
                return false;
            }
            const QByteArray rawName = header.mid(nameOffset, nameSize);
            if ((flags & 0x0200U) && !rawName.contains('\0'))
                entry.name = rawName;
            else if (!(flags & 0x0200U))
                entry.name = rawName;
            else if (!decodeRar4UnicodeName(rawName, &entry.name)) {
                if (error) *error = QStringLiteral(
                    "The RAR4 compressed Unicode filename is malformed.");
                return false;
            }
            if (entry.name.isEmpty()) {
                if (error) *error = QStringLiteral("The RAR4 member has an empty name.");
                return false;
            }
            const quint32 attributes = read32(header.constData() + 28);
            entry.directory = (flags & 0x00e0U) == 0x00e0U ||
                              (attributes & 0x10U) != 0;
            entry.editable = true;
            entry.unpackedSize = read32(header.constData() + 11);
            if (flags & 0x0100U)
                entry.unpackedSize |= quint64(read32(header.constData() + 36)) << 32;
            entry.unpackedCrc = read32(header.constData() + 16);
            entry.packedMethod = quint8(header.at(25));
            entry.solid = (flags & 0x0010U) != 0;
            entry.split = (flags & (0x0001U | 0x0002U)) != 0;
            entry.encrypted = (flags & 0x0004U) != 0;
            entry.hasRar4Salt = (flags & 0x0400U) != 0;
            if (entry.hasRar4Salt) {
                const int saltOffset = nameOffset + nameSize;
                if (saltOffset > header.size() - int(entry.rar4Salt.size())) {
                    if (error) *error = QStringLiteral(
                        "The RAR4 member encryption salt is truncated.");
                    return false;
                }
                std::memcpy(entry.rar4Salt.data(), header.constData() + saltOffset,
                            entry.rar4Salt.size());
            }
            entry.dictionarySize = (quint64(64) * 1024U) <<
                std::min<unsigned>((flags >> 5) & 7U, 6U);
            entry.rawDataSize = dataSize;
            entry.rawDataOffset = position + physicalHeaderSize;
            entry.rawChunks.clear();
            entry.rawChunks.push_back({currentVolumePath,
                position + physicalHeaderSize, dataSize});
            if (entry.solid) archiveSolid_ = true;
            entry.rawNameOffset = nameOffset;
            entry.rawNameLengthOffset = 26;
            entry.rawNameLengthBytes = 2;
            if (entry.directory && entry.name.endsWith('/')) entry.name.chop(1);
            const bool splitBefore = (flags & 0x0001U) != 0;
            const bool splitAfter = (flags & 0x0002U) != 0;
            if (splitBefore || splitAfter) archiveMultiVolume_ = true;
            if (splitBefore) {
                if (pendingSplitIndex == std::numeric_limits<std::size_t>::max() ||
                    pendingSplitIndex >= entries_.size()) {
                    if (error) *error = QStringLiteral(
                        "The RAR4 split member has no initial volume part.");
                    return false;
                }
                Entry& pending = entries_[pendingSplitIndex];
                if (pending.name != entry.name ||
                    pending.unpackedSize != entry.unpackedSize ||
                    pending.packedMethod != entry.packedMethod ||
                    pending.encrypted != entry.encrypted ||
                    pending.hasRar4Salt != entry.hasRar4Salt ||
                    pending.rar4Salt != entry.rar4Salt ||
                    pending.rawDataSize > std::numeric_limits<quint64>::max() - dataSize) {
                    if (error) *error = QStringLiteral(
                        "The RAR4 split member continuation does not match.");
                    return false;
                }
                pending.rawDataSize += dataSize;
                pending.rawChunks.push_back({currentVolumePath,
                    position + physicalHeaderSize, dataSize});
                pending.unpackedCrc = entry.unpackedCrc;
                pending.solid = entry.solid;
                if (!splitAfter) {
                    pending.split = false;
                    pendingSplitIndex = std::numeric_limits<std::size_t>::max();
                }
                archiveNeedsRewrite_ = true;
            } else if (!entry.directory && conflictsWithEntry(entries_, entry.name, false)) {
                if (error) *error = QStringLiteral("RAR4 contains conflicting member names.");
                return false;
            } else {
                if (splitAfter) {
                    archiveNeedsRewrite_ = true;
                    pendingSplitIndex = entries_.size();
                }
                entries_.push_back(std::move(entry));
            }
            if (splitBefore) {
                position += physicalHeaderSize + qint64(dataSize);
                continue;
            }
        } else if (!recoveryBlock) {
            entries_.push_back(std::move(entry));
        }
        position += physicalHeaderSize + qint64(dataSize);
    }
    if (!foundEnd) {
        if (error) *error = QStringLiteral("The RAR4 end-of-archive header is missing or invalid.");
        return false;
    }
    if (archiveRecovery_ && !recoverySectors_) {
        if (error) *error = QStringLiteral(
            "The RAR4 main header advertises recovery data, but no RR record was found.");
        return false;
    }
    return true;
}

bool RarArchiveWriter::readPackedData(const Entry& entry,
                                      std::vector<std::uint8_t>* packed,
                                      QString* error) const
{
    if (!packed || entry.rawDataSize >
            std::uint64_t(std::numeric_limits<std::size_t>::max())) return false;
    try {
        packed->assign(std::size_t(entry.rawDataSize), 0);
    } catch (...) {
        if (error) *error = QStringLiteral("Could not allocate packed RAR member data.");
        return false;
    }
    std::vector<DataChunk> chunks = entry.rawChunks;
    if (chunks.empty())
        chunks.push_back({updateSourcePath_, entry.rawDataOffset, entry.rawDataSize});
    quint64 outputOffset = 0;
    for (const DataChunk& chunk : chunks) {
        if (chunk.offset < 0 || chunk.size > entry.rawDataSize - outputOffset) {
            if (error) *error = QStringLiteral("The RAR member data ranges are invalid.");
            return false;
        }
        QFile source(chunk.path);
        if (!source.open(QIODevice::ReadOnly) ||
            !source.seek(chunk.offset)) {
            if (error) *error = source.errorString();
            return false;
        }
        quint64 done = 0;
        while (done < chunk.size) {
            const qint64 amount = qint64(std::min<quint64>(64 * 1024,
                chunk.size - done));
            const qint64 count = source.read(
                reinterpret_cast<char*>(packed->data()) +
                    std::size_t(outputOffset + done), amount);
            if (count <= 0) {
                if (error) *error = source.errorString();
                return false;
            }
            done += quint64(count);
        }
        outputOffset += chunk.size;
    }
    if (outputOffset != entry.rawDataSize) {
        if (error) *error = QStringLiteral("The RAR member data is incomplete.");
        return false;
    }
    return true;
}

bool RarArchiveWriter::clearSolidFlag(QString* error)
{
    if (version_ != RarArchiveVersion::Rar4) return clearRar5SolidFlag(error);
    const int markerSize = int(sizeof(kRar4Marker));
    if (archivePrefix_.size() < markerSize + 7 ||
        quint8(archivePrefix_.at(markerSize + 2)) != 0x73) {
        if (error) *error = QStringLiteral("The RAR4 main header is truncated.");
        return false;
    }
    QByteArray header = archivePrefix_.mid(markerSize);
    quint16 flags = read16(header.constData() + 3);
    flags &= ~quint16(0x0009U);
    header[3] = char(flags & 0xffU);
    header[4] = char(flags >> 8);
    const quint16 checksum = quint16(crc32(header.mid(2)) & 0xffffU);
    header[0] = char(checksum & 0xffU);
    header[1] = char(checksum >> 8);
    archivePrefix_ = archivePrefix_.left(markerSize) + header;
    return true;
}

bool RarArchiveWriter::clearRar5SolidFlag(QString* error)
{
    const int markerSize = int(sizeof(kRar5Marker));
    if (archivePrefix_.size() < markerSize + 6) {
        if (error) *error = QStringLiteral("The RAR5 main header is truncated.");
        return false;
    }
    const QByteArray prefix = archivePrefix_.left(markerSize);
    QByteArray header = archivePrefix_.mid(markerSize);
    if (header.size() < 5 || crc32(header.mid(4)) != read32(header.constData())) {
        if (error) *error = QStringLiteral("The RAR5 main header is corrupt.");
        return false;
    }
    int sizeOffset = 4;
    quint64 bodySize = 0;
    if (!readRar5Vint(header, &sizeOffset, &bodySize) ||
        bodySize > quint64(header.size() - sizeOffset)) {
        if (error) *error = QStringLiteral("The RAR5 main header size is invalid.");
        return false;
    }
    QByteArray body = header.mid(sizeOffset, int(bodySize));
    int cursor = 0;
    quint64 type = 0;
    quint64 blockFlags = 0;
    quint64 extraSize = 0;
    quint64 dataSize = 0;
    quint64 archiveFlags = 0;
    if (!readRar5Vint(body, &cursor, &type) || type != 1 ||
        !readRar5Vint(body, &cursor, &blockFlags) ||
        ((blockFlags & 1U) && !readRar5Vint(body, &cursor, &extraSize)) ||
        ((blockFlags & 2U) && !readRar5Vint(body, &cursor, &dataSize)) ||
        !readRar5Vint(body, &cursor, &archiveFlags)) {
        if (error) *error = QStringLiteral("The RAR5 solid flag cannot be rewritten.");
        return false;
    }
    if (!(archiveFlags & 4U)) return true;
    QByteArray updatedBody = body.left(cursor);
    QByteArray encodedFlags;
    appendVint(&encodedFlags, archiveFlags & ~quint64(4U));
    updatedBody.append(encodedFlags);
    updatedBody.append(body.mid(cursor));
    const QByteArray updatedHeader = rar5Block(updatedBody);
    archivePrefix_ = prefix + updatedHeader + archivePrefix_.mid(
        markerSize + sizeOffset + int(bodySize));
    Q_UNUSED(extraSize);
    Q_UNUSED(dataSize);
    return true;
}

bool RarArchiveWriter::materializeRar5SolidMembers(QString* error)
{
    Rar5UnpackState state;
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        Entry& entry = entries_[index];
        if (!entry.raw || !entry.editable || entry.directory) continue;
        if (entry.encrypted && password_.isNull()) {
            if (error) *error = QStringLiteral(
                "RAR5 encrypted member data requires a password for update.");
            return false;
        }
        if (!entry.solid) state = Rar5UnpackState();
        if (entry.rawDataSize > std::uint64_t(std::numeric_limits<std::size_t>::max()) ||
            entry.unpackedSize > std::uint64_t(std::numeric_limits<std::size_t>::max())) {
            if (error) *error = QStringLiteral("The RAR5 member is too large to stage.");
            return false;
        }
        std::vector<std::uint8_t> packed;
        std::vector<std::uint8_t> unpacked;
        QByteArray checksumKey;
        try {
            if (!readPackedData(entry, &packed, error)) return false;
            if (entry.encrypted) {
                std::vector<std::uint8_t> decrypted;
                if (!peare::compression::decryptRar5Data(password_.toUtf8(),
                        entry.rar5Salt.data(), entry.rar5Iv.data(), entry.kdfCount,
                        packed, &decrypted, &checksumKey)) {
                    if (error) *error = QStringLiteral(
                        "The RAR5 member could not be decrypted for update.");
                    return false;
                }
                packed.swap(decrypted);
            }
            if (entry.packedMethod == 0) {
                if (packed.size() < entry.unpackedSize ||
                    packed.size() - std::size_t(entry.unpackedSize) > 15 ||
                    std::any_of(packed.begin() + std::size_t(entry.unpackedSize),
                                packed.end(), [](std::uint8_t value) { return value != 0; })) {
                    if (error) *error = QStringLiteral("The RAR5 stored member is invalid.");
                    return false;
                }
                unpacked.assign(packed.begin(),
                    packed.begin() + std::size_t(entry.unpackedSize));
                bool preserveHistory = false;
                for (std::size_t next = index + 1; next < entries_.size(); ++next) {
                    if (entries_[next].raw && entries_[next].editable &&
                        !entries_[next].directory) {
                        preserveHistory = entries_[next].solid;
                        break;
                    }
                }
                if (preserveHistory) {
                    state.history.insert(state.history.end(), unpacked.begin(), unpacked.end());
                    if (state.history.size() > entry.dictionarySize) {
                        const std::size_t keep = std::size_t(entry.dictionarySize);
                        state.history.erase(state.history.begin(), state.history.end() - keep);
                    }
                }
            } else {
                bool preserveHistory = false;
                for (std::size_t next = index + 1; next < entries_.size(); ++next) {
                    if (entries_[next].raw && entries_[next].editable &&
                        !entries_[next].directory) {
                        preserveHistory = entries_[next].solid;
                        break;
                    }
                }
                if (!decodeRar5Member(packed.data(), packed.size(), entry.unpackedSize,
                        entry.dictionarySize, entry.extendedDistances, entry.encrypted,
                        preserveHistory, &state, &unpacked)) {
                    if (error) *error = QStringLiteral(
                        "The RAR5 solid member could not be decoded for update.");
                    return false;
                }
            }
        } catch (...) {
            if (error) *error = QStringLiteral("Could not allocate RAR5 member staging.");
            return false;
        }
        quint32 actualCrc = crc32(reinterpret_cast<const char*>(unpacked.data()),
                                  qint64(unpacked.size()));
        if (entry.encrypted && (entry.encryptionFlags & 2U))
            actualCrc = peare::compression::rar5Checksum(
                reinterpret_cast<const std::uint8_t*>(checksumKey.constData()),
                actualCrc);
        if (unpacked.size() != entry.unpackedSize || actualCrc != entry.unpackedCrc) {
            if (error) *error = QStringLiteral(
                "The RAR5 member data or CRC is invalid.");
            return false;
        }
        const QString staged = staging_->filePath(
            QStringLiteral("solid-%1").arg(qulonglong(index)));
        QFile stagedFile(staged);
        if (!stagedFile.open(QIODevice::WriteOnly) ||
            stagedFile.write(reinterpret_cast<const char*>(unpacked.data()),
                             qint64(unpacked.size())) != qint64(unpacked.size()) ||
            !stagedFile.flush()) {
            if (error) *error = stagedFile.errorString();
            return false;
        }
        entry.raw = false;
        entry.stagedPath = staged;
    }
    return true;
}

bool RarArchiveWriter::materializeSolidMembers(QString* error)
{
    if (version_ != RarArchiveVersion::Rar4)
        return materializeRar5SolidMembers(error);
    rar4::LzState state;
    for (std::size_t index = 0; index < entries_.size(); ++index) {
        Entry& entry = entries_[index];
        if (!entry.raw || !entry.editable || entry.directory) continue;
        if (entry.encrypted && (!entry.hasRar4Salt || password_.isNull())) {
            if (error) *error = QStringLiteral(
                "RAR4 encrypted member data requires its salt and password for update.");
            return false;
        }
        if (!entry.solid) state.reset();
        if (entry.rawDataSize > std::uint64_t(std::numeric_limits<std::size_t>::max()) ||
            entry.unpackedSize > std::uint64_t(std::numeric_limits<std::size_t>::max())) {
            if (error) *error = QStringLiteral("The RAR4 member is too large to stage.");
            return false;
        }
        std::vector<std::uint8_t> packed;
        std::vector<std::uint8_t> unpacked;
        try {
            if (!readPackedData(entry, &packed, error)) return false;
            if (entry.encrypted) {
                std::vector<std::uint8_t> decrypted;
                if (!peare::compression::decryptRar4Data(password_,
                        entry.rar4Salt.data(), packed.data(), packed.size(),
                        &decrypted)) {
                    if (error) *error = QStringLiteral(
                        "The RAR4 member could not be decrypted for update.");
                    return false;
                }
                packed.swap(decrypted);
            }
            if (entry.packedMethod == 0x30) {
                if (packed.size() < entry.unpackedSize ||
                    packed.size() - std::size_t(entry.unpackedSize) > 15 ||
                    std::any_of(packed.begin() + std::size_t(entry.unpackedSize),
                                packed.end(), [](std::uint8_t value) { return value != 0; })) {
                    if (error) *error = QStringLiteral("The RAR4 stored member size is invalid.");
                    return false;
                }
                unpacked.assign(packed.begin(),
                    packed.begin() + std::size_t(entry.unpackedSize));
                for (std::uint8_t value : unpacked) state.pushHistory(value);
            } else if (entry.packedMethod >= 0x31 && entry.packedMethod <= 0x35) {
                if (!rar4::decodeLz(packed.data(), packed.size(),
                        std::size_t(entry.unpackedSize), entry.solid,
                        std::size_t(entry.dictionarySize), &state, &unpacked)) {
                    if (error) *error = QStringLiteral(
                        "The RAR4 member %1 (method %2, solid %3) could not be decoded for update.")
                        .arg(QString::fromUtf8(entry.name))
                        .arg(entry.packedMethod).arg(entry.solid ? 1 : 0);
                    return false;
                }
            } else {
                if (error) *error = QStringLiteral("The RAR4 member method is unsupported.");
                return false;
            }
        } catch (...) {
            if (error) *error = QStringLiteral("Could not allocate RAR4 member staging.");
            return false;
        }
        if (unpacked.size() != entry.unpackedSize ||
            crc32(reinterpret_cast<const char*>(unpacked.data()),
                  qint64(unpacked.size())) != entry.unpackedCrc) {
            if (error) *error = QStringLiteral("The RAR4 member data or CRC is invalid.");
            return false;
        }
        const QString staged = staging_->filePath(
            QStringLiteral("solid-%1").arg(qulonglong(index)));
        QFile stagedFile(staged);
        if (!stagedFile.open(QIODevice::WriteOnly) ||
            stagedFile.write(reinterpret_cast<const char*>(unpacked.data()),
                             qint64(unpacked.size())) != qint64(unpacked.size()) ||
            !stagedFile.flush()) {
            if (error) *error = stagedFile.errorString();
            return false;
        }
        entry.raw = false;
        entry.stagedPath = staged;
    }
    return true;
}

bool RarArchiveWriter::rewriteRawName(Entry* entry, const QByteArray& name,
                                      QString* error)
{
    if (!entry || !entry->raw || !entry->editable ||
        entry->rawNameOffset < 0 || entry->rawNameLengthOffset < 0) {
        if (error) *error = QStringLiteral("This RAR member name cannot be rewritten.");
        return false;
    }
    if (version_ != RarArchiveVersion::Rar4) {
        const int bodyOffset = 4 + entry->rawHeaderSizeLength;
        if (entry->rawHeaderSizeLength <= 0 ||
            entry->rawNameLengthOffset < bodyOffset ||
            entry->rawNameOffset < entry->rawNameLengthOffset ||
            entry->rawNameOffset > entry->rawHeader.size()) {
            if (error) *error = QStringLiteral("The RAR5 member name offsets are invalid.");
            return false;
        }
        int oldNameLengthBytes = entry->rawNameLengthOffset;
        quint64 oldNameLength = 0;
        if (!readRar5Vint(entry->rawHeader, &oldNameLengthBytes, &oldNameLength) ||
            oldNameLength > quint64(entry->rawHeader.size() - entry->rawNameOffset)) {
            if (error) *error = QStringLiteral("The RAR5 member name range is invalid.");
            return false;
        }
        const int relativeLengthOffset = entry->rawNameLengthOffset - bodyOffset;
        const int relativeNameOffset = entry->rawNameOffset - bodyOffset;
        QByteArray body = entry->rawHeader.mid(bodyOffset);
        body.replace(relativeNameOffset, int(oldNameLength), name);
        QByteArray encodedLength;
        appendVint(&encodedLength, quint64(name.size()));
        body.replace(relativeLengthOffset, oldNameLengthBytes - entry->rawNameLengthOffset,
                     encodedLength);
        QByteArray sizeBytes;
        appendVint(&sizeBytes, quint64(body.size()));
        QByteArray checksumData = sizeBytes + body;
        QByteArray updated;
        append32(&updated, crc32(checksumData));
        updated += checksumData;
        entry->rawHeader.swap(updated);
        entry->rawHeaderSizeLength = sizeBytes.size();
        entry->rawNameLengthOffset = 4 + sizeBytes.size() + relativeLengthOffset;
        entry->rawNameLengthBytes = encodedLength.size();
        entry->rawNameOffset = entry->rawNameLengthOffset + encodedLength.size();
        return true;
    }
    if (version_ != RarArchiveVersion::Rar4 || entry->rawNameLengthBytes != 2) {
        if (error) *error = QStringLiteral("This RAR header type cannot be renamed yet.");
        return false;
    }
    if (name.size() > std::numeric_limits<quint16>::max() ||
        entry->rawNameOffset > entry->rawHeader.size()) {
        if (error) *error = QStringLiteral("The RAR member name is too long.");
        return false;
    }
    QByteArray header = entry->rawHeader;
    const quint16 oldLength = read16(header.constData() + entry->rawNameLengthOffset);
    if (entry->rawNameOffset + oldLength > header.size()) {
        if (error) *error = QStringLiteral("The RAR member header has an invalid name range.");
        return false;
    }
    header.replace(entry->rawNameOffset, oldLength, name);
    header[entry->rawNameLengthOffset] = char(name.size() & 0xff);
    header[entry->rawNameLengthOffset + 1] = char((name.size() >> 8) & 0xff);
    header[5] = char(header.size() & 0xff);
    header[6] = char((header.size() >> 8) & 0xff);
    quint16 flags = read16(header.constData() + 3);
    const bool unicodeName = std::any_of(name.begin(), name.end(),
        [](char value) { return (static_cast<unsigned char>(value) & 0x80U) != 0; });
    if (unicodeName) flags |= 0x0200U;
    else flags &= quint16(~0x0200U);
    header[3] = char(flags & 0xffU);
    header[4] = char((flags >> 8) & 0xffU);
    const quint16 checksum = quint16(crc32(header.constData() + 2,
                                           header.size() - 2));
    header[0] = char(checksum & 0xffU);
    header[1] = char((checksum >> 8) & 0xffU);
    entry->rawHeader.swap(header);
    return true;
}

bool RarArchiveWriter::beginCreate(const QString& outputPath,
                                   RarArchiveVersion version,
                                   unsigned compressionMethod, QString* error)
{
    if (active_ || outputPath.isEmpty() || !error || compressionMethod > 5)
        return false;
    const QFileInfo outputInfo(outputPath);
    if (!outputInfo.absoluteDir().exists()) {
        *error = QStringLiteral("The output directory does not exist.");
        return false;
    }
    const QString stageTemplate = outputInfo.absoluteDir().filePath(
        QStringLiteral(".peare-rar-XXXXXX"));
    staging_.reset(new (std::nothrow) QTemporaryDir(stageTemplate));
    if (!staging_ || !staging_->isValid()) {
        staging_.reset();
        *error = QStringLiteral("Could not create RAR staging beside the output.");
        return false;
    }
    outputPath_ = outputInfo.absoluteFilePath();
    version_ = version;
    compressionMethod_ = compressionMethod;
    nextStageIndex_ = 0;
    entries_.clear();
    active_ = true;
    return true;
}

bool RarArchiveWriter::addFile(const QString& sourcePath,
                               const QByteArray& entryName, QString* error)
{
    if (!active_ || sourcePath.isEmpty() || entryName.isEmpty() || !error) return false;
    const QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.isFile()) {
        *error = QStringLiteral("The RAR source must be a regular file.");
        return false;
    }
    if (conflictsWithEntry(entries_, entryName, false)) {
        *error = QStringLiteral("The RAR path conflicts with an existing entry.");
        return false;
    }
    QString stagedPath;
    if (!stageFile(sourcePath, &stagedPath, error)) return false;
    Entry entry;
    entry.name = entryName;
    entry.stagedPath = stagedPath;
    entries_.push_back(std::move(entry));
    return true;
}

bool RarArchiveWriter::addEmptyFile(const QByteArray& entryName, QString* error)
{
    if (!active_ || entryName.isEmpty() || !error) return false;
    if (conflictsWithEntry(entries_, entryName, false)) {
        *error = QStringLiteral("The RAR path conflicts with an existing entry.");
        return false;
    }
    const QString stagedPath = staging_->filePath(
        QStringLiteral("entry-%1").arg(nextStageIndex_++));
    QFile empty(stagedPath);
    if (!empty.open(QIODevice::WriteOnly)) {
        *error = empty.errorString();
        return false;
    }
    empty.close();
    Entry entry;
    entry.name = entryName;
    entry.stagedPath = stagedPath;
    entries_.push_back(std::move(entry));
    return true;
}

bool RarArchiveWriter::addDirectory(const QByteArray& entryName, QString* error)
{
    if (!active_ || entryName.isEmpty() || !error) return false;
    if (conflictsWithEntry(entries_, entryName, true)) {
        *error = QStringLiteral("The RAR path conflicts with an existing entry.");
        return false;
    }
    Entry entry;
    entry.name = entryName;
    entry.directory = true;
    entries_.push_back(std::move(entry));
    return true;
}

bool RarArchiveWriter::stageFile(const QString& sourcePath, QString* stagedPath,
                                 QString* error)
{
    if (!stagedPath || !error) return false;
    const QString path = staging_->filePath(
        QStringLiteral("entry-%1").arg(nextStageIndex_++));
    if (!copyPath(sourcePath, path)) {
        *error = QStringLiteral("Could not stage the RAR source file.");
        return false;
    }
    *stagedPath = path;
    return true;
}

bool RarArchiveWriter::replaceFile(const QString& sourcePath,
                                   const QByteArray& entryName, QString* error)
{
    if (!active_ || sourcePath.isEmpty() || entryName.isEmpty() || !error) return false;
    const QFileInfo sourceInfo(sourcePath);
    if (!sourceInfo.isFile()) {
        *error = QStringLiteral("The RAR replacement must be a regular file.");
        return false;
    }
    auto found = std::find_if(entries_.begin(), entries_.end(),
        [&](const Entry& entry) { return entry.name == entryName; });
    if (found == entries_.end() || found->directory) {
        *error = QStringLiteral("The RAR file to replace was not found.");
        return false;
    }
    QString stagedPath;
    if (!stageFile(sourcePath, &stagedPath, error)) return false;
    found->stagedPath = stagedPath;
    found->raw = false;
    found->rawHeader.clear();
    found->rawDataOffset = 0;
    found->rawDataSize = 0;
    return true;
}

bool RarArchiveWriter::renameEntry(const QByteArray& oldName,
                                   const QByteArray& newName, QString* error)
{
    if (!active_ || oldName.isEmpty() || newName.isEmpty() || !error) return false;
    auto found = std::find_if(entries_.begin(), entries_.end(),
        [&](const Entry& entry) { return entry.name == oldName; });
    const bool hasDescendants = std::any_of(entries_.begin(), entries_.end(),
        [&](const Entry& entry) {
            return entry.name.startsWith(oldName + '/');
        });
    if (found == entries_.end() && !hasDescendants) {
        *error = QStringLiteral("The RAR entry to rename was not found.");
        return false;
    }
    const bool directory = found != entries_.end() ? found->directory : true;
    std::vector<Entry> moving;
    for (const Entry& entry : entries_) {
        if (entry.name == oldName || (directory && entry.name.startsWith(oldName + '/')))
            moving.push_back(entry);
    }
    std::vector<Entry> remaining;
    for (const Entry& entry : entries_) {
        if (entry.name != oldName && !(directory && entry.name.startsWith(oldName + '/')))
            remaining.push_back(entry);
    }
    for (Entry& entry : moving) {
        const QByteArray suffix = entry.name.mid(oldName.size());
        entry.name = newName + suffix;
        if (entry.raw && entry.editable &&
            !rewriteRawName(&entry, entry.name, error)) return false;
        if (conflictsWithEntry(remaining, entry.name, entry.directory)) {
            *error = QStringLiteral("The renamed RAR path conflicts with an existing entry.");
            return false;
        }
        for (const Entry& prior : remaining) {
            if (prior.name == entry.name ||
                (!entry.directory && prior.name.startsWith(entry.name + '/')) ||
                (!prior.directory && entry.name.startsWith(prior.name + '/'))) {
                *error = QStringLiteral("The renamed RAR path conflicts with another moved entry.");
                return false;
            }
        }
        remaining.push_back(entry);
    }
    entries_.swap(remaining);
    return true;
}

bool RarArchiveWriter::deleteEntry(const QByteArray& entryName, bool recursive,
                                   QString* error)
{
    if (!active_ || entryName.isEmpty() || !error) return false;
    const QByteArray prefix = entryName + '/';
    bool found = false;
    bool directory = false;
    bool hasChildren = false;
    for (const Entry& entry : entries_) {
        if (entry.name == entryName) {
            found = true;
            directory = entry.directory;
        } else if (entry.name.startsWith(prefix)) {
            hasChildren = true;
        }
    }
    if (!found && !hasChildren) {
        *error = QStringLiteral("The RAR entry to delete was not found.");
        return false;
    }
    if (hasChildren && !recursive) {
        *error = QStringLiteral("The RAR directory is not empty.");
        return false;
    }
    if (!directory && found && hasChildren) {
        *error = QStringLiteral("The RAR path conflicts with a file and directory.");
        return false;
    }
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
        [&](const Entry& entry) {
            return entry.name == entryName ||
                (recursive && entry.name.startsWith(prefix));
        }), entries_.end());
    return true;
}

bool RarArchiveWriter::writeRar5MultiVolumeArchive(QString* error)
{
    if (!staging_ || !volumeSizeLimit_ || sourceVolumePaths_.empty()) {
        *error = QStringLiteral("The RAR5 volume set has no usable size or path.");
        return false;
    }
    const QString finalFirstPath = outputPath_;
    const QString singlePath = staging_->filePath(QStringLiteral("single-rar5.rar"));
    const QByteArray savedPrefix = archivePrefix_;
    const QByteArray savedEnd = archiveEnd_;
    const bool savedMultiVolume = archiveMultiVolume_;
    const bool savedRecoveryWrite = writingRecoveryArchive_;
    outputPath_ = singlePath;
    archiveMultiVolume_ = false;
    archiveEnd_ = rar5EndHeader(false);
    writingRecoveryArchive_ = true;
    const bool baseWritten = writeArchive(error);
    writingRecoveryArchive_ = savedRecoveryWrite;
    if (!baseWritten) {
        outputPath_ = finalFirstPath;
        archivePrefix_ = savedPrefix;
        archiveEnd_ = savedEnd;
        archiveMultiVolume_ = savedMultiVolume;
        return false;
    }
    outputPath_ = finalFirstPath;
    archivePrefix_ = savedPrefix;
    archiveEnd_ = savedEnd;
    archiveMultiVolume_ = savedMultiVolume;

    QFile single(singlePath);
    const QByteArray marker(reinterpret_cast<const char*>(kRar5Marker),
                            int(sizeof(kRar5Marker)));
    if (!single.open(QIODevice::ReadOnly) || single.size() <= marker.size()) {
        *error = single.errorString().isEmpty()
            ? QStringLiteral("The staged RAR5 archive is truncated.")
            : single.errorString();
        return false;
    }
    QByteArray stagedMarker;
    if (!readExact(&single, 0, marker.size(), &stagedMarker) || stagedMarker != marker) {
        *error = QStringLiteral("The staged RAR5 marker is invalid.");
        return false;
    }
    qint64 position = marker.size();
    QByteArray mainCrc;
    QByteArray mainSizeBytes;
    quint64 mainBodySize = 0;
    if (!readExact(&single, position, 4, &mainCrc)) {
        *error = QStringLiteral("The staged RAR5 main header is truncated.");
        return false;
    }
    position += 4;
    if (!readFileVint(&single, &position, &mainBodySize, &mainSizeBytes) ||
        !mainBodySize || mainBodySize > 1024U * 1024U) {
        *error = QStringLiteral("The staged RAR5 main header is malformed.");
        return false;
    }
    QByteArray mainBody;
    if (!readExact(&single, position, qint64(mainBodySize), &mainBody) ||
        crc32(mainSizeBytes + mainBody) != read32(mainCrc.constData())) {
        *error = QStringLiteral("The staged RAR5 main header is corrupt.");
        return false;
    }
    position += qint64(mainBodySize);
    int mainCursor = 0;
    quint64 mainType = 0;
    if (!readRar5Vint(mainBody, &mainCursor, &mainType) || mainType != 1) {
        *error = QStringLiteral("The staged RAR5 main header has an invalid type.");
        return false;
    }
    const quint64 limit = volumeSizeLimit_;
    const QByteArray endSize = rar5EndHeader(false);
    const bool addRecovery = archiveRecovery_ && !writingRecoveryArchive_;
    const quint64 recoveryPayloadReserve = addRecovery
        ? rar5RecoveryPayloadSize(limit, recoveryPercent_) : 0;
    const QByteArray recoveryHeaderReserve = addRecovery
        ? rar5RecoveryHeader(recoveryPayloadReserve, recoveryPercent_)
        : QByteArray();
    const quint64 recoveryReserve = quint64(recoveryHeaderReserve.size()) +
                                    recoveryPayloadReserve + (addRecovery ? 16U : 0U);
    if (addRecovery && (!recoveryPayloadReserve ||
        recoveryReserve > limit ||
        quint64(endSize.size()) > limit - recoveryReserve)) {
        *error = QStringLiteral("The RAR5 volume is too small for its recovery record.");
        return false;
    }
    QVector<QString> stagedVolumes;
    QVector<QString> targetVolumes;
    QFile* volume = nullptr;
    quint64 volumeBytes = 0;
    auto closeVolume = [&](bool hasNext) -> bool {
        if (!volume) return true;
        const QByteArray end = rar5EndHeader(hasNext);
        const QString committedPath = volume->fileName();
        bool ok = false;
        if (addRecovery) {
            ok = volume->flush();
            volume->close();
            delete volume;
            volume = nullptr;
            ok = ok && rewriteRar5VolumeRecoveryLocator(
                committedPath, stagedVolumes.size() - 1, error);
            QFile base(committedPath);
            const quint64 protectedSize = ok && base.open(QIODevice::ReadOnly)
                ? quint64(base.size()) : 0;
            const quint64 payloadSize = rar5RecoveryPayloadSize(
                protectedSize, recoveryPercent_);
            const QByteArray recoveryHeader = payloadSize
                ? rar5RecoveryHeader(payloadSize, recoveryPercent_) : QByteArray();
            QTemporaryFile recoveryPayload(staging_->filePath(
                QStringLiteral("rar5-volume-recovery-XXXXXX")));
            if (!protectedSize || !recoveryPayload.open() ||
                !writeRar5RecoveryPayload(&base, protectedSize, recoveryPercent_,
                                          &recoveryPayload, error) ||
                quint64(recoveryPayload.size()) != payloadSize ||
                !recoveryPayload.flush() || !recoveryPayload.seek(0) ||
                quint64(QFileInfo(committedPath).size()) +
                    quint64(recoveryHeader.size()) + payloadSize +
                    quint64(end.size()) > limit) {
                if (error && error->isEmpty()) *error = QStringLiteral(
                    "The RAR5 recovery record does not fit in its volume.");
            } else {
                QFile append(committedPath);
                ok = append.open(QIODevice::Append) &&
                    writeAll(&append, recoveryHeader.constData(),
                             recoveryHeader.size()) &&
                    copyFile(&recoveryPayload, &append) &&
                    writeAll(&append, end.constData(), end.size()) &&
                    append.flush();
                if (!ok && error) *error = append.errorString().isEmpty()
                    ? QStringLiteral("Could not append the RAR5 recovery record.")
                    : append.errorString();
            }
        } else {
            ok = volumeBytes + quint64(end.size()) <= limit &&
                writeAll(volume, end.constData(), end.size()) && volume->flush();
            if (!ok && error) *error = volume->errorString().isEmpty()
                ? QStringLiteral("The RAR5 volume exceeds the source volume size.")
                : volume->errorString();
            delete volume;
            volume = nullptr;
        }
        if (ok) stagedVolumes.back() = committedPath;
        return ok;
    };
    auto openVolume = [&]() -> bool {
        const int index = stagedVolumes.size();
        const QString target = rar4VolumePath(finalFirstPath, index);
        if (target.isEmpty()) {
            *error = QStringLiteral("Could not name the next RAR5 volume.");
            return false;
        }
        QFile* next = new (std::nothrow) QFile(
            staging_->filePath(QStringLiteral("volume5-%1.rar").arg(index)));
        if (!next || !next->open(QIODevice::WriteOnly)) {
            if (error) *error = next ? next->errorString()
                : QStringLiteral("Could not allocate a RAR5 volume file.");
            delete next;
            return false;
        }
        const QByteArray volumeMain = rar5VolumeMainHeader(index, addRecovery);
        if (!writeAll(next, marker.constData(), marker.size()) ||
            !writeAll(next, volumeMain.constData(), volumeMain.size())) {
            *error = next->errorString();
            delete next;
            return false;
        }
        volumeBytes = quint64(marker.size() + volumeMain.size());
        stagedVolumes.push_back(next->fileName());
        targetVolumes.push_back(target);
        volume = next;
        return true;
    };
    auto nextVolume = [&]() -> bool {
        return closeVolume(true) && openVolume();
    };
    auto writePayload = [&](quint64 offset, quint64 amount) -> bool {
        if (!copyRange(&single, volume, qint64(offset), amount)) {
            if (error) *error = volume->errorString().isEmpty()
                ? single.errorString() : volume->errorString();
            return false;
        }
        volumeBytes += amount;
        return true;
    };
    if (!openVolume()) return false;

    const qint64 archiveSize = single.size();
    bool sawEnd = false;
    while (position < archiveSize) {
        const qint64 headerOffset = position;
        QByteArray crcBytes;
        QByteArray sizeBytes;
        quint64 headerSize = 0;
        QByteArray body;
        if (!readExact(&single, position, 4, &crcBytes)) break;
        position += 4;
        if (!readFileVint(&single, &position, &headerSize, &sizeBytes) ||
            !headerSize || headerSize > 16U * 1024U * 1024U ||
            headerSize > quint64(archiveSize - position) ||
            !readExact(&single, position, qint64(headerSize), &body) ||
            crc32(sizeBytes + body) != read32(crcBytes.constData())) break;
        position += qint64(headerSize);
        int cursor = 0;
        quint64 type = 0;
        quint64 flags = 0;
        quint64 extraSize = 0;
        quint64 dataSize = 0;
        if (!readRar5Vint(body, &cursor, &type) ||
            !readRar5Vint(body, &cursor, &flags) ||
            ((flags & 1U) && !readRar5Vint(body, &cursor, &extraSize)) ||
            ((flags & 2U) && !readRar5Vint(body, &cursor, &dataSize)) ||
            dataSize > quint64(archiveSize - position)) break;
        if (type == 5) {
            sawEnd = true;
            break;
        }
        const QByteArray originalHeader = crcBytes + sizeBytes + body;
        const quint64 dataOffset = quint64(position);
        if (type == 2 && (flags & 2U) && dataSize) {
            quint64 consumed = 0;
            while (consumed < dataSize) {
                quint64 low = 0;
                quint64 high = dataSize - consumed;
                quint64 best = 0;
                QByteArray bestHeader;
                while (low <= high) {
                    const quint64 amount = low + (high - low) / 2U;
                    if (!amount) {
                        low = 1;
                        continue;
                    }
                    const bool splitBefore = consumed != 0;
                    const bool splitAfter = amount < dataSize - consumed;
                    const QByteArray partHeader = rar5WithPackedSize(
                        originalHeader, amount, splitBefore, splitAfter, false, 0);
                    if (partHeader.isEmpty()) {
                        *error = QStringLiteral("A RAR5 split file header could not be rebuilt.");
                        if (volume) { volume->close(); delete volume; volume = nullptr; }
                        return false;
                    }
                    const quint64 required = quint64(partHeader.size()) + amount +
                                             quint64(endSize.size()) + recoveryReserve;
                    if (required <= limit - std::min(limit, volumeBytes)) {
                        best = amount;
                        bestHeader = partHeader;
                        low = amount + 1U;
                    } else {
                        if (amount == 1U) break;
                        high = amount - 1U;
                    }
                }
                if (!best) {
                    if (volumeBytes + quint64(endSize.size()) + recoveryReserve >= limit ||
                        !nextVolume())
                        return false;
                    continue;
                }
                const bool splitAfter = best < dataSize - consumed;
                quint32 packedPartCrc = 0;
                if (splitAfter && !crc32Range(&single,
                        dataOffset + consumed, best, &packedPartCrc)) {
                    *error = single.errorString().isEmpty()
                        ? QStringLiteral("Could not calculate a RAR5 split-part checksum.")
                        : single.errorString();
                    if (volume) { volume->close(); delete volume; volume = nullptr; }
                    return false;
                }
                bestHeader = rar5WithPackedSize(originalHeader, best,
                    consumed != 0, splitAfter, splitAfter, packedPartCrc);
                if (bestHeader.isEmpty()) {
                    *error = QStringLiteral("A RAR5 split checksum header could not be rebuilt.");
                    if (volume) { volume->close(); delete volume; volume = nullptr; }
                    return false;
                }
                if (!writeAll(volume, bestHeader.constData(), bestHeader.size()) ||
                    !writePayload(dataOffset + consumed, best)) {
                    if (error) *error = volume->errorString();
                    return false;
                }
                volumeBytes += quint64(bestHeader.size());
                consumed += best;
                if (consumed < dataSize && !nextVolume()) return false;
            }
        } else {
            const quint64 required = quint64(originalHeader.size()) + dataSize +
                                     quint64(endSize.size()) + recoveryReserve;
            if (required > limit) {
                *error = QStringLiteral("A RAR5 block is larger than the volume limit.");
                if (volume) { volume->close(); delete volume; volume = nullptr; }
                return false;
            }
            if (volumeBytes + required > limit && !nextVolume()) return false;
            if (!writeAll(volume, originalHeader.constData(), originalHeader.size())) {
                *error = volume->errorString();
                return false;
            }
            volumeBytes += quint64(originalHeader.size());
            if (dataSize && !writePayload(dataOffset, dataSize)) return false;
        }
        position += qint64(dataSize);
        Q_UNUSED(headerOffset);
        Q_UNUSED(extraSize);
    }
    if (!sawEnd) {
        if (volume) { volume->close(); delete volume; volume = nullptr; }
        *error = QStringLiteral("The staged RAR5 archive has no end header.");
        return false;
    }
    if (!closeVolume(false)) return false;

    for (const QString& target : targetVolumes) {
        if (QFileInfo::exists(target) &&
            std::find(sourceVolumePaths_.begin(), sourceVolumePaths_.end(), target) ==
                sourceVolumePaths_.end()) {
            *error = QStringLiteral("A destination RAR5 volume already exists: %1")
                .arg(target);
            return false;
        }
    }
    QVector<QString> backups;
    backups.reserve(int(sourceVolumePaths_.size()));
    for (std::size_t index = 0; index < sourceVolumePaths_.size(); ++index) {
        const QString backup = staging_->filePath(
            QStringLiteral("original5-%1.rar").arg(qulonglong(index)));
        if (!QFile::rename(sourceVolumePaths_[index], backup)) {
            for (int moved = backups.size() - 1; moved >= 0; --moved)
                QFile::rename(backups[moved], sourceVolumePaths_[std::size_t(moved)]);
            *error = QStringLiteral("Could not stage the original RAR5 volume set.");
            return false;
        }
        backups.push_back(backup);
    }
    int installed = 0;
    for (; installed < stagedVolumes.size(); ++installed) {
        if (!QFile::rename(stagedVolumes[installed], targetVolumes[installed])) break;
    }
    if (installed != stagedVolumes.size()) {
        for (int index = 0; index < installed; ++index)
            QFile::remove(targetVolumes[index]);
        for (int index = backups.size() - 1; index >= 0; --index)
            QFile::rename(backups[index], sourceVolumePaths_[std::size_t(index)]);
        *error = QStringLiteral("Could not publish the updated RAR5 volume set.");
        return false;
    }
    for (const QString& backup : backups) QFile::remove(backup);
    return true;
}

bool RarArchiveWriter::writeRar4MultiVolumeArchive(QString* error)
{
    if (!staging_ || !volumeSizeLimit_ || sourceVolumePaths_.empty()) {
        *error = QStringLiteral("The RAR4 volume set has no usable size or path.");
        return false;
    }

    const QString finalFirstPath = outputPath_;
    const QString singlePath = staging_->filePath(QStringLiteral("single.rar"));
    const QByteArray savedPrefix = archivePrefix_;
    const QByteArray savedEnd = archiveEnd_;
    const bool savedMultiVolume = archiveMultiVolume_;
    const bool savedRecovery = archiveRecovery_;
    outputPath_ = singlePath;
    archiveMultiVolume_ = false;
    archiveRecovery_ = false;
    archiveEnd_ = rar4EndHeader(false);
    if (!writeArchive(error)) {
        outputPath_ = finalFirstPath;
        archivePrefix_ = savedPrefix;
        archiveEnd_ = savedEnd;
        archiveMultiVolume_ = savedMultiVolume;
        archiveRecovery_ = savedRecovery;
        return false;
    }
    outputPath_ = finalFirstPath;
    archivePrefix_ = savedPrefix;
    archiveEnd_ = savedEnd;
    archiveMultiVolume_ = savedMultiVolume;
    archiveRecovery_ = savedRecovery;

    QFile single(singlePath);
    if (!single.open(QIODevice::ReadOnly)) {
        *error = single.errorString();
        return false;
    }
    QByteArray marker;
    QByteArray mainHeader;
    if (!readExact(&single, 0, qint64(sizeof(kRar4Marker)), &marker) ||
        marker != QByteArray(reinterpret_cast<const char*>(kRar4Marker),
                             int(sizeof(kRar4Marker))) ||
        !readExact(&single, qint64(sizeof(kRar4Marker)), 7, &mainHeader)) {
        *error = QStringLiteral("The staged RAR4 archive has no valid header.");
        return false;
    }
    const quint16 mainSize = read16(mainHeader.constData() + 5);
    if (mainSize < 13 || !readExact(&single, qint64(sizeof(kRar4Marker)),
                                     mainSize, &mainHeader) ||
        quint16(crc32(mainHeader.constData() + 2, mainHeader.size() - 2)) !=
            read16(mainHeader.constData())) {
        *error = QStringLiteral("The staged RAR4 main header is corrupt.");
        return false;
    }

    QVector<QString> stagedVolumes;
    QVector<QString> targetVolumes;
    QFile* volume = nullptr;
    quint64 volumeBytes = 0;
    const quint64 limit = volumeSizeLimit_;
    auto closeVolume = [&](bool hasNext) -> bool {
        if (!volume) return true;
        bool ok = volume->flush();
        if (ok && archiveRecovery_) {
            volume->close();
            ok = volume->open(QIODevice::ReadWrite);
            QByteArray recoveryHeader;
            QByteArray recoveryPayload;
            ok = ok && makeRar4RecoveryBlock(volume, volumeBytes, recoverySectors_,
                                       &recoveryHeader, &recoveryPayload, error) &&
                 volume->seek(qint64(volumeBytes)) &&
                 writeAll(volume, recoveryHeader.constData(), recoveryHeader.size()) &&
                 writeAll(volume, recoveryPayload.constData(), recoveryPayload.size());
            if (ok) volumeBytes += quint64(recoveryHeader.size() + recoveryPayload.size());
        }
        const QByteArray end = rar4EndHeader(hasNext);
        ok = ok && volumeBytes + quint64(end.size()) <= limit &&
             writeAll(volume, end.constData(), end.size());
        const QString committedPath = volume->fileName();
        ok = ok && volume->flush();
        if (!ok && error) *error = volume->errorString().isEmpty()
            ? QStringLiteral("The RAR4 volume exceeds its source volume size.")
            : volume->errorString();
        delete volume;
        volume = nullptr;
        if (ok) stagedVolumes.back() = committedPath;
        return ok;
    };
    auto openVolume = [&]() -> bool {
        const int index = stagedVolumes.size();
        const QString target = rar4VolumePath(finalFirstPath, index);
        if (target.isEmpty()) {
            *error = QStringLiteral("Could not name the next RAR4 volume.");
            return false;
        }
        QFile* next = new (std::nothrow) QFile(
            staging_->filePath(QStringLiteral("volume-%1.rar").arg(index)));
        if (!next || !next->open(QIODevice::WriteOnly)) {
            if (error) *error = next ? next->errorString()
                : QStringLiteral("Could not allocate a RAR4 volume file.");
            delete next;
            return false;
        }
        QByteArray volumeMain = mainHeader;
        quint16 flags = read16(volumeMain.constData() + 3);
        flags |= 0x0001U;
        if (archiveRecovery_) flags |= 0x0040U;
        if (index == 0) flags |= 0x0100U;
        else flags &= quint16(~0x0100U);
        volumeMain[3] = char(flags & 0xffU);
        volumeMain[4] = char(flags >> 8);
        const quint16 checksum = quint16(crc32(volumeMain.constData() + 2,
                                               volumeMain.size() - 2));
        volumeMain[0] = char(checksum & 0xffU);
        volumeMain[1] = char(checksum >> 8);
        if (!writeAll(next, marker.constData(), marker.size()) ||
            !writeAll(next, volumeMain.constData(), volumeMain.size())) {
            *error = next->errorString();
            delete next;
            return false;
        }
        volumeBytes = quint64(marker.size() + volumeMain.size());
        stagedVolumes.push_back(next->fileName());
        targetVolumes.push_back(target);
        volume = next;
        return true;
    };
    auto nextVolume = [&]() -> bool {
        if (!closeVolume(true)) return false;
        return openVolume();
    };
    auto writePayload = [&](QFile* source, quint64 offset,
                            quint64 amount) -> bool {
        if (!source->seek(qint64(offset)) ||
            !copyRange(source, volume, qint64(offset), amount)) {
            if (error) *error = source->errorString().isEmpty()
                ? volume->errorString() : source->errorString();
            return false;
        }
        volumeBytes += amount;
        return true;
    };

    if (!openVolume()) return false;
    qint64 position = qint64(marker.size()) + mainSize;
    const qint64 archiveSize = single.size();
    bool sawEnd = false;
    while (position <= archiveSize - 7) {
        QByteArray fixed;
        if (!readExact(&single, position, 7, &fixed)) break;
        const quint16 headerSize = read16(fixed.constData() + 5);
        if (headerSize < 7 || position > archiveSize - headerSize) break;
        QByteArray header;
        if (!readExact(&single, position, headerSize, &header) ||
            quint16(crc32(header.constData() + 2, header.size() - 2)) !=
                read16(header.constData())) break;
        const quint8 type = quint8(header.at(2));
        quint64 dataSize = 0;
        if (read16(header.constData() + 3) & 0x8000U) {
            if (headerSize < 11) break;
            dataSize = read32(header.constData() + 7);
        }
        if (type == 0x74 && (read16(header.constData() + 3) & 0x0100U)) {
            if (headerSize < 40) break;
            dataSize |= quint64(read32(header.constData() + 32)) << 32;
        }
        const qint64 dataOffset = position + headerSize;
        if (dataSize > quint64(archiveSize - dataOffset)) break;
        if (type == 0x7b) { sawEnd = true; break; }

        if (type == 0x74 && dataSize) {
            quint64 consumed = 0;
            while (consumed < dataSize) {
                const quint64 recoveryReserve = archiveRecovery_
                    ? 54U + quint64(recoverySectors_) * 512U + (limit + 255U) / 256U
                    : 0U;
                const quint64 reserved = quint64(headerSize) + 7U + recoveryReserve;
                if (volumeBytes + reserved >= limit) {
                    if (!nextVolume()) return false;
                }
                const quint64 available = limit - volumeBytes - reserved;
                const quint64 amount = std::min(dataSize - consumed, available);
                if (!amount) {
                    *error = QStringLiteral("The RAR4 volume limit cannot fit a file part.");
                    return false;
                }
                const bool splitBefore = consumed != 0;
                const bool splitAfter = consumed + amount < dataSize;
                const QByteArray partHeader = rar4WithPackedSize(
                    header, amount, splitBefore, splitAfter);
                if (!writeAll(volume, partHeader.constData(), partHeader.size())) {
                    *error = volume->errorString();
                    return false;
                }
                volumeBytes += quint64(partHeader.size());
                if (!writePayload(&single, quint64(dataOffset) + consumed, amount))
                    return false;
                consumed += amount;
                if (splitAfter && !nextVolume()) return false;
            }
        } else {
            const quint64 recoveryReserve = archiveRecovery_
                ? 54U + quint64(recoverySectors_) * 512U + (limit + 255U) / 256U
                : 0U;
            const quint64 required = quint64(header.size()) + dataSize + 7U +
                                     recoveryReserve;
            if (volumeBytes + required > limit && !nextVolume()) return false;
            if (volumeBytes + required > limit) {
                *error = QStringLiteral("A RAR4 block is larger than the volume limit.");
                return false;
            }
            if (!writeAll(volume, header.constData(), header.size())) {
                *error = volume->errorString();
                return false;
            }
            volumeBytes += quint64(header.size());
            if (dataSize && !writePayload(&single, quint64(dataOffset), dataSize))
                return false;
        }
        position = dataOffset + qint64(dataSize);
    }
    if (!sawEnd) {
        if (volume) volume->close();
        delete volume;
        volume = nullptr;
        *error = QStringLiteral("The staged RAR4 archive has no end header.");
        return false;
    }
    if (!closeVolume(false)) return false;

    for (const QString& target : targetVolumes) {
        if (QFileInfo::exists(target) &&
            std::find(sourceVolumePaths_.begin(), sourceVolumePaths_.end(), target) ==
                sourceVolumePaths_.end()) {
            *error = QStringLiteral("A destination RAR4 volume already exists: %1")
                .arg(target);
            return false;
        }
    }

    QVector<QString> backups;
    backups.reserve(int(sourceVolumePaths_.size()));
    for (std::size_t index = 0; index < sourceVolumePaths_.size(); ++index) {
        const QString backup = staging_->filePath(
            QStringLiteral("original-%1.rar").arg(qulonglong(index)));
        if (!QFile::rename(sourceVolumePaths_[index], backup)) {
            for (int moved = backups.size() - 1; moved >= 0; --moved)
                QFile::rename(backups[moved], sourceVolumePaths_[std::size_t(moved)]);
            *error = QStringLiteral("Could not stage the original RAR volume set.");
            return false;
        }
        backups.push_back(backup);
    }
    int installed = 0;
    for (; installed < stagedVolumes.size(); ++installed) {
        if (!QFile::rename(stagedVolumes[installed], targetVolumes[installed])) break;
    }
    if (installed != stagedVolumes.size()) {
        for (int index = 0; index < installed; ++index)
            QFile::remove(targetVolumes[index]);
        for (int index = backups.size() - 1; index >= 0; --index)
            QFile::rename(backups[index], sourceVolumePaths_[std::size_t(index)]);
        *error = QStringLiteral("Could not publish the updated RAR4 volume set.");
        return false;
    }
    for (const QString& backup : backups) QFile::remove(backup);
    return true;
}

bool RarArchiveWriter::writeRar4RecoveryArchive(QString* error)
{
    if (!recoverySectors_ || !staging_) {
        *error = QStringLiteral("The RAR4 recovery record has no sector parameters.");
        return false;
    }
    const QString finalPath = outputPath_;
    const QString basePath = staging_->filePath(QStringLiteral("recovery-base.rar"));
    const QByteArray savedEnd = archiveEnd_;
    const bool savedRecovery = archiveRecovery_;
    outputPath_ = basePath;
    archiveRecovery_ = false;
    if (!writeArchive(error)) {
        outputPath_ = finalPath;
        archiveRecovery_ = savedRecovery;
        archiveEnd_ = savedEnd;
        return false;
    }
    outputPath_ = finalPath;
    archiveRecovery_ = savedRecovery;
    archiveEnd_ = savedEnd;

    QFile base(basePath);
    if (!base.open(QIODevice::ReadOnly) || base.size() < 7) {
        *error = base.errorString().isEmpty()
            ? QStringLiteral("The staged RAR4 archive is truncated.")
            : base.errorString();
        return false;
    }
    const qint64 endOffset = base.size() - 7;
    QByteArray end;
    if (!readExact(&base, endOffset, 7, &end) || quint8(end.at(2)) != 0x7b) {
        *error = QStringLiteral("The staged RAR4 end header is invalid.");
        return false;
    }
    QByteArray recoveryHeader;
    QByteArray recoveryPayload;
    if (!makeRar4RecoveryBlock(&base, quint64(endOffset), recoverySectors_,
                               &recoveryHeader, &recoveryPayload, error))
        return false;
    QSaveFile output(finalPath);
    if (!output.open(QIODevice::WriteOnly) ||
        !copyRange(&base, &output, 0, quint64(endOffset)) ||
        !writeAll(&output, recoveryHeader.constData(), recoveryHeader.size()) ||
        !writeAll(&output, recoveryPayload.constData(), recoveryPayload.size()) ||
        !writeAll(&output, end.constData(), end.size()) || !output.commit()) {
        *error = output.errorString().isEmpty()
            ? QStringLiteral("Could not publish the updated RAR4 recovery record.")
            : output.errorString();
        return false;
    }
    return true;
}

bool RarArchiveWriter::writeArchive(QString* error)
{
    if (updateMode_ && version_ != RarArchiveVersion::Rar4 && archiveMultiVolume_)
        return writeRar5MultiVolumeArchive(error);
    if (updateMode_ && version_ != RarArchiveVersion::Rar4 && archiveRecovery_ &&
        !writingRecoveryArchive_)
        return writeRar5RecoveryArchive(error);
    if (updateMode_ && version_ == RarArchiveVersion::Rar4 && archiveMultiVolume_)
        return writeRar4MultiVolumeArchive(error);
    if (updateMode_ && version_ == RarArchiveVersion::Rar4 && archiveRecovery_ &&
        !writingRecoveryArchive_)
        return writeRar4RecoveryArchive(error);
    QSaveFile output(outputPath_);
    if (!output.open(QIODevice::WriteOnly)) {
        *error = output.errorString();
        return false;
    }
    QFile original(updateSourcePath_);
    if (updateMode_) {
        if (!original.open(QIODevice::ReadOnly) ||
            !writeAll(&output, archivePrefix_.constData(), archivePrefix_.size())) {
            *error = original.isOpen() ? output.errorString() : original.errorString();
            return false;
        }
    } else if (version_ == RarArchiveVersion::Rar4) {
        if (!writeAll(&output, reinterpret_cast<const char*>(kRar4Marker),
                      sizeof(kRar4Marker))) {
            *error = output.errorString();
            return false;
        }
        QByteArray mainHeader;
        append16(&mainHeader, 0);
        mainHeader.append(char(0x73));
        append16(&mainHeader, 0);
        append16(&mainHeader, 13);
        append16(&mainHeader, 0);
        append32(&mainHeader, 0);
        const quint16 mainCrc = quint16(crc32(mainHeader.mid(2)) & 0xffffU);
        mainHeader[0] = char(mainCrc & 0xffU);
        mainHeader[1] = char((mainCrc >> 8) & 0xffU);
        if (!writeAll(&output, mainHeader.constData(), mainHeader.size())) {
            *error = output.errorString();
            return false;
        }
    } else {
        const std::uint8_t* marker = kRar5Marker;
        const qint64 markerSize = qint64(sizeof(kRar5Marker));
        if (!writeAll(&output, reinterpret_cast<const char*>(marker), markerSize)) {
            *error = output.errorString();
            return false;
        }
        QByteArray mainBody;
        appendVint(&mainBody, 1);
        appendVint(&mainBody, 0);
        appendVint(&mainBody, 0);
        const QByteArray mainHeader = rar5Block(mainBody);
        if (!writeAll(&output, mainHeader.constData(), mainHeader.size())) {
            *error = output.errorString();
            return false;
        }
    }
    std::size_t entryIndex = 0;
    for (const Entry& entry : entries_) {
        if (entry.raw) {
            if (entry.recoveryRecord) continue;
            QByteArray rawHeader = entry.rawHeader;
            if (version_ != RarArchiveVersion::Rar4 && entry.editable &&
                !entry.directory) {
                rawHeader = rar5WithPackedSize(entry.rawHeader,
                    entry.rawDataSize, false, false, true, entry.unpackedCrc);
                if (rawHeader.isEmpty()) {
                    *error = QStringLiteral(
                        "A preserved RAR5 file header could not be rebuilt.");
                    return false;
                }
            }
            bool copied = writeAll(&output, rawHeader.constData(),
                                   rawHeader.size());
            if (copied && entry.rawChunks.empty())
                copied = copyRange(&original, &output,
                                   entry.rawDataOffset, entry.rawDataSize);
            else if (copied) {
                quint64 total = 0;
                for (const DataChunk& chunk : entry.rawChunks) {
                    QFile source(chunk.path);
                    if (chunk.size > entry.rawDataSize - total ||
                        !source.open(QIODevice::ReadOnly) ||
                        !copyRange(&source, &output, chunk.offset, chunk.size)) {
                        copied = false;
                        break;
                    }
                    total += chunk.size;
                }
                copied = copied && total == entry.rawDataSize;
            }
            if (!copied) {
                *error = output.errorString();
                return false;
            }
            continue;
        }
        const bool ok = version_ == RarArchiveVersion::Rar4
            ? writeRar4Entry(&output, entry.stagedPath,
                             staging_->filePath(QStringLiteral("packed-%1")
                                                .arg(entryIndex++)),
                             entry.name, entry.directory, compressionMethod_, error)
            : writeRar5Entry(&output, entry.stagedPath,
                             staging_->filePath(QStringLiteral("packed-%1")
                                                .arg(entryIndex++)),
                             entry.name, entry.directory,
                             version_ == RarArchiveVersion::Rar7 ? 1U : 0U,
                             compressionMethod_, error);
        if (!ok) return false;
    }
    if (updateMode_) {
        if (!writeAll(&output, archiveEnd_.constData(), archiveEnd_.size())) {
            *error = output.errorString();
            return false;
        }
    } else if (version_ == RarArchiveVersion::Rar4) {
        QByteArray end;
        append16(&end, 0);
        end.append(char(0x7b));
        append16(&end, 0);
        append16(&end, 7);
        const quint16 endCrc = quint16(crc32(end.mid(2)) & 0xffffU);
        end[0] = char(endCrc & 0xffU);
        end[1] = char((endCrc >> 8) & 0xffU);
        if (!writeAll(&output, end.constData(), end.size())) {
            *error = output.errorString();
            return false;
        }
    } else {
        QByteArray endBody;
        appendVint(&endBody, 5);
        appendVint(&endBody, 0);
        appendVint(&endBody, 0);
        const QByteArray end = rar5Block(endBody);
        if (!writeAll(&output, end.constData(), end.size())) {
            *error = output.errorString();
            return false;
        }
    }
    original.close();
    if (!output.commit()) {
        *error = output.errorString();
        return false;
    }
    return true;
}

bool RarArchiveWriter::writeRar5RecoveryArchive(QString* error)
{
    if (!staging_ || !recoveryPercent_) {
        *error = QStringLiteral("The RAR5 recovery record has no valid percentage.");
        return false;
    }
    const QString finalPath = outputPath_;
    const QString basePath = staging_->filePath(QStringLiteral("recovery-base5.rar"));
    const QByteArray savedEnd = archiveEnd_;
    const bool savedRecoveryWrite = writingRecoveryArchive_;
    outputPath_ = basePath;
    archiveEnd_.clear();
    writingRecoveryArchive_ = true;
    if (!writeArchive(error)) {
        outputPath_ = finalPath;
        archiveEnd_ = savedEnd;
        writingRecoveryArchive_ = savedRecoveryWrite;
        return false;
    }
    outputPath_ = finalPath;
    archiveEnd_ = savedEnd;
    writingRecoveryArchive_ = savedRecoveryWrite;

    QFile base(basePath);
    if (!base.open(QIODevice::ReadOnly) || base.size() <= 0) {
        *error = base.errorString().isEmpty()
            ? QStringLiteral("The staged RAR5 archive is empty.") : base.errorString();
        return false;
    }
    const quint64 protectedSize = quint64(base.size());
    const quint64 dataShards = protectedSize >= 200U * 1024U ? 200U :
        std::max<quint64>(1U, (protectedSize + 1023U) / 1024U);
    quint64 recoveryShards = (quint64(recoveryPercent_) * dataShards) / 100U;
    if (!recoveryShards && protectedSize < 200U * 1024U) recoveryShards = 1;
    recoveryShards = std::min(recoveryShards, dataShards);
    quint64 groupSize = (protectedSize + dataShards - 1U) / dataShards;
    if (groupSize & 1U) ++groupSize;
    const quint64 shardSize = dataShards * 8U + 72U + groupSize;
    if (recoveryShards > std::numeric_limits<quint64>::max() / shardSize) {
        *error = QStringLiteral("The RAR5 recovery record size overflows.");
        return false;
    }
    const quint64 payloadSize = recoveryShards * shardSize;
    const QByteArray recoveryHeader = rar5RecoveryHeader(payloadSize,
                                                          recoveryPercent_);
    QTemporaryFile recoveryPayload(staging_->filePath(
        QStringLiteral("rar5-recovery-XXXXXX")));
    if (!recoveryPayload.open() ||
        !writeRar5RecoveryPayload(&base, protectedSize, recoveryPercent_,
                                  &recoveryPayload, error) ||
        quint64(recoveryPayload.size()) != payloadSize ||
        !recoveryPayload.flush() || !recoveryPayload.seek(0)) {
        if (error && error->isEmpty()) *error = recoveryPayload.errorString().isEmpty()
            ? QStringLiteral("Could not create the RAR5 recovery payload.")
            : recoveryPayload.errorString();
        return false;
    }
    QSaveFile output(finalPath);
    if (!output.open(QIODevice::WriteOnly) ||
        !copyRange(&base, &output, 0, protectedSize) ||
        !writeAll(&output, recoveryHeader.constData(), recoveryHeader.size()) ||
        !copyFile(&recoveryPayload, &output)) {
        *error = output.errorString().isEmpty()
            ? QStringLiteral("Could not write the RAR5 recovery record.")
            : output.errorString();
        return false;
    }
    const QByteArray end = rar5EndHeader(false);
    if (!writeAll(&output, end.constData(), end.size()) || !output.commit()) {
        *error = output.errorString();
        return false;
    }
    return true;
}

bool RarArchiveWriter::finish(QString* error)
{
    if (!active_ || !error) return false;
    const bool result = writeArchive(error);
    active_ = false;
    entries_.clear();
    updateSourcePath_.clear();
    archivePrefix_.clear();
    archiveEnd_.clear();
    updateMode_ = false;
    archiveSolid_ = false;
    archiveMultiVolume_ = false;
    archiveRecovery_ = false;
    recoverySectors_ = 0;
    recoveryPercent_ = 0;
    password_.fill(QChar(0));
    password_.clear();
    staging_->remove();
    staging_.reset();
    return result;
}

void RarArchiveWriter::abort()
{
    active_ = false;
    entries_.clear();
    outputPath_.clear();
    updateSourcePath_.clear();
    archivePrefix_.clear();
    archiveEnd_.clear();
    updateMode_ = false;
    archiveSolid_ = false;
    archiveMultiVolume_ = false;
    archiveRecovery_ = false;
    recoverySectors_ = 0;
    recoveryPercent_ = 0;
    password_.fill(QChar(0));
    password_.clear();
    if (staging_) staging_->remove();
    staging_.reset();
}

} // namespace writer
} // namespace peare
