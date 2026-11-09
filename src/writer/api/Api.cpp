#include "peare/peare_writer.h"
#include "formats/DiskImageWriter.h"
#include "formats/WinZipJpegEncoder.h"
#include "../third_party/miniz/miniz.h"
#include "../third_party/bzip2/bzlib.h"
#include "../third_party/lzma/LzmaEnc.h"
#include "../third_party/lzma/7zCrc.h"
#include "../third_party/lzma/XzCrc64.h"
#include "../third_party/lzma/XzEnc.h"
#include "../third_party/lzma/Ppmd8.h"
#include "../third_party/zstd/zstd.h"
#include "../../opener/compression/third_party/packmp3/packmp3lib.h"
#include "../../opener/compression/third_party/packmp3/bitops.h"
#include "../../opener/compression/third_party/wavpack/include/wavpack.h"

#include <QFile>
#include <QFileInfo>
#include <QByteArray>
#include <QSaveFile>
#include <QString>
#include <QStringList>
#include <QSet>
#include <QDateTime>
#include <QMap>
#include <QTemporaryFile>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <map>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

namespace {

const quint32 kLocalHeaderSignature = 0x04034b50u;
const quint32 kCentralHeaderSignature = 0x02014b50u;
const quint32 kEndRecordSignature = 0x06054b50u;
const quint32 kDataDescriptorSignature = 0x08074b50u;
const quint32 kTarBlockSize = 512u;
const quint32 kCpioBlockSize = 4u;
const quint32 kWadDirectoryEntrySize = 16u;
const quint64 kZip32Max = 0xfffffffeull;
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

struct ArEntry {
    QString key;
    QByteArray name;
    quint64 timestamp = 0;
    quint32 uid = 0;
    quint32 gid = 0;
    quint32 mode = 0644;
    quint64 size = 0;
    qint64 dataOffset = 0;
};

struct WadEntry {
    QByteArray name;
    quint64 size = 0;
    qint64 dataOffset = 0;
};

struct CpioEntry {
    QString key;
    QByteArray name;
    quint64 inode = 0;
    quint32 mode = 0;
    quint32 uid = 0;
    quint32 gid = 0;
    quint32 links = 1;
    quint64 modified = 0;
    quint32 deviceMajor = 0;
    quint32 deviceMinor = 0;
    quint32 specialMajor = 0;
    quint32 specialMinor = 0;
    quint32 legacyDevice = 0;
    quint32 legacySpecialDevice = 0;
    quint64 size = 0;
    qint64 dataOffset = 0;
};

enum class CpioFormat { Newc, Crc, Odc, AfioLarge, BinaryLe, BinaryBe };
enum class ArFormat { Gnu, Bsd };
enum class WadFormat { Iwad, Pwad };

} // namespace

struct peare_writer_handle_s {
    std::unique_ptr<QSaveFile> output;
    std::unique_ptr<QTemporaryFile> zipStage;
    std::unique_ptr<QTemporaryFile> tarStage;
    std::unique_ptr<QTemporaryFile> cpioStage;
    std::unique_ptr<QTemporaryFile> arStage;
    std::unique_ptr<QTemporaryFile> wadStage;
    std::vector<Entry> entries;
    std::vector<TarEntry> tarEntries;
    std::vector<CpioEntry> cpioEntries;
    std::vector<ArEntry> arEntries;
    std::vector<WadEntry> wadEntries;
    CpioFormat cpioFormat = CpioFormat::Newc;
    ArFormat arFormat = ArFormat::Gnu;
    WadFormat wadFormat = WadFormat::Pwad;
    quint16 zipMethod = 0;
    int bzip2Level = 9;
    int zstdLevel = 3;
    int lzmaLevel = 5;
    int unixCompressMaxBits = 16;
    bool unixCompressBlockMode = true;
    unsigned xzCheckId = XZ_CHECK_CRC64;
    qint64 cpioOpaqueOffset = 0;
    qint64 cpioOpaqueSize = 0;
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
    enum class Mode { None, ZipCreate, ZipUpdate, GzipCreate, Bzip2Create, ZstdCreate, XzCreate, LzmaAloneCreate, UnixCompressCreate, TarCreate, TarUpdate, CpioCreate,
                      CpioUpdate, ArCreate, ArUpdate, WadCreate, WadUpdate,
                      FatCreate, DiskUpdate } mode = Mode::None;
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
            writer->cpioStage.reset();
            writer->arStage.reset();
            writer->wadStage.reset();
            if (writer->diskUpdate) {
                writer->diskUpdate->abort();
                writer->diskUpdate.reset();
            }
            writer->entries.clear();
            writer->tarEntries.clear();
            writer->cpioEntries.clear();
            writer->arEntries.clear();
            writer->wadEntries.clear();
            writer->cpioFormat = CpioFormat::Newc;
            writer->cpioOpaqueOffset = 0;
            writer->cpioOpaqueSize = 0;
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
    writer->cpioStage.reset();
    writer->arStage.reset();
    writer->wadStage.reset();
    writer->entries.clear();
    writer->tarEntries.clear();
    writer->cpioEntries.clear();
    writer->arEntries.clear();
    writer->wadEntries.clear();
    writer->cpioFormat = CpioFormat::Newc;
    writer->cpioOpaqueOffset = 0;
    writer->cpioOpaqueSize = 0;
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

bool parseArNumber(const QByteArray& field, int base, quint64* value) {
    QByteArray text = field.trimmed();
    if (text.isEmpty()) { *value = 0; return true; }
    bool ok = false;
    const quint64 parsed = text.toULongLong(&ok, base);
    if (!ok) return false;
    *value = parsed;
    return true;
}

bool arSymbolName(const QByteArray& name) {
    return name == "/" || name == "/SYM64/" || name == "__.SYMDEF" ||
        name == "__.SYMDEF SORTED" || name == "__.SYMDEF_64" ||
        name == "__.SYMDEF_64 SORTED" || name == "__.GOSYMDEF";
}

peare_writer_status loadExistingAr(peare_writer_handle_s* writer, QFile* input) {
    if (!input || input->size() < 8 || !input->seek(0) || input->read(8) != "!<arch>\n")
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("The file is not a supported ar archive."), false);
    struct PendingArEntry { ArEntry entry; QByteArray field; };
    std::vector<PendingArEntry> pending;
    QByteArray nameTable;
    bool sawBsdNames = false;
    qint64 position = 8;
    while (position < input->size()) {
        QByteArray header;
        if (input->size() - position < 60 || !readAt(input, position, 60, &header) ||
            header.mid(58, 2) != "`\n")
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("The ar member header is truncated or invalid."), false);
        quint64 size = 0, timestamp = 0, uid = 0, gid = 0, mode = 0;
        if (!parseArNumber(header.mid(48, 10), 10, &size) ||
            !parseArNumber(header.mid(16, 12), 10, &timestamp) ||
            !parseArNumber(header.mid(28, 6), 10, &uid) ||
            !parseArNumber(header.mid(34, 6), 10, &gid) ||
            !parseArNumber(header.mid(40, 8), 8, &mode) ||
            size > quint64(input->size() - position - 60))
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("The ar member metadata is invalid."), false);
        QByteArray field = header.left(16).trimmed();
        qint64 dataOffset = position + 60;
        quint64 dataSize = size;
        if (field == "//") {
            if (size > 64u * 1024u * 1024u || !readAt(input, dataOffset, qint64(size), &nameTable))
                return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                            QStringLiteral("The GNU ar name table is invalid."), false);
        } else if (arSymbolName(field)) {
        } else if (field.startsWith("#1/")) {
            sawBsdNames = true;
            bool ok = false;
            const quint64 nameSize = field.mid(3).toULongLong(&ok, 10);
            if (!ok || nameSize == 0 || nameSize > size || nameSize > 65536)
                return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                            QStringLiteral("The BSD ar extended name is invalid."), false);
            QByteArray name;
            if (!readAt(input, dataOffset, qint64(nameSize), &name))
                return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                            QStringLiteral("The BSD ar extended name is truncated."), false);
            while (name.endsWith('\0')) name.chop(1);
            PendingArEntry item;
            item.field = name;
            item.entry.dataOffset = dataOffset + qint64(nameSize);
            item.entry.size = dataSize - nameSize;
            item.entry.timestamp = timestamp;
            item.entry.uid = quint32(uid);
            item.entry.gid = quint32(gid);
            item.entry.mode = quint32(mode);
            pending.push_back(std::move(item));
        } else if (field != "//") {
            PendingArEntry item;
            item.field = field;
            item.entry.dataOffset = dataOffset;
            item.entry.size = dataSize;
            item.entry.timestamp = timestamp;
            item.entry.uid = quint32(uid);
            item.entry.gid = quint32(gid);
            item.entry.mode = quint32(mode);
            pending.push_back(std::move(item));
        }
        const quint64 end = quint64(dataOffset) + size;
        if (end > quint64(input->size()) || (size & 1u && end == quint64(input->size())))
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("The ar member payload or padding is truncated."), false);
        position = qint64(end + (size & 1u));
    }
    writer->arEntries.clear();
    writer->names.clear();
    writer->arFormat = sawBsdNames ? ArFormat::Bsd : ArFormat::Gnu;
    for (PendingArEntry& item : pending) {
        QByteArray name = item.field;
        if (name.startsWith('/') && name.size() > 1) {
            bool ok = false;
            const quint64 offset = name.mid(1).toULongLong(&ok, 10);
            if (!ok || offset >= quint64(nameTable.size()))
                return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                            QStringLiteral("The GNU ar long-name reference is invalid."), false);
            int end = nameTable.indexOf('\n', int(offset));
            if (end < 0)
                return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                            QStringLiteral("The GNU ar long-name table is truncated."), false);
            name = nameTable.mid(int(offset), end - int(offset));
            if (name.endsWith('/')) name.chop(1);
        } else if (name.endsWith('/')) {
            name.chop(1);
        }
        if (arSymbolName(name)) continue;
        QString key = QString::fromUtf8(name.constData(), name.size());
        if (key.isEmpty() || key.toUtf8() != name || key.contains(QChar(0)) ||
            key.contains('/') || key.contains('\\') || writer->names.contains(key))
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("The ar member name is invalid or duplicated."), false);
        item.entry.key = key;
        item.entry.name = name;
        writer->names.insert(key);
        writer->arEntries.push_back(std::move(item.entry));
    }
    return PEARE_WRITER_OK;
}

bool wadName(const char* utf8, QByteArray* name) {
    if (!utf8 || !*utf8 || !name) return false;
    const QByteArray source(utf8);
    if (source.size() > 8) return false;
    for (unsigned char c : source) {
        if (c < 0x21 || c > 0x7e || c == '/' || c == '\\') return false;
    }
    *name = source.toUpper();
    return true;
}

peare_writer_status loadExistingWad(peare_writer_handle_s* writer, QFile* input) {
    if (!input || input->size() < 12) return PEARE_WRITER_INVALID_ARGUMENT;
    QByteArray header;
    if (!readAt(input, 0, 12, &header)) return PEARE_WRITER_INVALID_ARGUMENT;
    if (header.left(4) != "IWAD" && header.left(4) != "PWAD")
        return PEARE_WRITER_INVALID_ARGUMENT;
    const quint32 count = read32(header.constData() + 4);
    const quint32 directory = read32(header.constData() + 8);
    const quint64 directoryBytes = quint64(count) * kWadDirectoryEntrySize;
    if (directory < 12 || count > kMaxZipEntries || directory > quint64(input->size()) ||
        directoryBytes > quint64(input->size()) - directory)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("The WAD directory is invalid."), false);
    writer->wadEntries.clear();
    writer->wadFormat = header.left(4) == "IWAD" ? WadFormat::Iwad : WadFormat::Pwad;
    writer->wadStage.reset(new (std::nothrow) QTemporaryFile);
    if (!writer->wadStage || !writer->wadStage->open())
        return fail(writer, PEARE_WRITER_IO_ERROR,
                    writer->wadStage ? writer->wadStage->errorString()
                                     : QStringLiteral("Could not allocate WAD staging."), false);
    QByteArray record;
    for (quint32 index = 0; index < count; ++index) {
        if (!readAt(input, qint64(directory) + qint64(index) * 16, 16, &record))
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("The WAD directory is truncated."), false);
        const quint32 offset = read32(record.constData());
        const quint32 size = read32(record.constData() + 4);
        if (offset > quint64(input->size()) || size > quint64(input->size()) - offset)
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("A WAD lump is out of range."), false);
        WadEntry entry;
        entry.name = record.mid(8, 8);
        while (entry.name.endsWith('\0')) entry.name.chop(1);
        entry.size = size;
        entry.dataOffset = writer->wadStage->size();
        if (size) {
            if (!input->seek(offset))
                return fail(writer, PEARE_WRITER_IO_ERROR, input->errorString(), false);
            std::vector<char> buffer(kChunkSize);
            quint64 remaining = size;
            while (remaining) {
                const qint64 request = qint64(std::min<quint64>(remaining, buffer.size()));
                const qint64 read = input->read(buffer.data(), request);
                if (read != request || !writeAll(writer->wadStage.get(), buffer.data(), read))
                    return fail(writer, PEARE_WRITER_IO_ERROR,
                                QStringLiteral("Could not stage a WAD lump."), false);
                remaining -= quint64(read);
            }
        }
        writer->wadEntries.push_back(std::move(entry));
    }
    return PEARE_WRITER_OK;
}

peare_writer_status appendWadFile(peare_writer_handle_s* writer,
                                  const char* sourcePathUtf8,
                                  const char* entryNameUtf8) {
    QByteArray name;
    if (!wadName(entryNameUtf8, &name))
        return fail(writer, PEARE_WRITER_INVALID_ENTRY_NAME,
                    QStringLiteral("WAD lump names must contain 1 to 8 printable ASCII characters."),
                    false);
    QFile source(QString::fromUtf8(sourcePathUtf8));
    if (!source.open(QIODevice::ReadOnly) || source.size() < 0)
        return fail(writer, PEARE_WRITER_IO_ERROR, source.errorString(), false);
    const qint64 offset = writer->wadStage->size();
    if (quint64(offset) + quint64(source.size()) > 0xffffffffull)
        return fail(writer, PEARE_WRITER_UNSUPPORTED_LIMIT,
                    QStringLiteral("The WAD data exceeds its 32-bit offset limit."), false);
    if (!writer->wadStage->seek(offset))
        return fail(writer, PEARE_WRITER_IO_ERROR, writer->wadStage->errorString(), false);
    std::vector<char> buffer(kChunkSize);
    quint64 copied = 0;
    while (true) {
        const qint64 count = source.read(buffer.data(), qint64(buffer.size()));
        if (count < 0) return fail(writer, PEARE_WRITER_IO_ERROR, source.errorString(), false);
        if (!count) break;
        if (!writeAll(writer->wadStage.get(), buffer.data(), count))
            return fail(writer, PEARE_WRITER_IO_ERROR, writer->wadStage->errorString(), false);
        copied += quint64(count);
    }
    if (copied != quint64(source.size()))
        return fail(writer, PEARE_WRITER_IO_ERROR,
                    QStringLiteral("The source file changed while it was being added."), false);
    writer->wadEntries.push_back({name, copied, offset});
    writer->archiveDirty = true;
    return PEARE_WRITER_OK;
}

qint64 findWadEntry(const peare_writer_handle_s* writer, const char* entryNameUtf8) {
    if (!writer || !entryNameUtf8) return -1;
    QByteArray wanted(entryNameUtf8);
    if (wanted.startsWith("WAD#")) {
        bool ok = false;
        const qlonglong index = wanted.mid(4).toLongLong(&ok, 10);
        return ok && index >= 0 && quint64(index) < writer->wadEntries.size() ? index : -1;
    }
    qint64 match = -1;
    const QString wantedText = QString::fromUtf8(wanted);
    for (size_t i = 0; i < writer->wadEntries.size(); ++i) {
        if (QString::fromLatin1(writer->wadEntries[i].name) != wantedText) continue;
        if (match >= 0) return -2;
        match = qint64(i);
    }
    return match;
}

peare_writer_status replaceWadEntry(peare_writer_handle_s* writer, qint64 index,
                                     const char* sourcePathUtf8) {
    if (index < 0 || quint64(index) >= writer->wadEntries.size())
        return fail(writer, PEARE_WRITER_ENTRY_NOT_FOUND,
                    QStringLiteral("The WAD lump was not found."), false);
    QFile source(QString::fromUtf8(sourcePathUtf8));
    if (!source.open(QIODevice::ReadOnly) || source.size() < 0)
        return fail(writer, PEARE_WRITER_IO_ERROR, source.errorString(), false);
    const qint64 offset = writer->wadStage->size();
    if (quint64(offset) + quint64(source.size()) > 0xffffffffull)
        return fail(writer, PEARE_WRITER_UNSUPPORTED_LIMIT,
                    QStringLiteral("The WAD data exceeds its 32-bit offset limit."), false);
    if (!writer->wadStage->seek(offset))
        return fail(writer, PEARE_WRITER_IO_ERROR, writer->wadStage->errorString(), false);
    std::vector<char> buffer(kChunkSize);
    quint64 copied = 0;
    while (true) {
        const qint64 count = source.read(buffer.data(), qint64(buffer.size()));
        if (count < 0) return fail(writer, PEARE_WRITER_IO_ERROR, source.errorString(), false);
        if (!count) break;
        if (!writeAll(writer->wadStage.get(), buffer.data(), count))
            return fail(writer, PEARE_WRITER_IO_ERROR, writer->wadStage->errorString(), false);
        copied += quint64(count);
    }
    if (copied != quint64(source.size()))
        return fail(writer, PEARE_WRITER_IO_ERROR,
                    QStringLiteral("The source file changed while it was being read."), false);
    writer->wadEntries[size_t(index)].dataOffset = offset;
    writer->wadEntries[size_t(index)].size = copied;
    writer->archiveDirty = true;
    return PEARE_WRITER_OK;
}

peare_writer_status finishWad(peare_writer_handle_s* writer) {
    if (writer->mode == peare_writer_handle_s::Mode::WadUpdate && !writer->archiveDirty)
        return finishNoOpArchiveUpdate(writer);
    if (writer->wadEntries.size() > kMaxZipEntries || !writer->wadStage ||
        quint64(writer->wadStage->size()) + 12 > 0xffffffffull)
        return fail(writer, PEARE_WRITER_UNSUPPORTED_LIMIT,
                    QStringLiteral("The WAD cannot be represented with 32-bit directory offsets."));
    if (writer->mode == peare_writer_handle_s::Mode::WadUpdate) {
        writer->output.reset(new (std::nothrow) QSaveFile(writer->updatePath));
        if (!writer->output || !writer->output->open(QIODevice::WriteOnly))
            return fail(writer, PEARE_WRITER_IO_ERROR,
                        writer->output ? writer->output->errorString()
                                       : QStringLiteral("Could not allocate WAD output."));
    }
    const quint32 directoryOffset = quint32(12 + writer->wadStage->size());
    QByteArray header;
    header.append(writer->wadFormat == WadFormat::Iwad ? "IWAD" : "PWAD", 4);
    append32(&header, quint32(writer->wadEntries.size()));
    append32(&header, directoryOffset);
    if (!writeAll(writer->output.get(), header.constData(), header.size()) ||
        !writer->wadStage->seek(0))
        return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
    std::vector<char> buffer(kChunkSize);
    while (true) {
        const qint64 count = writer->wadStage->read(buffer.data(), qint64(buffer.size()));
        if (count < 0 || (count && !writeAll(writer->output.get(), buffer.data(), count)))
            return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
        if (!count) break;
    }
    for (const WadEntry& entry : writer->wadEntries) {
        const quint64 offset = 12 + quint64(entry.dataOffset);
        if (offset > 0xffffffffull || entry.size > 0xffffffffull || entry.name.size() > 8)
            return fail(writer, PEARE_WRITER_UNSUPPORTED_LIMIT,
                        QStringLiteral("A WAD lump exceeds the format limits."));
        QByteArray record;
        append32(&record, quint32(offset));
        append32(&record, quint32(entry.size));
        record.append(entry.name);
        record.append(QByteArray(8 - entry.name.size(), '\0'));
        if (!writeAll(writer->output.get(), record.constData(), record.size()))
            return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
    }
    if (!writer->output->commit())
        return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
    writer->output.reset();
    writer->wadStage.reset();
    writer->wadEntries.clear();
    writer->active = false;
    writer->updateMode = false;
    writer->archiveDirty = false;
    writer->mode = peare_writer_handle_s::Mode::None;
    writer->updatePath.clear();
    writer->error.clear();
    writer->errorUtf8.clear();
    return PEARE_WRITER_OK;
}

bool parseCpioHex(const char* bytes, quint32* value) {
    if (!bytes || !value) return false;
    quint32 result = 0;
    for (int i = 0; i < 8; ++i) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        quint32 digit = 0;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        result = (result << 4) | digit;
    }
    *value = result;
    return true;
}

bool parseCpioHexWidth(const char* bytes, int width, quint64* value) {
    if (!bytes || !value || width < 1 || width > 16) return false;
    quint64 result = 0;
    for (int i = 0; i < width; ++i) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        quint64 digit = 0;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        result = (result << 4) | digit;
    }
    *value = result;
    return true;
}

bool parseCpioOctal(const char* bytes, int width, quint64* value) {
    if (!bytes || !value || width < 1 || width > 22) return false;
    quint64 result = 0;
    bool digitSeen = false;
    for (int i = 0; i < width; ++i) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (!digitSeen && (c == ' ' || c == '\0')) continue;
        if (c < '0' || c > '7' || result > (std::numeric_limits<quint64>::max() >> 3))
            return false;
        result = (result << 3) | quint64(c - '0');
        digitSeen = true;
    }
    if (!digitSeen) return false;
    *value = result;
    return true;
}

QString cpioPathKey(const QByteArray& rawName, bool directory) {
    if (rawName.isEmpty() || rawName.contains('\0')) return {};
    QString path = QString::fromUtf8(rawName.constData(), rawName.size());
    if (path.toUtf8() != rawName) return {};
    while (path.startsWith(QStringLiteral("./"))) path.remove(0, 2);
    while (path.endsWith(QLatin1Char('/'))) path.chop(1);
    if (path == QStringLiteral(".") && directory) return path;
    QByteArray normalized;
    const QByteArray utf8 = path.toUtf8();
    if (!normalizedEntryName(utf8.constData(), false, &normalized)) return {};
    return QString::fromUtf8(normalized);
}

bool cpioByteSum(QIODevice* input, qint64 offset, quint64 size, quint32* sum) {
    if (!input || !sum || offset < 0 || offset > input->size() ||
        quint64(size) > quint64(input->size() - offset) || !input->seek(offset))
        return false;
    quint32 result = 0;
    std::vector<char> buffer(kChunkSize);
    quint64 remaining = size;
    while (remaining) {
        const qint64 request = qint64(std::min<quint64>(remaining, buffer.size()));
        const qint64 count = input->read(buffer.data(), request);
        if (count != request) return false;
        for (qint64 i = 0; i < count; ++i)
            result += static_cast<unsigned char>(buffer[size_t(i)]);
        remaining -= quint64(count);
    }
    *sum = result;
    return true;
}

bool cpioZeroTail(QFile* input, qint64 offset) {
    if (!input || offset < 0 || offset > input->size()) return false;
    while (offset < input->size()) {
        const qint64 request = qMin<qint64>(4096, input->size() - offset);
        if (!input->seek(offset)) return false;
        const QByteArray bytes = input->read(request);
        if (bytes.size() != request) return false;
        for (char byte : bytes)
            if (byte != 0) return false;
        offset += request;
    }
    return true;
}

peare_writer_status loadExistingCpio(peare_writer_handle_s* writer, QFile* input) {
    if (!writer || !input || input->size() < 26 || !input->seek(0))
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("The file is not a supported CPIO archive."), false);
    writer->cpioEntries.clear();
    writer->names.clear();
    writer->cpioFormat = CpioFormat::Newc;
    writer->cpioOpaqueOffset = 0;
    writer->cpioOpaqueSize = 0;
    qint64 offset = 0;
    bool sawTrailer = false;
    bool sawHeader = false;
    CpioFormat archiveFormat = CpioFormat::Newc;
    while (offset <= input->size() - 26) {
        QByteArray prefix;
        if (!readAt(input, offset, 6, &prefix)) break;
        CpioFormat format;
        qint64 headerSize = 0;
        bool binary = false;
        bool little = false;
        const QByteArray magic = prefix.left(6);
        if (magic == QByteArrayLiteral("070701")) {
            format = CpioFormat::Newc; headerSize = 110;
        } else if (magic == QByteArrayLiteral("070702")) {
            format = CpioFormat::Crc; headerSize = 110;
        } else if (magic == QByteArrayLiteral("070707")) {
            format = CpioFormat::Odc; headerSize = 76;
        } else if (magic == QByteArrayLiteral("070727")) {
            format = CpioFormat::AfioLarge; headerSize = 116;
        } else if (static_cast<uchar>(prefix.at(0)) == 0xc7 &&
                   static_cast<uchar>(prefix.at(1)) == 0x71) {
            format = CpioFormat::BinaryLe; headerSize = 26; binary = true; little = true;
        } else if (static_cast<uchar>(prefix.at(0)) == 0x71 &&
                   static_cast<uchar>(prefix.at(1)) == 0xc7) {
            format = CpioFormat::BinaryBe; headerSize = 26; binary = true;
        } else break;
        if (offset > input->size() - headerSize) break;
        QByteArray header;
        if (!readAt(input, offset, headerSize, &header)) break;

        quint64 inode = 0, mode = 0, uid = 0, gid = 0, links = 0;
        quint64 modified = 0, size = 0, devMajor = 0, devMinor = 0;
        quint64 rdevMajor = 0, rdevMinor = 0, legacyDevice = 0, legacyRdev = 0;
        quint64 nameSize = 0, checksum = 0;
        if (format == CpioFormat::Newc || format == CpioFormat::Crc) {
            quint32 fields[13] = {};
            bool valid = true;
            for (int i = 0; i < 13; ++i)
                valid = valid && parseCpioHex(header.constData() + 6 + i * 8, &fields[i]);
            if (!valid) break;
            inode = fields[0]; mode = fields[1]; uid = fields[2]; gid = fields[3];
            links = fields[4]; modified = fields[5]; size = fields[6];
            devMajor = fields[7]; devMinor = fields[8];
            rdevMajor = fields[9]; rdevMinor = fields[10];
            nameSize = fields[11]; checksum = fields[12];
            if (format == CpioFormat::Newc && checksum != 0) break;
        } else if (format == CpioFormat::Odc) {
            const int widths[] = {6, 6, 6, 6, 6, 6, 6, 11, 6, 11};
            quint64 fields[10] = {};
            bool valid = true;
            int cursor = 6;
            for (int i = 0; i < 10; ++i) {
                quint64 value = 0;
                valid = valid && parseCpioOctal(header.constData() + cursor, widths[i], &value);
                fields[i] = value;
                cursor += widths[i];
            }
            if (!valid) break;
            legacyDevice = fields[0]; inode = fields[1]; mode = fields[2];
            uid = fields[3]; gid = fields[4]; links = fields[5];
            legacyRdev = fields[6]; modified = fields[7]; nameSize = fields[8];
            size = fields[9];
        } else if (format == CpioFormat::AfioLarge) {
            if (header.at(30) != 'm' || header.at(85) != 'n' ||
                header.at(98) != 's' || header.at(115) != ':' ||
                !parseCpioHexWidth(header.constData() + 6, 8, &legacyDevice) ||
                !parseCpioHexWidth(header.constData() + 14, 16, &inode) ||
                !parseCpioOctal(header.constData() + 31, 6, &mode) ||
                !parseCpioHexWidth(header.constData() + 37, 8, &uid) ||
                !parseCpioHexWidth(header.constData() + 45, 8, &gid) ||
                !parseCpioHexWidth(header.constData() + 53, 8, &links) ||
                !parseCpioHexWidth(header.constData() + 61, 8, &legacyRdev) ||
                !parseCpioHexWidth(header.constData() + 69, 16, &modified) ||
                !parseCpioHexWidth(header.constData() + 86, 4, &nameSize) ||
                !parseCpioHexWidth(header.constData() + 90, 4, &checksum) ||
                !parseCpioHexWidth(header.constData() + 94, 4, &devMinor) ||
                !parseCpioHexWidth(header.constData() + 99, 16, &size)) break;
            if (checksum != 0 || devMinor != 0 ||
                modified > quint64(std::numeric_limits<qint64>::max()) ||
                size > quint64(std::numeric_limits<qint64>::max())) break;
        } else {
            auto get16 = [&](int at) -> quint16 {
                return little ? qFromLittleEndian<quint16>(header.constData() + at)
                              : qFromBigEndian<quint16>(header.constData() + at);
            };
            auto get32 = [&](int at) -> quint32 {
                return (quint32(get16(at)) << 16) | get16(at + 2);
            };
            legacyDevice = get16(2); inode = get16(4); mode = get16(6);
            uid = get16(8); gid = get16(10); links = get16(12);
            legacyRdev = get16(14); modified = get32(16);
            nameSize = get16(20); size = get32(22);
        }
        if (nameSize == 0 || nameSize > 1024 * 1024 ||
            size > quint64(std::numeric_limits<qint64>::max())) break;
        if (sawHeader && format != archiveFormat) {
            const bool afioPair =
                (format == CpioFormat::Odc && archiveFormat == CpioFormat::AfioLarge) ||
                (format == CpioFormat::AfioLarge && archiveFormat == CpioFormat::Odc);
            if (!afioPair) break;
            archiveFormat = CpioFormat::AfioLarge;
        } else if (!sawHeader) {
            archiveFormat = format;
        }
        sawHeader = true;
        const qint64 nameOffset = offset + headerSize;
        if (nameOffset > input->size() || nameSize > quint64(input->size() - nameOffset)) break;
        QByteArray name;
        if (!readAt(input, nameOffset, qint64(nameSize), &name) || name.back() != '\0' ||
            name.left(name.size() - 1).contains('\0'))
            break;
        name.chop(1);
        qint64 dataOffset = nameOffset + qint64(nameSize);
        if (format == CpioFormat::Newc || format == CpioFormat::Crc)
            dataOffset = (dataOffset + 3) & ~qint64(3);
        else if (binary)
            dataOffset = (dataOffset + 1) & ~qint64(1);
        if (dataOffset < nameOffset || size > quint64(input->size() - dataOffset))
            break;
        const quint64 dataEnd = quint64(dataOffset) + size;
        quint64 nextOffset = dataEnd;
        if (format == CpioFormat::Newc || format == CpioFormat::Crc)
            nextOffset = (dataEnd + 3) & ~quint64(3);
        else if (binary)
            nextOffset = (dataEnd + 1) & ~quint64(1);
        if (nextOffset < dataEnd || nextOffset > quint64(input->size())) break;
        quint32 byteSum = 0;
        if (format == CpioFormat::Crc) {
            if (!cpioByteSum(input, dataOffset, size, &byteSum)) break;
            const bool legacySymlinkCheck =
                (mode & 0170000u) == 0120000u && checksum == 0;
            if (byteSum != checksum && !legacySymlinkCheck) break;
        }

        if (name == QByteArrayLiteral("TRAILER!!!")) {
            sawTrailer = true;
            const qint64 opaqueStart = size ? dataOffset : qint64(nextOffset);
            if (size || !cpioZeroTail(input, opaqueStart)) {
                writer->cpioOpaqueOffset = opaqueStart;
                writer->cpioOpaqueSize = input->size() - opaqueStart;
            }
            offset = input->size();
            break;
        }
        const bool directory = (mode & 0170000u) == 0040000u;
        const QString key = cpioPathKey(name, directory);
        if (key.isEmpty() || writer->names.contains(key) || writer->cpioEntries.size() >= 1000000)
            break;
        CpioEntry entry;
        entry.key = key;
        entry.name = key.toUtf8();
        entry.inode = inode;
        entry.mode = quint32(mode);
        entry.uid = quint32(uid);
        entry.gid = quint32(gid);
        entry.links = quint32(links);
        entry.modified = modified;
        entry.size = size;
        entry.deviceMajor = quint32(devMajor);
        entry.deviceMinor = quint32(devMinor);
        entry.specialMajor = quint32(rdevMajor);
        entry.specialMinor = quint32(rdevMinor);
        entry.legacyDevice = quint32(legacyDevice);
        entry.legacySpecialDevice = quint32(legacyRdev);
        entry.dataOffset = dataOffset;
        writer->cpioEntries.push_back(std::move(entry));
        writer->names.insert(key);
        offset = qint64(nextOffset);
    }
    if (!sawTrailer || offset != input->size()) {
        writer->cpioEntries.clear();
        writer->names.clear();
        writer->cpioFormat = CpioFormat::Newc;
        writer->cpioOpaqueOffset = 0;
        writer->cpioOpaqueSize = 0;
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("The file is not a supported CPIO archive."), false);
    }
    writer->cpioFormat = archiveFormat;
    return PEARE_WRITER_OK;
}

bool cpioInsertPathValid(const peare_writer_handle_s* writer, const QString& key,
                         bool directory) {
    if (!writer || key.isEmpty() || writer->names.contains(key)) return false;
    for (const CpioEntry& entry : writer->cpioEntries) {
        if (!entry.key.isEmpty() && entry.key != QStringLiteral(".") &&
            key.startsWith(entry.key + QLatin1Char('/')) &&
            (entry.mode & 0170000u) != 0040000u)
            return false;
        if (!directory && entry.key.startsWith(key + QLatin1Char('/')))
            return false;
    }
    return true;
}

quint64 nextCpioInode(const peare_writer_handle_s* writer) {
    quint64 maximum = 0;
    if (writer)
        for (const CpioEntry& entry : writer->cpioEntries)
            maximum = qMax(maximum, entry.inode);
    const quint64 limit = !writer ? 0 :
        writer->cpioFormat == CpioFormat::BinaryLe ||
        writer->cpioFormat == CpioFormat::BinaryBe ? 0xffffu :
        writer->cpioFormat == CpioFormat::Odc ? 0777777u :
        writer->cpioFormat == CpioFormat::Newc || writer->cpioFormat == CpioFormat::Crc
            ? std::numeric_limits<quint32>::max()
            : std::numeric_limits<quint64>::max();
    return maximum >= limit ? 0 : maximum + 1;
}

bool appendCpioSource(peare_writer_handle_s* writer, const char* sourcePathUtf8,
                      CpioEntry* entry) {
    if (!writer || !writer->cpioStage || !sourcePathUtf8 || !*sourcePathUtf8 || !entry)
        return false;
    QFile source(QString::fromUtf8(sourcePathUtf8));
    if (!source.open(QIODevice::ReadOnly) || source.size() < 0)
        return false;
    QTemporaryFile* stage = writer->cpioStage.get();
    if (!stage->seek(stage->size())) return false;
    entry->dataOffset = stage->pos();
    std::vector<char> buffer(kChunkSize);
    quint64 copied = 0;
    while (true) {
        const qint64 count = source.read(buffer.data(), qint64(buffer.size()));
        if (count < 0) return false;
        if (count == 0) break;
        if (!writeAll(stage, buffer.data(), count)) return false;
        copied += quint64(count);
    }
    if (copied != quint64(source.size())) return false;
    entry->size = copied;
    return true;
}

void writeCpioHex(char* output, int width, quint64 value) {
    static const char digits[] = "0123456789abcdef";
    for (int i = width - 1; i >= 0; --i) {
        output[i] = digits[value & 15u];
        value >>= 4;
    }
}

bool writeCpioOctal(char* output, int width, quint64 value) {
    if (!output || width < 1) return false;
    for (int i = width - 1; i >= 0; --i) {
        output[i] = char('0' + (value & 7u));
        value >>= 3;
    }
    return value == 0;
}

void writeCpio16(char* output, quint16 value, bool little) {
    if (little) qToLittleEndian(value, reinterpret_cast<uchar*>(output));
    else qToBigEndian(value, reinterpret_cast<uchar*>(output));
}

void writeCpio32(char* output, quint32 value, bool little) {
    writeCpio16(output, quint16(value >> 16), little);
    writeCpio16(output + 2, quint16(value), little);
}

bool cpioEntryFits(const CpioEntry& entry, CpioFormat format) {
    const quint64 nameSize = quint64(entry.name.size()) + 1;
    const quint64 binaryLimit = std::numeric_limits<quint16>::max();
    const quint64 odcLimit = 0777777u;
    if (nameSize > 1024 * 1024 || nameSize > quint64(std::numeric_limits<qint64>::max()))
        return false;
    switch (format) {
    case CpioFormat::Newc:
    case CpioFormat::Crc:
        return entry.inode <= UINT32_MAX && entry.mode <= UINT32_MAX &&
            entry.uid <= UINT32_MAX && entry.gid <= UINT32_MAX &&
            entry.links <= UINT32_MAX && entry.modified <= UINT32_MAX &&
            entry.size <= UINT32_MAX && entry.deviceMajor <= UINT32_MAX &&
            entry.deviceMinor <= UINT32_MAX && entry.specialMajor <= UINT32_MAX &&
            entry.specialMinor <= UINT32_MAX && nameSize <= UINT32_MAX;
    case CpioFormat::Odc:
        return entry.legacyDevice <= odcLimit && entry.inode <= odcLimit &&
            entry.mode <= odcLimit && entry.uid <= odcLimit && entry.gid <= odcLimit &&
            entry.links <= odcLimit && entry.legacySpecialDevice <= odcLimit &&
            entry.modified <= 077777777777ull && nameSize <= odcLimit &&
            entry.size <= 077777777777ull;
    case CpioFormat::AfioLarge:
        return entry.legacyDevice <= UINT32_MAX && entry.inode <= UINT64_MAX &&
            entry.mode <= odcLimit && entry.uid <= UINT32_MAX && entry.gid <= UINT32_MAX &&
            entry.links <= UINT32_MAX && entry.legacySpecialDevice <= UINT32_MAX &&
            entry.modified <= quint64(std::numeric_limits<qint64>::max()) &&
            nameSize <= 0xffffu && entry.size <= quint64(std::numeric_limits<qint64>::max());
    case CpioFormat::BinaryLe:
    case CpioFormat::BinaryBe:
        return entry.legacyDevice <= binaryLimit && entry.inode <= binaryLimit &&
            entry.mode <= binaryLimit && entry.uid <= binaryLimit && entry.gid <= binaryLimit &&
            entry.links <= binaryLimit && entry.legacySpecialDevice <= binaryLimit &&
            entry.modified <= UINT32_MAX && nameSize <= binaryLimit && entry.size <= UINT32_MAX;
    }
    return false;
}

bool writeCpioRecord(QIODevice* output, QTemporaryFile* stage, const CpioEntry& entry,
                     CpioFormat format) {
    if (!output || !stage || entry.name.size() < 0 || !cpioEntryFits(entry, format))
        return false;
    quint32 checksum = 0;
    const bool crc = format == CpioFormat::Crc;
    if (crc && entry.size &&
        !cpioByteSum(stage, entry.dataOffset, entry.size, &checksum)) return false;
    const quint64 nameSize = quint64(entry.name.size()) + 1;
    bool binary = format == CpioFormat::BinaryLe || format == CpioFormat::BinaryBe;
    bool little = format == CpioFormat::BinaryLe;
    QByteArray header;
    if (format == CpioFormat::Newc || format == CpioFormat::Crc) {
        header.fill('\0', 110);
        std::memcpy(header.data(), crc ? "070702" : "070701", 6);
        const quint64 fields[] = {entry.inode, entry.mode, entry.uid, entry.gid, entry.links,
            entry.modified, entry.size, entry.deviceMajor, entry.deviceMinor,
            entry.specialMajor, entry.specialMinor, nameSize, checksum};
        for (int i = 0; i < 13; ++i)
            writeCpioHex(header.data() + 6 + i * 8, 8, fields[i]);
    } else if (format == CpioFormat::Odc) {
        header.fill('0', 76);
        std::memcpy(header.data(), "070707", 6);
        const quint64 fields[] = {entry.legacyDevice, entry.inode, entry.mode, entry.uid,
            entry.gid, entry.links, entry.legacySpecialDevice, entry.modified,
            nameSize, entry.size};
        const int widths[] = {6, 6, 6, 6, 6, 6, 6, 11, 6, 11};
        int cursor = 6;
        for (int i = 0; i < 10; ++i) {
            if (!writeCpioOctal(header.data() + cursor, widths[i], fields[i])) return false;
            cursor += widths[i];
        }
    } else if (format == CpioFormat::AfioLarge) {
        header.fill('0', 116);
        std::memcpy(header.data(), "070727", 6);
        writeCpioHex(header.data() + 6, 8, entry.legacyDevice);
        writeCpioHex(header.data() + 14, 16, entry.inode);
        header[30] = 'm';
        if (!writeCpioOctal(header.data() + 31, 6, entry.mode)) return false;
        writeCpioHex(header.data() + 37, 8, entry.uid);
        writeCpioHex(header.data() + 45, 8, entry.gid);
        writeCpioHex(header.data() + 53, 8, entry.links);
        writeCpioHex(header.data() + 61, 8, entry.legacySpecialDevice);
        writeCpioHex(header.data() + 69, 16, entry.modified);
        header[85] = 'n';
        writeCpioHex(header.data() + 86, 4, nameSize);
        writeCpioHex(header.data() + 90, 4, 0);
        writeCpioHex(header.data() + 94, 4, 0);
        header[98] = 's';
        writeCpioHex(header.data() + 99, 16, entry.size);
        header[115] = ':';
    } else {
        header.fill('\0', 26);
        writeCpio16(header.data(), 070707, little);
        writeCpio16(header.data() + 2, quint16(entry.legacyDevice), little);
        writeCpio16(header.data() + 4, quint16(entry.inode), little);
        writeCpio16(header.data() + 6, quint16(entry.mode), little);
        writeCpio16(header.data() + 8, quint16(entry.uid), little);
        writeCpio16(header.data() + 10, quint16(entry.gid), little);
        writeCpio16(header.data() + 12, quint16(entry.links), little);
        writeCpio16(header.data() + 14, quint16(entry.legacySpecialDevice), little);
        writeCpio32(header.data() + 16, quint32(entry.modified), little);
        writeCpio16(header.data() + 20, quint16(nameSize), little);
        writeCpio32(header.data() + 22, quint32(entry.size), little);
    }
    if (!writeAll(output, header.constData(), header.size()) ||
        !writeAll(output, entry.name.constData(), entry.name.size()) ||
        !writeAll(output, "\0", 1)) return false;
    const int alignment = format == CpioFormat::Newc || format == CpioFormat::Crc ? 4 :
                          binary ? 2 : 1;
    const qint64 namePadding = (alignment - (output->pos() % alignment)) % alignment;
    const QByteArray zeros(int(namePadding), '\0');
    if (namePadding && !writeAll(output, zeros.constData(), zeros.size())) return false;
    if (entry.size) {
        if (entry.dataOffset < 0 || !stage->seek(entry.dataOffset)) return false;
        std::vector<char> buffer(kChunkSize);
        quint64 remaining = entry.size;
        while (remaining) {
            const qint64 request = qint64(std::min<quint64>(remaining, buffer.size()));
            const qint64 count = stage->read(buffer.data(), request);
            if (count != request || !writeAll(output, buffer.data(), count)) return false;
            remaining -= quint64(count);
        }
    }
    const qint64 dataPadding = (alignment - (output->pos() % alignment)) % alignment;
    const QByteArray dataZeros(int(dataPadding), '\0');
    return !dataPadding || writeAll(output, dataZeros.constData(), dataZeros.size());
}

peare_writer_status writeCpioFile(peare_writer_handle_s* writer,
                                  const char* sourcePathUtf8, const char* entryNameUtf8) {
    QByteArray encoded;
    if (!normalizedEntryName(entryNameUtf8, false, &encoded))
        return fail(writer, PEARE_WRITER_INVALID_ENTRY_NAME,
                    QStringLiteral("The CPIO path is invalid or unsafe."), false);
    const QString key = QString::fromUtf8(encoded);
    if (!cpioInsertPathValid(writer, key, false))
        return fail(writer, PEARE_WRITER_DUPLICATE_ENTRY,
                    QStringLiteral("The CPIO path conflicts with an existing entry."), false);
    CpioEntry entry;
    entry.key = key;
    entry.name = encoded;
    entry.inode = nextCpioInode(writer);
    if (!entry.inode)
        return fail(writer, PEARE_WRITER_UNSUPPORTED_LIMIT,
                    QStringLiteral("The CPIO inode number limit was reached."), false);
    entry.mode = 0100644u;
    entry.links = 1;
    if (!appendCpioSource(writer, sourcePathUtf8, &entry))
        return fail(writer, PEARE_WRITER_IO_ERROR,
                    QStringLiteral("Could not stage the CPIO file payload."), false);
    writer->cpioEntries.push_back(std::move(entry));
    writer->names.insert(key);
    if (writer->mode == peare_writer_handle_s::Mode::CpioUpdate)
        writer->archiveDirty = true;
    return PEARE_WRITER_OK;
}

peare_writer_status writeCpioDirectory(peare_writer_handle_s* writer,
                                      const char* entryNameUtf8) {
    QByteArray encoded;
    if (!normalizedEntryName(entryNameUtf8, false, &encoded))
        return fail(writer, PEARE_WRITER_INVALID_ENTRY_NAME,
                    QStringLiteral("The CPIO directory path is invalid or unsafe."), false);
    const QString key = QString::fromUtf8(encoded);
    if (!cpioInsertPathValid(writer, key, true))
        return fail(writer, PEARE_WRITER_DUPLICATE_ENTRY,
                    QStringLiteral("The CPIO directory path conflicts with an entry."), false);
    CpioEntry entry;
    entry.key = key;
    entry.name = encoded;
    entry.inode = nextCpioInode(writer);
    if (!entry.inode)
        return fail(writer, PEARE_WRITER_UNSUPPORTED_LIMIT,
                    QStringLiteral("The CPIO inode number limit was reached."), false);
    entry.mode = 0040755u;
    entry.links = 2;
    writer->cpioEntries.push_back(std::move(entry));
    writer->names.insert(key);
    if (writer->mode == peare_writer_handle_s::Mode::CpioUpdate)
        writer->archiveDirty = true;
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

bool arFlatName(const char* utf8, QByteArray* name) {
    if (!utf8 || !*utf8 || !name) return false;
    const QString value = QString::fromUtf8(utf8);
    if (value.isEmpty() || value.toUtf8() != QByteArray(utf8) ||
        value.contains(QChar(0)) || value.contains('/') || value.contains('\\') ||
        value == QStringLiteral(".") || value == QStringLiteral("..")) return false;
    const QByteArray encoded = value.toUtf8();
    if (encoded.size() > 65536 || encoded.contains('\n') || encoded.contains('\r')) return false;
    *name = encoded;
    return true;
}

peare_writer_status writeArFile(peare_writer_handle_s* writer,
                                const char* sourcePathUtf8, const char* entryNameUtf8) {
    QByteArray name;
    if (!writer->arStage || !arFlatName(entryNameUtf8, &name))
        return fail(writer, PEARE_WRITER_INVALID_ENTRY_NAME,
                    QStringLiteral("The ar member name must be a single safe filename."), false);
    const QString key = QString::fromUtf8(name);
    if (writer->names.contains(key))
        return fail(writer, PEARE_WRITER_DUPLICATE_ENTRY,
                    QStringLiteral("The ar archive already contains this member."), false);
    QFile source(QString::fromUtf8(sourcePathUtf8 ? sourcePathUtf8 : ""));
    if (!sourcePathUtf8 || !*sourcePathUtf8 || !source.open(QIODevice::ReadOnly) ||
        source.size() < 0)
        return fail(writer, PEARE_WRITER_IO_ERROR,
                    source.errorString().isEmpty() ? QStringLiteral("Could not open the source file.")
                                                   : source.errorString(), false);
    ArEntry entry;
    entry.key = key;
    entry.name = name;
    entry.timestamp = quint64(QDateTime::currentSecsSinceEpoch());
    entry.dataOffset = writer->arStage->size();
    entry.size = quint64(source.size());
    if (!writer->arStage->seek(entry.dataOffset))
        return fail(writer, PEARE_WRITER_IO_ERROR, writer->arStage->errorString());
    std::vector<char> buffer(kChunkSize);
    quint64 copied = 0;
    while (true) {
        const qint64 count = source.read(buffer.data(), qint64(buffer.size()));
        if (count < 0) return fail(writer, PEARE_WRITER_IO_ERROR, source.errorString());
        if (!count) break;
        if (!writeAll(writer->arStage.get(), buffer.data(), count))
            return fail(writer, PEARE_WRITER_IO_ERROR, writer->arStage->errorString());
        copied += quint64(count);
    }
    if (copied != entry.size)
        return fail(writer, PEARE_WRITER_IO_ERROR,
                    QStringLiteral("The source file changed while it was being archived."));
    writer->arEntries.push_back(std::move(entry));
    writer->names.insert(key);
    if (writer->mode == peare_writer_handle_s::Mode::ArUpdate)
        writer->archiveDirty = true;
    return PEARE_WRITER_OK;
}

bool arWriteField(char* destination, int width, quint64 value, int base) {
    QByteArray text = QByteArray::number(value, base);
    if (text.size() > width) return false;
    std::memset(destination, ' ', size_t(width));
    std::memcpy(destination, text.constData(), size_t(text.size()));
    return true;
}

bool arWriteRecord(QIODevice* output, QIODevice* stage, const ArEntry& entry,
                   ArFormat format, const QMap<QString, quint64>& gnuNameOffsets) {
    if (!output || !stage || entry.dataOffset < 0 ||
        entry.dataOffset > stage->size() || entry.size > quint64(stage->size() - entry.dataOffset))
        return false;
    QByteArray field;
    QByteArray prefix;
    if (format == ArFormat::Bsd &&
        (entry.name.size() > 15 || entry.name.contains(' '))) {
        prefix = QByteArrayLiteral("#1/") + QByteArray::number(entry.name.size());
    } else if (entry.name.size() <= 15 && !entry.name.contains(' ')) {
        field = entry.name + '/';
    } else if (format == ArFormat::Gnu) {
        const auto offset = gnuNameOffsets.constFind(entry.key);
        if (offset == gnuNameOffsets.constEnd()) return false;
        field = '/' + QByteArray::number(offset.value());
    } else {
        return false;
    }
    if (format == ArFormat::Bsd && !prefix.isEmpty()) field = prefix;
    if (field.size() > 16) return false;
    const quint64 storedSize = entry.size + quint64(prefix.isEmpty() ? 0 : entry.name.size());
    if (storedSize > 9999999999ull || entry.timestamp > 999999999999ull ||
        entry.uid > 999999u || entry.gid > 999999u || entry.mode > 077777777u)
        return false;
    QByteArray header(60, ' ');
    std::memcpy(header.data(), field.constData(), size_t(field.size()));
    if (!arWriteField(header.data() + 16, 12, entry.timestamp, 10) ||
        !arWriteField(header.data() + 28, 6, entry.uid, 10) ||
        !arWriteField(header.data() + 34, 6, entry.gid, 10) ||
        !arWriteField(header.data() + 40, 8, entry.mode, 8) ||
        !arWriteField(header.data() + 48, 10, storedSize, 10)) return false;
    header[58] = '`';
    header[59] = '\n';
    if (!writeAll(output, header.constData(), header.size())) return false;
    if (!prefix.isEmpty() && !writeAll(output, entry.name.constData(), entry.name.size())) return false;
    if (!stage->seek(entry.dataOffset)) return false;
    std::vector<char> buffer(kChunkSize);
    quint64 remaining = entry.size;
    while (remaining) {
        const qint64 request = qint64(std::min<quint64>(remaining, buffer.size()));
        const qint64 count = stage->read(buffer.data(), request);
        if (count != request || !writeAll(output, buffer.data(), count)) return false;
        remaining -= quint64(count);
    }
    if (storedSize & 1u) return writeAll(output, "\n", 1);
    return true;
}

peare_writer_status finishAr(peare_writer_handle_s* writer) {
    if (writer->mode == peare_writer_handle_s::Mode::ArUpdate && !writer->archiveDirty)
        return finishNoOpArchiveUpdate(writer);
    if (writer->mode == peare_writer_handle_s::Mode::ArUpdate) {
        writer->output.reset(new (std::nothrow) QSaveFile(writer->updatePath));
        if (!writer->output || !writer->output->open(QIODevice::WriteOnly))
            return fail(writer, PEARE_WRITER_IO_ERROR,
                        writer->output ? writer->output->errorString()
                                       : QStringLiteral("Could not allocate ar output."));
    }
    QMap<QString, quint64> gnuNameOffsets;
    quint64 nameTableSize = 0;
    const qint64 nameTableOffset = writer->arStage->size();
    if (writer->arFormat == ArFormat::Gnu) {
        for (const ArEntry& entry : writer->arEntries) {
            if (entry.name.size() <= 15 && !entry.name.contains(' ')) continue;
            gnuNameOffsets.insert(entry.key, nameTableSize);
            nameTableSize += quint64(entry.name.size()) + 2;
            if (nameTableSize > 9999999999ull)
                return fail(writer, PEARE_WRITER_UNSUPPORTED_LIMIT,
                            QStringLiteral("The GNU ar name table exceeds the format limit."));
        }
        if (nameTableSize) {
            if (!writer->arStage->seek(nameTableOffset))
                return fail(writer, PEARE_WRITER_IO_ERROR, writer->arStage->errorString());
            for (const ArEntry& entry : writer->arEntries) {
                if (entry.name.size() <= 15 && !entry.name.contains(' ')) continue;
                if (!writeAll(writer->arStage.get(), entry.name.constData(), entry.name.size()) ||
                    !writeAll(writer->arStage.get(), "/\n", 2))
                    return fail(writer, PEARE_WRITER_IO_ERROR, writer->arStage->errorString());
            }
        }
    }
    if (!writeAll(writer->output.get(), "!<arch>\n", 8))
        return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
    if (nameTableSize) {
            QByteArray header(60, ' ');
            std::memcpy(header.data(), "//", 2);
            if (!arWriteField(header.data() + 48, 10, nameTableSize, 10))
                return fail(writer, PEARE_WRITER_UNSUPPORTED_LIMIT,
                            QStringLiteral("The GNU ar name table exceeds the format limit."));
            header[58] = '`';
            header[59] = '\n';
            if (!writeAll(writer->output.get(), header.constData(), header.size()) ||
                !writer->arStage->seek(nameTableOffset))
                return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
            std::vector<char> buffer(kChunkSize);
            quint64 remaining = nameTableSize;
            while (remaining) {
                const qint64 request = qint64(std::min<quint64>(remaining, buffer.size()));
                const qint64 count = writer->arStage->read(buffer.data(), request);
                if (count != request || !writeAll(writer->output.get(), buffer.data(), count))
                    return fail(writer, PEARE_WRITER_IO_ERROR,
                                QStringLiteral("Could not write the GNU ar name table."));
                remaining -= quint64(count);
            }
            if (nameTableSize & 1u)
                if (!writeAll(writer->output.get(), "\n", 1))
                    return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
    }
    for (const ArEntry& entry : writer->arEntries)
        if (!arWriteRecord(writer->output.get(), writer->arStage.get(), entry,
                           writer->arFormat, gnuNameOffsets))
            return fail(writer, PEARE_WRITER_UNSUPPORTED_LIMIT,
                        QStringLiteral("An ar member cannot be represented in the selected variant."));
    if (!writer->output->commit())
        return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
    writer->output.reset();
    writer->arStage.reset();
    writer->arEntries.clear();
    writer->names.clear();
    writer->active = false;
    writer->updateMode = false;
    writer->archiveDirty = false;
    writer->mode = peare_writer_handle_s::Mode::None;
    writer->updatePath.clear();
    writer->error.clear();
    writer->errorUtf8.clear();
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
    const quint16 versionNeeded = (entry.method >= 2 && entry.method <= 5) ? 10
        : entry.method == 9 ? 21
        : entry.method == 96 ? 20
        : (entry.method == 14 || entry.method == 93 ||
                                   entry.method == 95) ? 63
        : entry.method == 12 ? 46
        : entry.zip64Sizes ? 45 : 20;
    append16(&header, versionNeeded);
    append16(&header, quint16(entry.flags | (descriptor ? 0x0008 : 0)));
    append16(&header, entry.method);
    append16(&header, 0); // DOS time
    append16(&header, 0x0021); // 1980-01-01
    append32(&header, descriptor ? 0 : entry.crc);
    append32(&header, entry.zip64Sizes ? 0xffffffffu : (descriptor ? 0 : quint32(entry.compressedSize)));
    append32(&header, entry.zip64Sizes ? 0xffffffffu : (descriptor ? 0 : quint32(entry.size)));
    append16(&header, quint16(entry.name.size()));
    append16(&header, entry.zip64Sizes ? 20 : 0);
    header += entry.name;
    if (entry.zip64Sizes) {
        append16(&header, 1);
        append16(&header, 16);
        append64(&header, descriptor ? 0 : entry.compressedSize);
        append64(&header, descriptor ? 0 : entry.size);
    }
    return writeAll(output, header.constData(), header.size());
}

template <typename Read>
bool writeZipPayload(QIODevice* output, quint16 method, Read read,
                     quint32* crc, quint64* inputSize, quint64* outputSize,
                     QString* error) {
    std::vector<char> input(kChunkSize);
    std::vector<char> encoded(kChunkSize);
    quint32 checksum = 0xffffffffu;
    quint64 uncompressed = 0;
    quint64 compressed = 0;
    mz_stream stream = {};
    bz_stream bzipStream = {};
    const bool deflate = method == 8 || method == 9;
    const bool bzip2 = method == 12;
    if (deflate && mz_deflateInit2(&stream, 6, MZ_DEFLATED,
                                   -MZ_DEFAULT_WINDOW_BITS, 8,
                                   MZ_DEFAULT_STRATEGY) != MZ_OK) {
        if (error) *error = QStringLiteral("Could not initialize ZIP Deflate.");
        return false;
    }
    if (bzip2 && BZ2_bzCompressInit(&bzipStream, 9, 0, 30) != BZ_OK) {
        if (error) *error = QStringLiteral("Could not initialize ZIP BZip2.");
        return false;
    }
    const auto finish = [&]() {
        if (deflate) mz_deflateEnd(&stream);
        if (bzip2) BZ2_bzCompressEnd(&bzipStream);
    };
    while (true) {
        const qint64 count = read(input.data(), qint64(input.size()));
        if (count < 0) {
            if (error && error->isEmpty()) *error = QStringLiteral("Could not read ZIP input.");
            finish();
            return false;
        }
        if (count == 0) break;
        uncompressed += quint64(count);
        checksum = crc32Update(checksum, input.data(), size_t(count));
        if (!deflate && !bzip2) {
            if (!writeAll(output, input.data(), count)) {
                if (error) *error = output->errorString();
                finish();
                return false;
            }
            compressed += quint64(count);
            continue;
        }
        if (bzip2) {
            bzipStream.next_in = input.data();
            bzipStream.avail_in = (unsigned int)count;
            while (bzipStream.avail_in) {
                bzipStream.next_out = encoded.data();
                bzipStream.avail_out = (unsigned int)encoded.size();
                const int status = BZ2_bzCompress(&bzipStream, BZ_RUN);
                const qint64 produced = qint64(encoded.size() - bzipStream.avail_out);
                if (status != BZ_RUN_OK ||
                    (produced && !writeAll(output, encoded.data(), produced))) {
                    if (error) *error = status == BZ_RUN_OK ? output->errorString()
                        : QStringLiteral("ZIP BZip2 failed.");
                    finish();
                    return false;
                }
                compressed += quint64(produced);
            }
            continue;
        }
        stream.next_in = reinterpret_cast<const unsigned char*>(input.data());
        stream.avail_in = mz_uint(count);
        while (stream.avail_in) {
            stream.next_out = reinterpret_cast<unsigned char*>(encoded.data());
            stream.avail_out = mz_uint(encoded.size());
            const int status = mz_deflate(&stream, MZ_NO_FLUSH);
            const qint64 produced = qint64(encoded.size() - stream.avail_out);
            if (status != MZ_OK || (produced && !writeAll(output, encoded.data(), produced))) {
                if (error) *error = status == MZ_OK ? output->errorString()
                                                    : QStringLiteral("ZIP Deflate failed.");
                finish();
                return false;
            }
            compressed += quint64(produced);
        }
    }
    if (deflate) {
        int status = MZ_OK;
        while (status != MZ_STREAM_END) {
            stream.next_out = reinterpret_cast<unsigned char*>(encoded.data());
            stream.avail_out = mz_uint(encoded.size());
            status = mz_deflate(&stream, MZ_FINISH);
            const qint64 produced = qint64(encoded.size() - stream.avail_out);
            if ((status != MZ_OK && status != MZ_STREAM_END) ||
                (produced && !writeAll(output, encoded.data(), produced))) {
                if (error) *error = status == MZ_OK || status == MZ_STREAM_END
                    ? output->errorString() : QStringLiteral("ZIP Deflate finalization failed.");
                finish();
                return false;
            }
            compressed += quint64(produced);
        }
    }
    if (bzip2) {
        int status = BZ_RUN_OK;
        while (status != BZ_STREAM_END) {
            bzipStream.next_in = nullptr;
            bzipStream.avail_in = 0;
            bzipStream.next_out = encoded.data();
            bzipStream.avail_out = (unsigned int)encoded.size();
            status = BZ2_bzCompress(&bzipStream, BZ_FINISH);
            const qint64 produced = qint64(encoded.size() - bzipStream.avail_out);
            if ((status != BZ_FINISH_OK && status != BZ_STREAM_END) ||
                (produced && !writeAll(output, encoded.data(), produced))) {
                if (error) *error = status == BZ_FINISH_OK || status == BZ_STREAM_END
                    ? output->errorString() : QStringLiteral("ZIP BZip2 finalization failed.");
                finish();
                return false;
            }
            compressed += quint64(produced);
        }
    }
    finish();
    *crc = checksum ^ 0xffffffffu;
    *inputSize = uncompressed;
    *outputSize = compressed;
    return true;
}

peare_writer_status writeGzipMember(peare_writer_handle_s* writer,
                                    const char* sourcePathUtf8,
                                    const char* entryNameUtf8) {
    if (!sourcePathUtf8 || !*sourcePathUtf8 || !entryNameUtf8 || !*entryNameUtf8)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("A GZIP source and member name are required."));
    QByteArray safeName;
    if (!normalizedEntryName(entryNameUtf8, false, &safeName))
        return fail(writer, PEARE_WRITER_INVALID_ENTRY_NAME,
                    QStringLiteral("The GZIP member name is invalid or unsafe."));
    const QString requestedName = QFileInfo(QString::fromUtf8(safeName)).fileName();
    const QByteArray memberName = requestedName.toLatin1();
    const bool hasLatin1Name = !memberName.isEmpty() &&
        QString::fromLatin1(memberName) == requestedName &&
        memberName.size() <= 0xffff && !memberName.contains('\0');
    if (requestedName.isEmpty())
        return fail(writer, PEARE_WRITER_INVALID_ENTRY_NAME,
                    QStringLiteral("The GZIP member name cannot be represented."));

    QFile input(QString::fromUtf8(sourcePathUtf8));
    if (!input.open(QIODevice::ReadOnly))
        return fail(writer, PEARE_WRITER_IO_ERROR, input.errorString());
    QByteArray header;
    header.append(char(0x1f));
    header.append(char(0x8b));
    header.append(char(8));
    header.append(char(hasLatin1Name ? 8 : 0));
    append32(&header, 0);
    header.append(char(0));
    header.append(char(255));
    if (hasLatin1Name) {
        header += memberName;
        header.append(char(0));
    }
    if (!writeAll(writer->output.get(), header.constData(), header.size()))
        return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());

    std::vector<char> inputBuffer(kChunkSize);
    std::vector<char> outputBuffer(kChunkSize);
    mz_stream stream = {};
    if (mz_deflateInit2(&stream, 6, MZ_DEFLATED, -MZ_DEFAULT_WINDOW_BITS, 8,
                        MZ_DEFAULT_STRATEGY) != MZ_OK)
        return fail(writer, PEARE_WRITER_INTERNAL_ERROR,
                    QStringLiteral("Could not initialize the GZIP Deflate encoder."));

    quint32 checksum = 0xffffffffu;
    quint64 inputSize = 0;
    bool ok = true;
    QString error;
    while (true) {
        const qint64 count = input.read(inputBuffer.data(), qint64(inputBuffer.size()));
        if (count < 0) {
            error = input.errorString();
            ok = false;
            break;
        }
        if (count == 0) break;
        checksum = crc32Update(checksum, inputBuffer.data(), size_t(count));
        inputSize += quint64(count);
        stream.next_in = reinterpret_cast<const unsigned char*>(inputBuffer.data());
        stream.avail_in = mz_uint(count);
        while (stream.avail_in) {
            stream.next_out = reinterpret_cast<unsigned char*>(outputBuffer.data());
            stream.avail_out = mz_uint(outputBuffer.size());
            const int status = mz_deflate(&stream, MZ_NO_FLUSH);
            const qint64 produced = qint64(outputBuffer.size() - stream.avail_out);
            if (status != MZ_OK ||
                (produced && !writeAll(writer->output.get(), outputBuffer.data(), produced))) {
                error = status == MZ_OK ? writer->output->errorString()
                    : QStringLiteral("GZIP Deflate failed.");
                ok = false;
                break;
            }
        }
        if (!ok) break;
    }
    if (ok && input.error() != QFileDevice::NoError) {
        error = input.errorString();
        ok = false;
    }
    if (ok) {
        int status = MZ_OK;
        while (status != MZ_STREAM_END) {
            stream.next_out = reinterpret_cast<unsigned char*>(outputBuffer.data());
            stream.avail_out = mz_uint(outputBuffer.size());
            status = mz_deflate(&stream, MZ_FINISH);
            const qint64 produced = qint64(outputBuffer.size() - stream.avail_out);
            if ((status != MZ_OK && status != MZ_STREAM_END) ||
                (produced && !writeAll(writer->output.get(), outputBuffer.data(), produced))) {
                error = status == MZ_OK || status == MZ_STREAM_END
                    ? writer->output->errorString()
                    : QStringLiteral("GZIP Deflate finalization failed.");
                ok = false;
                break;
            }
        }
    }
    mz_deflateEnd(&stream);
    if (!ok) return fail(writer, PEARE_WRITER_IO_ERROR, error);

    QByteArray trailer;
    append32(&trailer, checksum ^ 0xffffffffu);
    append32(&trailer, quint32(inputSize));
    if (!writeAll(writer->output.get(), trailer.constData(), trailer.size()))
        return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
    return PEARE_WRITER_OK;
}

peare_writer_status writeBzip2Member(peare_writer_handle_s* writer,
                                     const char* sourcePathUtf8,
                                     const char* entryNameUtf8) {
    if (!sourcePathUtf8 || !*sourcePathUtf8 || !entryNameUtf8 || !*entryNameUtf8)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("A BZIP2 source and member name are required."));
    QByteArray safeName;
    if (!normalizedEntryName(entryNameUtf8, false, &safeName))
        return fail(writer, PEARE_WRITER_INVALID_ENTRY_NAME,
                    QStringLiteral("The BZIP2 member name is invalid or unsafe."));

    QFile input(QString::fromUtf8(sourcePathUtf8));
    if (!input.open(QIODevice::ReadOnly))
        return fail(writer, PEARE_WRITER_IO_ERROR, input.errorString());
    std::vector<char> inputBuffer(kChunkSize);
    std::vector<char> outputBuffer(kChunkSize);
    bz_stream stream = {};
    if (BZ2_bzCompressInit(&stream, writer->bzip2Level, 0, 30) != BZ_OK)
        return fail(writer, PEARE_WRITER_INTERNAL_ERROR,
                    QStringLiteral("Could not initialize the BZIP2 encoder."));

    bool ok = true;
    QString error;
    while (ok) {
        const qint64 count = input.read(inputBuffer.data(), qint64(inputBuffer.size()));
        if (count < 0) {
            error = input.errorString();
            ok = false;
            break;
        }
        if (count == 0) break;
        stream.next_in = inputBuffer.data();
        stream.avail_in = static_cast<unsigned int>(count);
        do {
            stream.next_out = outputBuffer.data();
            stream.avail_out = static_cast<unsigned int>(outputBuffer.size());
            const int status = BZ2_bzCompress(&stream, BZ_RUN);
            const qint64 produced = qint64(outputBuffer.size() - stream.avail_out);
            if (status != BZ_RUN_OK ||
                (produced && !writeAll(writer->output.get(), outputBuffer.data(), produced))) {
                error = status == BZ_RUN_OK ? writer->output->errorString()
                    : QStringLiteral("BZIP2 compression failed.");
                ok = false;
                break;
            }
        } while (stream.avail_in || stream.avail_out == 0);
    }
    if (ok && input.error() != QFileDevice::NoError) {
        error = input.errorString();
        ok = false;
    }
    if (ok) {
        int status = BZ_RUN_OK;
        while (status != BZ_STREAM_END) {
            stream.next_in = nullptr;
            stream.avail_in = 0;
            stream.next_out = outputBuffer.data();
            stream.avail_out = static_cast<unsigned int>(outputBuffer.size());
            status = BZ2_bzCompress(&stream, BZ_FINISH);
            const qint64 produced = qint64(outputBuffer.size() - stream.avail_out);
            if ((status != BZ_FINISH_OK && status != BZ_STREAM_END) ||
                (produced && !writeAll(writer->output.get(), outputBuffer.data(), produced))) {
                error = status == BZ_FINISH_OK || status == BZ_STREAM_END
                    ? writer->output->errorString()
                    : QStringLiteral("BZIP2 stream finalization failed.");
                ok = false;
                break;
            }
        }
    }
    BZ2_bzCompressEnd(&stream);
    if (!ok) return fail(writer, PEARE_WRITER_IO_ERROR, error);
    return PEARE_WRITER_OK;
}

peare_writer_status writeZstdMember(peare_writer_handle_s* writer,
                                    const char* sourcePathUtf8,
                                    const char* entryNameUtf8) {
    if (!sourcePathUtf8 || !*sourcePathUtf8 || !entryNameUtf8 || !*entryNameUtf8)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("A Zstandard source and member name are required."));
    QByteArray safeName;
    if (!normalizedEntryName(entryNameUtf8, false, &safeName))
        return fail(writer, PEARE_WRITER_INVALID_ENTRY_NAME,
                    QStringLiteral("The Zstandard member name is invalid or unsafe."));

    QFile input(QString::fromUtf8(sourcePathUtf8));
    if (!input.open(QIODevice::ReadOnly))
        return fail(writer, PEARE_WRITER_IO_ERROR, input.errorString());
    ZSTD_CCtx* context = ZSTD_createCCtx();
    if (!context)
        return fail(writer, PEARE_WRITER_ALLOCATION_FAILED,
                    QStringLiteral("Could not allocate the Zstandard encoder."));
    size_t code = ZSTD_CCtx_setParameter(context, ZSTD_c_compressionLevel,
                                         writer->zstdLevel);
    if (ZSTD_isError(code)) {
        ZSTD_freeCCtx(context);
        return fail(writer, PEARE_WRITER_INTERNAL_ERROR,
                    QString::fromLatin1(ZSTD_getErrorName(code)));
    }

    std::vector<char> inputBuffer(kChunkSize);
    std::vector<char> outputBuffer(ZSTD_CStreamOutSize());
    bool ok = true;
    QString error;
    while (ok) {
        const qint64 count = input.read(inputBuffer.data(), qint64(inputBuffer.size()));
        if (count < 0) {
            error = input.errorString();
            ok = false;
            break;
        }
        if (count == 0) break;
        ZSTD_inBuffer in = {inputBuffer.data(), size_t(count), 0};
        while (in.pos < in.size) {
            ZSTD_outBuffer out = {outputBuffer.data(), outputBuffer.size(), 0};
            code = ZSTD_compressStream2(context, &out, &in, ZSTD_e_continue);
            if (ZSTD_isError(code) ||
                (out.pos && !writeAll(writer->output.get(), outputBuffer.data(),
                                      qint64(out.pos)))) {
                error = ZSTD_isError(code)
                    ? QString::fromLatin1(ZSTD_getErrorName(code))
                    : writer->output->errorString();
                ok = false;
                break;
            }
        }
    }
    if (ok && input.error() != QFileDevice::NoError) {
        error = input.errorString();
        ok = false;
    }
    if (ok) {
        ZSTD_inBuffer in = {nullptr, 0, 0};
        size_t remaining = 1;
        while (remaining) {
            ZSTD_outBuffer out = {outputBuffer.data(), outputBuffer.size(), 0};
            remaining = ZSTD_compressStream2(context, &out, &in, ZSTD_e_end);
            if (ZSTD_isError(remaining) ||
                (out.pos && !writeAll(writer->output.get(), outputBuffer.data(),
                                      qint64(out.pos)))) {
                error = ZSTD_isError(remaining)
                    ? QString::fromLatin1(ZSTD_getErrorName(remaining))
                    : writer->output->errorString();
                ok = false;
                break;
            }
        }
    }
    ZSTD_freeCCtx(context);
    if (!ok) return fail(writer, PEARE_WRITER_IO_ERROR, error);
    return PEARE_WRITER_OK;
}

template <typename Read>
bool writeZipPackMp3Payload(QIODevice* output, Read read, quint32* crc,
                            quint64* inputSize, quint64* outputSize,
                            QString* error) {
    std::vector<std::uint8_t> source;
    std::vector<char> chunk(kChunkSize);
    quint32 checksum = 0xffffffffu;
    while (true) {
        const qint64 count = read(chunk.data(), qint64(chunk.size()));
        if (count < 0) {
            if (error) *error = QStringLiteral("Could not read ZIP MP3 input.");
            return false;
        }
        if (count == 0) break;
        if (source.size() > std::size_t(INT_MAX) - std::size_t(count)) {
            if (error) *error = QStringLiteral("The MP3 input exceeds the codec size limit.");
            return false;
        }
        checksum = crc32Update(checksum, chunk.data(), size_t(count));
        source.insert(source.end(), chunk.begin(), chunk.begin() + count);
    }
    if (source.size() < 4 || (source[0] == 'M' && source[1] == 'S')) {
        if (error) *error = QStringLiteral("ZIP method 94 requires an MP3 input file.");
        return false;
    }
    static std::mutex codecMutex;
    std::lock_guard<std::mutex> lock(codecMutex);
    iostream::set_memory_write_limit(-1);
    pmplib_init_streams(source.data(), 1, int(source.size()), nullptr, 1);
    unsigned char* encoded = nullptr;
    unsigned int encodedSize = 0;
    const bool valid = pmplib_convert_stream2mem(&encoded, &encodedSize, nullptr);
    std::unique_ptr<unsigned char, decltype(&std::free)> encodedOwner(encoded, &std::free);
    if (!valid || !encoded || !encodedSize ||
        !writeAll(output, reinterpret_cast<const char*>(encoded), encodedSize)) {
        if (error) *error = valid ? output->errorString()
                                  : QStringLiteral("The MP3 input could not be packed.");
        return false;
    }
    *crc = checksum ^ 0xffffffffu;
    *inputSize = source.size();
    *outputSize = encodedSize;
    return true;
}

struct WavPackWriterOutput {
    QIODevice* device;
    bool failed = false;
};

int writeWavPackBlock(void* context, void* data, int32_t size) {
    auto* output = static_cast<WavPackWriterOutput*>(context);
    if (!output || size <= 0 || !writeAll(output->device,
            static_cast<const char*>(data), size)) {
        if (output) output->failed = true;
        return 0;
    }
    return 1;
}

quint16 zipLe16(const std::uint8_t* data) {
    return quint16(data[0]) | quint16(quint16(data[1]) << 8);
}

quint32 zipLe32(const std::uint8_t* data) {
    return quint32(data[0]) | (quint32(data[1]) << 8) |
        (quint32(data[2]) << 16) | (quint32(data[3]) << 24);
}

template <typename Read>
bool writeZipWavPackPayload(QIODevice* output, Read read, quint32* crc,
                            quint64* inputSize, quint64* outputSize,
                            QString* error) {
    std::vector<std::uint8_t> source;
    std::vector<char> chunk(kChunkSize);
    quint32 checksum = 0xffffffffu;
    while (true) {
        const qint64 count = read(chunk.data(), qint64(chunk.size()));
        if (count < 0) {
            if (error) *error = QStringLiteral("Could not read ZIP WavPack input.");
            return false;
        }
        if (count == 0) break;
        if (source.size() > std::numeric_limits<std::uint32_t>::max() -
                                std::size_t(count)) {
            if (error) *error = QStringLiteral("The WAV input exceeds the WavPack wrapper limit.");
            return false;
        }
        checksum = crc32Update(checksum, chunk.data(), size_t(count));
        source.insert(source.end(), chunk.begin(), chunk.begin() + count);
    }
    if (source.size() < 44 || std::memcmp(source.data(), "RIFF", 4) != 0 ||
        std::memcmp(source.data() + 8, "WAVE", 4) != 0) {
        if (error) *error = QStringLiteral("ZIP method 97 requires a RIFF/WAVE input.");
        return false;
    }

    std::uint16_t format = 0;
    std::uint16_t channels = 0;
    std::uint16_t bitsPerSample = 0;
    std::uint16_t blockAlign = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t channelMask = 0;
    std::size_t dataOffset = 0;
    std::size_t dataSize = 0;
    bool haveFormat = false;
    bool haveData = false;
    for (std::size_t offset = 12; offset + 8 <= source.size();) {
        const std::uint8_t* header = source.data() + offset;
        const std::uint32_t chunkSize = zipLe32(header + 4);
        const std::size_t body = offset + 8;
        if (chunkSize > source.size() - body) break;
        if (std::memcmp(header, "fmt ", 4) == 0 && chunkSize >= 16) {
            format = zipLe16(source.data() + body);
            channels = zipLe16(source.data() + body + 2);
            sampleRate = zipLe32(source.data() + body + 4);
            blockAlign = zipLe16(source.data() + body + 12);
            bitsPerSample = zipLe16(source.data() + body + 14);
            if (format == 0xfffe && chunkSize >= 40) {
                const std::uint8_t* subFormat = source.data() + body + 24;
                const std::uint8_t pcmGuid[16] = {
                    1, 0, 0, 0, 0, 0, 16, 0, 128, 0, 0, 170, 0, 0x38, 0x9b, 0x71
                };
                if (std::memcmp(subFormat, pcmGuid, 16) != 0) {
                    if (error) *error = QStringLiteral("WavPack ZIP output supports PCM WAV only.");
                    return false;
                }
                format = 1;
                channelMask = zipLe32(source.data() + body + 20);
            }
            haveFormat = true;
        } else if (std::memcmp(header, "data", 4) == 0) {
            if (haveData) {
                if (error) *error = QStringLiteral("WavPack ZIP output does not accept multiple WAV data chunks.");
                return false;
            }
            dataOffset = body;
            dataSize = chunkSize;
            haveData = true;
        }
        const std::size_t padded = std::size_t(chunkSize) + (chunkSize & 1u);
        if (padded > source.size() - body) break;
        offset = body + padded;
    }
    if (!haveFormat || !haveData || format != 1 || !channels || channels > 256 ||
        !sampleRate || !dataSize || (bitsPerSample != 8 && bitsPerSample != 16 &&
                        bitsPerSample != 24 && bitsPerSample != 32) ||
        blockAlign != channels * (bitsPerSample / 8) || !blockAlign ||
        dataSize % blockAlign) {
        if (error) *error = QStringLiteral("The WAV must contain aligned integer PCM samples.");
        return false;
    }

    const std::size_t sampleCount = dataSize / blockAlign;
    const std::size_t trailingOffset = dataOffset + dataSize;
    if (!channelMask) {
        if (channels == 1) channelMask = 4;
        else if (channels == 2) channelMask = 3;
    }
    if (!channelMask) {
        if (error) *error = QStringLiteral("Multichannel WAV input requires an explicit channel mask.");
        return false;
    }
    const qint64 outputStart = output->pos();
    if (outputStart < 0) {
        if (error) *error = QStringLiteral("Could not determine the ZIP output position.");
        return false;
    }
    WavPackWriterOutput sink{output, false};
    std::unique_ptr<WavpackContext, decltype(&WavpackCloseFile)> context(
        WavpackOpenFileOutput(writeWavPackBlock, &sink, nullptr), WavpackCloseFile);
    if (!context) {
        if (error) *error = QStringLiteral("Could not initialize WavPack output.");
        return false;
    }
    WavpackConfig config = {};
    config.bits_per_sample = bitsPerSample;
    config.bytes_per_sample = bitsPerSample / 8;
    config.num_channels = channels;
    config.sample_rate = int(sampleRate);
    config.channel_mask = int(channelMask);
    config.flags = CONFIG_HIGH_FLAG;
    if (!WavpackSetConfiguration64(context.get(), &config,
                                    std::int64_t(sampleCount), nullptr) ||
        !WavpackAddWrapper(context.get(), source.data(), std::uint32_t(dataOffset)) ||
        !WavpackPackInit(context.get())) {
        if (error) *error = QStringLiteral("Could not configure WavPack for this WAV file.");
        return false;
    }

    constexpr std::size_t framesPerChunk = 4096;
    std::vector<std::int32_t> samples(framesPerChunk * channels);
    const std::uint8_t* pcm = source.data() + dataOffset;
    std::size_t frame = 0;
    while (frame < sampleCount) {
        const std::size_t frames = std::min(framesPerChunk, sampleCount - frame);
        for (std::size_t i = 0; i < frames * channels; ++i) {
            const std::uint8_t* sample = pcm + (frame * channels + i) * (bitsPerSample / 8);
            std::uint32_t value = sample[0];
            for (int byte = 1; byte < bitsPerSample / 8; ++byte)
                value |= std::uint32_t(sample[byte]) << (byte * 8);
            if (bitsPerSample == 8) samples[i] = std::int32_t(value) - 128;
            else if (bitsPerSample < 32 && (value & (1u << (bitsPerSample - 1))))
                samples[i] = std::int32_t(value | (~0u << bitsPerSample));
            else samples[i] = std::int32_t(value);
        }
        if (!WavpackPackSamples(context.get(), samples.data(), std::uint32_t(frames))) {
            if (error) *error = QStringLiteral("WavPack failed to encode PCM samples.");
            return false;
        }
        frame += frames;
    }
    if (!WavpackFlushSamples(context.get())) {
        if (error) *error = QStringLiteral("Could not finish the WavPack audio blocks.");
        return false;
    }
    if (trailingOffset < source.size() &&
        !WavpackAddWrapper(context.get(), source.data() + trailingOffset,
                           std::uint32_t(source.size() - trailingOffset))) {
        if (error) *error = QStringLiteral("Could not store the trailing WAV wrapper.");
        return false;
    }
    if (!WavpackFlushSamples(context.get()) || sink.failed) {
        if (error) *error = sink.failed ? output->errorString()
                                       : QStringLiteral("Could not finish WavPack output.");
        return false;
    }
    *crc = checksum ^ 0xffffffffu;
    *inputSize = source.size();
    *outputSize = quint64(output->pos() - outputStart);
    return true;
}

template <typename Read>
bool writeZipJpegPayload(QIODevice* output, Read read, quint32* crc,
                         quint64* inputSize, quint64* outputSize, QString* error) {
    std::vector<std::uint8_t> source;
    std::array<char, kChunkSize> buffer{};
    quint32 checksum = 0xffffffffu;
    for (;;) {
        const qint64 count = read(buffer.data(), qint64(buffer.size()));
        if (count < 0) {
            if (error) *error = QStringLiteral("Could not read JPEG input data.");
            return false;
        }
        if (!count) break;
        if (quint64(source.size()) > std::numeric_limits<size_t>::max() - quint64(count)) {
            if (error) *error = QStringLiteral("JPEG input exceeds addressable memory.");
            return false;
        }
        checksum = crc32Update(checksum, buffer.data(), size_t(count));
        source.insert(source.end(), buffer.data(), buffer.data() + count);
    }
    std::vector<std::uint8_t> encoded;
    if (!peare::writer::encodeWinZipJpeg(source.data(), source.size(), &encoded)) {
        if (error) *error = QStringLiteral("The JPEG input is invalid or unsupported by ZIP method 96.");
        return false;
    }
    if (encoded.size() > std::numeric_limits<quint64>::max() ||
        !writeAll(output, reinterpret_cast<const char*>(encoded.data()),
                  qint64(encoded.size()))) {
        if (error) *error = output->errorString();
        return false;
    }
    *crc = checksum ^ 0xffffffffu;
    *inputSize = source.size();
    *outputSize = encoded.size();
    return true;
}

class ZipShrinkOutput {
public:
    explicit ZipShrinkOutput(QIODevice* device) : device_(device) {}

    bool writeCode(quint16 code) {
        while (code > ((quint16(1) << width_) - 1)) {
            if (width_ >= 13 || !writeRaw(256, width_) || !writeRaw(1, width_))
                return false;
            ++width_;
        }
        return writeRaw(code, width_);
    }

    bool writeControl(quint16 command) {
        return writeRaw(256, width_) && writeRaw(command, width_);
    }

    bool writeRaw(quint16 code, unsigned width) {
        bits_ |= quint64(code) << available_;
        available_ += int(width);
        while (available_ >= 8) {
            const char byte = char(bits_ & 0xff);
            if (!writeAll(device_, &byte, 1)) return false;
            ++written_;
            bits_ >>= 8;
            available_ -= 8;
        }
        return true;
    }

    bool finish() {
        if (!available_) return true;
        const char byte = char(bits_ & 0xff);
        if (!writeAll(device_, &byte, 1)) return false;
        ++written_;
        available_ = 0;
        bits_ = 0;
        return true;
    }

    quint64 written() const { return written_; }

private:
    QIODevice* device_;
    quint64 bits_ = 0;
    unsigned available_ = 0;
    quint64 written_ = 0;
    unsigned width_ = 9;
};

class ZipBitOutput {
public:
    explicit ZipBitOutput(QIODevice* device) : device_(device) {}

    bool write(quint32 value, unsigned width) {
        bits_ |= quint64(value) << available_;
        available_ += width;
        while (available_ >= 8) {
            const char byte = char(bits_ & 0xff);
            if (!writeAll(device_, &byte, 1)) return false;
            bits_ >>= 8;
            available_ -= 8;
            ++written_;
        }
        return true;
    }

    bool finish() {
        if (!available_) return true;
        const char byte = char(bits_ & 0xff);
        if (!writeAll(device_, &byte, 1)) return false;
        ++written_;
        bits_ = 0;
        available_ = 0;
        return true;
    }

    quint64 written() const { return written_; }

private:
    QIODevice* device_;
    quint64 bits_ = 0;
    unsigned available_ = 0;
    quint64 written_ = 0;
};

template <typename Read>
bool writeZipReducePayload(QIODevice* output, Read read, int factor, quint32* crc,
                           quint64* inputSize, quint64* outputSize, QString* error) {
    if (factor < 1 || factor > 4) return false;
    std::vector<std::uint8_t> source;
    std::vector<char> chunk(kChunkSize);
    quint32 checksum = 0xffffffffu;
    while (true) {
        const qint64 count = read(chunk.data(), qint64(chunk.size()));
        if (count < 0) {
            if (error) *error = QStringLiteral("Could not read ZIP Reduce input.");
            return false;
        }
        if (!count) break;
        if (std::size_t(count) > source.max_size() - source.size()) {
            if (error) *error = QStringLiteral("The ZIP Reduce input is too large.");
            return false;
        }
        checksum = crc32Update(checksum, chunk.data(), std::size_t(count));
        source.insert(source.end(), chunk.begin(), chunk.begin() + count);
    }

    const unsigned lengthBits = unsigned(8 - factor);
    const std::size_t maxLength = ((std::size_t(1) << lengthBits) - 1) + 255 + 3;
    const std::size_t maxDistance = ((std::size_t(1) << factor) - 1) * 256 + 256;
    std::vector<std::uint8_t> tokens;
    tokens.reserve(source.size());
    std::map<std::uint32_t, std::vector<std::size_t>> chains;
    const auto addPosition = [&](std::size_t pos) {
        if (pos + 2 >= source.size()) return;
        const std::uint32_t key = (std::uint32_t(source[pos]) << 16) |
            (std::uint32_t(source[pos + 1]) << 8) | source[pos + 2];
        auto& positions = chains[key];
        positions.push_back(pos);
        while (!positions.empty() && pos - positions.front() > maxDistance)
            positions.erase(positions.begin());
    };
    for (std::size_t pos = 0; pos < source.size();) {
        std::size_t bestLength = 0;
        std::size_t bestDistance = 0;
        if (pos + 2 < source.size()) {
            const std::uint32_t key = (std::uint32_t(source[pos]) << 16) |
                (std::uint32_t(source[pos + 1]) << 8) | source[pos + 2];
            const auto found = chains.find(key);
            if (found != chains.end()) {
                const auto& positions = found->second;
                std::size_t examined = 0;
                for (auto it = positions.rbegin(); it != positions.rend() && examined < 128;
                     ++it, ++examined) {
                    const std::size_t distance = pos - *it;
                    if (!distance || distance > maxDistance) continue;
                    std::size_t length = 3;
                    while (length < maxLength && pos + length < source.size() &&
                           *it + length < pos && source[*it + length] == source[pos + length])
                        ++length;
                    if (length > bestLength) {
                        bestLength = length;
                        bestDistance = distance;
                    }
                }
            }
        }
        if (bestLength >= 3 && !(bestLength == 3 && bestDistance <= 256)) {
            const std::size_t reducedLength = bestLength - 3;
            const std::size_t reducedDistance = bestDistance - 1;
            const std::size_t lengthMask = (std::size_t(1) << lengthBits) - 1;
            std::uint8_t value = std::uint8_t(std::min(reducedLength, lengthMask) |
                ((reducedDistance >> 8) << lengthBits));
            tokens.push_back(0x90);
            tokens.push_back(value);
            if (reducedLength >= lengthMask)
                tokens.push_back(std::uint8_t(reducedLength - lengthMask));
            tokens.push_back(std::uint8_t(reducedDistance));
            for (std::size_t i = 0; i < bestLength; ++i) addPosition(pos + i);
            pos += bestLength;
        } else {
            const std::uint8_t value = source[pos];
            tokens.push_back(value);
            if (value == 0x90) tokens.push_back(0);
            addPosition(pos++);
        }
    }

    struct Follower { std::uint8_t byte; std::uint64_t count; };
    std::uint64_t counts[256][256] = {};
    std::uint8_t previous = 0;
    for (std::uint8_t value : tokens) {
        ++counts[previous][value];
        previous = value;
    }
    std::array<std::array<std::uint8_t, 32>, 256> followers = {};
    std::array<std::uint8_t, 256> followerCount = {};
    std::array<std::uint8_t, 256> followerWidth = {};
    for (int prior = 0; prior < 256; ++prior) {
        std::array<Follower, 256> ordered = {};
        for (int value = 0; value < 256; ++value)
            ordered[value] = {std::uint8_t(value), counts[prior][value]};
        std::sort(ordered.begin(), ordered.end(), [](const Follower& a, const Follower& b) {
            return a.count != b.count ? a.count > b.count : a.byte < b.byte;
        });
        std::uint64_t bestCost = std::numeric_limits<std::uint64_t>::max();
        unsigned bestCount = 0;
        for (unsigned n = 0; n <= 32; ++n) {
            const unsigned width = n <= 1 ? (n ? 1 : 0) :
                (n <= 2 ? 1 : n <= 4 ? 2 : n <= 8 ? 3 : n <= 16 ? 4 : 5);
            std::uint64_t cost = std::uint64_t(n) * 8;
            for (unsigned i = 0; i < 256; ++i) {
                if (i < n) cost += ordered[i].count * (1 + width);
                else cost += ordered[i].count * (n ? 9 : 8);
            }
            if (cost < bestCost) { bestCost = cost; bestCount = n; }
        }
        followerCount[prior] = std::uint8_t(bestCount);
        if (bestCount) followerWidth[prior] = std::uint8_t(bestCount <= 2 ? 1 :
            bestCount <= 4 ? 2 : bestCount <= 8 ? 3 : bestCount <= 16 ? 4 : 5);
        for (unsigned i = 0; i < bestCount; ++i) followers[prior][i] = ordered[i].byte;
    }

    const qint64 start = output->pos();
    if (start < 0) return false;
    ZipBitOutput encoded(output);
    for (int prior = 255; prior >= 0; --prior) {
        if (!encoded.write(followerCount[prior], 6)) goto reduce_write_error;
        for (unsigned i = 0; i < followerCount[prior]; ++i)
            if (!encoded.write(followers[prior][i], 8)) goto reduce_write_error;
    }
    previous = 0;
    for (std::uint8_t value : tokens) {
        const unsigned n = followerCount[previous];
        unsigned index = 0;
        while (index < n && followers[previous][index] != value) ++index;
        if (n && index < n) {
            if (!encoded.write(0, 1) || !encoded.write(index, followerWidth[previous]))
                goto reduce_write_error;
        } else if (n) {
            if (!encoded.write(1, 1) || !encoded.write(value, 8))
                goto reduce_write_error;
        } else if (!encoded.write(value, 8)) {
            goto reduce_write_error;
        }
        previous = value;
    }
    if (!encoded.finish()) goto reduce_write_error;
    *crc = checksum ^ 0xffffffffu;
    *inputSize = source.size();
    *outputSize = quint64(output->pos() - start);
    return true;

reduce_write_error:
    if (error) *error = output->errorString();
    return false;
}

void zipCanonicalCodes(const std::uint8_t* runs, std::size_t runCount,
                       std::size_t symbolCount, std::vector<std::uint8_t>* lengths,
                       std::vector<quint32>* codes) {
    lengths->clear();
    lengths->reserve(symbolCount);
    for (std::size_t i = 0; i < runCount; ++i) {
        const std::size_t count = std::size_t(runs[i] >> 4) + 1;
        lengths->insert(lengths->end(), count, std::uint8_t(runs[i] & 15));
    }
    std::uint32_t counts[17] = {};
    for (std::uint8_t length : *lengths) ++counts[length];
    std::uint32_t next[17] = {};
    std::uint32_t code = 0;
    for (unsigned length = 1; length <= 16; ++length) {
        code = (code + counts[length - 1]) << 1;
        next[length] = code;
    }
    codes->assign(symbolCount, 0);
    for (std::size_t symbol = 0; symbol < symbolCount; ++symbol) {
        const unsigned length = (*lengths)[symbol];
        if (length) (*codes)[symbol] = next[length]++;
    }
}

quint32 zipReverseBits(quint32 value, unsigned width) {
    quint32 reversed = 0;
    for (unsigned bit = 0; bit < width; ++bit) {
        reversed = (reversed << 1) | (value & 1u);
        value >>= 1;
    }
    return reversed;
}

template <typename Read>
bool writeZipImplodePayload(QIODevice* output, Read read, quint32* crc,
                            quint64* inputSize, quint64* outputSize,
                            QString* error) {
    const qint64 outputStart = output->pos();
    if (outputStart < 0) return false;
    ZipBitOutput encoded(output);
    const std::uint8_t packedTree[] = {0xf5, 0xf5, 0xf5, 0xf5};
    for (int tree = 0; tree < 2; ++tree) {
        if (!encoded.write(3, 8)) return false;
        for (std::uint8_t value : packedTree)
            if (!encoded.write(value, 8)) return false;
    }
    quint32 checksum = 0xffffffffu;
    quint64 size = 0;
    std::vector<char> chunk(kChunkSize);
    while (true) {
        const qint64 count = read(chunk.data(), qint64(chunk.size()));
        if (count < 0) {
            if (error) *error = QStringLiteral("Could not read ZIP Implode input.");
            return false;
        }
        if (!count) break;
        checksum = crc32Update(checksum, chunk.data(), size_t(count));
        size += quint64(count);
        for (qint64 i = 0; i < count; ++i)
            if (!encoded.write(1, 1) ||
                !encoded.write(static_cast<unsigned char>(chunk[size_t(i)]), 8)) {
                if (error) *error = output->errorString();
                return false;
            }
    }
    if (!encoded.finish()) {
        if (error) *error = output->errorString();
        return false;
    }
    *crc = checksum ^ 0xffffffffu;
    *inputSize = size;
    *outputSize = quint64(output->pos() - outputStart);
    return true;
}

template <typename Read>
bool writeZipDclImplodePayload(QIODevice* output, Read read, quint32* crc,
                              quint64* inputSize, quint64* outputSize,
                              QString* error) {
    static const std::uint8_t lengthRuns[] = {2, 35, 36, 53, 38, 23};
    std::vector<std::uint8_t> lengths;
    std::vector<quint32> codes;
    zipCanonicalCodes(lengthRuns, sizeof(lengthRuns), 16, &lengths, &codes);
    const quint32 endCode = codes[15];
    const unsigned endWidth = lengths[15];
    const qint64 outputStart = output->pos();
    if (outputStart < 0) return false;
    ZipBitOutput encoded(output);
    if (!encoded.write(0, 8) || !encoded.write(4, 8)) return false;
    quint32 checksum = 0xffffffffu;
    quint64 size = 0;
    std::vector<char> chunk(kChunkSize);
    while (true) {
        const qint64 count = read(chunk.data(), qint64(chunk.size()));
        if (count < 0) {
            if (error) *error = QStringLiteral("Could not read ZIP DCL Implode input.");
            return false;
        }
        if (!count) break;
        checksum = crc32Update(checksum, chunk.data(), size_t(count));
        size += quint64(count);
        for (qint64 i = 0; i < count; ++i)
            if (!encoded.write(0, 1) ||
                !encoded.write(static_cast<unsigned char>(chunk[size_t(i)]), 8)) {
                if (error) *error = output->errorString();
                return false;
            }
    }
    if (!encoded.write(1, 1) ||
        !encoded.write(zipReverseBits((~endCode) & ((1u << endWidth) - 1), endWidth),
                       endWidth) ||
        !encoded.write(255, 8) || !encoded.finish()) {
        if (error) *error = output->errorString();
        return false;
    }
    *crc = checksum ^ 0xffffffffu;
    *inputSize = size;
    *outputSize = quint64(output->pos() - outputStart);
    return true;
}

template <typename Read>
bool writeZipShrinkPayload(QIODevice* output, Read read, quint32* crc,
                           quint64* inputSize, quint64* outputSize,
                           QString* error) {
    constexpr quint16 escapeCode = 256;
    constexpr quint16 tableSize = 8192;
    std::map<quint32, quint16> dictionary;
    std::vector<quint16> prefix(tableSize, 0);
    std::vector<std::uint8_t> suffix(tableSize, 0);
    std::vector<std::uint8_t> used(tableSize, 0);
    quint16 current = 0;
    quint32 checksum = 0xffffffffu;
    quint64 uncompressed = 0;
    bool haveCurrent = false;
    ZipShrinkOutput encoded(output);
    const auto clearPartial = [&]() {
        std::vector<std::uint8_t> hasChild(tableSize, 0);
        for (quint16 code = 257; code < tableSize; ++code) {
            if (used[code] && prefix[code] > escapeCode)
                hasChild[prefix[code]] = 1;
        }
        for (quint16 code = 257; code < tableSize; ++code) {
            if (used[code] && !hasChild[code]) {
                dictionary.erase((quint32(prefix[code]) << 8) | suffix[code]);
                used[code] = 0;
            }
        }
        return true;
    };

    std::vector<char> chunk(kChunkSize);
    bool ok = true;
    while (ok) {
        const qint64 count = read(chunk.data(), qint64(chunk.size()));
        if (count < 0) {
            if (error) *error = QStringLiteral("Could not read ZIP Shrink input.");
            ok = false;
            break;
        }
        if (count == 0) break;
        checksum = crc32Update(checksum, chunk.data(), size_t(count));
        uncompressed += quint64(count);
        for (qint64 index = 0; ok && index < count; ++index) {
            const quint16 byte = static_cast<unsigned char>(chunk[size_t(index)]);
            if (!haveCurrent) {
                current = byte;
                haveCurrent = true;
                continue;
            }
            const quint32 key = (quint32(current) << 8) | byte;
            const auto found = dictionary.find(key);
            if (found != dictionary.end()) {
                current = found->second;
                continue;
            }
            ok = encoded.writeCode(current);
            if (!ok) break;
            quint16 newCode = 257;
            while (newCode < tableSize && used[newCode]) ++newCode;
            if (newCode == tableSize) {
                ok = clearPartial() && encoded.writeControl(2);
                if (!ok) break;
                newCode = 257;
                while (newCode < tableSize && used[newCode]) ++newCode;
            }
            if (newCode < tableSize) {
                dictionary.emplace(key, newCode);
                prefix[newCode] = current;
                suffix[newCode] = std::uint8_t(byte);
                used[newCode] = 1;
            }
            current = byte;
        }
    }
    if (ok && haveCurrent) ok = encoded.writeCode(current);
    if (ok) ok = encoded.finish();
    if (!ok) {
        if (error && error->isEmpty()) *error = output->errorString();
        return false;
    }
    *crc = checksum ^ 0xffffffffu;
    *inputSize = uncompressed;
    *outputSize = encoded.written();
    return true;
}

template <typename Read>
struct ZipLzmaInput {
    ISeqInStream stream = {};
    Read* read = nullptr;
    quint32 crc = 0xffffffffu;
    quint64 size = 0;
    QString error;

    static SRes readStream(ISeqInStreamPtr stream, void* buffer, size_t* size) {
        auto* self = reinterpret_cast<ZipLzmaInput*>(
            const_cast<ISeqInStream*>(stream));
        const qint64 count = (*self->read)(static_cast<char*>(buffer), qint64(*size));
        if (count < 0) {
            self->error = QStringLiteral("Could not read ZIP input.");
            return SZ_ERROR_READ;
        }
        *size = size_t(count);
        if (count > 0) {
            self->crc = crc32Update(self->crc, static_cast<const char*>(buffer),
                                    size_t(count));
            self->size += quint64(count);
        }
        return SZ_OK;
    }
};

struct ZipLzmaOutput {
    ISeqOutStream stream = {};
    QIODevice* device = nullptr;
    quint64 size = 0;
    QString error;

    static size_t writeStream(ISeqOutStreamPtr stream, const void* buffer,
                              size_t size) {
        auto* self = reinterpret_cast<ZipLzmaOutput*>(
            const_cast<ISeqOutStream*>(stream));
        if (size > size_t(std::numeric_limits<qint64>::max()) ||
            !writeAll(self->device, static_cast<const char*>(buffer), qint64(size))) {
            self->error = self->device->errorString();
            return 0;
        }
        self->size += quint64(size);
        return size;
    }
};

void* zipLzmaAllocate(ISzAllocPtr, size_t size) {
    return size ? std::malloc(size) : nullptr;
}

void zipLzmaFree(ISzAllocPtr, void* address) {
    std::free(address);
}

const ISzAlloc kZipLzmaAllocator = {zipLzmaAllocate, zipLzmaFree};

template <typename Read>
bool writeZipLzmaPayload(QIODevice* output, Read read, quint32* crc,
                         quint64* inputSize, quint64* outputSize,
                         QString* error) {
    QByteArray prefix;
    append16(&prefix, 0x0013);
    append16(&prefix, LZMA_PROPS_SIZE);
    if (!writeAll(output, prefix.constData(), prefix.size())) {
        if (error) *error = output->errorString();
        return false;
    }

    CLzmaEncProps properties;
    LzmaEncProps_Init(&properties);
    properties.writeEndMark = 1;
    properties.numThreads = 1;
    CLzmaEncHandle encoder = LzmaEnc_Create(&kZipLzmaAllocator);
    if (!encoder) {
        if (error) *error = QStringLiteral("Could not allocate the ZIP LZMA encoder.");
        return false;
    }
    SRes status = LzmaEnc_SetProps(encoder, &properties);
    Byte encodedProperties[LZMA_PROPS_SIZE] = {};
    SizeT encodedPropertiesSize = LZMA_PROPS_SIZE;
    if (status == SZ_OK)
        status = LzmaEnc_WriteProperties(encoder, encodedProperties,
                                         &encodedPropertiesSize);
    bool ok = status == SZ_OK && encodedPropertiesSize == LZMA_PROPS_SIZE &&
        writeAll(output, reinterpret_cast<const char*>(encodedProperties),
                 qint64(encodedPropertiesSize));
    if (!ok && error) {
        *error = status == SZ_OK ? output->errorString()
            : QStringLiteral("Could not configure the ZIP LZMA encoder.");
    }

    ZipLzmaInput<Read> input;
    input.stream.Read = &ZipLzmaInput<Read>::readStream;
    input.read = &read;
    ZipLzmaOutput encoded;
    encoded.stream.Write = &ZipLzmaOutput::writeStream;
    encoded.device = output;
    if (ok) {
        status = LzmaEnc_Encode(encoder, &encoded.stream, &input.stream, nullptr,
                                &kZipLzmaAllocator, &kZipLzmaAllocator);
        ok = status == SZ_OK;
        if (!ok && error) {
            *error = !input.error.isEmpty() ? input.error
                : !encoded.error.isEmpty() ? encoded.error
                : QStringLiteral("ZIP LZMA compression failed (%1).").arg(status);
        }
    }
    LzmaEnc_Destroy(encoder, &kZipLzmaAllocator, &kZipLzmaAllocator);
    if (!ok) return false;
    *crc = input.crc ^ 0xffffffffu;
    *inputSize = input.size;
    *outputSize = quint64(prefix.size() + encodedPropertiesSize) + encoded.size;
    return true;
}

class UnixCompressBitWriter {
public:
    explicit UnixCompressBitWriter(QIODevice* output) : output_(output) {}

    bool writeCode(quint32 code, unsigned width) {
        buffer_ |= quint64(code) << bitCount_;
        bitCount_ += width;
        while (bitCount_ >= 8) {
            if (!writeByte(char(buffer_ & 0xff))) return false;
            buffer_ >>= 8;
            bitCount_ -= 8;
        }
        return true;
    }

    bool flushClearBoundary(unsigned width) {
        if (bitCount_) {
            if (!writeByte(char(buffer_ & 0xff))) return false;
            buffer_ = 0;
            bitCount_ = 0;
        }
        const qint64 remainder = sectionBytes_ % qint64(width);
        const qint64 padding = (qint64(width) - remainder) % qint64(width);
        for (qint64 i = 0; i < padding; ++i)
            if (!writeByte('\0')) return false;
        return true;
    }

    void beginWidthSection() { sectionBytes_ = 0; }

    bool finish() {
        if (!bitCount_) return true;
        if (!writeByte(char(buffer_ & 0xff))) return false;
        buffer_ = 0;
        bitCount_ = 0;
        return true;
    }

private:
    bool writeByte(char value) {
        if (!output_ || output_->write(&value, 1) != 1) return false;
        ++sectionBytes_;
        return true;
    }

    QIODevice* output_;
    quint64 buffer_ = 0;
    unsigned bitCount_ = 0;
    qint64 sectionBytes_ = 0;
};

peare_writer_status writeUnixCompressMember(peare_writer_handle_s* writer,
                                            const char* sourcePathUtf8,
                                            const char* entryNameUtf8) {
    if (writer->output->pos() != 0)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("Unix compress output accepts one input file."));
    if (!sourcePathUtf8 || !*sourcePathUtf8 || !entryNameUtf8 || !*entryNameUtf8)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("A Unix compress source and name are required."));
    QByteArray safeName;
    if (!normalizedEntryName(entryNameUtf8, false, &safeName))
        return fail(writer, PEARE_WRITER_INVALID_ENTRY_NAME,
                    QStringLiteral("The Unix compress input name is invalid or unsafe."));

    QFile input(QString::fromUtf8(sourcePathUtf8));
    if (!input.open(QIODevice::ReadOnly))
        return fail(writer, PEARE_WRITER_IO_ERROR, input.errorString());
    const quint8 flags = quint8(writer->unixCompressMaxBits |
        (writer->unixCompressBlockMode ? 0x80 : 0));
    const char header[] = {char(0x1f), char(0x9d), char(flags)};
    if (!writeAll(writer->output.get(), header, sizeof(header)))
        return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());

    const int maxBits = writer->unixCompressMaxBits;
    const quint32 maxCodes = quint32(1) << maxBits;
    quint32 nextCode = writer->unixCompressBlockMode ? 257u : 256u;
    unsigned codeWidth = 9;
    std::unordered_map<quint32, quint32> dictionary;
    dictionary.reserve(size_t(maxCodes));
    UnixCompressBitWriter bits(writer->output.get());
    char byte = 0;
    if (!input.getChar(&byte)) {
        if (input.error() != QFileDevice::NoError)
            return fail(writer, PEARE_WRITER_IO_ERROR, input.errorString());
        return PEARE_WRITER_OK;
    }
    quint32 phrase = quint8(byte);
    while (input.getChar(&byte)) {
        const quint8 value = quint8(byte);
        const quint32 key = (phrase << 8) | value;
        const auto found = dictionary.find(key);
        if (found != dictionary.end()) {
            phrase = found->second;
            continue;
        }
        if (!bits.writeCode(phrase, codeWidth))
            return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
        if (nextCode < maxCodes) {
            dictionary.emplace(key, nextCode++);
            if (codeWidth < unsigned(maxBits) && nextCode > (quint32(1) << codeWidth)) {
                if (!bits.flushClearBoundary(codeWidth))
                    return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
                ++codeWidth;
                bits.beginWidthSection();
            }
        } else if (writer->unixCompressBlockMode) {
            if (!bits.writeCode(256, codeWidth) || !bits.flushClearBoundary(codeWidth))
                return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
            dictionary.clear();
            nextCode = 257;
            codeWidth = 9;
            bits.beginWidthSection();
        }
        phrase = value;
    }
    if (input.error() != QFileDevice::NoError)
        return fail(writer, PEARE_WRITER_IO_ERROR, input.errorString());
    if (!bits.writeCode(phrase, codeWidth) || !bits.finish())
        return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
    return PEARE_WRITER_OK;
}

peare_writer_status writeLzmaAloneMember(peare_writer_handle_s* writer,
                                         const char* sourcePathUtf8,
                                         const char* entryNameUtf8) {
    if (writer->output->pos() != 0)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("LZMA-alone output accepts one input file."));
    if (!sourcePathUtf8 || !*sourcePathUtf8 || !entryNameUtf8 || !*entryNameUtf8)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("An LZMA source and member name are required."));
    QByteArray safeName;
    if (!normalizedEntryName(entryNameUtf8, false, &safeName))
        return fail(writer, PEARE_WRITER_INVALID_ENTRY_NAME,
                    QStringLiteral("The LZMA member name is invalid or unsafe."));

    QFile input(QString::fromUtf8(sourcePathUtf8));
    if (!input.open(QIODevice::ReadOnly))
        return fail(writer, PEARE_WRITER_IO_ERROR, input.errorString());
    const qint64 declaredSize = input.size();
    if (declaredSize < 0)
        return fail(writer, PEARE_WRITER_IO_ERROR,
                    QStringLiteral("Could not determine the LZMA input size."));
    CLzmaEncProps properties;
    LzmaEncProps_Init(&properties);
    properties.level = writer->lzmaLevel;
    properties.dictSize = 1u << 24;
    properties.writeEndMark = 0;
    properties.numThreads = 1;
    properties.reduceSize = quint64(declaredSize);
    CLzmaEncHandle encoder = LzmaEnc_Create(&kZipLzmaAllocator);
    if (!encoder)
        return fail(writer, PEARE_WRITER_ALLOCATION_FAILED,
                    QStringLiteral("Could not allocate the LZMA encoder."));
    SRes status = LzmaEnc_SetProps(encoder, &properties);
    Byte encodedProperties[LZMA_PROPS_SIZE] = {};
    SizeT encodedPropertiesSize = LZMA_PROPS_SIZE;
    if (status == SZ_OK)
        status = LzmaEnc_WriteProperties(encoder, encodedProperties,
                                         &encodedPropertiesSize);
    QByteArray header;
    if (status == SZ_OK && encodedPropertiesSize == LZMA_PROPS_SIZE) {
        header.append(reinterpret_cast<const char*>(encodedProperties),
                      int(encodedPropertiesSize));
        append64(&header, quint64(declaredSize));
    }
    if (status != SZ_OK || encodedPropertiesSize != LZMA_PROPS_SIZE ||
        !writeAll(writer->output.get(), header.constData(), header.size())) {
        LzmaEnc_Destroy(encoder, &kZipLzmaAllocator, &kZipLzmaAllocator);
        return fail(writer, PEARE_WRITER_IO_ERROR,
                    status == SZ_OK ? writer->output->errorString()
                    : QStringLiteral("Could not configure the LZMA encoder."));
    }

    auto read = [&input](char* buffer, qint64 size) {
        return input.read(buffer, size);
    };
    ZipLzmaInput<decltype(read)> source;
    source.stream.Read = &ZipLzmaInput<decltype(read)>::readStream;
    source.read = &read;
    ZipLzmaOutput encoded;
    encoded.stream.Write = &ZipLzmaOutput::writeStream;
    encoded.device = writer->output.get();
    status = LzmaEnc_Encode(encoder, &encoded.stream, &source.stream, nullptr,
                            &kZipLzmaAllocator, &kZipLzmaAllocator);
    LzmaEnc_Destroy(encoder, &kZipLzmaAllocator, &kZipLzmaAllocator);
    if (status != SZ_OK || source.size != quint64(declaredSize) ||
        input.error() != QFileDevice::NoError)
        return fail(writer, PEARE_WRITER_IO_ERROR,
                    !source.error.isEmpty() ? source.error
                    : !encoded.error.isEmpty() ? encoded.error
                    : input.error() != QFileDevice::NoError ? input.errorString()
                    : QStringLiteral("LZMA compression failed (%1).").arg(status));
    return PEARE_WRITER_OK;
}

std::once_flag gXzCrcInitialization;

void initializeXzCrcTables() {
    std::call_once(gXzCrcInitialization, [] {
        CrcGenerateTable();
        Crc64GenerateTable();
    });
}

template <typename Read>
bool writeZipXzPayload(QIODevice* output, Read read, quint32* crc,
                       quint64* inputSize, quint64* outputSize,
                       QString* error) {
    initializeXzCrcTables();
    ZipLzmaInput<Read> input;
    input.stream.Read = &ZipLzmaInput<Read>::readStream;
    input.read = &read;
    ZipLzmaOutput encoded;
    encoded.stream.Write = &ZipLzmaOutput::writeStream;
    encoded.device = output;
    CXzProps properties;
    XzProps_Init(&properties);
    properties.lzma2Props.numTotalThreads = 1;
    properties.lzma2Props.lzmaProps.numThreads = 1;
    properties.numTotalThreads = 1;
    CXzEncHandle encoder = XzEnc_Create(&kZipLzmaAllocator,
                                        &kZipLzmaAllocator);
    if (!encoder) {
        if (error) *error = QStringLiteral("Could not allocate the ZIP XZ encoder.");
        return false;
    }
    SRes status = XzEnc_SetProps(encoder, &properties);
    if (status == SZ_OK)
        status = XzEnc_Encode(encoder, &encoded.stream, &input.stream, nullptr);
    XzEnc_Destroy(encoder);
    if (status != SZ_OK) {
        if (error) {
            *error = !input.error.isEmpty() ? input.error
                : !encoded.error.isEmpty() ? encoded.error
                : QStringLiteral("ZIP XZ compression failed (%1).").arg(status);
        }
        return false;
    }
    *crc = input.crc ^ 0xffffffffu;
    *inputSize = input.size;
    *outputSize = encoded.size;
    return true;
}

peare_writer_status writeXzMember(peare_writer_handle_s* writer,
                                  const char* sourcePathUtf8,
                                  const char* entryNameUtf8) {
    if (!sourcePathUtf8 || !*sourcePathUtf8 || !entryNameUtf8 || !*entryNameUtf8)
        return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                    QStringLiteral("An XZ source and member name are required."));
    QByteArray safeName;
    if (!normalizedEntryName(entryNameUtf8, false, &safeName))
        return fail(writer, PEARE_WRITER_INVALID_ENTRY_NAME,
                    QStringLiteral("The XZ member name is invalid or unsafe."));

    QFile input(QString::fromUtf8(sourcePathUtf8));
    if (!input.open(QIODevice::ReadOnly))
        return fail(writer, PEARE_WRITER_IO_ERROR, input.errorString());
    initializeXzCrcTables();
    auto read = [&input](char* buffer, qint64 size) {
        return input.read(buffer, size);
    };
    ZipLzmaInput<decltype(read)> source;
    source.stream.Read = &ZipLzmaInput<decltype(read)>::readStream;
    source.read = &read;
    ZipLzmaOutput encoded;
    encoded.stream.Write = &ZipLzmaOutput::writeStream;
    encoded.device = writer->output.get();
    CXzProps properties;
    XzProps_Init(&properties);
    properties.lzma2Props.numTotalThreads = 1;
    properties.lzma2Props.lzmaProps.numThreads = 1;
    properties.numTotalThreads = 1;
    properties.checkId = writer->xzCheckId;
    CXzEncHandle encoder = XzEnc_Create(&kZipLzmaAllocator,
                                        &kZipLzmaAllocator);
    if (!encoder)
        return fail(writer, PEARE_WRITER_ALLOCATION_FAILED,
                    QStringLiteral("Could not allocate the XZ encoder."));
    SRes status = XzEnc_SetProps(encoder, &properties);
    if (status == SZ_OK)
        status = XzEnc_Encode(encoder, &encoded.stream, &source.stream, nullptr);
    XzEnc_Destroy(encoder);
    if (status != SZ_OK || input.error() != QFileDevice::NoError)
        return fail(writer, PEARE_WRITER_IO_ERROR,
                    !source.error.isEmpty() ? source.error
                    : !encoded.error.isEmpty() ? encoded.error
                    : input.error() != QFileDevice::NoError ? input.errorString()
                    : QStringLiteral("XZ compression failed (%1).").arg(status));
    return PEARE_WRITER_OK;
}

template <typename Read>
bool writeZipZstdPayload(QIODevice* output, Read read, quint32* crc,
                         quint64* inputSize, quint64* outputSize,
                         QString* error) {
    ZSTD_CCtx* encoder = ZSTD_createCCtx();
    if (!encoder) {
        if (error) *error = QStringLiteral("Could not allocate the ZIP Zstandard encoder.");
        return false;
    }
    size_t status = ZSTD_CCtx_setParameter(encoder, ZSTD_c_compressionLevel, 3);
    if (!ZSTD_isError(status))
        status = ZSTD_CCtx_setParameter(encoder, ZSTD_c_checksumFlag, 1);
    if (ZSTD_isError(status)) {
        if (error) *error = QString::fromLatin1(ZSTD_getErrorName(status));
        ZSTD_freeCCtx(encoder);
        return false;
    }
    std::vector<char> input(kChunkSize);
    std::vector<char> encoded(kChunkSize);
    quint32 checksum = 0xffffffffu;
    quint64 uncompressed = 0;
    quint64 compressed = 0;
    bool ok = true;
    while (ok) {
        const qint64 count = read(input.data(), qint64(input.size()));
        if (count < 0) {
            if (error) *error = QStringLiteral("Could not read ZIP Zstandard input.");
            ok = false;
            break;
        }
        if (count == 0) break;
        checksum = crc32Update(checksum, input.data(), size_t(count));
        uncompressed += quint64(count);
        ZSTD_inBuffer source{input.data(), size_t(count), 0};
        while (source.pos < source.size) {
            ZSTD_outBuffer destination{encoded.data(), encoded.size(), 0};
            status = ZSTD_compressStream2(encoder, &destination, &source, ZSTD_e_continue);
            if (ZSTD_isError(status) ||
                (destination.pos && !writeAll(output, encoded.data(), qint64(destination.pos)))) {
                if (error) *error = ZSTD_isError(status)
                    ? QString::fromLatin1(ZSTD_getErrorName(status)) : output->errorString();
                ok = false;
                break;
            }
            compressed += quint64(destination.pos);
        }
    }
    if (ok) {
        ZSTD_inBuffer source{nullptr, 0, 0};
        size_t remaining = 1;
        while (remaining != 0) {
            ZSTD_outBuffer destination{encoded.data(), encoded.size(), 0};
            remaining = ZSTD_compressStream2(encoder, &destination, &source, ZSTD_e_end);
            if (ZSTD_isError(remaining) ||
                (destination.pos && !writeAll(output, encoded.data(), qint64(destination.pos)))) {
                if (error) *error = ZSTD_isError(remaining)
                    ? QString::fromLatin1(ZSTD_getErrorName(remaining)) : output->errorString();
                ok = false;
                break;
            }
            compressed += quint64(destination.pos);
        }
    }
    ZSTD_freeCCtx(encoder);
    if (!ok) return false;
    *crc = checksum ^ 0xffffffffu;
    *inputSize = uncompressed;
    *outputSize = compressed;
    return true;
}

struct ZipPpmdOutput {
    IByteOut stream = {};
    QIODevice* device = nullptr;
    std::array<char, 65536> buffer = {};
    size_t used = 0;
    quint64 written = 0;
    bool failed = false;

    static void writeByte(IByteOutPtr stream, Byte value) {
        auto* self = reinterpret_cast<ZipPpmdOutput*>(const_cast<IByteOut*>(stream));
        if (self->failed) return;
        self->buffer[self->used++] = char(value);
        if (self->used == self->buffer.size()) self->flush();
    }

    void flush() {
        if (!used || failed) return;
        if (!writeAll(device, buffer.data(), qint64(used))) {
            failed = true;
            return;
        }
        written += used;
        used = 0;
    }
};

template <typename Read>
bool writeZipPpmdPayload(QIODevice* output, Read read, quint32* crc,
                         quint64* inputSize, quint64* outputSize,
                         QString* error) {
    constexpr unsigned order = 16;
    constexpr unsigned memorySize = 1u << 20;
    constexpr unsigned restoreMethod = PPMD8_RESTORE_METHOD_FREEZE;
    const quint16 parameters = quint16((restoreMethod << 12) | (order - 1));
    const char properties[] = {char(parameters & 0xff), char(parameters >> 8)};
    if (!writeAll(output, properties, sizeof(properties))) {
        if (error) *error = output->errorString();
        return false;
    }

    CPpmd8 model;
    Ppmd8_Construct(&model);
    if (!Ppmd8_Alloc(&model, memorySize, &kZipLzmaAllocator)) {
        if (error) *error = QStringLiteral("Could not allocate the ZIP PPMd model.");
        return false;
    }
    ZipPpmdOutput encoded;
    encoded.stream.Write = &ZipPpmdOutput::writeByte;
    encoded.device = output;
    encoded.written = sizeof(properties);
    model.Stream.Out = &encoded.stream;
    Ppmd8_Init(&model, order, restoreMethod);
    Ppmd8_Init_RangeEnc(&model);

    std::vector<char> input(kChunkSize);
    quint32 checksum = 0xffffffffu;
    quint64 uncompressed = 0;
    bool ok = true;
    while (true) {
        const qint64 count = read(input.data(), qint64(input.size()));
        if (count < 0) {
            if (error) *error = QStringLiteral("Could not read ZIP PPMd input.");
            ok = false;
            break;
        }
        if (count == 0) break;
        checksum = crc32Update(checksum, input.data(), size_t(count));
        uncompressed += quint64(count);
        for (qint64 index = 0; index < count; ++index)
            Ppmd8_EncodeSymbol(&model, static_cast<unsigned char>(input[size_t(index)]));
        if (encoded.failed) {
            if (error) *error = output->errorString();
            ok = false;
            break;
        }
    }
    if (ok) {
        Ppmd8_EncodeSymbol(&model, -1);
        Ppmd8_Flush_RangeEnc(&model);
        encoded.flush();
        if (encoded.failed) {
            if (error) *error = output->errorString();
            ok = false;
        }
    }
    Ppmd8_Free(&model, &kZipLzmaAllocator);
    if (!ok) return false;
    *crc = checksum ^ 0xffffffffu;
    *inputSize = uncompressed;
    *outputSize = encoded.written;
    return true;
}

peare_writer_status writeEntryData(peare_writer_handle_s* writer, Entry* entry,
                                   QFile* input) {
    entry->descriptor = true;
    entry->flags = quint16(entry->flags | 0x0800);
    entry->method = writer->zipMethod;
    if (entry->method == 14) entry->flags = quint16(entry->flags | 0x0002);
    entry->zip64Sizes = input->size() >= qint64(kZip32Max - 1024 * 1024);
    QIODevice* output = zipDevice(writer);
    if (!output || !writeLocalHeader(output, *entry, true))
        return fail(writer, PEARE_WRITER_IO_ERROR,
                    output ? output->errorString() : QStringLiteral("The ZIP output is unavailable."));
    QString writeError;
    QString inputError;
    const auto inputReader = [input, &inputError](char* buffer, qint64 size) {
                const qint64 count = input->read(buffer, size);
                if (count < 0) inputError = input->errorString();
                return count;
            };
    const bool payloadWritten = entry->method == 96
        ? writeZipJpegPayload(output, inputReader, &entry->crc, &entry->size,
                              &entry->compressedSize, &writeError)
        : entry->method == 14
        ? writeZipLzmaPayload(output, inputReader, &entry->crc, &entry->size,
                              &entry->compressedSize, &writeError)
        : entry->method == 95
            ? writeZipXzPayload(output, inputReader, &entry->crc, &entry->size,
                                &entry->compressedSize, &writeError)
            : entry->method == 93
                ? writeZipZstdPayload(output, inputReader, &entry->crc, &entry->size,
                                      &entry->compressedSize, &writeError)
            : entry->method == 98
                ? writeZipPpmdPayload(output, inputReader, &entry->crc, &entry->size,
                                      &entry->compressedSize, &writeError)
            : entry->method == 1
                ? writeZipShrinkPayload(output, inputReader, &entry->crc, &entry->size,
                                        &entry->compressedSize, &writeError)
            : entry->method == 94
                ? writeZipPackMp3Payload(output, inputReader, &entry->crc, &entry->size,
                                         &entry->compressedSize, &writeError)
            : entry->method == 97
                ? writeZipWavPackPayload(output, inputReader, &entry->crc, &entry->size,
                                         &entry->compressedSize, &writeError)
            : entry->method == 6
                ? writeZipImplodePayload(output, inputReader, &entry->crc, &entry->size,
                                         &entry->compressedSize, &writeError)
            : entry->method == 10
                ? writeZipDclImplodePayload(output, inputReader, &entry->crc, &entry->size,
                                            &entry->compressedSize, &writeError)
            : entry->method >= 2 && entry->method <= 5
                ? writeZipReducePayload(output, inputReader, entry->method - 1,
                    &entry->crc, &entry->size, &entry->compressedSize, &writeError)
            : writeZipPayload(output, entry->method, inputReader, &entry->crc,
                              &entry->size, &entry->compressedSize, &writeError);
    if (!payloadWritten)
        return fail(writer, PEARE_WRITER_IO_ERROR,
                    inputError.isEmpty() ? writeError : inputError);
    if (!input->atEnd() && input->error() != QFileDevice::NoError)
        return fail(writer, PEARE_WRITER_IO_ERROR, input->errorString());
    if (entry->size > kZip32Max && !entry->zip64Sizes)
        return fail(writer, PEARE_WRITER_IO_ERROR,
                    QStringLiteral("The source file grew while it was being archived."));
    entry->uncompressedSize = entry->size;
    QByteArray descriptor;
    append32(&descriptor, kDataDescriptorSignature);
    append32(&descriptor, entry->crc);
    if (entry->zip64Sizes) {
        append64(&descriptor, entry->compressedSize);
        append64(&descriptor, entry->size);
    } else {
        append32(&descriptor, quint32(entry->compressedSize));
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
    writer->zipMethod = 0;
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

size_t peare_writer_create_format_count(void) { return 17; }

peare_writer_status peare_writer_get_create_format(size_t index,
                                                    peare_writer_format* outFormat) {
    if (!outFormat) return PEARE_WRITER_INVALID_ARGUMENT;
    static const peare_writer_format formats[] = {
        {"zip", "ZIP archive", "zip"},
        {"gzip", "GZIP compressed file", "gz"},
        {"bzip2", "BZIP2 compressed file", "bz2"},
        {"zstd", "Zstandard compressed file", "zst"},
        {"xz", "XZ compressed file", "xz"},
        {"lzma", "LZMA compressed file", "lzma"},
        {"tar", "TAR archive", "tar"},
        {"cpio", "CPIO newc archive", "cpio"},
        {"ar", "Unix ar archive", "a"},
        {"fat", "FAT image", "img"},
        {"floppy", "Floppy image", "img"},
        {"raw-disk", "Raw partitioned disk", "img"},
        {"vhd", "VHD virtual disk", "vhd"},
        {"vhdx", "VHDX virtual disk", "vhdx"},
        {"vmdk", "VMDK virtual disk", "vmdk"},
        {"wad", "Doom WAD archive", "wad"},
        {"unix-compress", "Unix compress stream", "Z"}
    };
    if (index >= sizeof(formats) / sizeof(formats[0]))
        return PEARE_WRITER_INVALID_ARGUMENT;
    *outFormat = formats[index];
    return PEARE_WRITER_OK;
}

size_t peare_writer_create_variant_count(const char* formatIdUtf8) {
    if (!formatIdUtf8) return 0;
    if (std::strcmp(formatIdUtf8, "zip") == 0) return 18;
    if (std::strcmp(formatIdUtf8, "gzip") == 0) return 1;
    if (std::strcmp(formatIdUtf8, "bzip2") == 0) return 9;
    if (std::strcmp(formatIdUtf8, "zstd") == 0) return 27;
    if (std::strcmp(formatIdUtf8, "xz") == 0) return 4;
    if (std::strcmp(formatIdUtf8, "lzma") == 0) return 9;
    if (std::strcmp(formatIdUtf8, "unix-compress") == 0) return 16;
    if (std::strcmp(formatIdUtf8, "tar") == 0) return 1;
    if (std::strcmp(formatIdUtf8, "cpio") == 0) return 6;
    if (std::strcmp(formatIdUtf8, "ar") == 0) return 2;
    if (std::strcmp(formatIdUtf8, "wad") == 0) return 2;
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
    static const peare_writer_variant zipVariants[] = {
        {"deflate", "Deflate"}, {"bzip2", "BZip2"}, {"store", "Store"},
        {"lzma", "LZMA"}, {"xz", "XZ (ZIPX)"}, {"zstd", "Zstandard (ZIPX)"},
        {"ppmd", "PPMd (ZIPX)"}, {"shrink", "Shrink"},
        {"deflate64", "Deflate64"}, {"mp3", "MP3 (ZIPX)"},
        {"wavpack", "WavPack (ZIPX)"}, {"implode", "Implode"},
        {"dcl-implode", "DCL Implode"}, {"reduce-1", "Reduce 1"},
        {"reduce-2", "Reduce 2"}, {"reduce-3", "Reduce 3"},
        {"reduce-4", "Reduce 4"}, {"jpeg", "JPEG (ZIPX)"}
    };
    static const peare_writer_variant gzipVariants[] = {
        {"deflate", "Deflate"}
    };
    static const peare_writer_variant bzip2Variants[] = {
        {"level-1", "Level 1"}, {"level-2", "Level 2"},
        {"level-3", "Level 3"}, {"level-4", "Level 4"},
        {"level-5", "Level 5"}, {"level-6", "Level 6"},
        {"level-7", "Level 7"}, {"level-8", "Level 8"},
        {"level-9", "Level 9"}
    };
    static const peare_writer_variant zstdVariants[] = {
        {"fast-5", "Fastest (-5)"}, {"fast-4", "Fast (-4)"},
        {"fast-3", "Fast (-3)"}, {"fast-2", "Fast (-2)"},
        {"fast-1", "Fast (-1)"},
        {"level-1", "Level 1"}, {"level-2", "Level 2"},
        {"level-3", "Level 3"}, {"level-4", "Level 4"},
        {"level-5", "Level 5"}, {"level-6", "Level 6"},
        {"level-7", "Level 7"}, {"level-8", "Level 8"},
        {"level-9", "Level 9"}, {"level-10", "Level 10"},
        {"level-11", "Level 11"}, {"level-12", "Level 12"},
        {"level-13", "Level 13"}, {"level-14", "Level 14"},
        {"level-15", "Level 15"}, {"level-16", "Level 16"},
        {"level-17", "Level 17"}, {"level-18", "Level 18"},
        {"level-19", "Level 19"}, {"level-20", "Level 20"},
        {"level-21", "Level 21"}, {"level-22", "Level 22"}
    };
    static const peare_writer_variant xzVariants[] = {
        {"none", "No check"}, {"crc32", "CRC32"},
        {"crc64", "CRC64"}, {"sha256", "SHA-256"}
    };
    static const peare_writer_variant lzmaVariants[] = {
        {"level-1", "Level 1"}, {"level-2", "Level 2"},
        {"level-3", "Level 3"}, {"level-4", "Level 4"},
        {"level-5", "Level 5"}, {"level-6", "Level 6"},
        {"level-7", "Level 7"}, {"level-8", "Level 8"},
        {"level-9", "Level 9"}
    };
    static const peare_writer_variant unixCompressVariants[] = {
        {"block-9", "Block mode, 9 bits"},
        {"block-10", "Block mode, 10 bits"},
        {"block-11", "Block mode, 11 bits"},
        {"block-12", "Block mode, 12 bits"},
        {"block-13", "Block mode, 13 bits"},
        {"block-14", "Block mode, 14 bits"},
        {"block-15", "Block mode, 15 bits"},
        {"block-16", "Block mode, 16 bits"},
        {"legacy-9", "Non-block mode, 9 bits"},
        {"legacy-10", "Non-block mode, 10 bits"},
        {"legacy-11", "Non-block mode, 11 bits"},
        {"legacy-12", "Non-block mode, 12 bits"},
        {"legacy-13", "Non-block mode, 13 bits"},
        {"legacy-14", "Non-block mode, 14 bits"},
        {"legacy-15", "Non-block mode, 15 bits"},
        {"legacy-16", "Non-block mode, 16 bits"}
    };
    static const peare_writer_variant tarVariants[] = { {"ustar", "USTAR"} };
    static const peare_writer_variant cpioVariants[] = {
        {"newc", "SVR4 newc"}, {"crc", "SVR4 CRC"},
        {"odc", "POSIX odc"}, {"afio-large", "afio large ASCII"},
        {"binary-le", "Old binary little-endian"},
        {"binary-be", "Old binary big-endian"}
    };
    static const peare_writer_variant arVariants[] = {
        {"gnu", "GNU ar"}, {"bsd", "BSD ar"}
    };
    static const peare_writer_variant wadVariants[] = {
        {"iwad", "Doom internal WAD (IWAD)"},
        {"pwad", "Doom patch WAD (PWAD)"}
    };
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
    if (std::strcmp(formatIdUtf8, "zip") == 0 && index < 18) {
        *outVariant = zipVariants[index];
        return PEARE_WRITER_OK;
    }
    if (std::strcmp(formatIdUtf8, "gzip") == 0 && index == 0) {
        *outVariant = gzipVariants[0];
        return PEARE_WRITER_OK;
    }
    if (std::strcmp(formatIdUtf8, "bzip2") == 0 && index < 9) {
        *outVariant = bzip2Variants[index];
        return PEARE_WRITER_OK;
    }
    if (std::strcmp(formatIdUtf8, "zstd") == 0 && index < 27) {
        *outVariant = zstdVariants[index];
        return PEARE_WRITER_OK;
    }
    if (std::strcmp(formatIdUtf8, "xz") == 0 && index < 4) {
        *outVariant = xzVariants[index];
        return PEARE_WRITER_OK;
    }
    if (std::strcmp(formatIdUtf8, "lzma") == 0 && index < 9) {
        *outVariant = lzmaVariants[index];
        return PEARE_WRITER_OK;
    }
    if (std::strcmp(formatIdUtf8, "unix-compress") == 0 && index < 16) {
        *outVariant = unixCompressVariants[index];
        return PEARE_WRITER_OK;
    }
    if (std::strcmp(formatIdUtf8, "tar") == 0 && index == 0) {
        *outVariant = tarVariants[0];
        return PEARE_WRITER_OK;
    }
    if (std::strcmp(formatIdUtf8, "cpio") == 0 && index < 6) {
        *outVariant = cpioVariants[index];
        return PEARE_WRITER_OK;
    }
    if (std::strcmp(formatIdUtf8, "ar") == 0 && index < 2) {
        *outVariant = arVariants[index];
        return PEARE_WRITER_OK;
    }
    if (std::strcmp(formatIdUtf8, "wad") == 0 && index < 2) {
        *outVariant = wadVariants[index];
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
        if (variant && std::strcmp(variant, "store") != 0 &&
            std::strcmp(variant, "deflate") != 0 &&
            std::strcmp(variant, "bzip2") != 0 &&
            std::strcmp(variant, "lzma") != 0 &&
            std::strcmp(variant, "xz") != 0 &&
            std::strcmp(variant, "zstd") != 0 &&
            std::strcmp(variant, "ppmd") != 0 &&
            std::strcmp(variant, "shrink") != 0 &&
            std::strcmp(variant, "deflate64") != 0 &&
            std::strcmp(variant, "mp3") != 0 &&
            std::strcmp(variant, "wavpack") != 0 &&
            std::strcmp(variant, "implode") != 0 &&
            std::strcmp(variant, "dcl-implode") != 0 &&
            std::strcmp(variant, "reduce-1") != 0 &&
            std::strcmp(variant, "reduce-2") != 0 &&
            std::strcmp(variant, "reduce-3") != 0 &&
            std::strcmp(variant, "reduce-4") != 0 &&
            std::strcmp(variant, "jpeg") != 0)
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("The requested ZIP variant is not supported."), false);
        const peare_writer_status status = peare_writer_zip_begin(writer, outputPathUtf8);
        if (status == PEARE_WRITER_OK && variant &&
            std::strcmp(variant, "deflate") == 0)
            writer->zipMethod = 8;
        else if (status == PEARE_WRITER_OK && variant &&
                 std::strcmp(variant, "bzip2") == 0)
            writer->zipMethod = 12;
        else if (status == PEARE_WRITER_OK && variant &&
                 std::strcmp(variant, "lzma") == 0)
            writer->zipMethod = 14;
        else if (status == PEARE_WRITER_OK && variant &&
                 std::strcmp(variant, "xz") == 0)
            writer->zipMethod = 95;
        else if (status == PEARE_WRITER_OK && variant &&
                 std::strcmp(variant, "zstd") == 0)
            writer->zipMethod = 93;
        else if (status == PEARE_WRITER_OK && variant &&
                 std::strcmp(variant, "ppmd") == 0)
            writer->zipMethod = 98;
        else if (status == PEARE_WRITER_OK && variant &&
                 std::strcmp(variant, "shrink") == 0)
            writer->zipMethod = 1;
        else if (status == PEARE_WRITER_OK && variant &&
                 std::strcmp(variant, "deflate64") == 0)
            writer->zipMethod = 9;
        else if (status == PEARE_WRITER_OK && variant &&
                 std::strcmp(variant, "mp3") == 0)
            writer->zipMethod = 94;
        else if (status == PEARE_WRITER_OK && variant &&
                 std::strcmp(variant, "wavpack") == 0)
            writer->zipMethod = 97;
        else if (status == PEARE_WRITER_OK && variant &&
                 std::strcmp(variant, "jpeg") == 0)
            writer->zipMethod = 96;
        else if (status == PEARE_WRITER_OK && variant &&
                 std::strcmp(variant, "implode") == 0)
            writer->zipMethod = 6;
        else if (status == PEARE_WRITER_OK && variant &&
                 std::strcmp(variant, "dcl-implode") == 0)
            writer->zipMethod = 10;
        else if (status == PEARE_WRITER_OK && variant &&
                 std::strncmp(variant, "reduce-", 7) == 0)
            writer->zipMethod = quint16(1 + variant[7] - '0');
        return status;
    }
    if (std::strcmp(formatIdUtf8, "gzip") == 0) {
        if (variant && std::strcmp(variant, "deflate") != 0)
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("The requested GZIP variant is not supported."), false);
        return cBoundary(writer, [&]() -> peare_writer_status {
            if (writer->active)
                return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                            QStringLiteral("A writer operation is already active."), false);
            writer->error.clear();
            writer->errorUtf8.clear();
            writer->output.reset(new (std::nothrow) QSaveFile(QString::fromUtf8(outputPathUtf8)));
            if (!writer->output || !writer->output->open(QIODevice::WriteOnly)) {
                const QString error = writer->output ? writer->output->errorString()
                    : QStringLiteral("Could not allocate the GZIP output.");
                writer->output.reset();
                return fail(writer, PEARE_WRITER_IO_ERROR, error, false);
            }
            writer->active = true;
            writer->updateMode = false;
            writer->mode = peare_writer_handle_s::Mode::GzipCreate;
            return PEARE_WRITER_OK;
        });
    }
    if (std::strcmp(formatIdUtf8, "bzip2") == 0) {
        int level = 9;
        if (variant) {
            if (std::strlen(variant) != 7 || std::strncmp(variant, "level-", 6) != 0 ||
                variant[6] < '1' || variant[6] > '9' || variant[7] != '\0')
                return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                            QStringLiteral("The requested BZIP2 level is invalid."), false);
            level = variant[6] - '0';
        }
        return cBoundary(writer, [&]() -> peare_writer_status {
            if (writer->active)
                return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                            QStringLiteral("A writer operation is already active."), false);
            writer->error.clear();
            writer->errorUtf8.clear();
            writer->output.reset(new (std::nothrow) QSaveFile(QString::fromUtf8(outputPathUtf8)));
            if (!writer->output || !writer->output->open(QIODevice::WriteOnly)) {
                const QString error = writer->output ? writer->output->errorString()
                    : QStringLiteral("Could not allocate the BZIP2 output.");
                writer->output.reset();
                return fail(writer, PEARE_WRITER_IO_ERROR, error, false);
            }
            writer->bzip2Level = level;
            writer->active = true;
            writer->updateMode = false;
            writer->mode = peare_writer_handle_s::Mode::Bzip2Create;
            return PEARE_WRITER_OK;
        });
    }
    if (std::strcmp(formatIdUtf8, "zstd") == 0) {
        int level = 3;
        if (variant) {
            char* end = nullptr;
            long parsed = 0;
            if (std::strncmp(variant, "fast-", 5) == 0) {
                parsed = std::strtol(variant + 5, &end, 10);
                if (!end || *end || parsed < 1 || parsed > 5)
                    return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                                QStringLiteral("The requested Zstandard level is invalid."), false);
                parsed = -parsed;
            } else if (std::strncmp(variant, "level-", 6) == 0) {
                parsed = std::strtol(variant + 6, &end, 10);
                if (!end || *end || parsed < 1 || parsed > 22)
                    return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                                QStringLiteral("The requested Zstandard level is invalid."), false);
            } else {
                return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                            QStringLiteral("The requested Zstandard level is invalid."), false);
            }
            level = int(parsed);
        }
        return cBoundary(writer, [&]() -> peare_writer_status {
            if (writer->active)
                return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                            QStringLiteral("A writer operation is already active."), false);
            writer->error.clear();
            writer->errorUtf8.clear();
            writer->output.reset(new (std::nothrow) QSaveFile(QString::fromUtf8(outputPathUtf8)));
            if (!writer->output || !writer->output->open(QIODevice::WriteOnly)) {
                const QString error = writer->output ? writer->output->errorString()
                    : QStringLiteral("Could not allocate the Zstandard output.");
                writer->output.reset();
                return fail(writer, PEARE_WRITER_IO_ERROR, error, false);
            }
            writer->zstdLevel = level;
            writer->active = true;
            writer->updateMode = false;
            writer->mode = peare_writer_handle_s::Mode::ZstdCreate;
            return PEARE_WRITER_OK;
        });
    }
    if (std::strcmp(formatIdUtf8, "xz") == 0) {
        unsigned checkId = XZ_CHECK_CRC64;
        if (!variant || std::strcmp(variant, "crc64") == 0)
            checkId = XZ_CHECK_CRC64;
        else if (std::strcmp(variant, "none") == 0)
            checkId = XZ_CHECK_NO;
        else if (std::strcmp(variant, "crc32") == 0)
            checkId = XZ_CHECK_CRC32;
        else if (std::strcmp(variant, "sha256") == 0)
            checkId = XZ_CHECK_SHA256;
        else
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("The requested XZ integrity check is invalid."), false);
        return cBoundary(writer, [&]() -> peare_writer_status {
            if (writer->active)
                return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                            QStringLiteral("A writer operation is already active."), false);
            writer->error.clear();
            writer->errorUtf8.clear();
            writer->output.reset(new (std::nothrow) QSaveFile(QString::fromUtf8(outputPathUtf8)));
            if (!writer->output || !writer->output->open(QIODevice::WriteOnly)) {
                const QString error = writer->output ? writer->output->errorString()
                    : QStringLiteral("Could not allocate the XZ output.");
                writer->output.reset();
                return fail(writer, PEARE_WRITER_IO_ERROR, error, false);
            }
            writer->xzCheckId = checkId;
            writer->active = true;
            writer->updateMode = false;
            writer->mode = peare_writer_handle_s::Mode::XzCreate;
            return PEARE_WRITER_OK;
        });
    }
    if (std::strcmp(formatIdUtf8, "lzma") == 0) {
        int level = 5;
        if (variant) {
            if (std::strlen(variant) != 7 || std::strncmp(variant, "level-", 6) != 0 ||
                variant[6] < '1' || variant[6] > '9')
                return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                            QStringLiteral("The requested LZMA level is invalid."), false);
            level = variant[6] - '0';
        }
        return cBoundary(writer, [&]() -> peare_writer_status {
            if (writer->active)
                return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                            QStringLiteral("A writer operation is already active."), false);
            writer->error.clear();
            writer->errorUtf8.clear();
            writer->output.reset(new (std::nothrow) QSaveFile(QString::fromUtf8(outputPathUtf8)));
            if (!writer->output || !writer->output->open(QIODevice::WriteOnly)) {
                const QString error = writer->output ? writer->output->errorString()
                    : QStringLiteral("Could not allocate the LZMA output.");
                writer->output.reset();
                return fail(writer, PEARE_WRITER_IO_ERROR, error, false);
            }
            writer->lzmaLevel = level;
            writer->active = true;
            writer->updateMode = false;
            writer->mode = peare_writer_handle_s::Mode::LzmaAloneCreate;
            return PEARE_WRITER_OK;
        });
    }
    if (std::strcmp(formatIdUtf8, "unix-compress") == 0) {
        if (!variant)
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("A Unix compress width and mode are required."), false);
        const bool blockMode = std::strncmp(variant, "block-", 6) == 0;
        const bool legacyMode = std::strncmp(variant, "legacy-", 7) == 0;
        const char* widthText = blockMode ? variant + 6 : legacyMode ? variant + 7 : nullptr;
        if (!widthText || !*widthText)
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("The Unix compress mode is invalid."), false);
        char* end = nullptr;
        const long maxBits = std::strtol(widthText, &end, 10);
        if (!end || *end || maxBits < 9 || maxBits > 16)
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("The Unix compress code width is invalid."), false);
        return cBoundary(writer, [&]() -> peare_writer_status {
            if (writer->active)
                return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                            QStringLiteral("A writer operation is already active."), false);
            writer->error.clear();
            writer->errorUtf8.clear();
            writer->output.reset(new (std::nothrow) QSaveFile(QString::fromUtf8(outputPathUtf8)));
            if (!writer->output || !writer->output->open(QIODevice::WriteOnly)) {
                const QString error = writer->output ? writer->output->errorString()
                    : QStringLiteral("Could not allocate the Unix compress output.");
                writer->output.reset();
                return fail(writer, PEARE_WRITER_IO_ERROR, error, false);
            }
            writer->unixCompressMaxBits = int(maxBits);
            writer->unixCompressBlockMode = blockMode;
            writer->active = true;
            writer->updateMode = false;
            writer->mode = peare_writer_handle_s::Mode::UnixCompressCreate;
            return PEARE_WRITER_OK;
        });
    }
    if (std::strcmp(formatIdUtf8, "cpio") == 0) {
        CpioFormat format = CpioFormat::Newc;
        if (variant && std::strcmp(variant, "newc") == 0) format = CpioFormat::Newc;
        else if (variant && std::strcmp(variant, "crc") == 0) format = CpioFormat::Crc;
        else if (variant && std::strcmp(variant, "odc") == 0) format = CpioFormat::Odc;
        else if (variant && std::strcmp(variant, "afio-large") == 0)
            format = CpioFormat::AfioLarge;
        else if (variant && std::strcmp(variant, "binary-le") == 0)
            format = CpioFormat::BinaryLe;
        else if (variant && std::strcmp(variant, "binary-be") == 0)
            format = CpioFormat::BinaryBe;
        else if (variant)
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("The requested CPIO variant is not supported."), false);
        return cBoundary(writer, [&]() -> peare_writer_status {
            if (writer->active)
                return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                            QStringLiteral("A writer operation is already active."), false);
            writer->error.clear();
            writer->errorUtf8.clear();
            writer->cpioEntries.clear();
            writer->names.clear();
            writer->updatePath.clear();
            writer->cpioOpaqueOffset = 0;
            writer->cpioOpaqueSize = 0;
            writer->cpioStage.reset(new (std::nothrow) QTemporaryFile);
            if (!writer->cpioStage || !writer->cpioStage->open())
                return fail(writer, PEARE_WRITER_IO_ERROR,
                    writer->cpioStage ? writer->cpioStage->errorString()
                                      : QStringLiteral("Could not allocate CPIO staging."), false);
            writer->output.reset(new (std::nothrow) QSaveFile(QString::fromUtf8(outputPathUtf8)));
            if (!writer->output || !writer->output->open(QIODevice::WriteOnly)) {
                const QString error = writer->output ? writer->output->errorString()
                    : QStringLiteral("Could not allocate the CPIO output.");
                writer->output.reset();
                writer->cpioStage.reset();
                return fail(writer, PEARE_WRITER_IO_ERROR, error, false);
            }
            writer->active = true;
            writer->updateMode = false;
            writer->archiveDirty = true;
            writer->cpioFormat = format;
            writer->mode = peare_writer_handle_s::Mode::CpioCreate;
            return PEARE_WRITER_OK;
        });
    }
    if (std::strcmp(formatIdUtf8, "ar") == 0) {
        ArFormat format = ArFormat::Gnu;
        if (variant && std::strcmp(variant, "gnu") == 0) format = ArFormat::Gnu;
        else if (variant && std::strcmp(variant, "bsd") == 0) format = ArFormat::Bsd;
        else if (variant)
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("The requested ar variant is not supported."), false);
        return cBoundary(writer, [&]() -> peare_writer_status {
            if (writer->active)
                return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                            QStringLiteral("A writer operation is already active."), false);
            writer->error.clear();
            writer->errorUtf8.clear();
            writer->arEntries.clear();
            writer->names.clear();
            writer->updatePath.clear();
            writer->arFormat = format;
            writer->arStage.reset(new (std::nothrow) QTemporaryFile);
            if (!writer->arStage || !writer->arStage->open())
                return fail(writer, PEARE_WRITER_IO_ERROR,
                    writer->arStage ? writer->arStage->errorString()
                                    : QStringLiteral("Could not allocate ar staging."), false);
            writer->output.reset(new (std::nothrow) QSaveFile(QString::fromUtf8(outputPathUtf8)));
            if (!writer->output || !writer->output->open(QIODevice::WriteOnly)) {
                const QString error = writer->output ? writer->output->errorString()
                    : QStringLiteral("Could not allocate ar output.");
                writer->output.reset();
                writer->arStage.reset();
                return fail(writer, PEARE_WRITER_IO_ERROR, error, false);
            }
            writer->active = true;
            writer->updateMode = false;
            writer->archiveDirty = true;
            writer->mode = peare_writer_handle_s::Mode::ArCreate;
            return PEARE_WRITER_OK;
        });
    }
    if (std::strcmp(formatIdUtf8, "wad") == 0) {
        WadFormat format = WadFormat::Pwad;
        if (variant && std::strcmp(variant, "iwad") == 0) format = WadFormat::Iwad;
        else if (variant && std::strcmp(variant, "pwad") != 0)
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("The requested WAD variant is not supported."), false);
        return cBoundary(writer, [&]() -> peare_writer_status {
            if (writer->active)
                return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                            QStringLiteral("A writer operation is already active."), false);
            writer->error.clear();
            writer->errorUtf8.clear();
            writer->wadEntries.clear();
            writer->wadFormat = format;
            writer->wadStage.reset(new (std::nothrow) QTemporaryFile);
            if (!writer->wadStage || !writer->wadStage->open())
                return fail(writer, PEARE_WRITER_IO_ERROR,
                    writer->wadStage ? writer->wadStage->errorString()
                                     : QStringLiteral("Could not allocate WAD staging."), false);
            writer->output.reset(new (std::nothrow) QSaveFile(QString::fromUtf8(outputPathUtf8)));
            if (!writer->output || !writer->output->open(QIODevice::WriteOnly)) {
                const QString error = writer->output ? writer->output->errorString()
                    : QStringLiteral("Could not allocate WAD output.");
                writer->output.reset();
                writer->wadStage.reset();
                return fail(writer, PEARE_WRITER_IO_ERROR, error, false);
            }
            writer->active = true;
            writer->updateMode = false;
            writer->archiveDirty = true;
            writer->mode = peare_writer_handle_s::Mode::WadCreate;
            return PEARE_WRITER_OK;
        });
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
        probe.tarEntries.clear();
        probe.names.clear();
        if (loadExistingCpio(&probe, &input) == PEARE_WRITER_OK) return 1;
        probe.cpioEntries.clear();
        probe.names.clear();
        if (loadExistingAr(&probe, &input) == PEARE_WRITER_OK) return 1;
        probe.arEntries.clear();
        probe.names.clear();
        if (loadExistingWad(&probe, &input) == PEARE_WRITER_OK) return 1;
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
            if (input.isOpen() && loadExistingTar(&probe, &input) == PEARE_WRITER_OK) {
                *outCapabilities |= PEARE_WRITER_CAP_CREATE_DIRECTORY |
                                    PEARE_WRITER_CAP_UPDATE_FILE |
                                    PEARE_WRITER_CAP_RENAME_ENTRY |
                                    PEARE_WRITER_CAP_DELETE_ENTRY;
            } else {
                probe.tarEntries.clear();
                probe.names.clear();
                if (input.isOpen() && loadExistingCpio(&probe, &input) == PEARE_WRITER_OK)
                    *outCapabilities |= PEARE_WRITER_CAP_CREATE_DIRECTORY |
                                        PEARE_WRITER_CAP_UPDATE_FILE |
                                        PEARE_WRITER_CAP_RENAME_ENTRY |
                                        PEARE_WRITER_CAP_DELETE_ENTRY;
                else if (peare_writer_add_target_count(containerPathUtf8))
                    *outCapabilities |= PEARE_WRITER_CAP_CREATE_DIRECTORY |
                                        PEARE_WRITER_CAP_UPDATE_FILE |
                                        PEARE_WRITER_CAP_RENAME_ENTRY |
                                        PEARE_WRITER_CAP_DELETE_ENTRY;
                else {
                    probe.cpioEntries.clear();
                    probe.names.clear();
                    if (input.isOpen() && loadExistingAr(&probe, &input) == PEARE_WRITER_OK) {
                        *outCapabilities |= PEARE_WRITER_CAP_UPDATE_FILE |
                                            PEARE_WRITER_CAP_RENAME_ENTRY |
                                            PEARE_WRITER_CAP_DELETE_ENTRY;
                    } else {
                        probe.arEntries.clear();
                        probe.names.clear();
                        if (input.isOpen() && loadExistingWad(&probe, &input) == PEARE_WRITER_OK)
                            *outCapabilities |= PEARE_WRITER_CAP_UPDATE_FILE |
                                                PEARE_WRITER_CAP_RENAME_ENTRY |
                                                PEARE_WRITER_CAP_DELETE_ENTRY;
                    }
                }
            }
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
        if (parsed == PEARE_WRITER_OK) writer->zipMethod = 8;
        if (parsed != PEARE_WRITER_OK) {
            writer->entries.clear();
            writer->names.clear();
            writer->archiveComment.clear();
            parsed = loadExistingTar(writer, &input);
            mode = peare_writer_handle_s::Mode::TarUpdate;
            if (parsed != PEARE_WRITER_OK) {
                writer->entries.clear();
                writer->tarEntries.clear();
                writer->names.clear();
                writer->archiveComment.clear();
                parsed = loadExistingCpio(writer, &input);
                mode = peare_writer_handle_s::Mode::CpioUpdate;
                if (parsed != PEARE_WRITER_OK) {
                    writer->entries.clear();
                    writer->tarEntries.clear();
                    writer->cpioEntries.clear();
                    writer->names.clear();
                    writer->archiveComment.clear();
                    parsed = loadExistingAr(writer, &input);
                    mode = peare_writer_handle_s::Mode::ArUpdate;
                    if (parsed != PEARE_WRITER_OK) {
                        writer->arEntries.clear();
                        writer->names.clear();
                        parsed = loadExistingWad(writer, &input);
                        mode = peare_writer_handle_s::Mode::WadUpdate;
                        if (parsed != PEARE_WRITER_OK) {
                            writer->wadEntries.clear();
                            writer->wadStage.reset();
                            writer->fatTargetSpecified = false;
                            input.close();
                            return beginDiskImageUpdate(writer, QString::fromUtf8(containerPathUtf8),
                                                        false, 0);
                        }
                    }
                }
            }
        }
        writer->error.clear();
        writer->errorUtf8.clear();
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
        } else if (mode == peare_writer_handle_s::Mode::CpioUpdate) {
            writer->updatePath = QString::fromUtf8(containerPathUtf8);
            writer->cpioStage.reset(new (std::nothrow) QTemporaryFile);
            if (!writer->cpioStage || !writer->cpioStage->open()) {
                const QString error = writer->cpioStage ? writer->cpioStage->errorString()
                    : QStringLiteral("Could not allocate CPIO staging.");
                writer->cpioStage.reset();
                writer->updatePath.clear();
                return fail(writer, PEARE_WRITER_IO_ERROR, error, false);
            }
            stagedOutput = writer->cpioStage.get();
        } else if (mode == peare_writer_handle_s::Mode::ArUpdate) {
            writer->updatePath = QString::fromUtf8(containerPathUtf8);
            writer->arStage.reset(new (std::nothrow) QTemporaryFile);
            if (!writer->arStage || !writer->arStage->open()) {
                const QString error = writer->arStage ? writer->arStage->errorString()
                    : QStringLiteral("Could not allocate ar staging.");
                writer->arStage.reset();
                writer->updatePath.clear();
                return fail(writer, PEARE_WRITER_IO_ERROR, error, false);
            }
            stagedOutput = writer->arStage.get();
        } else if (mode == peare_writer_handle_s::Mode::WadUpdate) {
            writer->updatePath = QString::fromUtf8(containerPathUtf8);
            stagedOutput = writer->wadStage.get();
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
        if (mode == peare_writer_handle_s::Mode::WadUpdate) return PEARE_WRITER_OK;
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
        if (mode == peare_writer_handle_s::Mode::CpioUpdate && writer->cpioStage &&
            !writer->cpioStage->seek(writer->cpioStage->size()))
            return fail(writer, PEARE_WRITER_IO_ERROR, writer->cpioStage->errorString());
        if (mode == peare_writer_handle_s::Mode::ArUpdate && writer->arStage &&
            !writer->arStage->seek(writer->arStage->size()))
            return fail(writer, PEARE_WRITER_IO_ERROR, writer->arStage->errorString());
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
    if (writer->mode == peare_writer_handle_s::Mode::CpioCreate ||
        writer->mode == peare_writer_handle_s::Mode::CpioUpdate)
        return cBoundary(writer, [&] { return writeCpioFile(writer, sourcePathUtf8, entryNameUtf8); });
    if (writer->mode == peare_writer_handle_s::Mode::ArCreate ||
        writer->mode == peare_writer_handle_s::Mode::ArUpdate)
        return cBoundary(writer, [&] { return writeArFile(writer, sourcePathUtf8, entryNameUtf8); });
    if (writer->mode == peare_writer_handle_s::Mode::WadCreate ||
        writer->mode == peare_writer_handle_s::Mode::WadUpdate)
        return cBoundary(writer, [&] {
            if (!sourcePathUtf8 || !*sourcePathUtf8)
                return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                            QStringLiteral("A source file path is required."), false);
            return appendWadFile(writer, sourcePathUtf8, entryNameUtf8);
        });
    if (writer->mode == peare_writer_handle_s::Mode::GzipCreate)
        return cBoundary(writer, [&] {
            return writeGzipMember(writer, sourcePathUtf8, entryNameUtf8);
        });
    if (writer->mode == peare_writer_handle_s::Mode::Bzip2Create)
        return cBoundary(writer, [&] {
            return writeBzip2Member(writer, sourcePathUtf8, entryNameUtf8);
        });
    if (writer->mode == peare_writer_handle_s::Mode::ZstdCreate)
        return cBoundary(writer, [&] {
            return writeZstdMember(writer, sourcePathUtf8, entryNameUtf8);
        });
    if (writer->mode == peare_writer_handle_s::Mode::XzCreate)
        return cBoundary(writer, [&] {
            return writeXzMember(writer, sourcePathUtf8, entryNameUtf8);
        });
    if (writer->mode == peare_writer_handle_s::Mode::LzmaAloneCreate)
        return cBoundary(writer, [&] {
            return writeLzmaAloneMember(writer, sourcePathUtf8, entryNameUtf8);
        });
    if (writer->mode == peare_writer_handle_s::Mode::UnixCompressCreate)
        return cBoundary(writer, [&] {
            return writeUnixCompressMember(writer, sourcePathUtf8, entryNameUtf8);
        });
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
    if (writer->mode == peare_writer_handle_s::Mode::CpioCreate ||
        writer->mode == peare_writer_handle_s::Mode::CpioUpdate)
        return cBoundary(writer, [&] { return writeCpioDirectory(writer, entryNameUtf8); });
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
    if (writer->mode == peare_writer_handle_s::Mode::WadUpdate) {
        const qint64 index = findWadEntry(writer, entryNameUtf8);
        if (index == -2)
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("The WAD lump name is ambiguous; use its entry ID."), false);
        return cBoundary(writer, [&] { return replaceWadEntry(writer, index, sourcePathUtf8); });
    }
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
    if (writer->mode == peare_writer_handle_s::Mode::CpioUpdate) {
        QByteArray encoded;
        if (!normalizedEntryName(entryNameUtf8, false, &encoded))
            return fail(writer, PEARE_WRITER_INVALID_ENTRY_NAME,
                        QStringLiteral("The CPIO entry name is invalid or unsafe."), false);
        const QString key = QString::fromUtf8(encoded);
        auto found = std::find_if(writer->cpioEntries.begin(), writer->cpioEntries.end(),
                                  [&](const CpioEntry& entry) { return entry.key == key; });
        if (found == writer->cpioEntries.end())
            return fail(writer, PEARE_WRITER_ENTRY_NOT_FOUND,
                        QStringLiteral("The CPIO file to replace was not found."), false);
        if ((found->mode & 0170000u) != 0100000u)
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("Only regular CPIO files can be replaced."), false);
        CpioEntry replacement = *found;
        replacement.inode = nextCpioInode(writer);
        if (!replacement.inode)
            return fail(writer, PEARE_WRITER_UNSUPPORTED_LIMIT,
                        QStringLiteral("The CPIO inode number limit was reached."), false);
        replacement.deviceMajor = 0;
        replacement.deviceMinor = 0;
        replacement.links = 1;
        if (!appendCpioSource(writer, sourcePathUtf8, &replacement))
            return fail(writer, PEARE_WRITER_IO_ERROR,
                        QStringLiteral("Could not stage the replacement CPIO payload."), false);
        *found = std::move(replacement);
        writer->archiveDirty = true;
        return PEARE_WRITER_OK;
    }
    if (writer->mode == peare_writer_handle_s::Mode::ArUpdate) {
        QByteArray name;
        if (!arFlatName(entryNameUtf8, &name))
            return fail(writer, PEARE_WRITER_INVALID_ENTRY_NAME,
                        QStringLiteral("The ar member name is invalid."), false);
        const QString key = QString::fromUtf8(name);
        auto found = std::find_if(writer->arEntries.begin(), writer->arEntries.end(),
            [&](const ArEntry& entry) { return entry.key == key; });
        if (found == writer->arEntries.end())
            return fail(writer, PEARE_WRITER_ENTRY_NOT_FOUND,
                        QStringLiteral("The ar member to replace was not found."), false);
        ArEntry replacement = *found;
        QFile source(QString::fromUtf8(sourcePathUtf8));
        if (!source.open(QIODevice::ReadOnly) || source.size() < 0)
            return fail(writer, PEARE_WRITER_IO_ERROR, source.errorString(), false);
        replacement.dataOffset = writer->arStage->size();
        replacement.size = quint64(source.size());
        replacement.timestamp = quint64(QDateTime::currentSecsSinceEpoch());
        if (!writer->arStage->seek(replacement.dataOffset))
            return fail(writer, PEARE_WRITER_IO_ERROR, writer->arStage->errorString());
        std::vector<char> buffer(kChunkSize);
        quint64 copied = 0;
        while (true) {
            const qint64 count = source.read(buffer.data(), qint64(buffer.size()));
            if (count < 0) return fail(writer, PEARE_WRITER_IO_ERROR, source.errorString());
            if (!count) break;
            if (!writeAll(writer->arStage.get(), buffer.data(), count))
                return fail(writer, PEARE_WRITER_IO_ERROR, writer->arStage->errorString());
            copied += quint64(count);
        }
        if (copied != replacement.size)
            return fail(writer, PEARE_WRITER_IO_ERROR,
                        QStringLiteral("The source file changed while it was being archived."));
        *found = std::move(replacement);
        writer->archiveDirty = true;
        return PEARE_WRITER_OK;
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

peare_writer_status peare_writer_replace_entry_by_id(peare_writer_handle writer,
                                                      const char* sourcePathUtf8,
                                                      const char* entryNameUtf8,
                                                      int64_t entryId) {
    if (writer && writer->active && writer->mode == peare_writer_handle_s::Mode::WadUpdate) {
        if (!sourcePathUtf8 || !*sourcePathUtf8 || entryId < 0)
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("A source path and valid WAD entry ID are required."), false);
        return cBoundary(writer, [&] { return replaceWadEntry(writer, entryId, sourcePathUtf8); });
    }
    return peare_writer_replace_file(writer, sourcePathUtf8, entryNameUtf8);
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
    if (writer->mode == peare_writer_handle_s::Mode::WadUpdate) {
        const qint64 index = findWadEntry(writer, entryNameUtf8);
        if (index < 0)
            return fail(writer, index == -2 ? PEARE_WRITER_INVALID_ARGUMENT
                                            : PEARE_WRITER_ENTRY_NOT_FOUND,
                        index == -2 ? QStringLiteral("The WAD lump name is ambiguous; use its entry ID.")
                                    : QStringLiteral("The WAD lump was not found."), false);
        QByteArray name;
        if (!wadName(newEntryNameUtf8, &name))
            return fail(writer, PEARE_WRITER_INVALID_ENTRY_NAME,
                        QStringLiteral("WAD lump names must contain 1 to 8 printable ASCII characters."),
                        false);
        writer->wadEntries[size_t(index)].name = name;
        writer->archiveDirty = true;
        return PEARE_WRITER_OK;
    }
    if (writer->mode == peare_writer_handle_s::Mode::ArUpdate) {
        QByteArray oldName, newName;
        if (!arFlatName(entryNameUtf8, &oldName) || !arFlatName(newEntryNameUtf8, &newName))
            return fail(writer, PEARE_WRITER_INVALID_ENTRY_NAME,
                        QStringLiteral("The ar member name is invalid."), false);
        const QString oldKey = QString::fromUtf8(oldName);
        const QString newKey = QString::fromUtf8(newName);
        if (oldKey == newKey) return PEARE_WRITER_OK;
        if (writer->names.contains(newKey))
            return fail(writer, PEARE_WRITER_DUPLICATE_ENTRY,
                        QStringLiteral("The destination ar member already exists."), false);
        auto found = std::find_if(writer->arEntries.begin(), writer->arEntries.end(),
            [&](const ArEntry& entry) { return entry.key == oldKey; });
        if (found == writer->arEntries.end())
            return fail(writer, PEARE_WRITER_ENTRY_NOT_FOUND,
                        QStringLiteral("The ar member to rename was not found."), false);
        found->key = newKey;
        found->name = newName;
        writer->names.remove(oldKey);
        writer->names.insert(newKey);
        writer->archiveDirty = true;
        return PEARE_WRITER_OK;
    }
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
    if (writer->mode == peare_writer_handle_s::Mode::CpioUpdate) {
        QByteArray oldName, newName;
        if (!normalizedEntryName(entryNameUtf8, false, &oldName) ||
            !normalizedEntryName(newEntryNameUtf8, false, &newName))
            return fail(writer, PEARE_WRITER_INVALID_ENTRY_NAME,
                        QStringLiteral("The CPIO path is invalid or unsafe."), false);
        const QString oldKey = QString::fromUtf8(oldName);
        const QString newKey = QString::fromUtf8(newName);
        const QString prefix = oldKey + QLatin1Char('/');
        bool exact = false;
        bool directory = false;
        bool children = false;
        for (const CpioEntry& entry : writer->cpioEntries) {
            if (entry.key == oldKey) {
                exact = true;
                directory = (entry.mode & 0170000u) == 0040000u;
            } else if (entry.key.startsWith(prefix)) {
                children = true;
            }
        }
        if (!exact && !children)
            return fail(writer, PEARE_WRITER_ENTRY_NOT_FOUND,
                        QStringLiteral("The CPIO entry to rename was not found."), false);
        if (directory && newKey.startsWith(prefix))
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("A CPIO directory cannot be moved inside itself."), false);
        QSet<QString> planned;
        for (const CpioEntry& entry : writer->cpioEntries) {
            if (entry.key != oldKey && !(children && entry.key.startsWith(prefix))) continue;
            const QString mapped = entry.key == oldKey
                ? newKey : newKey + entry.key.mid(oldKey.size());
            if (planned.contains(mapped))
                return fail(writer, PEARE_WRITER_DUPLICATE_ENTRY,
                            QStringLiteral("The destination CPIO path conflicts with an entry."), false);
            planned.insert(mapped);
        }
        for (const CpioEntry& entry : writer->cpioEntries) {
            if (planned.contains(entry.key) && entry.key != oldKey &&
                !(children && entry.key.startsWith(prefix)))
                return fail(writer, PEARE_WRITER_DUPLICATE_ENTRY,
                            QStringLiteral("The destination CPIO path already exists."), false);
        }
        for (CpioEntry& entry : writer->cpioEntries) {
            if (entry.key != oldKey && !(children && entry.key.startsWith(prefix))) continue;
            entry.key = entry.key == oldKey ? newKey : newKey + entry.key.mid(oldKey.size());
            entry.name = entry.key.toUtf8();
        }
        writer->names.clear();
        for (const CpioEntry& entry : writer->cpioEntries) writer->names.insert(entry.key);
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

peare_writer_status peare_writer_rename_entry_by_id(peare_writer_handle writer,
                                                     const char* entryNameUtf8,
                                                     const char* newEntryNameUtf8,
                                                     int64_t entryId) {
    if (writer && writer->active && writer->mode == peare_writer_handle_s::Mode::WadUpdate) {
        if (entryId < 0 || quint64(entryId) >= writer->wadEntries.size())
            return fail(writer, PEARE_WRITER_ENTRY_NOT_FOUND,
                        QStringLiteral("The WAD lump ID was not found."), false);
        QByteArray name;
        if (!wadName(newEntryNameUtf8, &name))
            return fail(writer, PEARE_WRITER_INVALID_ENTRY_NAME,
                        QStringLiteral("WAD lump names must contain 1 to 8 printable ASCII characters."),
                        false);
        writer->wadEntries[size_t(entryId)].name = name;
        writer->archiveDirty = true;
        return PEARE_WRITER_OK;
    }
    return peare_writer_rename_entry(writer, entryNameUtf8, newEntryNameUtf8);
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
    if (writer->mode == peare_writer_handle_s::Mode::WadUpdate) {
        const qint64 index = findWadEntry(writer, entryNameUtf8);
        if (index < 0)
            return fail(writer, index == -2 ? PEARE_WRITER_INVALID_ARGUMENT
                                            : PEARE_WRITER_ENTRY_NOT_FOUND,
                        index == -2 ? QStringLiteral("The WAD lump name is ambiguous; use its entry ID.")
                                    : QStringLiteral("The WAD lump was not found."), false);
        writer->wadEntries.erase(writer->wadEntries.begin() + index);
        writer->archiveDirty = true;
        return PEARE_WRITER_OK;
    }
    if (writer->mode == peare_writer_handle_s::Mode::ArUpdate) {
        QByteArray name;
        if (!arFlatName(entryNameUtf8, &name))
            return fail(writer, PEARE_WRITER_INVALID_ENTRY_NAME,
                        QStringLiteral("The ar member name is invalid."), false);
        const QString key = QString::fromUtf8(name);
        const size_t before = writer->arEntries.size();
        writer->arEntries.erase(std::remove_if(writer->arEntries.begin(), writer->arEntries.end(),
            [&](const ArEntry& entry) { return entry.key == key; }), writer->arEntries.end());
        if (writer->arEntries.size() == before)
            return fail(writer, PEARE_WRITER_ENTRY_NOT_FOUND,
                        QStringLiteral("The ar member to delete was not found."), false);
        writer->names.remove(key);
        writer->archiveDirty = true;
        return PEARE_WRITER_OK;
    }
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
    if (writer->mode == peare_writer_handle_s::Mode::CpioUpdate) {
        QByteArray encoded;
        if (!normalizedEntryName(entryNameUtf8, false, &encoded))
            return fail(writer, PEARE_WRITER_INVALID_ENTRY_NAME,
                        QStringLiteral("The CPIO entry name is invalid or unsafe."), false);
        const QString key = QString::fromUtf8(encoded);
        const QString prefix = key + QLatin1Char('/');
        bool exact = false;
        bool directory = false;
        bool children = false;
        for (const CpioEntry& entry : writer->cpioEntries) {
            if (entry.key == key) {
                exact = true;
                directory = (entry.mode & 0170000u) == 0040000u;
            } else if (entry.key.startsWith(prefix)) {
                children = true;
            }
        }
        if (!exact && !children)
            return fail(writer, PEARE_WRITER_ENTRY_NOT_FOUND,
                        QStringLiteral("The CPIO entry to delete was not found."), false);
        if (children && !recursive)
            return fail(writer, PEARE_WRITER_INVALID_ARGUMENT,
                        QStringLiteral("The CPIO directory is not empty."), false);
        std::vector<CpioEntry> removed;
        for (const CpioEntry& entry : writer->cpioEntries)
            if (entry.key == key || (recursive && entry.key.startsWith(prefix)))
                removed.push_back(entry);
        writer->cpioEntries.erase(std::remove_if(writer->cpioEntries.begin(),
            writer->cpioEntries.end(), [&](const CpioEntry& entry) {
                return entry.key == key || (recursive && entry.key.startsWith(prefix));
            }), writer->cpioEntries.end());
        for (const CpioEntry& removedEntry : removed) {
            if (!removedEntry.size) continue;
            auto survivor = std::find_if(writer->cpioEntries.begin(), writer->cpioEntries.end(),
                [&](const CpioEntry& entry) {
                    return entry.inode == removedEntry.inode &&
                        entry.deviceMajor == removedEntry.deviceMajor &&
                        entry.deviceMinor == removedEntry.deviceMinor;
                });
            if (survivor != writer->cpioEntries.end() && !survivor->size) {
                survivor->size = removedEntry.size;
                survivor->dataOffset = removedEntry.dataOffset;
            }
        }
        for (CpioEntry& entry : writer->cpioEntries) {
            const quint32 linkCount = quint32(std::count_if(
                writer->cpioEntries.begin(), writer->cpioEntries.end(),
                [&](const CpioEntry& other) {
                    return entry.inode == other.inode &&
                        entry.deviceMajor == other.deviceMajor &&
                        entry.deviceMinor == other.deviceMinor;
                }));
            if (entry.links > 1) entry.links = linkCount;
        }
        writer->names.clear();
        for (const CpioEntry& entry : writer->cpioEntries) writer->names.insert(entry.key);
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

peare_writer_status peare_writer_delete_entry_by_id(peare_writer_handle writer,
                                                     const char* entryNameUtf8,
                                                     int recursive, int64_t entryId) {
    if (writer && writer->active && writer->mode == peare_writer_handle_s::Mode::WadUpdate) {
        if ((recursive != 0 && recursive != 1) || entryId < 0 ||
            quint64(entryId) >= writer->wadEntries.size())
            return fail(writer, PEARE_WRITER_ENTRY_NOT_FOUND,
                        QStringLiteral("The WAD lump ID was not found."), false);
        writer->wadEntries.erase(writer->wadEntries.begin() + entryId);
        writer->archiveDirty = true;
        return PEARE_WRITER_OK;
    }
    return peare_writer_delete_entry(writer, entryNameUtf8, recursive);
}

peare_writer_status peare_writer_update_finish(peare_writer_handle writer) {
    if (!writer) return PEARE_WRITER_INVALID_HANDLE;
    if (writer->mode != peare_writer_handle_s::Mode::ZipUpdate &&
        writer->mode != peare_writer_handle_s::Mode::TarUpdate &&
        writer->mode != peare_writer_handle_s::Mode::CpioUpdate &&
        writer->mode != peare_writer_handle_s::Mode::ArUpdate &&
        writer->mode != peare_writer_handle_s::Mode::WadUpdate &&
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
    if (writer->mode == peare_writer_handle_s::Mode::GzipCreate)
        return cBoundary(writer, [&]() -> peare_writer_status {
            if (!writer->output->commit())
                return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
            writer->output.reset();
            writer->active = false;
            writer->mode = peare_writer_handle_s::Mode::None;
            writer->error.clear();
            writer->errorUtf8.clear();
            return PEARE_WRITER_OK;
        });
    if (writer->mode == peare_writer_handle_s::Mode::Bzip2Create)
        return cBoundary(writer, [&]() -> peare_writer_status {
            if (!writer->output->commit())
                return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
            writer->output.reset();
            writer->active = false;
            writer->mode = peare_writer_handle_s::Mode::None;
            writer->error.clear();
            writer->errorUtf8.clear();
            return PEARE_WRITER_OK;
        });
    if (writer->mode == peare_writer_handle_s::Mode::ZstdCreate)
        return cBoundary(writer, [&]() -> peare_writer_status {
            if (!writer->output->commit())
                return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
            writer->output.reset();
            writer->active = false;
            writer->mode = peare_writer_handle_s::Mode::None;
            writer->error.clear();
            writer->errorUtf8.clear();
            return PEARE_WRITER_OK;
        });
    if (writer->mode == peare_writer_handle_s::Mode::XzCreate)
        return cBoundary(writer, [&]() -> peare_writer_status {
            if (!writer->output->commit())
                return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
            writer->output.reset();
            writer->active = false;
            writer->mode = peare_writer_handle_s::Mode::None;
            writer->error.clear();
            writer->errorUtf8.clear();
            return PEARE_WRITER_OK;
        });
    if (writer->mode == peare_writer_handle_s::Mode::LzmaAloneCreate ||
        writer->mode == peare_writer_handle_s::Mode::UnixCompressCreate)
        return cBoundary(writer, [&]() -> peare_writer_status {
            if (!writer->output->commit())
                return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
            writer->output.reset();
            writer->active = false;
            writer->mode = peare_writer_handle_s::Mode::None;
            writer->error.clear();
            writer->errorUtf8.clear();
            return PEARE_WRITER_OK;
        });
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
    if (writer->mode == peare_writer_handle_s::Mode::CpioCreate ||
        writer->mode == peare_writer_handle_s::Mode::CpioUpdate)
        return cBoundary(writer, [&]() -> peare_writer_status {
            if (writer->mode == peare_writer_handle_s::Mode::CpioUpdate &&
                !writer->archiveDirty)
                return finishNoOpArchiveUpdate(writer);
            if (writer->mode == peare_writer_handle_s::Mode::CpioUpdate) {
                writer->output.reset(new (std::nothrow) QSaveFile(writer->updatePath));
                if (!writer->output || !writer->output->open(QIODevice::WriteOnly))
                    return fail(writer, PEARE_WRITER_IO_ERROR,
                        writer->output ? writer->output->errorString()
                                       : QStringLiteral("Could not allocate CPIO output."));
            }
            for (const CpioEntry& entry : writer->cpioEntries)
                if (!writeCpioRecord(writer->output.get(), writer->cpioStage.get(), entry,
                                     writer->cpioFormat))
                    return fail(writer, PEARE_WRITER_IO_ERROR,
                                writer->output->errorString().isEmpty()
                                    ? QStringLiteral("Could not write a CPIO record.")
                                    : writer->output->errorString());
            CpioEntry trailer;
            trailer.name = QByteArrayLiteral("TRAILER!!!");
            trailer.links = 1;
            if (!writeCpioRecord(writer->output.get(), writer->cpioStage.get(), trailer,
                                 writer->cpioFormat))
                return fail(writer, PEARE_WRITER_IO_ERROR,
                            QStringLiteral("Could not write the CPIO trailer."));
            const qint64 padding = (512 - (writer->output->pos() % 512)) % 512;
            const QByteArray zeros(int(padding), '\0');
            if (padding && !writeAll(writer->output.get(), zeros.constData(), zeros.size()))
                return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
            if (writer->cpioOpaqueSize) {
                if (writer->cpioOpaqueOffset < 0 ||
                    quint64(writer->cpioOpaqueSize) >
                        quint64(writer->cpioStage->size() - writer->cpioOpaqueOffset) ||
                    !writer->cpioStage->seek(writer->cpioOpaqueOffset))
                    return fail(writer, PEARE_WRITER_IO_ERROR,
                                QStringLiteral("Could not read trailing CPIO data."));
                std::vector<char> buffer(kChunkSize);
                qint64 remaining = writer->cpioOpaqueSize;
                while (remaining) {
                    const qint64 request = qMin<qint64>(remaining, qint64(buffer.size()));
                    const qint64 count = writer->cpioStage->read(buffer.data(), request);
                    if (count != request || !writeAll(writer->output.get(), buffer.data(), count))
                        return fail(writer, PEARE_WRITER_IO_ERROR,
                                    QStringLiteral("Could not preserve trailing CPIO data."));
                    remaining -= count;
                }
            }
            if (!writer->output->commit())
                return fail(writer, PEARE_WRITER_IO_ERROR, writer->output->errorString());
            writer->output.reset();
            writer->cpioStage.reset();
            writer->cpioEntries.clear();
            writer->cpioFormat = CpioFormat::Newc;
            writer->cpioOpaqueOffset = 0;
            writer->cpioOpaqueSize = 0;
            writer->names.clear();
            writer->active = false;
            writer->updateMode = false;
            writer->archiveDirty = false;
            writer->mode = peare_writer_handle_s::Mode::None;
            writer->updatePath.clear();
            writer->error.clear();
            writer->errorUtf8.clear();
            return PEARE_WRITER_OK;
        });
    if (writer->mode == peare_writer_handle_s::Mode::ArCreate ||
        writer->mode == peare_writer_handle_s::Mode::ArUpdate)
        return cBoundary(writer, [&]() { return finishAr(writer); });
    if (writer->mode == peare_writer_handle_s::Mode::WadCreate ||
        writer->mode == peare_writer_handle_s::Mode::WadUpdate)
        return cBoundary(writer, [&]() { return finishWad(writer); });
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
    writer->zipMethod = 0;
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
    entry.method = 0;
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
    Entry entry;
    peare_writer_status status = prepareEntry(writer, entryNameUtf8, false, &entry);
    if (status != PEARE_WRITER_OK) return status;
    entry.method = writer->zipMethod;
    entry.flags = 0x0808;
    if (entry.method == 14) entry.flags = quint16(entry.flags | 0x0002);
    entry.zip64Sizes = quint64(length) >= kZip32Max - 1024 * 1024;
    QIODevice* output = zipDevice(writer);
    if (!output || !writeLocalHeader(output, entry, true))
        return fail(writer, PEARE_WRITER_IO_ERROR,
                    output ? output->errorString() : QStringLiteral("The ZIP output is unavailable."));
    size_t position = 0;
    QString writeError;
    const auto bufferReader = [&bytes, length, &position](char* buffer,
                                                          qint64 request) -> qint64 {
                if (position == length) return 0;
                const size_t count = std::min<size_t>(size_t(request), length - position);
                std::memcpy(buffer, bytes + position, count);
                position += count;
                return qint64(count);
            };
    const bool payloadWritten = entry.method == 96
        ? writeZipJpegPayload(output, bufferReader, &entry.crc, &entry.size,
                              &entry.compressedSize, &writeError)
        : entry.method == 14
        ? writeZipLzmaPayload(output, bufferReader, &entry.crc, &entry.size,
                              &entry.compressedSize, &writeError)
        : entry.method == 95
            ? writeZipXzPayload(output, bufferReader, &entry.crc, &entry.size,
                                &entry.compressedSize, &writeError)
            : entry.method == 93
                ? writeZipZstdPayload(output, bufferReader, &entry.crc, &entry.size,
                                      &entry.compressedSize, &writeError)
            : entry.method == 98
                ? writeZipPpmdPayload(output, bufferReader, &entry.crc, &entry.size,
                                      &entry.compressedSize, &writeError)
            : entry.method == 1
                ? writeZipShrinkPayload(output, bufferReader, &entry.crc, &entry.size,
                                        &entry.compressedSize, &writeError)
            : entry.method == 94
                ? writeZipPackMp3Payload(output, bufferReader, &entry.crc, &entry.size,
                                         &entry.compressedSize, &writeError)
            : entry.method == 97
                ? writeZipWavPackPayload(output, bufferReader, &entry.crc, &entry.size,
                                         &entry.compressedSize, &writeError)
            : entry.method == 6
                ? writeZipImplodePayload(output, bufferReader, &entry.crc, &entry.size,
                                         &entry.compressedSize, &writeError)
            : entry.method == 10
                ? writeZipDclImplodePayload(output, bufferReader, &entry.crc, &entry.size,
                                            &entry.compressedSize, &writeError)
            : entry.method >= 2 && entry.method <= 5
                ? writeZipReducePayload(output, bufferReader, entry.method - 1,
                    &entry.crc, &entry.size, &entry.compressedSize, &writeError)
            : writeZipPayload(output, entry.method, bufferReader, &entry.crc,
                              &entry.size, &entry.compressedSize, &writeError);
    if (!payloadWritten)
        return fail(writer, PEARE_WRITER_IO_ERROR, writeError);
    entry.uncompressedSize = entry.size;
    QByteArray descriptor;
    append32(&descriptor, kDataDescriptorSignature);
    append32(&descriptor, entry.crc);
    if (entry.zip64Sizes) {
        append64(&descriptor, entry.compressedSize);
        append64(&descriptor, entry.size);
    } else {
        append32(&descriptor, quint32(entry.compressedSize));
        append32(&descriptor, quint32(entry.size));
    }
    if (!writeAll(output, descriptor.constData(), descriptor.size()))
        return fail(writer, PEARE_WRITER_IO_ERROR, output->errorString());
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
        const bool zip64Size = entry.size > kZip32Max || entry.compressedSize > kZip32Max;
        const bool zip64Offset = entry.offset > kZip32Max;
        if (zip64Size || zip64Offset) {
            append16(&extra, 1);
            append16(&extra, quint16((zip64Size ? 16 : 0) + (zip64Offset ? 8 : 0)));
            if (zip64Size) {
                append64(&extra, entry.compressedSize);
                append64(&extra, entry.size);
            }
            if (zip64Offset) append64(&extra, entry.offset);
        }
        header.reserve(46 + entry.name.size() + extra.size());
        append32(&header, kCentralHeaderSignature);
        append16(&header, zip64Size || zip64Offset ? 45 : 20); // version made by
        const quint16 versionNeeded = (entry.method >= 2 && entry.method <= 5) ? 10
            : entry.method == 9 ? 21
            : entry.method == 96 ? 20
            : (entry.method == 14 || entry.method == 93 ||
                                       entry.method == 95) ? 63
            : entry.method == 12 ? 46
            : zip64Size || zip64Offset ? 45 : 20;
        append16(&header, versionNeeded);
        append16(&header, quint16(entry.flags | (entry.descriptor ? 0x0008 : 0)));
        append16(&header, entry.method);
        append16(&header, 0); // DOS time
        append16(&header, 0x0021); // 1980-01-01
        append32(&header, entry.crc);
        append32(&header, zip64Size ? 0xffffffffu : quint32(entry.compressedSize));
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
    writer->cpioStage.reset();
    writer->arStage.reset();
    writer->wadStage.reset();
    if (writer->diskUpdate) {
        writer->diskUpdate->abort();
        writer->diskUpdate.reset();
    }
    writer->entries.clear();
    writer->tarEntries.clear();
    writer->cpioEntries.clear();
    writer->arEntries.clear();
    writer->wadEntries.clear();
    writer->cpioFormat = CpioFormat::Newc;
    writer->cpioOpaqueOffset = 0;
    writer->cpioOpaqueSize = 0;
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
