#include "peare/peare_writer.h"
#include "formats/DiskImageWriter.h"

#include <QFile>
#include <QByteArray>
#include <QSaveFile>
#include <QString>
#include <QStringList>
#include <QSet>
#include <QTemporaryFile>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <vector>

namespace {

const quint32 kLocalHeaderSignature = 0x04034b50u;
const quint32 kCentralHeaderSignature = 0x02014b50u;
const quint32 kEndRecordSignature = 0x06054b50u;
const quint32 kDataDescriptorSignature = 0x08074b50u;
const quint32 kTarBlockSize = 512u;
const quint64 kZip32Max = 0xfffffffeull;
const quint64 kMaxZipUncompressed = 536870912ull;
const size_t kMaxZipEntries = 1000000u;
const size_t kChunkSize = 1024 * 1024;

void append16(QByteArray* bytes, quint16 value) {
    bytes->append(char(value & 0xff));
    bytes->append(char((value >> 8) & 0xff));
}

void append32(QByteArray* bytes, quint32 value) {
    append16(bytes, quint16(value & 0xffff));
    append16(bytes, quint16(value >> 16));
}

void append64(QByteArray* bytes, quint64 value) {
    append32(bytes, quint32(value));
    append32(bytes, quint32(value >> 32));
}

bool writeAll(QIODevice* device, const char* data, qint64 length) {
    qint64 written = 0;
    while (written < length) {
        const qint64 n = device->write(data + written, length - written);
        if (n <= 0) return false;
        written += n;
    }
    return true;
}

quint32 crc32Update(quint32 crc, const char* data, size_t length) {
    static const std::array<quint32, 256> table = [] {
        std::array<quint32, 256> values = {};
        for (quint32 i = 0; i < values.size(); ++i) {
            quint32 value = i;
            for (int bit = 0; bit < 8; ++bit)
                value = (value & 1) ? (value >> 1) ^ 0xedb88320u : value >> 1;
            values[i] = value;
        }
        return values;
    }();
    for (size_t i = 0; i < length; ++i)
        crc = table[(crc ^ static_cast<unsigned char>(data[i])) & 0xff] ^ (crc >> 8);
    return crc;
}

bool normalizedEntryName(const char* utf8, bool directory, QByteArray* encoded) {
    if (!utf8 || !*utf8 || !encoded) return false;
    QString name = QString::fromUtf8(utf8);
    if (name.isEmpty() || name.contains(QChar(0))) return false;
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (name.startsWith(QLatin1Char('/')) ||
        (name.size() >= 2 && name.at(1) == QLatin1Char(':')))
        return false;
    while (name.startsWith(QStringLiteral("./"))) name.remove(0, 2);
    const QStringList parts = name.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) return false;
    static const QString forbidden = QStringLiteral("<>:\"|?*");
    for (const QString& part : parts) {
        if (part == QStringLiteral(".") || part == QStringLiteral("..")) return false;
        if (part.endsWith(QLatin1Char(' ')) || part.endsWith(QLatin1Char('.'))) return false;
        for (const QChar c : part)
            if (c.unicode() < 0x20 || forbidden.contains(c)) return false;
    }
    name = parts.join(QLatin1Char('/'));
    if (directory) name += QLatin1Char('/');
    *encoded = name.toUtf8();
    return !encoded->isEmpty() && encoded->size() <= 0xffff;
}

struct Entry {
    QByteArray name;
    QByteArray centralRecord;
    QString key;
    quint32 crc = 0;
    quint64 size = 0;
    quint64 offset = 0;
    quint64 compressedSize = 0;
    quint64 uncompressedSize = 0;
    quint16 flags = 0;
    quint16 method = 0;
    bool directory = false;
    bool descriptor = false;
    bool zip64Sizes = false;
};

struct TarEntry {
    QString key;
    QByteArray header;
    qint64 offset = 0;
    quint64 size = 0;
    bool directory = false;
};

} // namespace

struct peare_writer_handle_s {
    std::unique_ptr<QSaveFile> output;
    std::unique_ptr<QTemporaryFile> zipStage;
    std::unique_ptr<QTemporaryFile> tarStage;
    std::vector<Entry> entries;
    std::vector<TarEntry> tarEntries;
    QSet<QString> names;
    QString error;
    QByteArray errorUtf8;
    QByteArray archiveComment;
    QString updatePath;
    std::unique_ptr<peare::writer::DiskImageUpdate> diskUpdate;
    qint64 tarContentEnd = 0;
    bool active = false;
    bool updateMode = false;
    bool archiveDirty = false;
    bool fatTargetSpecified = false;
    size_t fatTargetIndex = 0;
    enum class Mode { None, ZipCreate, ZipUpdate, TarCreate, TarUpdate, FatCreate, DiskUpdate } mode = Mode::None;
};

namespace {

QIODevice* zipDevice(peare_writer_handle_s* writer) {
    return writer->zipStage ? static_cast<QIODevice*>(writer->zipStage.get())
                            : static_cast<QIODevice*>(writer->output.get());
}

peare_writer_status fail(peare_writer_handle_s* writer, peare_writer_status status,
                         const QString& message, bool abortArchive = true) {
    if (writer) {
        writer->error = message;
        writer->errorUtf8 = message.toUtf8();
        if (abortArchive && writer->active) {
            if (writer->output) {
                writer->output->cancelWriting();
                writer->output.reset();
            }
            writer->zipStage.reset();
            writer->tarStage.reset();
            if (writer->diskUpdate) {
                writer->diskUpdate->abort();
                writer->diskUpdate.reset();
            }
            writer->entries.clear();
            writer->tarEntries.clear();
            writer->names.clear();
            writer->archiveComment.clear();
            writer->active = false;
            writer->mode = peare_writer_handle_s::Mode::None;
            writer->updateMode = false;
            writer->archiveDirty = false;
            writer->tarContentEnd = 0;
            writer->updatePath.clear();
        }
    }
    return status;
}

peare_writer_status finishNoOpArchiveUpdate(peare_writer_handle_s* writer) {
    writer->zipStage.reset();
    writer->tarStage.reset();
    writer->entries.clear();
    writer->tarEntries.clear();
    writer->names.clear();
    writer->archiveComment.clear();
    writer->active = false;
    writer->updateMode = false;
    writer->archiveDirty = false;
    writer->mode = peare_writer_handle_s::Mode::None;
    writer->tarContentEnd = 0;
    writer->updatePath.clear();
    writer->error.clear();
    writer->errorUtf8.clear();
    return PEARE_WRITER_OK;
}

quint16 read16(const char* data) { return qFromLittleEndian<quint16>(data); }
quint32 read32(const char* data) { return qFromLittleEndian<quint32>(data); }
quint64 read64(const char* data) { return qFromLittleEndian<quint64>(data); }

bool readAt(QIODevice* file, qint64 offset, qint64 length, QByteArray* bytes) {
    if (!file->seek(offset)) return false;
    *bytes = file->read(length);
    return bytes->size() == length;
}

bool tarZeroBlock(const QByteArray& block) {
    if (block.size() != qint64(kTarBlockSize)) return false;
    for (char byte : block)
        if (byte != 0) return false;
    return true;
}

quint64 tarOctal(const char* data, int length, bool* valid) {
    quint64 value = 0;
    bool sawDigit = false;
    for (int i = 0; i < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(data[i]);
        if (c == 0 || c == ' ') continue;
        if (c < '0' || c > '7') {
            *valid = false;
            return 0;
        }
        sawDigit = true;
        if (value > (std::numeric_limits<quint64>::max() >> 3)) {
            *valid = false;
            return 0;
        }
        value = (value << 3) | quint64(c - '0');
    }
    *valid = sawDigit || value == 0;
    return value;
}

bool validTarHeader(const QByteArray& header, quint64* dataSize, QString* name) {
    if (header.size() != qint64(kTarBlockSize) || tarZeroBlock(header)) return false;
    const QByteArray magic = header.mid(257, 6);
    const bool v7Header = magic == QByteArray(6, '\0');
    const bool ustarHeader = magic.startsWith(QByteArrayLiteral("ustar")) &&
        (magic.at(5) == '\0' || magic.at(5) == ' ') &&
        (header.mid(263, 2) == QByteArrayLiteral("00") ||
         header.mid(263, 2) == QByteArrayLiteral(" \0"));
    if (!v7Header && !ustarHeader) return false;
    quint64 checksum = 0;
    for (int i = 0; i < header.size(); ++i)
        checksum += (i >= 148 && i < 156) ? quint64(' ') :
                    quint64(static_cast<unsigned char>(header.at(i)));
    bool valid = false;
    const quint64 storedChecksum = tarOctal(header.constData() + 148, 8, &valid);
    if (!valid || storedChecksum != checksum) return false;
    bool sizeValid = false;
    *dataSize = tarOctal(header.constData() + 124, 12, &sizeValid);
    if (!sizeValid || *dataSize > quint64(std::numeric_limits<qint64>::max())) return false;
    int nameLength = 0;
    while (nameLength < 100 && header.at(nameLength) != '\0') ++nameLength;
    int prefixLength = 0;
    while (prefixLength < 155 && header.at(345 + prefixLength) != '\0') ++prefixLength;
    QString fileName = QString::fromUtf8(header.constData(), nameLength);
    if (prefixLength) fileName = QString::fromUtf8(header.constData() + 345, prefixLength) +
                                 QLatin1Char('/') + fileName;
    if (fileName.isEmpty()) return false;
    *name = fileName;
    return true;
}

bool tarZeroTail(QFile* input, qint64 offset) {
    if (!input || offset < 0 || input->size() - offset < 2 * qint64(kTarBlockSize) ||
        (input->size() - offset) % qint64(kTarBlockSize)) return false;
    for (qint64 pos = offset; pos < input->size(); pos += qint64(kTarBlockSize)) {
        QByteArray block;
        if (!readAt(input, pos, qint64(kTarBlockSize), &block) || !tarZeroBlock(block))
            return false;
    }
    return true;
}

bool findTarContentEnd(QFile* input, qint64* contentEnd, QSet<QString>* names,
                      std::vector<TarEntry>* entries = nullptr) {
    if (!input || !contentEnd || !names || !input->seek(0)) return false;
    const qint64 fileSize = input->size();
    qint64 pos = 0;
    bool sawHeader = false;
    while (pos + qint64(kTarBlockSize) <= fileSize) {
        QByteArray header;
        if (!readAt(input, pos, kTarBlockSize, &header)) return false;
        if (tarZeroBlock(header)) {
            if (!sawHeader) {
                if (pos != 0 ||
                    !input->fileName().endsWith(QStringLiteral(".tar"), Qt::CaseInsensitive) ||
                    fileSize > qint64(kTarBlockSize) * 20 || !tarZeroTail(input, pos))
                    return false;
            } else if (!tarZeroTail(input, pos)) {
                return false;
            }
            *contentEnd = pos;
            return true;
        }
        quint64 size = 0;
        QString name;
        if (!validTarHeader(header, &size, &name)) return false;
        sawHeader = true;
        name.replace(QLatin1Char('\\'), QLatin1Char('/'));
        while (name.startsWith(QStringLiteral("./"))) name.remove(0, 2);
        while (name.startsWith(QLatin1Char('/'))) name.remove(0, 1);
        if (name.endsWith(QLatin1Char('/'))) name.chop(1);
        names->insert(name);
        if (entries) {
            TarEntry entry;
            entry.key = name;
            entry.header = header;
            entry.offset = pos;
            entry.size = size;
            entry.directory = header.at(156) == '5' || name.endsWith(QLatin1Char('/'));
            if (entry.key.endsWith(QLatin1Char('/'))) entry.key.chop(1);
            entries->push_back(std::move(entry));
        }
        if (size > quint64(fileSize - pos - qint64(kTarBlockSize))) return false;
        const quint64 padded = ((size + kTarBlockSize - 1) / kTarBlockSize) * kTarBlockSize;
        if (padded > quint64(fileSize - pos - qint64(kTarBlockSize))) return false;
        pos += qint64(kTarBlockSize) + qint64(padded);
    }
    return false;
}

peare_writer_status loadExistingTar(peare_writer_handle_s* writer, QFile* input) {
    QSet<QString> names;
    qint64 contentEnd = 0;
    if (!findTarContentEnd(input, &contentEnd, &names, &writer->tarEntries))
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("The file is not a supported TAR archive."), false);
    writer->names = std::move(names);
    writer->tarContentEnd = contentEnd;
    return PEARE_WRITER_OK;
}

bool writeTarOctal(char* field, int width, quint64 value) {
    if (!field || width < 2) return false;
    std::memset(field, '0', size_t(width));
    field[width - 1] = '\0';
    int pos = width - 2;
    while (value && pos >= 0) {
        field[pos--] = char('0' + (value & 7));
        value >>= 3;
    }
    return value == 0;
}

bool tarEntryName(const char* utf8, QByteArray* name, QByteArray* prefix) {
    if (!utf8 || !*utf8 || !name || !prefix) return false;
    QString path = QString::fromUtf8(utf8);
    if (path.isEmpty() || path.contains(QChar(0))) return false;
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (path.startsWith(QLatin1Char('/')) ||
        (path.size() >= 2 && path.at(1) == QLatin1Char(':'))) return false;
    const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) return false;
    for (const QString& part : parts)
        if (part == QStringLiteral(".") || part == QStringLiteral("..") ||
            part.contains(QChar(0))) return false;
    path = parts.join(QLatin1Char('/'));
    QByteArray encoded = path.toUtf8();
    if (encoded.size() <= 100) {
        *name = encoded;
        prefix->clear();
        return true;
    }
    const int slash = encoded.lastIndexOf('/');
    if (slash <= 0 || slash >= encoded.size() - 1 || slash > 155 ||
        encoded.size() - slash - 1 > 100) return false;
    *prefix = encoded.left(slash);
    *name = encoded.mid(slash + 1);
    return true;
}

bool rewriteTarName(TarEntry* entry, const QString& key) {
    if (!entry || entry->header.size() != qint64(kTarBlockSize)) return false;
    QByteArray name;
    QByteArray prefix;
    QByteArray full = key.toUtf8();
    if (entry->directory) full += '/';
    if (!tarEntryName(full.constData(), &name, &prefix)) return false;
    QByteArray header = entry->header;
    std::memset(header.data(), 0, 100);
    std::memset(header.data() + 345, 0, 155);
    std::memcpy(header.data(), name.constData(), size_t(name.size()));
    if (!prefix.isEmpty()) std::memcpy(header.data() + 345, prefix.constData(), size_t(prefix.size()));
    std::memset(header.data() + 148, ' ', 8);
    quint64 checksum = 0;
    for (char byte : header) checksum += quint64(static_cast<unsigned char>(byte));
    if (!writeTarOctal(header.data() + 148, 7, checksum)) return false;
    header[155] = ' ';
    entry->header = std::move(header);
    entry->key = key;
    return true;
}

peare_writer_status writeTarFile(peare_writer_handle_s* writer,
                                 const char* sourcePathUtf8, const char* entryNameUtf8) {
    if (!sourcePathUtf8 || !*sourcePathUtf8)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("A source file path is required."), false);
    QByteArray name;
    QByteArray prefix;
    if (!tarEntryName(entryNameUtf8, &name, &prefix))
        return fail(writer, PEARE_WRITER_INVALID_ENTRY_NAME,
                    QStringLiteral("The TAR path is invalid or exceeds USTAR name limits."), false);
    const QByteArray fullName = prefix.isEmpty() ? name : prefix + '/' + name;
    const QString key = QString::fromUtf8(fullName);
    if (writer->names.contains(key))
        return fail(writer, PEARE_WRITER_DUPLICATE_ENTRY,
                    QStringLiteral("The TAR archive already contains this path."), false);
    QFile input(QString::fromUtf8(sourcePathUtf8));
    if (!input.open(QIODevice::ReadOnly))
        return fail(writer, PEARE_WRITER_IO_ERROR, input.errorString());
    const qint64 sourceSize = input.size();
    if (sourceSize < 0) return fail(writer, PEARE_WRITER_IO_ERROR, input.errorString());

    QByteArray header(int(kTarBlockSize), '\0');
    std::memcpy(header.data(), name.constData(), size_t(name.size()));
    if (!writeTarOctal(header.data() + 100, 8, 0644) ||
        !writeTarOctal(header.data() + 108, 8, 0) ||
        !writeTarOctal(header.data() + 116, 8, 0) ||
        !writeTarOctal(header.data() + 124, 12, quint64(sourceSize)) ||
        !writeTarOctal(header.data() + 136, 12, 0))
        return fail(writer, PEARE_WRITER_UNSUPPORTED_LIMIT,
                    QStringLiteral("The file is too large for the supported TAR header."));
    std::memset(header.data() + 148, ' ', 8);
    header[156] = '0';
    std::memcpy(header.data() + 257, "ustar", 5);
    header[262] = '\0';
    std::memcpy(header.data() + 263, "00", 2);
    if (!prefix.isEmpty()) std::memcpy(header.data() + 345, prefix.constData(), size_t(prefix.size()));
    quint64 checksum = 0;
    for (char byte : header) checksum += quint64(static_cast<unsigned char>(byte));
    if (!writeTarOctal(header.data() + 148, 7, checksum))
        return fail(writer, PEARE_WRITER_INTERNAL_ERROR,
                    QStringLiteral("Could not encode the TAR header checksum."));
    header[155] = ' ';
    QIODevice* output = writer->mode == peare_writer_handle_s::Mode::TarUpdate
        ? static_cast<QIODevice*>(writer->tarStage.get())
        : static_cast<QIODevice*>(writer->output.get());
    if (!output || !output->isOpen())
        return fail(writer, PEARE_WRITER_IO_ERROR, QStringLiteral("The TAR output is unavailable."));
    const qint64 recordOffset = output->pos();
    if (!writeAll(output, header.constData(), header.size()))
        return fail(writer, PEARE_WRITER_IO_ERROR, output->errorString());

    std::vector<char> chunk(kChunkSize);
    quint64 copied = 0;
    while (true) {
        const qint64 count = input.read(chunk.data(), qint64(chunk.size()));
        if (count < 0) return fail(writer, PEARE_WRITER_IO_ERROR, input.errorString());
        if (!count) break;
        copied += quint64(count);
        if (!writeAll(output, chunk.data(), count))
            return fail(writer, PEARE_WRITER_IO_ERROR, output->errorString());
    }
    if (copied != quint64(sourceSize))
        return fail(writer, PEARE_WRITER_IO_ERROR,
                    QStringLiteral("The source file changed while it was being archived."));
    const quint64 padding = (kTarBlockSize - (copied % kTarBlockSize)) % kTarBlockSize;
    if (padding) {
        const QByteArray zeros(int(padding), '\0');
        if (!writeAll(output, zeros.constData(), zeros.size()))
            return fail(writer, PEARE_WRITER_IO_ERROR, output->errorString());
    }
    writer->names.insert(key);
    if (writer->mode == peare_writer_handle_s::Mode::TarUpdate) {
        TarEntry entry;
        entry.key = key;
        entry.header = header;
        entry.offset = recordOffset;
        entry.size = quint64(sourceSize);
        writer->tarEntries.push_back(std::move(entry));
        writer->archiveDirty = true;
    }
    return PEARE_WRITER_OK;
}

peare_writer_status writeTarDirectory(peare_writer_handle_s* writer, const char* entryNameUtf8) {
    QByteArray name;
    QByteArray prefix;
    if (!tarEntryName(entryNameUtf8, &name, &prefix))
        return fail(writer, PEARE_WRITER_INVALID_ENTRY_NAME,
                    QStringLiteral("The TAR directory name is invalid or exceeds USTAR limits."), false);
    QString key = QString::fromUtf8(prefix.isEmpty() ? name : prefix + '/' + name);
    if (key.endsWith(QLatin1Char('/'))) key.chop(1);
    if (writer->names.contains(key))
        return fail(writer, PEARE_WRITER_DUPLICATE_ENTRY,
                    QStringLiteral("The TAR archive already contains this path."), false);
    QByteArray header(int(kTarBlockSize), '\0');
    std::memcpy(header.data(), name.constData(), size_t(name.size()));
    if (!prefix.isEmpty()) std::memcpy(header.data() + 345, prefix.constData(), size_t(prefix.size()));
    if (!writeTarOctal(header.data() + 100, 8, 0755) ||
        !writeTarOctal(header.data() + 108, 8, 0) ||
        !writeTarOctal(header.data() + 116, 8, 0) ||
        !writeTarOctal(header.data() + 124, 12, 0) ||
        !writeTarOctal(header.data() + 136, 12, 0))
        return fail(writer, PEARE_WRITER_INTERNAL_ERROR,
                    QStringLiteral("Could not encode the TAR directory header."));
    header[156] = '5';
    std::memcpy(header.data() + 257, "ustar", 5);
    std::memcpy(header.data() + 263, "00", 2);
    std::memset(header.data() + 148, ' ', 8);
    quint64 checksum = 0;
    for (char byte : header) checksum += quint64(static_cast<unsigned char>(byte));
    if (!writeTarOctal(header.data() + 148, 7, checksum))
        return fail(writer, PEARE_WRITER_INTERNAL_ERROR,
                    QStringLiteral("Could not encode the TAR header checksum."));
    header[155] = ' ';
    QIODevice* output = writer->mode == peare_writer_handle_s::Mode::TarUpdate
        ? static_cast<QIODevice*>(writer->tarStage.get())
        : static_cast<QIODevice*>(writer->output.get());
    if (!output) return fail(writer, PEARE_WRITER_IO_ERROR,
                             QStringLiteral("The TAR output is unavailable."));
    TarEntry entry;
    entry.key = key;
    entry.header = header;
    entry.offset = output->pos();
    entry.directory = true;
    if (!writeAll(output, header.constData(), header.size()))
        return fail(writer, PEARE_WRITER_IO_ERROR, output->errorString());
    writer->tarEntries.push_back(std::move(entry));
    writer->names.insert(key);
    if (writer->mode == peare_writer_handle_s::Mode::TarUpdate)
        writer->archiveDirty = true;
    return PEARE_WRITER_OK;
}

peare_writer_status loadExistingZip(peare_writer_handle_s* writer, QFile* input) {
    const qint64 fileSize = input->size();
    const qint64 tailLength = qMin<qint64>(fileSize, 65557);
    QByteArray tail;
    if (tailLength < 22 || !readAt(input, fileSize - tailLength, tailLength, &tail))
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("The file is not a supported ZIP archive."), false);
    qint64 eocd = -1;
    for (qint64 i = tail.size() - 22; i >= 0; --i) {
        if (read32(tail.constData() + i) != kEndRecordSignature) continue;
        if (i + 22 + read16(tail.constData() + i + 20) == tail.size()) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("The file is not a supported ZIP archive."), false);
    const char* end = tail.constData() + eocd;
    quint64 disk = read16(end + 4);
    quint64 centralDisk = read16(end + 6);
    quint64 diskEntries = read16(end + 8);
    quint64 totalEntries = read16(end + 10);
    quint64 centralSize = read32(end + 12);
    quint64 centralOffset = read32(end + 16);
    const quint16 archiveCommentLength = read16(end + 20);
    const qint64 eocdOffset = fileSize - tailLength + eocd;
    writer->archiveComment = tail.mid(eocd + 22, archiveCommentLength);
    const bool needsZip64 = totalEntries == 0xffffu || diskEntries == 0xffffu ||
                            centralSize == 0xffffffffu || centralOffset == 0xffffffffu;
    if (needsZip64) {
        QByteArray locator;
        if (eocdOffset < 20 || !readAt(input, eocdOffset - 20, 20, &locator) ||
            read32(locator.constData()) != 0x07064b50u ||
            read32(locator.constData() + 4) != 0 || read32(locator.constData() + 16) != 1)
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("The ZIP64 locator is invalid."), false);
        const quint64 zip64Offset = read64(locator.constData() + 8);
        QByteArray zip64;
        if (zip64Offset > quint64(fileSize) ||
            !readAt(input, qint64(zip64Offset), 56, &zip64) ||
            read32(zip64.constData()) != 0x06064b50u ||
            read64(zip64.constData() + 4) < 44 ||
            zip64Offset + 12 + read64(zip64.constData() + 4) > quint64(eocdOffset - 20))
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("The ZIP64 end record is invalid."), false);
        disk = read32(zip64.constData() + 16);
        centralDisk = read32(zip64.constData() + 20);
        diskEntries = read64(zip64.constData() + 24);
        totalEntries = read64(zip64.constData() + 32);
        centralSize = read64(zip64.constData() + 40);
        centralOffset = read64(zip64.constData() + 48);
        if (totalEntries > kMaxZipEntries)
            return fail(writer, PEARE_WRITER_UNSUPPORTED_LIMIT,
                        QStringLiteral("The ZIP64 archive has too many entries to update."), false);
    }
    if (disk || centralDisk || diskEntries != totalEntries)
        return fail(writer, PEARE_WRITER_UNSUPPORTED_LIMIT,
                    QStringLiteral("Multi-disk archives cannot be updated."), false);
    if (centralOffset > quint64(fileSize) || centralSize > quint64(fileSize) - centralOffset ||
        totalEntries > kMaxZipEntries)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("The ZIP central directory is invalid."), false);

    qint64 cursor = qint64(centralOffset);
    const qint64 centralEnd = cursor + qint64(centralSize);
    if (!input->seek(cursor))
        return fail(writer, PEARE_WRITER_IO_ERROR, input->errorString(), false);
    for (quint64 i = 0; i < totalEntries; ++i) {
        if (cursor + 46 > centralEnd)
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("The ZIP central directory is invalid."), false);
        QByteArray fixed = input->read(46);
        if (fixed.size() != 46 || read32(fixed.constData()) != kCentralHeaderSignature)
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("The ZIP central directory is invalid."), false);
        const quint16 nameLength = read16(fixed.constData() + 28);
        const quint16 extraLength = read16(fixed.constData() + 30);
        const quint16 commentLength = read16(fixed.constData() + 32);
        quint64 compressedSize = read32(fixed.constData() + 20);
        quint64 uncompressedSize = read32(fixed.constData() + 24);
        quint64 localOffset = read32(fixed.constData() + 42);
        quint32 diskStart = read16(fixed.constData() + 34);
        const qint64 recordSize = 46ll + nameLength + extraLength + commentLength;
        if (cursor + recordSize > centralEnd)
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("The ZIP central directory is truncated."), false);
        QByteArray record = fixed;
        const QByteArray variable = input->read(recordSize - fixed.size());
        if (variable.size() != recordSize - fixed.size())
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("The ZIP central directory is truncated."), false);
        record.append(variable);
        const QByteArray rawName = record.mid(46, nameLength);
        const char* extra = record.constData() + 46 + nameLength;
        int extraPos = 0;
        const bool needCompressed = compressedSize == 0xffffffffu;
        const bool needUncompressed = uncompressedSize == 0xffffffffu;
        const bool needOffset = localOffset == 0xffffffffu;
        const bool needDisk = diskStart == 0xffffu;
        if (needCompressed || needUncompressed || needOffset || needDisk) {
            bool foundZip64 = false;
            bool validZip64 = false;
            while (extraPos + 4 <= extraLength) {
                const quint16 id = read16(extra + extraPos);
                const quint16 size = read16(extra + extraPos + 2);
                extraPos += 4;
                if (size > extraLength - extraPos) break;
                if (id == 1) {
                    foundZip64 = true;
                    int valuePos = 0;
                    auto take64 = [&](quint64* value) {
                        if (valuePos > int(size) - 8) return false;
                        *value = read64(extra + extraPos + valuePos);
                        valuePos += 8;
                        return true;
                    };
                    if ((needUncompressed && !take64(&uncompressedSize)) ||
                        (needCompressed && !take64(&compressedSize)) ||
                        (needOffset && !take64(&localOffset)))
                        break;
                    if (needDisk) {
                        if (valuePos > int(size) - 4) break;
                        diskStart = read32(extra + extraPos + valuePos);
                    }
                    validZip64 = true;
                    break;
                }
                extraPos += size;
            }
            if (!foundZip64 || !validZip64)
                return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                            QStringLiteral("A ZIP64 entry is missing its extra field."), false);
        }
        if (diskStart != 0 || localOffset > quint64(fileSize) ||
            compressedSize > quint64(fileSize))
            return fail(writer, PEARE_WRITER_UNSUPPORTED_LIMIT,
                        QStringLiteral("Multi-disk or out-of-range ZIP entries cannot be updated."), false);
        QString decodedName = (read16(fixed.constData() + 8) & 0x0800)
            ? QString::fromUtf8(rawName) : QString::fromLocal8Bit(rawName);
        decodedName.replace(QLatin1Char('\\'), QLatin1Char('/'));
        while (decodedName.endsWith(QLatin1Char('/'))) decodedName.chop(1);
        if (decodedName.isEmpty() || writer->names.contains(decodedName))
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("The ZIP contains an invalid or duplicate entry name."), false);
        Entry entry;
        entry.name = rawName;
        entry.key = decodedName;
        const quint32 externalAttributes = read32(fixed.constData() + 38);
        const quint32 unixMode = externalAttributes >> 16;
        entry.directory = rawName.endsWith('/') || (externalAttributes & 0x10u) ||
                          ((unixMode & 0170000u) == 0040000u);
        entry.centralRecord = record;
        entry.crc = read32(fixed.constData() + 16);
        entry.compressedSize = compressedSize;
        entry.uncompressedSize = uncompressedSize;
        entry.size = uncompressedSize;
        entry.offset = localOffset;
        entry.flags = read16(fixed.constData() + 8);
        entry.method = read16(fixed.constData() + 10);
        entry.zip64Sizes = needCompressed || needUncompressed;
        entry.descriptor = (entry.flags & 0x0008) != 0;
        writer->names.insert(entry.key);
        writer->entries.push_back(std::move(entry));
        cursor += recordSize;
    }
    if (cursor != centralEnd)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("The ZIP central directory size is inconsistent."), false);
    return PEARE_WRITER_OK;
}

bool filterZipExtra(const QByteArray& source, QByteArray* filtered) {
    if (!filtered) return false;
    filtered->clear();
    int cursor = 0;
    while (cursor < source.size()) {
        if (source.size() - cursor < 4) return false;
        const quint16 id = read16(source.constData() + cursor);
        const quint16 size = read16(source.constData() + cursor + 2);
        const int recordSize = 4 + int(size);
        if (recordSize > source.size() - cursor) return false;
        if (id != 0x7075) filtered->append(source.constData() + cursor, recordSize);
        cursor += recordSize;
    }
    return filtered->size() <= 0xffff;
}

bool rewriteZipCentralRecord(const Entry& entry, const QByteArray& newName,
                             quint64 newOffset, QByteArray* rewritten) {
    if (entry.centralRecord.size() < 46 || !rewritten || newName.size() > 0xffff)
        return false;
    const QByteArray fixed = entry.centralRecord.left(46);
    const quint16 nameLength = read16(fixed.constData() + 28);
    const quint16 extraLength = read16(fixed.constData() + 30);
    const quint16 commentLength = read16(fixed.constData() + 32);
    if (46 + int(nameLength) + int(extraLength) + int(commentLength) !=
        entry.centralRecord.size()) return false;
    const QByteArray sourceExtra = entry.centralRecord.mid(46 + nameLength, extraLength);
    const QByteArray comment = entry.centralRecord.right(commentLength);
    const bool needUncompressed = read32(fixed.constData() + 24) == 0xffffffffu;
    const bool needCompressed = read32(fixed.constData() + 20) == 0xffffffffu;
    const bool needOffset = read32(fixed.constData() + 42) == 0xffffffffu ||
                            newOffset > kZip32Max;
    const bool needDisk = read16(fixed.constData() + 34) == 0xffffu;
    QByteArray extra;
    bool wroteZip64 = false;
    int cursor = 0;
    while (cursor < sourceExtra.size()) {
        if (sourceExtra.size() - cursor < 4) return false;
        const quint16 id = read16(sourceExtra.constData() + cursor);
        const quint16 size = read16(sourceExtra.constData() + cursor + 2);
        const int payloadStart = cursor + 4;
        if (int(size) > sourceExtra.size() - payloadStart) return false;
        if (id == 0x0001) {
            if (wroteZip64) return false;
            QByteArray payload;
            int consumed = 0;
            if (needUncompressed) {
                append64(&payload, entry.uncompressedSize);
                consumed += 8;
            }
            if (needCompressed) {
                append64(&payload, entry.compressedSize);
                consumed += 8;
            }
            if (needOffset) {
                append64(&payload, newOffset);
                consumed += 8;
            }
            if (needDisk) {
                append32(&payload, 0);
                consumed += 4;
            }
            if (consumed > int(size)) return false;
            payload += sourceExtra.mid(payloadStart + consumed, int(size) - consumed);
            if (payload.size() > 0xffff) return false;
            append16(&extra, 0x0001);
            append16(&extra, quint16(payload.size()));
            extra += payload;
            wroteZip64 = true;
        } else if (id != 0x7075) {
            extra.append(sourceExtra.constData() + cursor, 4 + int(size));
        }
        cursor = payloadStart + int(size);
    }
    if ((needUncompressed || needCompressed || needOffset || needDisk) && !wroteZip64) {
        QByteArray payload;
        if (needUncompressed) append64(&payload, entry.uncompressedSize);
        if (needCompressed) append64(&payload, entry.compressedSize);
        if (needOffset) append64(&payload, newOffset);
        if (needDisk) append32(&payload, 0);
        if (payload.size() > 0xffff) return false;
        append16(&extra, 0x0001);
        append16(&extra, quint16(payload.size()));
        extra += payload;
    }
    if (extra.size() > 0xffff) return false;
    QByteArray output = fixed;
    const quint16 flags = quint16(read16(output.constData() + 8) | 0x0800);
    qToLittleEndian(flags, reinterpret_cast<uchar*>(output.data() + 8));
    qToLittleEndian(quint16(newName.size()), reinterpret_cast<uchar*>(output.data() + 28));
    qToLittleEndian(quint16(extra.size()), reinterpret_cast<uchar*>(output.data() + 30));
    qToLittleEndian(needOffset ? 0xffffffffu : quint32(newOffset),
                    reinterpret_cast<uchar*>(output.data() + 42));
    output += newName;
    output += extra;
    output += comment;
    *rewritten = std::move(output);
    return true;
}

bool readZipDescriptor(QIODevice* input, qint64 offset, const Entry& entry,
                       bool zip64, QByteArray* descriptor) {
    for (int signature = 1; signature >= 0; --signature) {
        for (int wide = 1; wide >= 0; --wide) {
            if (wide && !zip64) continue;
            const int length = signature * 4 + 4 + (wide ? 16 : 8);
            QByteArray candidate;
            if (!readAt(input, offset, length, &candidate)) continue;
            int position = 0;
            if (signature) {
                if (read32(candidate.constData()) != kDataDescriptorSignature) continue;
                position = 4;
            }
            if (read32(candidate.constData() + position) != entry.crc) continue;
            position += 4;
            const quint64 compressed = wide ? read64(candidate.constData() + position)
                                              : read32(candidate.constData() + position);
            position += wide ? 8 : 4;
            const quint64 uncompressed = wide ? read64(candidate.constData() + position)
                                                : read32(candidate.constData() + position);
            if (compressed == entry.compressedSize &&
                uncompressed == entry.uncompressedSize) {
                *descriptor = candidate;
                return true;
            }
        }
    }
    return false;
}

bool appendRelocatedZipEntry(peare_writer_handle_s* writer, Entry* entry,
                             const QByteArray& newName, const QString& newKey,
                             QString* error) {
    QTemporaryFile* stage = writer->zipStage.get();
    if (!stage || !entry || newName.size() > 0xffff) {
        if (error) *error = QStringLiteral("The ZIP staging data is invalid.");
        return false;
    }
    QByteArray local;
    if (!readAt(stage, qint64(entry->offset), 30, &local) ||
        read32(local.constData()) != kLocalHeaderSignature) {
        if (error) *error = QStringLiteral("The ZIP local file header is invalid.");
        return false;
    }
    const quint16 localNameLength = read16(local.constData() + 26);
    const quint16 localExtraLength = read16(local.constData() + 28);
    const qint64 dataOffset = qint64(entry->offset) + 30 + localNameLength + localExtraLength;
    QByteArray localVariable;
    if (dataOffset < 0 || dataOffset > stage->size() ||
        !readAt(stage, qint64(entry->offset) + 30,
                qint64(localNameLength) + localExtraLength, &localVariable)) {
        if (error) *error = QStringLiteral("The ZIP local header fields are out of range.");
        return false;
    }
    const quint16 localFlags = read16(local.constData() + 6);
    const quint16 expectedFlags = quint16(entry->flags |
                                         (entry->descriptor ? 0x0008 : 0));
    if (localVariable.left(localNameLength) != entry->name ||
        read16(local.constData() + 8) != entry->method ||
        (localFlags & ~0x0800u) != (expectedFlags & ~0x0800u)) {
        if (error) *error = QStringLiteral("The ZIP local and central records do not match.");
        return false;
    }
    QByteArray filteredExtra;
    if (!filterZipExtra(localVariable.mid(localNameLength), &filteredExtra)) {
        if (error) *error = QStringLiteral("The ZIP local extra fields are invalid.");
        return false;
    }
    QByteArray descriptor;
    if (bool(localFlags & 0x0008) != entry->descriptor ||
        (entry->descriptor && !readZipDescriptor(
            stage, dataOffset + qint64(entry->compressedSize), *entry,
            entry->zip64Sizes || read32(local.constData() + 18) == 0xffffffffu ||
                read32(local.constData() + 22) == 0xffffffffu,
            &descriptor))) {
        if (error) *error = QStringLiteral("The ZIP data descriptor is invalid.");
        return false;
    }
    if (entry->compressedSize > quint64(stage->size() - dataOffset)) {
        if (error) *error = QStringLiteral("The ZIP member data is out of range.");
        return false;
    }
    const qint64 appendOffset = stage->size();
    if (!stage->seek(appendOffset)) {
        if (error) *error = stage->errorString();
        return false;
    }
    const quint16 newFlags = quint16(expectedFlags | 0x0800);
    qToLittleEndian(newFlags, reinterpret_cast<uchar*>(local.data() + 6));
    qToLittleEndian(quint16(newName.size()), reinterpret_cast<uchar*>(local.data() + 26));
    qToLittleEndian(quint16(filteredExtra.size()), reinterpret_cast<uchar*>(local.data() + 28));
    if (!writeAll(stage, local.constData(), local.size()) ||
        !writeAll(stage, newName.constData(), newName.size()) ||
        !writeAll(stage, filteredExtra.constData(), filteredExtra.size())) {
        if (error) *error = stage->errorString();
        return false;
    }
    qint64 appended = appendOffset + local.size() + newName.size() + filteredExtra.size();
    quint64 copied = 0;
    std::vector<char> chunk(kChunkSize);
    while (copied < entry->compressedSize) {
        const qint64 request = qint64(qMin<quint64>(chunk.size(), entry->compressedSize - copied));
        if (!stage->seek(dataOffset + qint64(copied))) {
            if (error) *error = stage->errorString();
            return false;
        }
        const qint64 count = stage->read(chunk.data(), request);
        if (count <= 0 || !stage->seek(appended) || !writeAll(stage, chunk.data(), count)) {
            if (error) *error = stage->errorString();
            return false;
        }
        copied += quint64(count);
        appended += count;
    }
    if (!descriptor.isEmpty()) {
        if (!stage->seek(appended) || !writeAll(stage, descriptor.constData(), descriptor.size())) {
            if (error) *error = stage->errorString();
            return false;
        }
    }
    QByteArray central;
    if (!entry->centralRecord.isEmpty() &&
        !rewriteZipCentralRecord(*entry, newName, quint64(appendOffset), &central)) {
        if (error) *error = QStringLiteral("The ZIP central record cannot be updated safely.");
        return false;
    }
    entry->offset = quint64(appendOffset);
    entry->name = newName;
    entry->key = newKey;
    entry->flags = newFlags;
    if (!entry->centralRecord.isEmpty()) entry->centralRecord = std::move(central);
    return true;
}

bool normalizedZipKey(const char* pathUtf8, QByteArray* name, QString* key) {
    if (!normalizedEntryName(pathUtf8, false, name) || !key) return false;
    *key = QString::fromUtf8(*name);
    return !key->isEmpty();
}

peare_writer_status prepareEntry(peare_writer_handle_s* writer, const char* entryName,
                                 bool directory, Entry* entry) {
    if (!writer) return PEARE_WRITER_INVALID_HANDLE;
    if (!writer->active) return fail(writer, PEARE_WRITER_NOT_ACTIVE,
                                     QStringLiteral("No ZIP archive is being written."), false);
    if (writer->mode != peare_writer_handle_s::Mode::ZipCreate &&
        writer->mode != peare_writer_handle_s::Mode::ZipUpdate)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("The active writer is not a ZIP backend."), false);
    if (writer->entries.size() >= kMaxZipEntries)
        return fail(writer, PEARE_WRITER_UNSUPPORTED_LIMIT,
                    QStringLiteral("The ZIP entry limit supported by Peare was reached."));
    if (!normalizedEntryName(entryName, directory, &entry->name))
        return fail(writer, PEARE_WRITER_INVALID_ENTRY_NAME,
                    QStringLiteral("The ZIP entry name is invalid or unsafe."), false);
    entry->key = QString::fromUtf8(entry->name);
    if (directory && entry->key.endsWith(QLatin1Char('/'))) entry->key.chop(1);
    if (writer->names.contains(entry->key))
        return fail(writer, PEARE_WRITER_DUPLICATE_ENTRY,
                    QStringLiteral("The ZIP archive already contains this entry."), false);
    QIODevice* output = zipDevice(writer);
    if (!output)
        return fail(writer, PEARE_WRITER_IO_ERROR,
                    QStringLiteral("The ZIP output is unavailable."));
    const qint64 offset = output->pos();
    if (offset < 0)
        return fail(writer, PEARE_WRITER_IO_ERROR,
                    QStringLiteral("Could not determine the output position."));
    entry->offset = quint64(offset);
    entry->directory = directory;
    return PEARE_WRITER_OK;
}

bool writeLocalHeader(QIODevice* output, const Entry& entry, bool descriptor) {
    QByteArray header;
    header.reserve(50 + entry.name.size());
    append32(&header, kLocalHeaderSignature);
    append16(&header, entry.zip64Sizes ? 45 : 20); // version needed
    append16(&header, quint16(entry.flags | (descriptor ? 0x0008 : 0)));
    append16(&header, entry.method);
    append16(&header, 0); // DOS time
    append16(&header, 0x0021); // 1980-01-01
    append32(&header, descriptor ? 0 : entry.crc);
    append32(&header, entry.zip64Sizes ? 0xffffffffu : (descriptor ? 0 : quint32(entry.size)));
    append32(&header, entry.zip64Sizes ? 0xffffffffu : (descriptor ? 0 : quint32(entry.size)));
    append16(&header, quint16(entry.name.size()));
    append16(&header, entry.zip64Sizes ? 20 : 0);
    header += entry.name;
    if (entry.zip64Sizes) {
        append16(&header, 1);
        append16(&header, 16);
        append64(&header, descriptor ? 0 : entry.size);
        append64(&header, descriptor ? 0 : entry.size);
    }
    return writeAll(output, header.constData(), header.size());
}

peare_writer_status writeEntryData(peare_writer_handle_s* writer, Entry* entry,
                                   QFile* input) {
    if (input->size() < 0 || quint64(input->size()) > kMaxZipUncompressed)
        return fail(writer, PEARE_WRITER_UNSUPPORTED_LIMIT,
                    QStringLiteral("ZIP entries above the Opener's 512 MiB limit cannot be added."));
    entry->descriptor = true;
    entry->flags = quint16(entry->flags | 0x0800);
    entry->method = 0;
    entry->zip64Sizes = input->size() > qint64(kZip32Max);
    QIODevice* output = zipDevice(writer);
    if (!output || !writeLocalHeader(output, *entry, true))
        return fail(writer, PEARE_WRITER_IO_ERROR,
                    output ? output->errorString() : QStringLiteral("The ZIP output is unavailable."));
    std::vector<char> chunk(kChunkSize);
    quint32 crc = 0xffffffffu;
    quint64 size = 0;
    while (true) {
        const qint64 count = input->read(chunk.data(), qint64(chunk.size()));
        if (count < 0)
            return fail(writer, PEARE_WRITER_IO_ERROR, input->errorString());
        if (count == 0) break;
        size += quint64(count);
        if (size > kMaxZipUncompressed)
            return fail(writer, PEARE_WRITER_UNSUPPORTED_LIMIT,
                        QStringLiteral("ZIP entries above the Opener's 512 MiB limit cannot be added."));
        crc = crc32Update(crc, chunk.data(), size_t(count));
        if (!writeAll(output, chunk.data(), count))
            return fail(writer, PEARE_WRITER_IO_ERROR,
                        output->errorString());
    }
    if (!input->atEnd() && input->error() != QFileDevice::NoError)
        return fail(writer, PEARE_WRITER_IO_ERROR, input->errorString());
    entry->crc = crc ^ 0xffffffffu;
    if (size > kZip32Max && !entry->zip64Sizes)
        return fail(writer, PEARE_WRITER_IO_ERROR,
                    QStringLiteral("The source file grew while it was being archived."));
    entry->size = size;
    entry->compressedSize = size;
    entry->uncompressedSize = size;
    QByteArray descriptor;
    append32(&descriptor, kDataDescriptorSignature);
    append32(&descriptor, entry->crc);
    if (entry->zip64Sizes) {
        append64(&descriptor, entry->size);
        append64(&descriptor, entry->size);
    } else {
        append32(&descriptor, quint32(entry->size));
        append32(&descriptor, quint32(entry->size));
    }
    if (!writeAll(output, descriptor.constData(), descriptor.size()))
        return fail(writer, PEARE_WRITER_IO_ERROR, output->errorString());
    writer->names.insert(entry->key);
    writer->entries.push_back(*entry);
    if (writer->mode == peare_writer_handle_s::Mode::ZipUpdate)
        writer->archiveDirty = true;
    return PEARE_WRITER_OK;
}

void discardAfterException(peare_writer_handle_s* writer) noexcept {
    if (!writer) return;
    if (writer->output) {
        writer->output->cancelWriting();
        writer->output.reset();
    }
    writer->zipStage.reset();
    if (writer->diskUpdate) {
        writer->diskUpdate->abort();
        writer->diskUpdate.reset();
    }
    writer->entries.clear();
    writer->names.clear();
    writer->archiveComment.clear();
    writer->updatePath.clear();
    writer->mode = peare_writer_handle_s::Mode::None;
    writer->updateMode = false;
    writer->active = false;
    writer->error.clear();
    writer->errorUtf8.clear();
}

template <typename Function>
peare_writer_status cBoundary(peare_writer_handle_s* writer, Function function) noexcept {
    try {
        return function();
    } catch (const std::bad_alloc&) {
        discardAfterException(writer);
        return PEARE_WRITER_ALLOCATION_FAILED;
    } catch (...) {
        discardAfterException(writer);
        return PEARE_WRITER_INTERNAL_ERROR;
    }
}

peare_writer_status beginDiskImageUpdate(peare_writer_handle_s* writer,
                                         const QString& imagePath,
                                         bool targetSpecified, size_t targetIndex,
                                         qint64 journalOffset = 0,
                                         qint64 journalSize = 0) {
    if (!writer) return PEARE_WRITER_INVALID_HANDLE;
    std::unique_ptr<peare::writer::DiskImageUpdate> disk(
        new (std::nothrow) peare::writer::DiskImageUpdate);
    if (!disk)
        return fail(writer, PEARE_WRITER_ALLOCATION_FAILED,
                    QStringLiteral("Could not allocate the disk image writer."), false);
    QString error;
    const bool started = targetSpecified
        ? disk->begin(imagePath, targetIndex, &error,
                      journalOffset, journalSize)
        : disk->begin(imagePath, &error);
    if (!started)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT, error, false);
    writer->error.clear();
    writer->errorUtf8.clear();
    writer->diskUpdate = std::move(disk);
    writer->active = true;
    writer->updateMode = true;
    writer->mode = peare_writer_handle_s::Mode::DiskUpdate;
    return PEARE_WRITER_OK;
}

peare_writer_status addFilesToImpl(
    peare_writer_handle writer, const char* targetPathUtf8, const char* formatIdUtf8,
    const char* variantIdUtf8, const char* filesystemIdUtf8, size_t targetIndex,
    const peare_writer_input_file* files, size_t fileCount) {
    if (!writer) return PEARE_WRITER_INVALID_HANDLE;
    if (writer->active)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("A writer operation is already active."), false);
    return cBoundary(writer, [&]() -> peare_writer_status {
        if (!targetPathUtf8 || !*targetPathUtf8 || !files || !fileCount)
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("A destination and at least one file are required."), false);
        const bool createNew = formatIdUtf8 && *formatIdUtf8;
        const bool fatContainer = createNew &&
            (std::strcmp(formatIdUtf8, "vhd") == 0 ||
             std::strcmp(formatIdUtf8, "vhdx") == 0 ||
             std::strcmp(formatIdUtf8, "vmdk") == 0 ||
             std::strcmp(formatIdUtf8, "raw-disk") == 0 ||
             std::strcmp(formatIdUtf8, "floppy") == 0);
        if ((createNew && targetIndex != 0) ||
            (!createNew && ((variantIdUtf8 && *variantIdUtf8) ||
                            (filesystemIdUtf8 && *filesystemIdUtf8))) ||
            (fatContainer && (!variantIdUtf8 || !*variantIdUtf8 ||
                              !filesystemIdUtf8 || !*filesystemIdUtf8)) ||
            (!fatContainer && filesystemIdUtf8 && *filesystemIdUtf8))
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("The format, variant, filesystem, or target selection is invalid."),
                        false);
        for (size_t i = 0; i < fileCount; ++i)
            if (!files[i].source_path_utf8 || !*files[i].source_path_utf8 ||
                !files[i].entry_name_utf8 || !*files[i].entry_name_utf8)
                return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                            QStringLiteral("Every input file needs a source and entry name."), false);

        peare_writer_status status = PEARE_WRITER_OK;
        if (fatContainer) {
            std::unique_ptr<peare::writer::DiskImageUpdate> fat(
                new (std::nothrow) peare::writer::DiskImageUpdate);
            if (!fat)
                return fail(writer, PEARE_WRITER_ALLOCATION_FAILED,
                            QStringLiteral("Could not allocate the virtual disk writer."), false);
            QString error;
            if (!fat->beginNew(QString::fromUtf8(targetPathUtf8),
                               QString::fromUtf8(formatIdUtf8),
                               QString::fromUtf8(variantIdUtf8),
                               QString::fromUtf8(filesystemIdUtf8), &error))
                return fail(writer, PEARE_WRITER_IO_ERROR, error, false);
            writer->error.clear();
            writer->errorUtf8.clear();
            writer->diskUpdate = std::move(fat);
            writer->active = true;
            writer->updateMode = false;
            writer->mode = peare_writer_handle_s::Mode::FatCreate;
        } else {
            status = createNew
                ? peare_writer_create_begin_variant(writer, formatIdUtf8, variantIdUtf8,
                                                    targetPathUtf8)
                : peare_writer_update_begin_target(writer, targetPathUtf8, targetIndex);
        }
        for (size_t i = 0; status == PEARE_WRITER_OK && i < fileCount; ++i)
            status = peare_writer_create_file(writer, files[i].source_path_utf8,
                                              files[i].entry_name_utf8);
        if (status == PEARE_WRITER_OK)
            status = createNew ? peare_writer_finish(writer)
                               : peare_writer_update_finish(writer);
        if (status != PEARE_WRITER_OK) {
            const QString error = writer->error;
            const QByteArray errorUtf8 = writer->errorUtf8;
            peare_writer_abort(writer);
            writer->error = error;
            writer->errorUtf8 = errorUtf8;
        }
        return status;
    });
}

} // namespace

extern "C" {

peare_writer_status peare_writer_create(peare_writer_handle* outWriter) {
    if (!outWriter) return PEARE_WRITER_INVALID_ARGUMENT;
    *outWriter = nullptr;
    try {
        peare_writer_handle_s* writer = new (std::nothrow) peare_writer_handle_s;
        if (!writer) return PEARE_WRITER_ALLOCATION_FAILED;
        *outWriter = writer;
        return PEARE_WRITER_OK;
    } catch (...) {
        return PEARE_WRITER_ALLOCATION_FAILED;
    }
}

size_t peare_writer_create_format_count(void) { return 8; }

peare_writer_status peare_writer_get_create_format(size_t index,
                                                    peare_writer_format* outFormat) {
    if (!outFormat) return PEARE_WRITER_INVALID_ARGUMENT;
    static const peare_writer_format formats[] = {
        {"zip", "ZIP archive", "zip"},
        {"tar", "TAR archive", "tar"},
        {"fat", "FAT image", "img"},
        {"floppy", "Floppy image", "img"},
        {"raw-disk", "Raw partitioned disk", "img"},
        {"vhd", "VHD virtual disk", "vhd"},
        {"vhdx", "VHDX virtual disk", "vhdx"},
        {"vmdk", "VMDK virtual disk", "vmdk"}
    };
    if (index >= sizeof(formats) / sizeof(formats[0]))
        return PEARE_WRITER_INVALID_ARGUMENT;
    *outFormat = formats[index];
    return PEARE_WRITER_OK;
}

size_t peare_writer_create_variant_count(const char* formatIdUtf8) {
    if (!formatIdUtf8) return 0;
    if (std::strcmp(formatIdUtf8, "zip") == 0 ||
        std::strcmp(formatIdUtf8, "tar") == 0) return 1;
    if (std::strcmp(formatIdUtf8, "fat") == 0) return 3;
    if (std::strcmp(formatIdUtf8, "floppy") == 0) return 13;
    if (std::strcmp(formatIdUtf8, "raw-disk") == 0) return 2;
    if (std::strcmp(formatIdUtf8, "vhd") == 0) return 2;
    if (std::strcmp(formatIdUtf8, "vhdx") == 0) return 4;
    return std::strcmp(formatIdUtf8, "vmdk") == 0 ? 2 : 0;
}

peare_writer_status peare_writer_get_create_variant(const char* formatIdUtf8,
                                                    size_t index,
                                                    peare_writer_variant* outVariant) {
    if (!formatIdUtf8 || !outVariant)
        return PEARE_WRITER_INVALID_ARGUMENT;
    static const peare_writer_variant zipVariants[] = { {"store", "Store"} };
    static const peare_writer_variant tarVariants[] = { {"ustar", "USTAR"} };
    static const peare_writer_variant fatVariants[] = {
        {"fat12", "FAT12 (1.44 MiB)"},
        {"fat16", "FAT16 (32 MiB)"},
        {"fat32", "FAT32 (128 MiB)"}
    };
    static const peare_writer_variant vhdVariants[] = {
        {"fixed", "Fixed"}, {"dynamic", "Dynamic"}
    };
    static const peare_writer_variant vhdxVariants[] = {
        {"fixed", "Fixed (512-byte sectors)"},
        {"dynamic", "Dynamic (512-byte sectors)"},
        {"fixed-4kn", "Fixed (4Kn)"},
        {"dynamic-4kn", "Dynamic (4Kn)"}
    };
    static const peare_writer_variant vmdkVariants[] = {
        {"monolithic-sparse", "Monolithic sparse"},
        {"two-gb-sparse", "Split sparse (2 GiB extents)"}
    };
    static const peare_writer_variant floppyVariants[] = {
        {"160k", "160 KiB (40x1x8)"}, {"180k", "180 KiB (40x1x9)"},
        {"320k", "320 KiB (40x2x8)"}, {"360k", "360 KiB (40x2x9)"},
        {"400k", "400 KiB (40x2x10)"}, {"640k", "640 KiB (80x2x8)"},
        {"720k", "720 KiB (80x2x9)"}, {"800k", "800 KiB (80x2x10)"},
        {"1.2m", "1.2 MiB (80x2x15)"}, {"1.44m", "1.44 MiB (80x2x18)"},
        {"1.68m", "1.68 MiB DMF (80x2x21)"}, {"2.88m", "2.88 MiB (80x2x36)"},
        {"xdf-1.84m", "OS/2 XDF 1.84 MiB (80x2x23)"}
    };
    static const peare_writer_variant rawDiskVariants[] = {
        {"mbr", "MBR partition table"}, {"gpt", "GPT partition table"}
    };
    if (std::strcmp(formatIdUtf8, "zip") == 0 && index == 0) {
        *outVariant = zipVariants[0];
        return PEARE_WRITER_OK;
    }
    if (std::strcmp(formatIdUtf8, "tar") == 0 && index == 0) {
        *outVariant = tarVariants[0];
        return PEARE_WRITER_OK;
    }
    if (std::strcmp(formatIdUtf8, "fat") == 0 && index < 3) {
        *outVariant = fatVariants[index];
        return PEARE_WRITER_OK;
    }
    if (std::strcmp(formatIdUtf8, "floppy") == 0 && index < 13) {
        *outVariant = floppyVariants[index];
        return PEARE_WRITER_OK;
    }
    if (std::strcmp(formatIdUtf8, "raw-disk") == 0 && index < 2) {
        *outVariant = rawDiskVariants[index];
        return PEARE_WRITER_OK;
    }
    if (std::strcmp(formatIdUtf8, "vhd") == 0 && index < 2) {
        *outVariant = vhdVariants[index];
        return PEARE_WRITER_OK;
    }
    if (std::strcmp(formatIdUtf8, "vhdx") == 0 && index < 4) {
        *outVariant = vhdxVariants[index];
        return PEARE_WRITER_OK;
    }
    if (std::strcmp(formatIdUtf8, "vmdk") == 0 && index < 2) {
        *outVariant = vmdkVariants[index];
        return PEARE_WRITER_OK;
    }
    return PEARE_WRITER_INVALID_ARGUMENT;
}

size_t peare_writer_create_filesystem_count(const char* formatIdUtf8) {
    if (!formatIdUtf8) return 0;
    return std::strcmp(formatIdUtf8, "vhd") == 0 ||
           std::strcmp(formatIdUtf8, "vhdx") == 0 ||
           std::strcmp(formatIdUtf8, "vmdk") == 0 ||
           std::strcmp(formatIdUtf8, "raw-disk") == 0 ? 8 :
           std::strcmp(formatIdUtf8, "floppy") == 0 ? 1 : 0;
}

peare_writer_status peare_writer_get_create_filesystem(
    const char* formatIdUtf8, size_t index, peare_writer_variant* outFilesystem) {
    if (!formatIdUtf8 || !outFilesystem ||
        peare_writer_create_filesystem_count(formatIdUtf8) == 0)
        return PEARE_WRITER_INVALID_ARGUMENT;
    static const peare_writer_variant filesystems[] = {
        {"fat12", "FAT12"}, {"fat16", "FAT16"}, {"fat32", "FAT32"},
        {"exfat", "exFAT"},
#ifdef PEARE_NTFS_WRITER
        {"ntfs", "NTFS"},
#endif
        {"ext2", "Ext2"}, {"ext3", "Ext3"}, {"ext4", "Ext4"}
    };
    if (std::strcmp(formatIdUtf8, "floppy") == 0) {
        if (index != 0) return PEARE_WRITER_INVALID_ARGUMENT;
        *outFilesystem = {"fat12", "FAT12"};
        return PEARE_WRITER_OK;
    }
    if (index >= 8) return PEARE_WRITER_INVALID_ARGUMENT;
    *outFilesystem = filesystems[index];
    return PEARE_WRITER_OK;
}

peare_writer_status peare_writer_create_begin(peare_writer_handle writer,
                                               const char* formatIdUtf8,
                                               const char* outputPathUtf8) {
    return peare_writer_create_begin_variant(writer, formatIdUtf8, nullptr,
                                             outputPathUtf8);
}

peare_writer_status peare_writer_create_begin_variant(peare_writer_handle writer,
                                                       const char* formatIdUtf8,
                                                       const char* variantIdUtf8,
                                                       const char* outputPathUtf8) {
    if (!writer) return PEARE_WRITER_INVALID_HANDLE;
    if (!formatIdUtf8 || !outputPathUtf8 || !*outputPathUtf8)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("A format and output path are required."), false);
    const char* variant = variantIdUtf8 && *variantIdUtf8 ? variantIdUtf8 : nullptr;
    if (std::strcmp(formatIdUtf8, "zip") == 0) {
        if (variant && std::strcmp(variant, "store") != 0)
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("The requested ZIP variant is not supported."), false);
        return peare_writer_zip_begin(writer, outputPathUtf8);
    }
    if (std::strcmp(formatIdUtf8, "fat") == 0) {
        if (!variant)
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("A FAT image variant must be selected."), false);
        return cBoundary(writer, [&]() -> peare_writer_status {
            if (writer->active)
                return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                            QStringLiteral("A writer operation is already active."), false);
            writer->error.clear();
            writer->errorUtf8.clear();
            std::unique_ptr<peare::writer::DiskImageUpdate> fat(
                new (std::nothrow) peare::writer::DiskImageUpdate);
            if (!fat)
                return fail(writer, PEARE_WRITER_ALLOCATION_FAILED,
                            QStringLiteral("Could not allocate the disk image writer."), false);
            QString error;
            if (!fat->beginNew(QString::fromUtf8(outputPathUtf8), QString::fromUtf8(variant),
                               &error))
                return fail(writer, PEARE_WRITER_IO_ERROR, error, false);
            writer->diskUpdate = std::move(fat);
            writer->active = true;
            writer->updateMode = false;
            writer->mode = peare_writer_handle_s::Mode::FatCreate;
            return PEARE_WRITER_OK;
        });
    }
    if (std::strcmp(formatIdUtf8, "tar") != 0)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("The requested archive format is not supported."), false);
    if (variant && std::strcmp(variant, "ustar") != 0)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("The requested TAR variant is not supported."), false);
    return cBoundary(writer, [&]() -> peare_writer_status {
        if (writer->active)
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("A writer operation is already active."), false);
        writer->error.clear();
        writer->errorUtf8.clear();
        writer->entries.clear();
        writer->tarEntries.clear();
        writer->names.clear();
        writer->tarStage.reset();
        writer->archiveComment.clear();
        writer->output.reset(new (std::nothrow) QSaveFile(QString::fromUtf8(outputPathUtf8)));
        if (!writer->output)
            return fail(writer, PEARE_WRITER_ALLOCATION_FAILED,
                        QStringLiteral("Could not allocate the TAR output."), false);
        if (!writer->output->open(QIODevice::WriteOnly)) {
            const QString error = writer->output->errorString();
            writer->output.reset();
            return fail(writer, PEARE_WRITER_IO_ERROR, error, false);
        }
        writer->active = true;
        writer->updateMode = false;
        writer->mode = peare_writer_handle_s::Mode::TarCreate;
        return PEARE_WRITER_OK;
    });
}

peare_writer_status peare_writer_add_files_to(
    peare_writer_handle writer, const char* targetPathUtf8,
    const char* formatIdUtf8, const char* variantIdUtf8, size_t targetIndex,
    const peare_writer_input_file* files, size_t fileCount) {
    return addFilesToImpl(writer, targetPathUtf8, formatIdUtf8, variantIdUtf8,
                          nullptr, targetIndex, files, fileCount);
}

peare_writer_status peare_writer_add_files_to_ex(
    peare_writer_handle writer, const char* targetPathUtf8,
    const peare_writer_create_options* createOptions, size_t targetIndex,
    const peare_writer_input_file* files, size_t fileCount) {
    return addFilesToImpl(writer, targetPathUtf8,
        createOptions ? createOptions->format_id_utf8 : nullptr,
        createOptions ? createOptions->variant_id_utf8 : nullptr,
        createOptions ? createOptions->filesystem_id_utf8 : nullptr,
        targetIndex, files, fileCount);
}

peare_writer_status peare_writer_finish(peare_writer_handle writer);

int peare_writer_can_add_files(const char* containerPathUtf8) {
    if (!containerPathUtf8 || !*containerPathUtf8) return 0;
    try {
        peare_writer_handle_s probe;
        QFile input(QString::fromUtf8(containerPathUtf8));
        if (!input.open(QIODevice::ReadOnly)) return 0;
        if (loadExistingZip(&probe, &input) == PEARE_WRITER_OK) return 1;
        probe.entries.clear();
        probe.names.clear();
        probe.archiveComment.clear();
        if (loadExistingTar(&probe, &input) == PEARE_WRITER_OK) return 1;
        return peare::writer::DiskImageUpdate::canUpdate(QString::fromUtf8(containerPathUtf8));
    } catch (...) {
        return 0;
    }
}

size_t peare_writer_add_target_count(const char* containerPathUtf8) {
    if (!containerPathUtf8 || !*containerPathUtf8) return 0;
    try {
        return peare::writer::DiskImageUpdate::targets(
            QString::fromUtf8(containerPathUtf8)).size();
    } catch (...) {
        return 0;
    }
}

size_t peare_writer_update_target_count(const char* containerPathUtf8) {
    if (!peare_writer_can_add_files(containerPathUtf8)) return 0;
    try {
        const size_t fatTargets = peare_writer_add_target_count(containerPathUtf8);
        return fatTargets ? fatTargets : 1;
    } catch (...) {
        return 0;
    }
}

peare_writer_status peare_writer_get_update_target_name(const char* containerPathUtf8,
                                                        size_t targetIndex,
                                                        char* nameBuffer,
                                                        size_t nameBufferSize) {
    if (!containerPathUtf8 || !*containerPathUtf8 || !nameBuffer || !nameBufferSize)
        return PEARE_WRITER_INVALID_ARGUMENT;
    const size_t targetCount = peare_writer_update_target_count(containerPathUtf8);
    if (targetIndex >= targetCount) return PEARE_WRITER_INVALID_ARGUMENT;
    if (peare_writer_add_target_count(containerPathUtf8))
        return peare_writer_get_add_target_name(containerPathUtf8, targetIndex,
                                                 nameBuffer, nameBufferSize);
    static const char archiveName[] = "Archive";
    if (sizeof(archiveName) > nameBufferSize) return PEARE_WRITER_UNSUPPORTED_LIMIT;
    std::memcpy(nameBuffer, archiveName, sizeof(archiveName));
    return PEARE_WRITER_OK;
}

peare_writer_status peare_writer_get_update_capabilities(
    const char* containerPathUtf8, size_t targetIndex,
    peare_writer_update_capabilities* outCapabilities) {
    if (!outCapabilities || !containerPathUtf8 || !*containerPathUtf8)
        return PEARE_WRITER_INVALID_ARGUMENT;
    *outCapabilities = 0;
    const size_t targetCount = peare_writer_update_target_count(containerPathUtf8);
    if (targetIndex >= targetCount) return PEARE_WRITER_INVALID_ARGUMENT;
    *outCapabilities = PEARE_WRITER_CAP_CREATE_FILE |
                       PEARE_WRITER_CAP_CREATE_EMPTY_FILE;
    try {
        peare_writer_handle_s probe;
        QFile input(QString::fromUtf8(containerPathUtf8));
        if (input.open(QIODevice::ReadOnly) &&
            loadExistingZip(&probe, &input) == PEARE_WRITER_OK) {
            *outCapabilities |= PEARE_WRITER_CAP_CREATE_DIRECTORY |
                                PEARE_WRITER_CAP_UPDATE_FILE |
                                PEARE_WRITER_CAP_RENAME_ENTRY |
                                PEARE_WRITER_CAP_DELETE_ENTRY;
        } else {
            probe.entries.clear();
            probe.names.clear();
            probe.archiveComment.clear();
            if (input.isOpen() && loadExistingTar(&probe, &input) == PEARE_WRITER_OK)
                *outCapabilities |= PEARE_WRITER_CAP_CREATE_DIRECTORY |
                                    PEARE_WRITER_CAP_UPDATE_FILE |
                                    PEARE_WRITER_CAP_RENAME_ENTRY |
                                    PEARE_WRITER_CAP_DELETE_ENTRY;
            else if (peare_writer_add_target_count(containerPathUtf8))
                *outCapabilities |= PEARE_WRITER_CAP_CREATE_DIRECTORY |
                                    PEARE_WRITER_CAP_UPDATE_FILE |
                                    PEARE_WRITER_CAP_RENAME_ENTRY |
                                    PEARE_WRITER_CAP_DELETE_ENTRY;
        }
    } catch (const std::bad_alloc&) {
        return PEARE_WRITER_ALLOCATION_FAILED;
    } catch (...) {
        return PEARE_WRITER_INTERNAL_ERROR;
    }
    return PEARE_WRITER_OK;
}

peare_writer_status peare_writer_get_add_target_name(const char* containerPathUtf8,
                                                      size_t targetIndex,
                                                      char* nameBuffer,
                                                      size_t nameBufferSize) {
    if (!containerPathUtf8 || !*containerPathUtf8 || !nameBuffer || !nameBufferSize)
        return PEARE_WRITER_INVALID_ARGUMENT;
    try {
        const std::vector<peare::writer::DiskImageTarget> targets =
            peare::writer::DiskImageUpdate::targets(QString::fromUtf8(containerPathUtf8));
        if (targetIndex >= targets.size()) return PEARE_WRITER_INVALID_ARGUMENT;
        QByteArray name = targets[targetIndex].label.toUtf8();
        if (targets[targetIndex].efiSystem) name += QByteArrayLiteral(" (EFI System)");
        if (name.size() < 0 || size_t(name.size()) >= nameBufferSize)
            return PEARE_WRITER_UNSUPPORTED_LIMIT;
        std::memcpy(nameBuffer, name.constData(), size_t(name.size()));
        nameBuffer[name.size()] = '\0';
        return PEARE_WRITER_OK;
    } catch (const std::bad_alloc&) {
        return PEARE_WRITER_ALLOCATION_FAILED;
    } catch (...) {
        return PEARE_WRITER_INTERNAL_ERROR;
    }
}

peare_writer_status peare_writer_add_files_begin(peare_writer_handle writer,
                                                 const char* containerPathUtf8) {
    return cBoundary(writer, [&]() -> peare_writer_status {
        if (!writer) return PEARE_WRITER_INVALID_HANDLE;
        if (!containerPathUtf8 || !*containerPathUtf8)
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("A container path is required."), false);
        if (writer->active)
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("A writer operation is already active."), false);
        writer->entries.clear();
        writer->names.clear();
        writer->error.clear();
        writer->errorUtf8.clear();
        const size_t diskTargetCount = peare_writer_add_target_count(containerPathUtf8);
        if (diskTargetCount) {
            const bool targetSpecified = writer->fatTargetSpecified;
            const size_t targetIndex = writer->fatTargetIndex;
            writer->fatTargetSpecified = false;
            return beginDiskImageUpdate(writer, QString::fromUtf8(containerPathUtf8),
                                        targetSpecified, targetIndex);
        }
        QFile input(QString::fromUtf8(containerPathUtf8));
        if (!input.open(QIODevice::ReadOnly))
            return fail(writer, PEARE_WRITER_IO_ERROR, input.errorString(), false);
        peare_writer_status parsed = loadExistingZip(writer, &input);
        auto mode = peare_writer_handle_s::Mode::ZipUpdate;
        if (parsed != PEARE_WRITER_OK) {
            writer->entries.clear();
            writer->names.clear();
            writer->archiveComment.clear();
            parsed = loadExistingTar(writer, &input);
            mode = peare_writer_handle_s::Mode::TarUpdate;
            if (parsed != PEARE_WRITER_OK) {
                writer->entries.clear();
                writer->names.clear();
                writer->archiveComment.clear();
                writer->fatTargetSpecified = false;
                input.close();
                return beginDiskImageUpdate(writer, QString::fromUtf8(containerPathUtf8),
                                            false, 0);
            }
        }
        QIODevice* stagedOutput = nullptr;
        if (mode == peare_writer_handle_s::Mode::ZipUpdate) {
            writer->updatePath = QString::fromUtf8(containerPathUtf8);
            writer->zipStage.reset(new (std::nothrow) QTemporaryFile);
            if (!writer->zipStage)
                return fail(writer, PEARE_WRITER_ALLOCATION_FAILED,
                            QStringLiteral("Could not allocate the ZIP staging file."), false);
            if (!writer->zipStage->open()) {
                const QString error = writer->zipStage->errorString();
                writer->zipStage.reset();
                writer->updatePath.clear();
                return fail(writer, PEARE_WRITER_IO_ERROR, error, false);
            }
            stagedOutput = writer->zipStage.get();
        } else if (mode == peare_writer_handle_s::Mode::TarUpdate) {
            writer->updatePath = QString::fromUtf8(containerPathUtf8);
            writer->tarStage.reset(new (std::nothrow) QTemporaryFile);
            if (!writer->tarStage || !writer->tarStage->open()) {
                const QString error = writer->tarStage ? writer->tarStage->errorString()
                                                       : QStringLiteral("Could not allocate TAR staging.");
                writer->tarStage.reset();
                return fail(writer, PEARE_WRITER_IO_ERROR, error, false);
            }
            stagedOutput = writer->tarStage.get();
        } else {
            writer->output.reset(new (std::nothrow) QSaveFile(
                QString::fromUtf8(containerPathUtf8)));
            if (!writer->output)
                return fail(writer, PEARE_WRITER_ALLOCATION_FAILED,
                            QStringLiteral("Could not allocate the container update."), false);
            if (!writer->output->open(QIODevice::WriteOnly)) {
                const QString error = writer->output->errorString();
                writer->output.reset();
                writer->entries.clear();
                writer->names.clear();
                return fail(writer, PEARE_WRITER_IO_ERROR, error, false);
            }
            stagedOutput = writer->output.get();
        }
        writer->active = true;
        writer->updateMode = true;
        writer->archiveDirty = false;
        writer->mode = mode;
        if (!input.seek(0))
            return fail(writer, PEARE_WRITER_IO_ERROR, input.errorString());
        std::vector<char> chunk(kChunkSize);
        qint64 remaining = mode == peare_writer_handle_s::Mode::TarUpdate
            ? writer->tarContentEnd : input.size();
        while (true) {
            const qint64 request = qMin<qint64>(qint64(chunk.size()), remaining);
            if (request == 0) break;
            const qint64 count = input.read(chunk.data(), request);
            if (count < 0)
                return fail(writer, PEARE_WRITER_IO_ERROR, input.errorString());
            if (!count)
                return fail(writer, PEARE_WRITER_IO_ERROR,
                            QStringLiteral("The container ended while it was being copied."));
            if (!writeAll(stagedOutput, chunk.data(), count))
                return fail(writer, PEARE_WRITER_IO_ERROR, stagedOutput->errorString());
            remaining -= count;
        }
        if (mode == peare_writer_handle_s::Mode::TarUpdate && writer->tarStage &&
            !writer->tarStage->seek(writer->tarContentEnd))
            return fail(writer, PEARE_WRITER_IO_ERROR, writer->tarStage->errorString());
        return PEARE_WRITER_OK;
    });
}

peare_writer_status peare_writer_add_files_begin_target(peare_writer_handle writer,
                                                         const char* containerPathUtf8,
                                                         size_t targetIndex) {
    if (!writer) return PEARE_WRITER_INVALID_HANDLE;
    if (!containerPathUtf8 || !*containerPathUtf8)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("A container path is required."), false);
    if (writer->active || writer->fatTargetSpecified)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("A writer operation is already active."), false);
    const size_t count = peare_writer_add_target_count(containerPathUtf8);
    if (!count) {
        if (targetIndex != 0)
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("The selected filesystem volume is no longer available."), false);
        return peare_writer_add_files_begin(writer, containerPathUtf8);
    }
    if (targetIndex >= count)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("The selected filesystem volume is no longer available."), false);
    return cBoundary(writer, [&] {
        return beginDiskImageUpdate(writer, QString::fromUtf8(containerPathUtf8),
                                    true, targetIndex);
    });
}

peare_writer_status peare_writer_update_begin(peare_writer_handle writer,
                                              const char* containerPathUtf8) {
    return peare_writer_add_files_begin(writer, containerPathUtf8);
}

peare_writer_status peare_writer_update_begin_target(peare_writer_handle writer,
                                                      const char* containerPathUtf8,
                                                      size_t targetIndex) {
    return peare_writer_add_files_begin_target(writer, containerPathUtf8, targetIndex);
}

peare_writer_status peare_writer_update_begin_target_with_journal(
    peare_writer_handle writer, const char* containerPathUtf8,
    size_t targetIndex, int64_t journalOffset, int64_t journalSize) {
    if (!writer) return PEARE_WRITER_INVALID_HANDLE;
    if (!containerPathUtf8 || !*containerPathUtf8 ||
        journalOffset <= 0 || journalSize <= 0)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("An image path and external journal range are required."), false);
    if (writer->active)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("A writer operation is already active."), false);
    const size_t count = peare_writer_update_target_count(containerPathUtf8);
    if (targetIndex >= count)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("The selected filesystem volume is no longer available."), false);
    return cBoundary(writer, [&] {
        return beginDiskImageUpdate(writer, QString::fromUtf8(containerPathUtf8),
                                    true, targetIndex, journalOffset, journalSize);
    });
}

peare_writer_status peare_writer_add_file(peare_writer_handle writer,
                                          const char* sourcePathUtf8,
                                          const char* entryNameUtf8) {
    if (!writer) return PEARE_WRITER_INVALID_HANDLE;
    if (!writer->active)
        return fail(writer, PEARE_WRITER_NOT_ACTIVE,
                    QStringLiteral("No container write is active."), false);
    if (writer->mode == peare_writer_handle_s::Mode::TarCreate ||
        writer->mode == peare_writer_handle_s::Mode::TarUpdate)
        return cBoundary(writer, [&] { return writeTarFile(writer, sourcePathUtf8, entryNameUtf8); });
    if (writer->mode == peare_writer_handle_s::Mode::ZipCreate ||
        writer->mode == peare_writer_handle_s::Mode::ZipUpdate)
        return peare_writer_zip_add_file(writer, sourcePathUtf8, entryNameUtf8);
    if ((writer->mode == peare_writer_handle_s::Mode::DiskUpdate ||
         writer->mode == peare_writer_handle_s::Mode::FatCreate) && writer->diskUpdate)
        return cBoundary(writer, [&]() -> peare_writer_status {
            if (!sourcePathUtf8 || !*sourcePathUtf8 || !entryNameUtf8 || !*entryNameUtf8)
                return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                            QStringLiteral("A source path and FAT entry name are required."));
            QString error;
            if (!writer->diskUpdate->addFile(QString::fromUtf8(sourcePathUtf8),
                                            QByteArray(entryNameUtf8 ? entryNameUtf8 : ""),
                                            &error))
                return fail(writer, PEARE_WRITER_IO_ERROR, error);
            return PEARE_WRITER_OK;
        });
    return fail(writer, PEARE_WRITER_NOT_ACTIVE,
                QStringLiteral("No container write is active."), false);
}

peare_writer_status peare_writer_create_file(peare_writer_handle writer,
                                             const char* sourcePathUtf8,
                                             const char* entryNameUtf8) {
    return peare_writer_add_file(writer, sourcePathUtf8, entryNameUtf8);
}

peare_writer_status peare_writer_create_empty_file(peare_writer_handle writer,
                                                   const char* entryNameUtf8) {
    if (!writer) return PEARE_WRITER_INVALID_HANDLE;
    if (!writer->active)
        return fail(writer, PEARE_WRITER_NOT_ACTIVE,
                    QStringLiteral("No writer operation is active."), false);
    if (!entryNameUtf8 || !*entryNameUtf8)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("An entry name is required."));
    return cBoundary(writer, [&]() -> peare_writer_status {
        QTemporaryFile emptyFile;
        if (!emptyFile.open())
            return fail(writer, PEARE_WRITER_IO_ERROR, emptyFile.errorString());
        const QByteArray path = emptyFile.fileName().toUtf8();
        emptyFile.close();
        return peare_writer_add_file(writer, path.constData(), entryNameUtf8);
    });
}

peare_writer_status peare_writer_create_directory(peare_writer_handle writer,
                                                  const char* entryNameUtf8) {
    if (!writer) return PEARE_WRITER_INVALID_HANDLE;
    if (!writer->active)
        return fail(writer, PEARE_WRITER_NOT_ACTIVE,
                    QStringLiteral("No writer operation is active."), false);
    if (!entryNameUtf8 || !*entryNameUtf8)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("An entry name is required."));
    if (writer->mode == peare_writer_handle_s::Mode::ZipCreate ||
        writer->mode == peare_writer_handle_s::Mode::ZipUpdate)
        return peare_writer_zip_add_directory(writer, entryNameUtf8);
    if (writer->mode == peare_writer_handle_s::Mode::TarCreate ||
        writer->mode == peare_writer_handle_s::Mode::TarUpdate)
        return cBoundary(writer, [&] { return writeTarDirectory(writer, entryNameUtf8); });
    if ((writer->mode == peare_writer_handle_s::Mode::DiskUpdate ||
         writer->mode == peare_writer_handle_s::Mode::FatCreate) && writer->diskUpdate)
        return cBoundary(writer, [&]() -> peare_writer_status {
            QString error;
            if (!writer->diskUpdate->createDirectory(QByteArray(entryNameUtf8), &error))
                return fail(writer, PEARE_WRITER_IO_ERROR, error);
            return PEARE_WRITER_OK;
        });
    return fail(writer, PEARE_WRITER_UNSUPPORTED_OPERATION,
                QStringLiteral("This writer backend cannot create directories."), false);
}

peare_writer_status peare_writer_replace_file(peare_writer_handle writer,
                                              const char* sourcePathUtf8,
                                              const char* entryNameUtf8) {
    if (!writer) return PEARE_WRITER_INVALID_HANDLE;
    if (!writer->active)
        return fail(writer, PEARE_WRITER_NOT_ACTIVE,
                    QStringLiteral("No writer operation is active."), false);
    if (!sourcePathUtf8 || !*sourcePathUtf8 || !entryNameUtf8 || !*entryNameUtf8)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("A source path and entry name are required."));
    if (writer->mode == peare_writer_handle_s::Mode::TarUpdate) {
        QByteArray name;
        QByteArray prefix;
        if (!tarEntryName(entryNameUtf8, &name, &prefix))
            return fail(writer, PEARE_WRITER_INVALID_ENTRY_NAME,
                        QStringLiteral("The TAR entry name is invalid or unsafe."), false);
        QString key = QString::fromUtf8(prefix.isEmpty() ? name : prefix + '/' + name);
        if (key.endsWith(QLatin1Char('/'))) key.chop(1);
        auto found = std::find_if(writer->tarEntries.begin(), writer->tarEntries.end(),
                                  [&](const TarEntry& entry) { return entry.key == key; });
        if (found == writer->tarEntries.end())
            return fail(writer, PEARE_WRITER_ENTRY_NOT_FOUND,
                        QStringLiteral("The TAR file to replace was not found."), false);
        if (found->directory)
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("A TAR directory cannot be replaced with a file."), false);
        QFile source(QString::fromUtf8(sourcePathUtf8));
        if (!source.open(QIODevice::ReadOnly))
            return fail(writer, PEARE_WRITER_IO_ERROR, source.errorString(), false);
        const size_t index = size_t(found - writer->tarEntries.begin());
        TarEntry previous = *found;
        writer->tarEntries.erase(found);
        writer->names.remove(key);
        const peare_writer_status status = writeTarFile(writer, sourcePathUtf8,
                                                        key.toUtf8().constData());
        if (status != PEARE_WRITER_OK && writer->active) {
            writer->tarEntries.insert(writer->tarEntries.begin() + index, std::move(previous));
            writer->names.insert(key);
        }
        return status;
    }
    if ((writer->mode == peare_writer_handle_s::Mode::DiskUpdate ||
         writer->mode == peare_writer_handle_s::Mode::FatCreate) && writer->diskUpdate)
        return cBoundary(writer, [&]() -> peare_writer_status {
            QString error;
            if (!writer->diskUpdate->replaceFile(QString::fromUtf8(sourcePathUtf8),
                                                QByteArray(entryNameUtf8), &error))
                return fail(writer, PEARE_WRITER_IO_ERROR, error);
            return PEARE_WRITER_OK;
        });
    if (writer->mode != peare_writer_handle_s::Mode::ZipUpdate)
        return fail(writer, PEARE_WRITER_UNSUPPORTED_OPERATION,
                    QStringLiteral("This writer backend cannot replace entries."), false);
    QByteArray entryName;
    QString key;
    if (!normalizedZipKey(entryNameUtf8, &entryName, &key))
        return fail(writer, PEARE_WRITER_INVALID_ENTRY_NAME,
                    QStringLiteral("The ZIP entry name is invalid or unsafe."), false);
    auto found = std::find_if(writer->entries.begin(), writer->entries.end(),
                              [&](const Entry& entry) { return entry.key == key; });
    if (found == writer->entries.end())
        return fail(writer, PEARE_WRITER_ENTRY_NOT_FOUND,
                    QStringLiteral("The ZIP file to replace was not found."), false);
    if (found->directory)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("A directory entry cannot be replaced with a file."), false);
    QFile source(QString::fromUtf8(sourcePathUtf8));
    if (!source.open(QIODevice::ReadOnly))
        return fail(writer, PEARE_WRITER_IO_ERROR, source.errorString(), false);
    source.close();
    const size_t index = size_t(found - writer->entries.begin());
    Entry previous = *found;
    writer->entries.erase(found);
    writer->names.remove(key);
    const peare_writer_status status = peare_writer_zip_add_file(
        writer, sourcePathUtf8, entryName.constData());
    if (status != PEARE_WRITER_OK && writer->active) {
        writer->entries.insert(writer->entries.begin() + index, std::move(previous));
        writer->names.insert(key);
    }
    return status;
}

peare_writer_status peare_writer_enable_fsverity(peare_writer_handle writer,
                                                  const char* entryNameUtf8) {
    if (!writer) return PEARE_WRITER_INVALID_HANDLE;
    if (!writer->active)
        return fail(writer, PEARE_WRITER_NOT_ACTIVE,
                    QStringLiteral("No writer operation is active."), false);
    if (!entryNameUtf8 || !*entryNameUtf8)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("An Ext file path is required."));
    if ((writer->mode == peare_writer_handle_s::Mode::DiskUpdate ||
         writer->mode == peare_writer_handle_s::Mode::FatCreate) && writer->diskUpdate)
        return cBoundary(writer, [&]() -> peare_writer_status {
            QString error;
            if (!writer->diskUpdate->enableFsVerity(QByteArray(entryNameUtf8), &error))
                return fail(writer, PEARE_WRITER_IO_ERROR, error);
            return PEARE_WRITER_OK;
        });
    return fail(writer, PEARE_WRITER_UNSUPPORTED_OPERATION,
                QStringLiteral("fs-verity requires an active Ext filesystem update."), false);
}

peare_writer_status peare_writer_rename_entry(peare_writer_handle writer,
                                              const char* entryNameUtf8,
                                              const char* newEntryNameUtf8) {
    if (!writer) return PEARE_WRITER_INVALID_HANDLE;
    if (!writer->active)
        return fail(writer, PEARE_WRITER_NOT_ACTIVE,
                    QStringLiteral("No writer operation is active."), false);
    if (!entryNameUtf8 || !*entryNameUtf8 || !newEntryNameUtf8 || !*newEntryNameUtf8)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("Source and destination entry names are required."));
    if (writer->mode == peare_writer_handle_s::Mode::TarUpdate) {
        QByteArray oldName, oldPrefix, newName, newPrefix;
        if (!tarEntryName(entryNameUtf8, &oldName, &oldPrefix) ||
            !tarEntryName(newEntryNameUtf8, &newName, &newPrefix))
            return fail(writer, PEARE_WRITER_INVALID_ENTRY_NAME,
                        QStringLiteral("The TAR entry name is invalid or unsafe."), false);
        QString oldKey = QString::fromUtf8(oldPrefix.isEmpty() ? oldName : oldPrefix + '/' + oldName);
        QString newKey = QString::fromUtf8(newPrefix.isEmpty() ? newName : newPrefix + '/' + newName);
        if (oldKey.endsWith(QLatin1Char('/'))) oldKey.chop(1);
        if (newKey.endsWith(QLatin1Char('/'))) newKey.chop(1);
        const QString prefix = oldKey + QLatin1Char('/');
        bool exact = false, directory = false, children = false;
        for (const TarEntry& entry : writer->tarEntries) {
            if (entry.key == oldKey) { exact = true; directory = entry.directory; }
            else if (entry.key.startsWith(prefix)) children = true;
        }
        if (!exact && !children)
            return fail(writer, PEARE_WRITER_ENTRY_NOT_FOUND,
                        QStringLiteral("The TAR entry to rename was not found."), false);
        if (exact && !directory && children)
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("The TAR path conflicts with a file and a directory."), false);
        directory = directory || children;
        if (directory && newKey.startsWith(prefix))
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("A directory cannot be moved inside itself."), false);
        QSet<QString> planned;
        for (const TarEntry& entry : writer->tarEntries) {
            if (entry.key != oldKey && !(directory && entry.key.startsWith(prefix))) continue;
            const QString mapped = entry.key == oldKey ? newKey : newKey + entry.key.mid(oldKey.size());
            if (planned.contains(mapped))
                return fail(writer, PEARE_WRITER_DUPLICATE_ENTRY,
                            QStringLiteral("The destination TAR path conflicts with an entry."), false);
            planned.insert(mapped);
        }
        for (const TarEntry& entry : writer->tarEntries) {
            if (planned.contains(entry.key) && entry.key != oldKey &&
                !(directory && entry.key.startsWith(prefix)))
                return fail(writer, PEARE_WRITER_DUPLICATE_ENTRY,
                            QStringLiteral("The destination TAR path already exists."), false);
        }
        for (TarEntry& entry : writer->tarEntries) {
            if (entry.key != oldKey && !(directory && entry.key.startsWith(prefix))) continue;
            const QString mapped = entry.key == oldKey ? newKey : newKey + entry.key.mid(oldKey.size());
            if (!rewriteTarName(&entry, mapped))
                return fail(writer, PEARE_WRITER_UNSUPPORTED_LIMIT,
                            QStringLiteral("The renamed path cannot be represented in USTAR."), false);
        }
        writer->names.clear();
        for (const TarEntry& entry : writer->tarEntries) writer->names.insert(entry.key);
        writer->archiveDirty = true;
        return PEARE_WRITER_OK;
    }
    if ((writer->mode == peare_writer_handle_s::Mode::DiskUpdate ||
         writer->mode == peare_writer_handle_s::Mode::FatCreate) && writer->diskUpdate)
        return cBoundary(writer, [&]() -> peare_writer_status {
            QString error;
            if (!writer->diskUpdate->renameEntry(QByteArray(entryNameUtf8),
                                                QByteArray(newEntryNameUtf8), &error))
                return fail(writer, PEARE_WRITER_IO_ERROR, error);
            return PEARE_WRITER_OK;
        });
    if (writer->mode != peare_writer_handle_s::Mode::ZipUpdate)
        return fail(writer, PEARE_WRITER_UNSUPPORTED_OPERATION,
                    QStringLiteral("This writer backend cannot rename entries."), false);
    return cBoundary(writer, [&]() -> peare_writer_status {
        QByteArray oldName;
        QByteArray newName;
        QString oldKey;
        QString newKey;
        if (!normalizedZipKey(entryNameUtf8, &oldName, &oldKey) ||
            !normalizedZipKey(newEntryNameUtf8, &newName, &newKey))
            return fail(writer, PEARE_WRITER_INVALID_ENTRY_NAME,
                        QStringLiteral("The ZIP entry name is invalid or unsafe."), false);
        const QString oldPrefix = oldKey + QLatin1Char('/');
        bool exactFound = false;
        bool hasChildren = false;
        bool sourceDirectory = false;
        for (const Entry& entry : writer->entries) {
            if (entry.key == oldKey) {
                exactFound = true;
                sourceDirectory = entry.directory;
            } else if (entry.key.startsWith(oldPrefix)) {
                hasChildren = true;
            }
        }
        if (!exactFound && !hasChildren)
            return fail(writer, PEARE_WRITER_ENTRY_NOT_FOUND,
                        QStringLiteral("The ZIP entry to rename was not found."), false);
        if (exactFound && !sourceDirectory && hasChildren)
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("The ZIP path conflicts with both a file and a directory."),
                        false);
        sourceDirectory = sourceDirectory || hasChildren;
        if (sourceDirectory && newKey.startsWith(oldPrefix))
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("A directory cannot be moved inside itself."), false);
        if (oldKey == newKey) return PEARE_WRITER_OK;

        struct RenameItem {
            size_t index;
            QString key;
            QByteArray name;
        };
        std::vector<RenameItem> plan;
        QSet<size_t> affected;
        QSet<QString> plannedKeys;
        for (size_t index = 0; index < writer->entries.size(); ++index) {
            const Entry& entry = writer->entries[index];
            const bool root = entry.key == oldKey;
            const bool child = sourceDirectory && entry.key.startsWith(oldPrefix);
            if (!root && !child) continue;
            QString mappedKey = root ? newKey : newKey + entry.key.mid(oldKey.size());
            const QByteArray mappedName = (entry.directory ? mappedKey + QLatin1Char('/')
                                                            : mappedKey).toUtf8();
            if (mappedName.size() > 0xffff || plannedKeys.contains(mappedKey))
                return fail(writer, PEARE_WRITER_UNSUPPORTED_LIMIT,
                            QStringLiteral("The renamed ZIP path cannot be represented safely."),
                            false);
            affected.insert(index);
            plannedKeys.insert(mappedKey);
            plan.push_back({index, mappedKey, mappedName});
        }
        for (const Entry& entry : writer->entries) {
            if (plannedKeys.contains(entry.key) &&
                !affected.contains(size_t(&entry - writer->entries.data())))
                return fail(writer, PEARE_WRITER_DUPLICATE_ENTRY,
                            QStringLiteral("The destination ZIP path already exists."), false);
            for (const QString& planned : plannedKeys) {
                const bool existingFileIsAncestor = !entry.directory &&
                    planned.startsWith(entry.key + QLatin1Char('/'));
                if (entry.key == planned ||
                    entry.key.startsWith(planned + QLatin1Char('/')) ||
                    existingFileIsAncestor) {
                    if (!affected.contains(size_t(&entry - writer->entries.data())))
                        return fail(writer, PEARE_WRITER_DUPLICATE_ENTRY,
                                    QStringLiteral("The destination ZIP path conflicts with an entry."),
                                    false);
                }
            }
        }
        for (const RenameItem& item : plan) {
            QString error;
            Entry& entry = writer->entries[item.index];
            if (!appendRelocatedZipEntry(writer, &entry, item.name, item.key, &error))
                return fail(writer, PEARE_WRITER_IO_ERROR, error);
        }
        writer->names.clear();
        for (const Entry& entry : writer->entries) writer->names.insert(entry.key);
        writer->archiveDirty = true;
        return PEARE_WRITER_OK;
    });
}

peare_writer_status peare_writer_delete_entry(peare_writer_handle writer,
                                              const char* entryNameUtf8,
                                              int recursive) {
    if (!writer) return PEARE_WRITER_INVALID_HANDLE;
    if (!writer->active)
        return fail(writer, PEARE_WRITER_NOT_ACTIVE,
                    QStringLiteral("No writer operation is active."), false);
    if (!entryNameUtf8 || !*entryNameUtf8 || (recursive != 0 && recursive != 1))
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("A valid entry name and recursive flag are required."));
    if (writer->mode == peare_writer_handle_s::Mode::TarUpdate) {
        QByteArray name, prefix;
        if (!tarEntryName(entryNameUtf8, &name, &prefix))
            return fail(writer, PEARE_WRITER_INVALID_ENTRY_NAME,
                        QStringLiteral("The TAR entry name is invalid or unsafe."), false);
        QString key = QString::fromUtf8(prefix.isEmpty() ? name : prefix + '/' + name);
        if (key.endsWith(QLatin1Char('/'))) key.chop(1);
        const QString childPrefix = key + QLatin1Char('/');
        bool exact = false, directory = false, children = false;
        for (const TarEntry& entry : writer->tarEntries) {
            if (entry.key == key) { exact = true; directory = entry.directory; }
            else if (entry.key.startsWith(childPrefix)) children = true;
        }
        if (!exact && !children)
            return fail(writer, PEARE_WRITER_ENTRY_NOT_FOUND,
                        QStringLiteral("The TAR entry to delete was not found."), false);
        if ((directory || children) && children && !recursive)
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("The TAR directory is not empty."), false);
        writer->tarEntries.erase(std::remove_if(writer->tarEntries.begin(), writer->tarEntries.end(),
            [&](const TarEntry& entry) { return entry.key == key ||
                (recursive && entry.key.startsWith(childPrefix)); }), writer->tarEntries.end());
        writer->names.clear();
        for (const TarEntry& entry : writer->tarEntries) writer->names.insert(entry.key);
        writer->archiveDirty = true;
        return PEARE_WRITER_OK;
    }
    if ((writer->mode == peare_writer_handle_s::Mode::DiskUpdate ||
         writer->mode == peare_writer_handle_s::Mode::FatCreate) && writer->diskUpdate)
        return cBoundary(writer, [&]() -> peare_writer_status {
            QString error;
            if (!writer->diskUpdate->deleteEntry(QByteArray(entryNameUtf8), recursive != 0, &error))
                return fail(writer, PEARE_WRITER_IO_ERROR, error);
            return PEARE_WRITER_OK;
        });
    if (writer->mode != peare_writer_handle_s::Mode::ZipUpdate)
        return fail(writer, PEARE_WRITER_UNSUPPORTED_OPERATION,
                    QStringLiteral("This writer backend cannot delete entries."), false);
    QByteArray name;
    QString key;
    if (!normalizedZipKey(entryNameUtf8, &name, &key))
        return fail(writer, PEARE_WRITER_INVALID_ENTRY_NAME,
                    QStringLiteral("The ZIP entry name is invalid or unsafe."), false);
    const QString prefix = key + QLatin1Char('/');
    bool exactFound = false;
    bool exactDirectory = false;
    bool hasChildren = false;
    for (const Entry& entry : writer->entries) {
        if (entry.key == key) {
            exactFound = true;
            exactDirectory = entry.directory;
        } else if (entry.key.startsWith(prefix)) {
            hasChildren = true;
        }
    }
    if (!exactFound && !hasChildren)
        return fail(writer, PEARE_WRITER_ENTRY_NOT_FOUND,
                    QStringLiteral("The ZIP entry to delete was not found."), false);
    if (exactFound && !exactDirectory && hasChildren)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("The ZIP path conflicts with both a file and a directory."),
                    false);
    if ((exactDirectory || hasChildren) && hasChildren && !recursive)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("The ZIP directory is not empty."), false);
    writer->entries.erase(std::remove_if(writer->entries.begin(), writer->entries.end(),
        [&](const Entry& entry) {
            const bool remove = entry.key == key ||
                                (recursive && entry.key.startsWith(prefix));
            if (remove) writer->names.remove(entry.key);
            return remove;
        }), writer->entries.end());
    writer->archiveDirty = true;
    return PEARE_WRITER_OK;
}

peare_writer_status peare_writer_update_finish(peare_writer_handle writer) {
    if (!writer) return PEARE_WRITER_INVALID_HANDLE;
    if (writer->mode != peare_writer_handle_s::Mode::ZipUpdate &&
        writer->mode != peare_writer_handle_s::Mode::TarUpdate &&
        writer->mode != peare_writer_handle_s::Mode::DiskUpdate)
        return fail(writer, PEARE_WRITER_NOT_ACTIVE,
                    QStringLiteral("No container update is active."), false);
    return peare_writer_finish(writer);
}

peare_writer_status peare_writer_finish(peare_writer_handle writer) {
    if (!writer) return PEARE_WRITER_INVALID_HANDLE;
    if (!writer->active)
        return fail(writer, PEARE_WRITER_NOT_ACTIVE,
                    QStringLiteral("No container write is active."), false);
    if (writer->mode == peare_writer_handle_s::Mode::ZipCreate ||
        writer->mode == peare_writer_handle_s::Mode::ZipUpdate)
        return peare_writer_zip_finish(writer);
    if (writer->mode == peare_writer_handle_s::Mode::TarCreate ||
        writer->mode == peare_writer_handle_s::Mode::TarUpdate)
        return cBoundary(writer, [&]() -> peare_writer_status {
            if (writer->mode == peare_writer_handle_s::Mode::TarUpdate) {
                if (!writer->archiveDirty)
                    return finishNoOpArchiveUpdate(writer);
                writer->output.reset(new (std::nothrow) QSaveFile(writer->updatePath));
                if (!writer->output || !writer->output->open(QIODevice::WriteOnly))
                    return fail(writer, PEARE_WRITER_IO_ERROR,
                                writer->output ? writer->output->errorString()
                                               : QStringLiteral("Could not allocate TAR output."));
                std::vector<char> chunk(kChunkSize);
                for (const TarEntry& entry : writer->tarEntries) {
                    if (!writeAll(writer->output.get(), entry.header.constData(), entry.header.size()))
                        return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
                    if (!writer->tarStage->seek(entry.offset + qint64(kTarBlockSize)))
                        return fail(writer, PEARE_WRITER_IO_ERROR, writer->tarStage->errorString());
                    quint64 remaining = entry.size;
                    while (remaining) {
                        const qint64 request = qint64(std::min<quint64>(remaining, chunk.size()));
                        const qint64 count = writer->tarStage->read(chunk.data(), request);
                        if (count != request)
                            return fail(writer, PEARE_WRITER_IO_ERROR,
                                        QStringLiteral("The TAR staging data is incomplete."));
                        if (!writeAll(writer->output.get(), chunk.data(), count))
                            return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
                        remaining -= quint64(count);
                    }
                    const quint64 padding = (kTarBlockSize - (entry.size % kTarBlockSize)) % kTarBlockSize;
                    if (padding) {
                        const QByteArray zeros(int(padding), '\0');
                        if (!writeAll(writer->output.get(), zeros.constData(), zeros.size()))
                            return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
                    }
                }
            }
            const QByteArray end(int(kTarBlockSize * 2), '\0');
            if (!writeAll(writer->output.get(), end.constData(), end.size()))
                return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
            if (!writer->output->commit())
                return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
            writer->output.reset();
            writer->tarStage.reset();
            writer->tarEntries.clear();
            writer->entries.clear();
            writer->names.clear();
            writer->active = false;
            writer->updateMode = false;
            writer->archiveDirty = false;
            writer->mode = peare_writer_handle_s::Mode::None;
            writer->tarContentEnd = 0;
            writer->updatePath.clear();
            writer->error.clear();
            writer->errorUtf8.clear();
            return PEARE_WRITER_OK;
        });
    if ((writer->mode == peare_writer_handle_s::Mode::DiskUpdate ||
         writer->mode == peare_writer_handle_s::Mode::FatCreate) && writer->diskUpdate)
        return cBoundary(writer, [&]() -> peare_writer_status {
            QString error;
            if (!writer->diskUpdate->finish(&error))
                return fail(writer, PEARE_WRITER_IO_ERROR, error);
            writer->diskUpdate.reset();
            writer->active = false;
            writer->updateMode = false;
            writer->mode = peare_writer_handle_s::Mode::None;
            writer->error.clear();
            writer->errorUtf8.clear();
            return PEARE_WRITER_OK;
        });
    return fail(writer, PEARE_WRITER_NOT_ACTIVE,
                QStringLiteral("No container write is active."), false);
}

void peare_writer_destroy(peare_writer_handle writer) {
    if (!writer) return;
    if (writer->output) writer->output->cancelWriting();
    if (writer->zipStage) writer->zipStage->close();
    if (writer->diskUpdate) writer->diskUpdate->abort();
    delete writer;
}

peare_writer_status peare_writer_zip_begin(peare_writer_handle writer,
                                           const char* outputPathUtf8) {
    return cBoundary(writer, [&]() -> peare_writer_status {
    if (!writer) return PEARE_WRITER_INVALID_HANDLE;
    if (!outputPathUtf8 || !*outputPathUtf8)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("An output path is required."), false);
    if (writer->active)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("A ZIP archive is already active."), false);
    writer->error.clear();
    writer->errorUtf8.clear();
    writer->entries.clear();
    writer->names.clear();
    writer->zipStage.reset();
    writer->updatePath.clear();
    writer->updateMode = false;
    writer->mode = peare_writer_handle_s::Mode::None;
    writer->output.reset(new (std::nothrow) QSaveFile(QString::fromUtf8(outputPathUtf8)));
    if (!writer->output)
        return fail(writer, PEARE_WRITER_ALLOCATION_FAILED,
                    QStringLiteral("Could not allocate the ZIP output."), false);
    if (!writer->output->open(QIODevice::WriteOnly)) {
        const QString error = writer->output->errorString();
        writer->output.reset();
        return fail(writer, PEARE_WRITER_IO_ERROR, error, false);
    }
    writer->active = true;
    writer->mode = peare_writer_handle_s::Mode::ZipCreate;
    return PEARE_WRITER_OK;
    });
}

peare_writer_status peare_writer_zip_add_directory(peare_writer_handle writer,
                                                   const char* entryNameUtf8) {
    return cBoundary(writer, [&]() -> peare_writer_status {
    if (!writer) return PEARE_WRITER_INVALID_HANDLE;
    Entry entry;
    peare_writer_status status = prepareEntry(writer, entryNameUtf8, true, &entry);
    if (status != PEARE_WRITER_OK) return status;
    entry.crc = 0;
    entry.size = 0;
    entry.compressedSize = 0;
    entry.uncompressedSize = 0;
    entry.flags = 0x0800;
    QIODevice* output = zipDevice(writer);
    if (!output || !writeLocalHeader(output, entry, false))
        return fail(writer, PEARE_WRITER_IO_ERROR,
                    output ? output->errorString() : QStringLiteral("The ZIP output is unavailable."));
    writer->names.insert(entry.key);
    writer->entries.push_back(entry);
    if (writer->mode == peare_writer_handle_s::Mode::ZipUpdate)
        writer->archiveDirty = true;
    return PEARE_WRITER_OK;
    });
}

peare_writer_status peare_writer_zip_add_buffer(peare_writer_handle writer,
                                                const char* entryNameUtf8,
                                                const uint8_t* bytes, size_t length) {
    return cBoundary(writer, [&]() -> peare_writer_status {
    if (!writer) return PEARE_WRITER_INVALID_HANDLE;
    if (length && !bytes)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("A non-empty buffer requires data."), false);
    if (length > size_t(kMaxZipUncompressed))
        return fail(writer, PEARE_WRITER_UNSUPPORTED_LIMIT,
                    QStringLiteral("ZIP entries above the Opener's 512 MiB limit cannot be added."));
    Entry entry;
    peare_writer_status status = prepareEntry(writer, entryNameUtf8, false, &entry);
    if (status != PEARE_WRITER_OK) return status;
    entry.size = quint64(length);
    entry.compressedSize = quint64(length);
    entry.uncompressedSize = quint64(length);
    entry.flags = 0x0800;
    entry.zip64Sizes = quint64(length) > kZip32Max;
    entry.crc = crc32Update(0xffffffffu, reinterpret_cast<const char*>(bytes), length) ^ 0xffffffffu;
    QIODevice* output = zipDevice(writer);
    if (!output || !writeLocalHeader(output, entry, false) ||
        (length && !writeAll(output, reinterpret_cast<const char*>(bytes),
                             qint64(length))))
        return fail(writer, PEARE_WRITER_IO_ERROR,
                    output ? output->errorString() : QStringLiteral("The ZIP output is unavailable."));
    writer->names.insert(entry.key);
    writer->entries.push_back(entry);
    if (writer->mode == peare_writer_handle_s::Mode::ZipUpdate)
        writer->archiveDirty = true;
    return PEARE_WRITER_OK;
    });
}

peare_writer_status peare_writer_zip_add_file(peare_writer_handle writer,
                                              const char* sourcePathUtf8,
                                              const char* entryNameUtf8) {
    return cBoundary(writer, [&]() -> peare_writer_status {
    if (!writer) return PEARE_WRITER_INVALID_HANDLE;
    if (!sourcePathUtf8 || !*sourcePathUtf8)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("A source file path is required."), false);
    Entry entry;
    peare_writer_status status = prepareEntry(writer, entryNameUtf8, false, &entry);
    if (status != PEARE_WRITER_OK) return status;
    QFile input(QString::fromUtf8(sourcePathUtf8));
    if (!input.open(QIODevice::ReadOnly))
        return fail(writer, PEARE_WRITER_IO_ERROR, input.errorString());
    return writeEntryData(writer, &entry, &input);
    });
}

peare_writer_status peare_writer_zip_finish(peare_writer_handle writer) {
    return cBoundary(writer, [&]() -> peare_writer_status {
    if (!writer) return PEARE_WRITER_INVALID_HANDLE;
    if (!writer->active)
        return fail(writer, PEARE_WRITER_NOT_ACTIVE,
                    QStringLiteral("No ZIP archive is being written."), false);
    if (writer->mode != peare_writer_handle_s::Mode::ZipCreate &&
        writer->mode != peare_writer_handle_s::Mode::ZipUpdate)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("The active writer is not a ZIP backend."), false);
    if (writer->mode == peare_writer_handle_s::Mode::ZipUpdate &&
        !writer->archiveDirty)
        return finishNoOpArchiveUpdate(writer);
    QIODevice* output = zipDevice(writer);
    if (!output)
        return fail(writer, PEARE_WRITER_IO_ERROR,
                    QStringLiteral("The ZIP output is unavailable."));
    const qint64 centralOffset = output->pos();
    if (centralOffset < 0)
        return fail(writer, PEARE_WRITER_IO_ERROR,
                    QStringLiteral("Could not determine the central directory position."));
    for (const Entry& entry : writer->entries) {
        if (!entry.centralRecord.isEmpty()) {
            if (!writeAll(output, entry.centralRecord.constData(),
                          entry.centralRecord.size()))
                return fail(writer, PEARE_WRITER_IO_ERROR, output->errorString());
            continue;
        }
        QByteArray header;
        QByteArray extra;
        const bool zip64Size = entry.size > kZip32Max;
        const bool zip64Offset = entry.offset > kZip32Max;
        if (zip64Size || zip64Offset) {
            append16(&extra, 1);
            append16(&extra, quint16((zip64Size ? 16 : 0) + (zip64Offset ? 8 : 0)));
            if (zip64Size) {
                append64(&extra, entry.size);
                append64(&extra, entry.size);
            }
            if (zip64Offset) append64(&extra, entry.offset);
        }
        header.reserve(46 + entry.name.size() + extra.size());
        append32(&header, kCentralHeaderSignature);
        append16(&header, zip64Size || zip64Offset ? 45 : 20); // version made by
        append16(&header, zip64Size || zip64Offset ? 45 : 20); // version needed
        append16(&header, quint16(entry.flags | (entry.descriptor ? 0x0008 : 0)));
        append16(&header, entry.method);
        append16(&header, 0); // DOS time
        append16(&header, 0x0021); // 1980-01-01
        append32(&header, entry.crc);
        append32(&header, zip64Size ? 0xffffffffu : quint32(entry.size));
        append32(&header, zip64Size ? 0xffffffffu : quint32(entry.size));
        append16(&header, quint16(entry.name.size()));
        append16(&header, quint16(extra.size()));
        append16(&header, 0); // comment length
        append16(&header, 0); // disk number
        append16(&header, 0); // internal attributes
        append32(&header, entry.directory ? 0x10u : 0u);
        append32(&header, zip64Offset ? 0xffffffffu : quint32(entry.offset));
        header += entry.name;
        header += extra;
        if (!writeAll(output, header.constData(), header.size()))
            return fail(writer, PEARE_WRITER_IO_ERROR, output->errorString());
    }
    const qint64 endOffset = output->pos();
    if (endOffset < centralOffset)
        return fail(writer, PEARE_WRITER_IO_ERROR,
                    QStringLiteral("Could not determine the central directory size."));
    const quint64 centralSize = quint64(endOffset - centralOffset);
    const bool needsZip64End = writer->entries.size() >= 0xffffu ||
        quint64(centralOffset) > kZip32Max || centralSize > kZip32Max;
    QByteArray end;
    if (needsZip64End) {
        const quint64 zip64EndOffset = quint64(endOffset);
        append32(&end, 0x06064b50u);
        append64(&end, 44);
        append16(&end, 45);
        append16(&end, 45);
        append32(&end, 0);
        append32(&end, 0);
        append64(&end, writer->entries.size());
        append64(&end, writer->entries.size());
        append64(&end, centralSize);
        append64(&end, quint64(centralOffset));
        append32(&end, 0x07064b50u);
        append32(&end, 0);
        append64(&end, zip64EndOffset);
        append32(&end, 1);
    }
    append32(&end, kEndRecordSignature);
    append16(&end, 0);
    append16(&end, 0);
    append16(&end, needsZip64End ? 0xffffu : quint16(writer->entries.size()));
    append16(&end, needsZip64End ? 0xffffu : quint16(writer->entries.size()));
    append32(&end, needsZip64End ? 0xffffffffu : quint32(centralSize));
    append32(&end, needsZip64End ? 0xffffffffu : quint32(centralOffset));
    append16(&end, quint16(writer->archiveComment.size()));
    end += writer->archiveComment;
    if (!writeAll(output, end.constData(), end.size()))
        return fail(writer, PEARE_WRITER_IO_ERROR, output->errorString());
    if (writer->zipStage) {
        if (!writer->zipStage->flush() || !writer->zipStage->seek(0))
            return fail(writer, PEARE_WRITER_IO_ERROR, writer->zipStage->errorString());
        writer->output.reset(new (std::nothrow) QSaveFile(writer->updatePath));
        if (!writer->output)
            return fail(writer, PEARE_WRITER_ALLOCATION_FAILED,
                        QStringLiteral("Could not allocate the ZIP replacement."));
        if (!writer->output->open(QIODevice::WriteOnly))
            return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
        std::vector<char> chunk(kChunkSize);
        qint64 remaining = writer->zipStage->size();
        while (remaining > 0) {
            const qint64 request = qMin<qint64>(qint64(chunk.size()), remaining);
            const qint64 count = writer->zipStage->read(chunk.data(), request);
            if (count <= 0)
                return fail(writer, PEARE_WRITER_IO_ERROR, writer->zipStage->errorString());
            if (!writeAll(writer->output.get(), chunk.data(), count))
                return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
            remaining -= count;
        }
        if (!writer->output->commit())
            return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
        writer->output.reset();
        writer->zipStage.reset();
        writer->updatePath.clear();
    } else {
        if (!writer->output->commit())
            return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
        writer->output.reset();
    }
    writer->entries.clear();
    writer->names.clear();
    writer->archiveComment.clear();
    writer->updateMode = false;
    writer->archiveDirty = false;
    writer->active = false;
    writer->mode = peare_writer_handle_s::Mode::None;
    writer->error.clear();
    writer->errorUtf8.clear();
    return PEARE_WRITER_OK;
    });
}

void peare_writer_abort(peare_writer_handle writer) {
    if (!writer || !writer->active) return;
    if (writer->output) {
        writer->output->cancelWriting();
        writer->output.reset();
    }
    writer->zipStage.reset();
    writer->tarStage.reset();
    if (writer->diskUpdate) {
        writer->diskUpdate->abort();
        writer->diskUpdate.reset();
    }
    writer->entries.clear();
    writer->tarEntries.clear();
    writer->names.clear();
    writer->archiveComment.clear();
    writer->updatePath.clear();
    writer->active = false;
    writer->updateMode = false;
    writer->archiveDirty = false;
    writer->mode = peare_writer_handle_s::Mode::None;
}

const char* peare_writer_last_error(peare_writer_handle writer) {
    if (!writer) return "Invalid writer handle.";
    return writer->errorUtf8.constData();
}

const char* peare_writer_status_message(peare_writer_status status) {
    switch (status) {
    case PEARE_WRITER_OK: return "Success.";
    case PEARE_WRITER_INVALID_ARGUMENT: return "Invalid argument.";
    case PEARE_WRITER_INVALID_HANDLE: return "Invalid writer handle.";
    case PEARE_WRITER_NOT_ACTIVE: return "No ZIP archive is being written.";
    case PEARE_WRITER_IO_ERROR: return "File I/O failed.";
    case PEARE_WRITER_UNSUPPORTED_LIMIT: return "A ZIP size or entry limit was exceeded.";
    case PEARE_WRITER_INVALID_ENTRY_NAME: return "The ZIP entry name is invalid or unsafe.";
    case PEARE_WRITER_DUPLICATE_ENTRY: return "The ZIP entry already exists.";
    case PEARE_WRITER_ALLOCATION_FAILED: return "Memory allocation failed.";
    case PEARE_WRITER_INTERNAL_ERROR: return "Unexpected internal error.";
    case PEARE_WRITER_UNSUPPORTED_OPERATION: return "The operation is not supported by this backend.";
    case PEARE_WRITER_ENTRY_NOT_FOUND: return "The requested ZIP entry was not found.";
    }
    return "Unknown writer error.";
}

} // extern "C"
