#include "ZipModule.h"
#include "Compat.h"
#include "DeflateDecoder.h"
#include "WavPackDecoder.h"
#include "XzModule.h"
#include "ZstdModule.h"
#include "../compression/WinZipJpegDecoder.h"
#include "../compression/PackMp3Decoder.h"
#include "../compression/third_party/blast/blast.h"
#undef local
#include "../compression/LzmaDecoder.h"
#include "../compression/Crc32.h"
#include "../compression/peare/Bzip2Decoder.h"
#include "../fs/QFileByteStore.h"

#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QStringList>
#include <QXmlStreamReader>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace peare {
namespace {

const quint32 kZipLocalHeader = 0x04034b50u;
const quint32 kZipCentralHeader = 0x02014b50u;
const quint32 kZipEndCentralDirectory = 0x06054b50u;
const std::int64_t kMaxZipVariableField = std::int64_t(16) * 1024 * 1024;

struct DclInput {
    const std::uint8_t* data;
    std::size_t size;
    std::size_t offset;
};

struct DclOutput {
    std::vector<std::uint8_t>* data;
    std::size_t limit;
};

std::mutex& dclDecoderMutex()
{
    static std::mutex mutex;
    return mutex;
}

unsigned readDclInput(void* context, unsigned char** buffer)
{
    DclInput* input = static_cast<DclInput*>(context);
    if (input->offset == input->size) return 0;
    const std::size_t count = std::min<std::size_t>(4096, input->size - input->offset);
    *buffer = const_cast<unsigned char*>(input->data + input->offset);
    input->offset += count;
    return static_cast<unsigned>(count);
}

int writeDclOutput(void* context, unsigned char* buffer, unsigned size)
{
    DclOutput* output = static_cast<DclOutput*>(context);
    if (size > output->limit - output->data->size()) return 1;
    try {
        output->data->insert(output->data->end(), buffer, buffer + size);
    } catch (...) {
        return 1;
    }
    return 0;
}

bool decodeDclZip(const std::vector<std::uint8_t>& packed, std::size_t unpackedSize,
                  std::vector<std::uint8_t>* output)
{
    if (!output || packed.empty() || packed.size() > std::numeric_limits<unsigned>::max())
        return false;
    std::vector<std::uint8_t> decoded;
    try {
        decoded.reserve(unpackedSize);
    } catch (...) {
        return false;
    }
    DclInput input = {packed.data(), packed.size(), 0};
    DclOutput result = {&decoded, unpackedSize};
    unsigned remaining = 0;
    unsigned char* unused = nullptr;
    int status;
    {
        std::lock_guard<std::mutex> lock(dclDecoderMutex());
        status = blast(readDclInput, &input, writeDclOutput, &result, &remaining, &unused);
    }
    if (status != 0 || decoded.size() != unpackedSize || remaining != 0 ||
        input.offset != packed.size())
        return false;
    output->swap(decoded);
    return true;
}

quint16 le16(const std::uint8_t* p) {
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

quint32 le32(const std::uint8_t* p) {
    return quint32(p[0]) | (quint32(p[1]) << 8) |
           (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

quint64 le64(const std::uint8_t* p) {
    return quint64(le32(p)) | (quint64(le32(p + 4)) << 32);
}

bool readExact(const fs::IByteStore& store, std::int64_t offset,
               std::uint8_t* dst, int count);
QByteArray readArray(const fs::IByteStore& store, std::int64_t offset,
                     std::int64_t count);

bool takeLengthPrefixed(const QByteArray& data, int* offset, int end,
                        int* valueOffset, int* valueSize) {
    if (*offset < 0 || end < *offset || end - *offset < 4) return false;
    const quint32 size = le32(reinterpret_cast<const std::uint8_t*>(data.constData() + *offset));
    *offset += 4;
    if (size > quint32(end - *offset)) return false;
    *valueOffset = *offset;
    *valueSize = int(size);
    *offset += int(size);
    return true;
}

bool takeDerField(const QByteArray& data, int* offset, int end, quint8 expectedTag,
                 int* valueOffset, int* valueSize) {
    if (*offset < 0 || end - *offset < 2 || quint8(data.at(*offset)) != expectedTag)
        return false;
    ++*offset;
    const quint8 firstLength = quint8(data.at(*offset));
    ++*offset;
    quint64 length = firstLength;
    if (firstLength & 0x80) {
        const int lengthBytes = firstLength & 0x7f;
        if (lengthBytes == 0 || lengthBytes > 4 || end - *offset < lengthBytes)
            return false;
        length = 0;
        for (int i = 0; i < lengthBytes; ++i)
            length = (length << 8) | quint8(data.at((*offset)++));
    }
    if (length > quint64(end - *offset)) return false;
    *valueOffset = *offset;
    *valueSize = int(length);
    *offset += int(length);
    return true;
}

QString certificateCommonName(const QByteArray& certificate) {
    int offset = 0;
    int certificateOffset = 0;
    int certificateSize = 0;
    if (!takeDerField(certificate, &offset, certificate.size(), 0x30,
                      &certificateOffset, &certificateSize)) return {};
    const int certificateEnd = certificateOffset + certificateSize;
    offset = certificateOffset;
    int tbsOffset = 0;
    int tbsSize = 0;
    if (!takeDerField(certificate, &offset, certificateEnd, 0x30, &tbsOffset, &tbsSize))
        return {};
    const int tbsEnd = tbsOffset + tbsSize;
    offset = tbsOffset;
    if (offset < tbsEnd && quint8(certificate.at(offset)) == 0xa0) {
        int ignoredOffset = 0;
        int ignoredSize = 0;
        if (!takeDerField(certificate, &offset, tbsEnd, 0xa0,
                          &ignoredOffset, &ignoredSize)) return {};
    }
    for (int field = 0; field < 2; ++field) {
        int ignoredOffset = 0;
        int ignoredSize = 0;
        if (!takeDerField(certificate, &offset, tbsEnd,
                          field == 0 ? 0x02 : 0x30, &ignoredOffset, &ignoredSize)) return {};
    }
    int issuerOffset = 0;
    int issuerSize = 0;
    if (!takeDerField(certificate, &offset, tbsEnd, 0x30, &issuerOffset, &issuerSize))
        return {};
    int validityOffset = 0;
    int validitySize = 0;
    if (!takeDerField(certificate, &offset, tbsEnd, 0x30,
                      &validityOffset, &validitySize)) return {};
    int subjectOffset = 0;
    int subjectSize = 0;
    if (!takeDerField(certificate, &offset, tbsEnd, 0x30,
                      &subjectOffset, &subjectSize)) return {};

    int nameCursor = subjectOffset;
    const int nameEnd = subjectOffset + subjectSize;
    while (nameCursor < nameEnd) {
        int setOffset = 0;
        int setSize = 0;
        if (!takeDerField(certificate, &nameCursor, nameEnd, 0x31, &setOffset, &setSize))
            return {};
        int sequenceCursor = setOffset;
        const int setEnd = setOffset + setSize;
        while (sequenceCursor < setEnd) {
            int attributeOffset = 0;
            int attributeSize = 0;
            if (!takeDerField(certificate, &sequenceCursor, setEnd, 0x30,
                              &attributeOffset, &attributeSize)) return {};
            int attributeCursor = attributeOffset;
            const int attributeEnd = attributeOffset + attributeSize;
            int oidOffset = 0;
            int oidSize = 0;
            if (!takeDerField(certificate, &attributeCursor, attributeEnd, 0x06,
                              &oidOffset, &oidSize)) return {};
            int valueOffset = 0;
            int valueSize = 0;
            if (attributeCursor >= attributeEnd) return {};
            const quint8 valueTag = quint8(certificate.at(attributeCursor));
            if (!takeDerField(certificate, &attributeCursor, attributeEnd, valueTag,
                              &valueOffset, &valueSize)) return {};
            static const char commonNameOid[] = {0x55, 0x04, 0x03};
            if (oidSize != int(sizeof(commonNameOid)) ||
                std::memcmp(certificate.constData() + oidOffset, commonNameOid,
                            sizeof(commonNameOid)) != 0) continue;
            const QByteArray value = certificate.mid(valueOffset, valueSize);
            if (valueTag == 0x0c) return QString::fromUtf8(value);
            if (valueTag == 0x1e && value.size() % 2 == 0) {
                QVector<ushort> chars;
                chars.reserve(value.size() / 2);
                for (int i = 0; i < value.size(); i += 2)
                    chars.push_back((quint8(value.at(i)) << 8) | quint8(value.at(i + 1)));
                return QString::fromUtf16(chars.constData(), chars.size());
            }
            if (valueTag == 0x12 || valueTag == 0x13 || valueTag == 0x14 || valueTag == 0x16)
                return QString::fromLatin1(value);
            return {};
        }
    }
    return {};
}

bool appendApkRotationLineage(const QByteArray& data, QStringList* lineage) {
    if (data.size() < 4 || le32(reinterpret_cast<const std::uint8_t*>(data.constData())) != 1)
        return false;
    int offset = 4;
    while (offset < data.size()) {
        int nodeOffset = 0;
        int nodeSize = 0;
        if (!takeLengthPrefixed(data, &offset, data.size(), &nodeOffset, &nodeSize))
            return false;
        const int nodeEnd = nodeOffset + nodeSize;
        int cursor = nodeOffset;
        int signedOffset = 0;
        int signedSize = 0;
        if (!takeLengthPrefixed(data, &cursor, nodeEnd, &signedOffset, &signedSize))
            return false;
        const int signedEnd = signedOffset + signedSize;
        int certificateOffset = 0;
        int certificateSize = 0;
        if (!takeLengthPrefixed(data, &signedOffset, signedEnd,
                                &certificateOffset, &certificateSize) ||
            signedEnd - signedOffset != 4) return false;
        const QByteArray certificate = data.mid(certificateOffset, certificateSize);
        if (certificate.isEmpty() || quint8(certificate.at(0)) != 0x30) return false;
        signedOffset += 4;
        if (nodeEnd - cursor < 8) return false;
        const quint32 flags = le32(reinterpret_cast<const std::uint8_t*>(data.constData() + cursor));
        cursor += 8;
        int signatureOffset = 0;
        int signatureSize = 0;
        if (!takeLengthPrefixed(data, &cursor, nodeEnd, &signatureOffset, &signatureSize) ||
            cursor != nodeEnd) return false;
        QString description = certificateCommonName(certificate);
        if (description.isEmpty()) description = QStringLiteral("certificate");
        description += QStringLiteral(" [SHA-256 ") + QString::fromLatin1(
            QCryptographicHash::hash(certificate, QCryptographicHash::Sha256).toHex());
        description += QStringLiteral(", flags 0x%1]").arg(flags, 0, 16);
        lineage->push_back(description);
    }
    return !lineage->isEmpty();
}

bool appendApkSignerFingerprints(const QByteArray& scheme, int schemeVersion,
                                QStringList* fingerprints, QStringList* lineage) {
    int offset = 0;
    int signersOffset = 0;
    int signersSize = 0;
    if (!takeLengthPrefixed(scheme, &offset, scheme.size(), &signersOffset, &signersSize) ||
        offset != scheme.size()) return false;

    const int signersEnd = signersOffset + signersSize;
    offset = signersOffset;
    while (offset < signersEnd) {
        int signerOffset = 0;
        int signerSize = 0;
        if (!takeLengthPrefixed(scheme, &offset, signersEnd, &signerOffset, &signerSize))
            return false;
        const int signerEnd = signerOffset + signerSize;
        int cursor = signerOffset;
        int signedDataOffset = 0;
        int signedDataSize = 0;
        if (!takeLengthPrefixed(scheme, &cursor, signerEnd,
                                &signedDataOffset, &signedDataSize)) return false;
        quint32 signerMinSdk = 0;
        quint32 signerMaxSdk = 0;
        if (schemeVersion >= 3) {
            if (signerEnd - cursor < 8) return false;
            signerMinSdk = le32(reinterpret_cast<const std::uint8_t*>(scheme.constData() + cursor));
            signerMaxSdk = le32(reinterpret_cast<const std::uint8_t*>(scheme.constData() + cursor + 4));
            cursor += 8;
        }
        int signaturesOffset = 0;
        int signaturesSize = 0;
        if (!takeLengthPrefixed(scheme, &cursor, signerEnd,
                                &signaturesOffset, &signaturesSize)) return false;
        int publicKeyOffset = 0;
        int publicKeySize = 0;
        if (!takeLengthPrefixed(scheme, &cursor, signerEnd,
                                &publicKeyOffset, &publicKeySize) || cursor != signerEnd)
            return false;

        int signedCursor = signedDataOffset;
        const int signedEnd = signedDataOffset + signedDataSize;
        int ignoredOffset = 0;
        int ignoredSize = 0;
        if (!takeLengthPrefixed(scheme, &signedCursor, signedEnd,
                                &ignoredOffset, &ignoredSize)) return false;
        int certificatesOffset = 0;
        int certificatesSize = 0;
        if (!takeLengthPrefixed(scheme, &signedCursor, signedEnd,
                                &certificatesOffset, &certificatesSize)) return false;

        const int certificatesEnd = certificatesOffset + certificatesSize;
        int certificateCursor = certificatesOffset;
        bool foundCertificate = false;
        while (certificateCursor < certificatesEnd) {
            int certificateOffset = 0;
            int certificateSize = 0;
            if (!takeLengthPrefixed(scheme, &certificateCursor, certificatesEnd,
                                    &certificateOffset, &certificateSize)) return false;
            if (!foundCertificate) {
                const QByteArray certificate = scheme.mid(certificateOffset, certificateSize);
                if (certificate.isEmpty() || quint8(certificate.at(0)) != 0x30) return false;
                QString fingerprint = QString::fromLatin1(
                    QCryptographicHash::hash(certificate, QCryptographicHash::Sha256).toHex());
                const QString commonName = certificateCommonName(certificate);
                if (!commonName.isEmpty()) fingerprint += QStringLiteral(" (CN=%1)").arg(commonName);
                if (schemeVersion >= 3)
                    fingerprint += QStringLiteral(" (SDK %1-%2)")
                        .arg(signerMinSdk).arg(signerMaxSdk);
                fingerprints->push_back(fingerprint);
                foundCertificate = true;
            }
        }
        if (!foundCertificate) return false;
        if (schemeVersion >= 3) {
            if (signedEnd - signedCursor < 8) return false;
            const quint32 signedMinSdk = le32(
                reinterpret_cast<const std::uint8_t*>(scheme.constData() + signedCursor));
            const quint32 signedMaxSdk = le32(
                reinterpret_cast<const std::uint8_t*>(scheme.constData() + signedCursor + 4));
            if (signedMinSdk != signerMinSdk || signedMaxSdk != signerMaxSdk)
                return false;
            signedCursor += 8;
        }
        int attributesOffset = 0;
        int attributesSize = 0;
        if (!takeLengthPrefixed(scheme, &signedCursor, signedEnd,
                                &attributesOffset, &attributesSize)) return false;
        const int attributesEnd = attributesOffset + attributesSize;
        int attributeCursor = attributesOffset;
        while (attributeCursor < attributesEnd) {
            int attributeOffset = 0;
            int attributeSize = 0;
            if (!takeLengthPrefixed(scheme, &attributeCursor, attributesEnd,
                                    &attributeOffset, &attributeSize) || attributeSize < 4)
                return false;
            const quint32 attributeId = le32(
                reinterpret_cast<const std::uint8_t*>(scheme.constData() + attributeOffset));
            if (attributeId == 0x3ba06f8cu &&
                !appendApkRotationLineage(
                    scheme.mid(attributeOffset + 4, attributeSize - 4), lineage)) return false;
        }
    }
    return offset == signersEnd && !fingerprints->isEmpty();
}

QString apkSigningDescription(const fs::IByteStore& file, quint64 centralDirectoryOffset) {
    static const char magic[] = "APK Sig Block 42";
    if (centralDirectoryOffset < 24 || centralDirectoryOffset > quint64(file.capacity()))
        return QStringLiteral("APK signing block: unavailable");
    std::uint8_t trailer[24];
    const std::int64_t trailerOffset = std::int64_t(centralDirectoryOffset - sizeof(trailer));
    if (!readExact(file, trailerOffset, trailer, sizeof(trailer)) ||
        std::memcmp(trailer + 8, magic, 16) != 0)
        return QStringLiteral("APK signing block: not present");

    const quint64 blockSize = le64(trailer);
    if (blockSize < 24 || blockSize > centralDirectoryOffset - 8 ||
        blockSize > quint64(std::numeric_limits<int>::max()))
        return QStringLiteral("APK signing block: malformed");
    const quint64 blockOffset = centralDirectoryOffset - blockSize - 8;
    std::uint8_t firstSize[8];
    if (!readExact(file, std::int64_t(blockOffset), firstSize, sizeof(firstSize)) ||
        le64(firstSize) != blockSize)
        return QStringLiteral("APK signing block: malformed");

    const QByteArray pairs = readArray(file, std::int64_t(blockOffset + 8),
                                       std::int64_t(blockSize - 24));
    if (pairs.size() != int(blockSize - 24))
        return QStringLiteral("APK signing block: malformed");
    QStringList schemes;
    QStringList fingerprints;
    QStringList lineages;
    int offset = 0;
    while (offset < pairs.size()) {
        if (pairs.size() - offset < 8) return QStringLiteral("APK signing block: malformed");
        const quint64 pairSize = le64(reinterpret_cast<const std::uint8_t*>(pairs.constData() + offset));
        offset += 8;
        if (pairSize < 4 || pairSize > quint64(pairs.size() - offset))
            return QStringLiteral("APK signing block: malformed");
        const quint32 id = le32(reinterpret_cast<const std::uint8_t*>(pairs.constData() + offset));
        const int valueSize = int(pairSize - 4);
        if (id == 0x7109871au || id == 0xf05368c0u || id == 0x1b93ad61u) {
            QStringList signerFingerprints;
            const int schemeVersion = id == 0x7109871au ? 2 : 3;
            if (!appendApkSignerFingerprints(pairs.mid(offset + 4, valueSize),
                                             schemeVersion,
                                             &signerFingerprints, &lineages))
                return QStringLiteral("APK signing block: malformed v%1 data")
                    .arg(id == 0x7109871au ? QStringLiteral("2")
                        : id == 0xf05368c0u ? QStringLiteral("3") : QStringLiteral("3.1"));
            schemes.push_back(QStringLiteral("v%1 (%2 signer%3)")
                .arg(id == 0x7109871au ? QStringLiteral("2")
                    : id == 0xf05368c0u ? QStringLiteral("3") : QStringLiteral("3.1"))
                .arg(signerFingerprints.size())
                .arg(signerFingerprints.size() == 1 ? QString() : QStringLiteral("s")));
            fingerprints.append(signerFingerprints);
        }
        offset += 4 + valueSize;
    }
    if (schemes.isEmpty())
        return QStringLiteral("APK signing block: no supported v2/v3/v3.1 signer data");
    fingerprints.removeDuplicates();
    QString description = QStringLiteral(
        "Android APK, APK Signature Scheme %1, signer leaf certificates: %2")
        .arg(schemes.join(QStringLiteral(", ")), fingerprints.join(QStringLiteral(", ")));
    if (!lineages.isEmpty())
        description += QStringLiteral(", v3 key lineage: ") + lineages.join(QStringLiteral(" -> "));
    return description;
}

bool resolveZip64Extra(const QByteArray& extra, bool needUncompressed,
                       bool needCompressed, bool needOffset, bool needDisk,
                       quint64* uncompressed, quint64* compressed,
                       quint64* offset, quint32* disk) {
    int pos = 0;
    while (pos + 4 <= extra.size()) {
        const auto* header = reinterpret_cast<const std::uint8_t*>(extra.constData() + pos);
        const quint16 id = le16(header);
        const quint16 size = le16(header + 2);
        pos += 4;
        if (size > extra.size() - pos) return false;
        if (id == 0x0001) {
            const auto* values = reinterpret_cast<const std::uint8_t*>(extra.constData() + pos);
            int cursor = 0;
            auto take64 = [&](quint64* value) {
                if (cursor > size - 8) return false;
                *value = le64(values + cursor);
                cursor += 8;
                return true;
            };
            if ((needUncompressed && !take64(uncompressed)) ||
                (needCompressed && !take64(compressed)) ||
                (needOffset && !take64(offset)))
                return false;
            if (needDisk) {
                if (cursor > size - 4) return false;
                *disk = le32(values + cursor);
            }
            return true;
        }
        pos += size;
    }
    return !needUncompressed && !needCompressed && !needOffset && !needDisk;
}

bool readExact(const fs::IByteStore& store, std::int64_t offset,
               std::uint8_t* dst, int count) {
    if (offset < 0 || count < 0 || offset > store.capacity() - count) return false;
    int done = 0;
    while (done < count) {
        const int n = store.read(offset + done, dst + done, count - done);
        if (n <= 0) return false;
        done += n;
    }
    return true;
}

QByteArray readArray(const fs::IByteStore& store, std::int64_t offset,
                     std::int64_t count) {
    if (count < 0 || count > std::numeric_limits<int>::max() ||
        offset < 0 || offset > store.capacity() - count)
        return QByteArray();
    QByteArray out(int(count), Qt::Uninitialized);
    if (count && !readExact(store, offset,
                            reinterpret_cast<std::uint8_t*>(out.data()), int(count)))
        return QByteArray();
    return out;
}

QString decodeZipName(const QByteArray& bytes, quint16 flags) {
    return (flags & 0x0800) ? QString::fromUtf8(bytes) : QString::fromLocal8Bit(bytes);
}

QString normalizeName(QString name) {
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (name.startsWith(QLatin1String("./"))) name.remove(0, 2);
    while (name.startsWith(QLatin1Char('/'))) name.remove(0, 1);
    return name;
}

bool splitSafeName(QString name, QStringList* hierarchy, QString* leaf) {
    name = normalizeName(name);
    const QStringList parts = name.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (parts.isEmpty()) return false;
    for (const QString& part : parts) {
        if (part == QLatin1String(".") || part == QLatin1String("..")) return false;
        for (const QChar c : part)
            if (c.unicode() < 0x20) return false;
    }
    *leaf = parts.last();
    *hierarchy = parts;
    hierarchy->removeLast();
    static const QString forbidden = QStringLiteral("<>:\"/\\|?*");
    for (int i = 0; i < leaf->size(); ++i)
        if (forbidden.contains(leaf->at(i))) (*leaf)[i] = QLatin1Char('_');
    while (leaf->endsWith(QLatin1Char(' ')) || leaf->endsWith(QLatin1Char('.')))
        leaf->chop(1);
    return !leaf->isEmpty();
}

fs::ByteStorePtr storeForFile(const QString& path) {
    auto holder = std::make_shared<QFile>(path);
    if (!holder->open(QIODevice::ReadOnly)) return fs::ByteStorePtr();
    const qint64 size = holder->size();
    uchar* mapped = size > 0 ? holder->map(0, size) : nullptr;
    if (mapped)
        return std::make_shared<fs::ExternalStore>(
            reinterpret_cast<const std::uint8_t*>(mapped), std::int64_t(size),
            std::static_pointer_cast<void>(holder));
    auto source = std::make_shared<fs::QFileByteStore>(path);
    return source->validate() ? source : fs::ByteStorePtr();
}

// Keep Deflate members compressed during listing and inflate only requested data.
// Header probes read a prefix; extraction reads the full member.

class ZipReduceStore final : public fs::IByteStore {
public:
    ZipReduceStore(fs::ByteStorePtr source, std::int64_t offset,
                   std::int64_t compressed, std::int64_t uncompressed, int factor)
        : source_(std::move(source)), offset_(offset), compressed_(compressed),
          uncompressed_(uncompressed), factor_(factor) {}

    std::int64_t capacity() const override { return uncompressed_; }
    bool cheapRandomAccess() const override { return false; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!source_ || !dst || pos < 0 || count <= 0 || pos >= uncompressed_) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ensureDecoded()) return 0;
        const int n = int(std::min<std::int64_t>(count, uncompressed_ - pos));
        std::copy(decoded_.begin() + std::ptrdiff_t(pos),
                  decoded_.begin() + std::ptrdiff_t(pos + n), dst);
        return n;
    }

private:
    bool ensureDecoded() const {
        if (decodedReady_) return true;
        if (failed_ || factor_ < 1 || factor_ > 4 || compressed_ < 0 ||
            uncompressed_ < 0 || offset_ < 0 ||
            std::uint64_t(compressed_) > std::numeric_limits<std::size_t>::max() ||
            std::uint64_t(uncompressed_) > std::numeric_limits<std::size_t>::max() ||
            offset_ > source_->capacity() - compressed_) {
            failed_ = true;
            return false;
        }
        const auto input = source_->readRange(offset_, std::size_t(compressed_));
        if (input.size() != std::size_t(compressed_)) { failed_ = true; return false; }

        class BitReader {
        public:
            explicit BitReader(const std::vector<std::uint8_t>& bytes) : bytes_(bytes) {}
            bool read(unsigned bits, std::uint32_t* value) {
                while (available_ < bits) {
                    if (offset_ >= bytes_.size()) return false;
                    buffer_ |= std::uint64_t(bytes_[offset_++]) << available_;
                    available_ += 8;
                }
                *value = std::uint32_t(buffer_ & ((std::uint64_t(1) << bits) - 1));
                buffer_ >>= bits;
                available_ -= bits;
                return true;
            }
        private:
            const std::vector<std::uint8_t>& bytes_;
            std::size_t offset_ = 0;
            std::uint64_t buffer_ = 0;
            unsigned available_ = 0;
        } bits(input);

        struct Followers { std::uint8_t count = 0, width = 0; std::uint8_t values[32] = {}; };
        Followers followers[256];
        for (int previous = 255; previous >= 0; --previous) {
            std::uint32_t count = 0;
            if (!bits.read(6, &count) || count > 32) { failed_ = true; return false; }
            followers[previous].count = std::uint8_t(count);
            if (count) followers[previous].width = 1;
            while ((std::uint32_t(1) << followers[previous].width) < count)
                ++followers[previous].width;
            for (std::uint32_t i = 0; i < count; ++i) {
                std::uint32_t byte = 0;
                if (!bits.read(8, &byte)) { failed_ = true; return false; }
                followers[previous].values[i] = std::uint8_t(byte);
            }
        }

        decoded_.clear();
        decoded_.reserve(std::size_t(uncompressed_));
        std::uint8_t previous = 0;
        auto next = [&](std::uint8_t* out) {
            const Followers& set = followers[previous];
            std::uint32_t value = 0;
            if (!set.count) {
                if (!bits.read(8, &value)) return false;
            } else {
                if (!bits.read(1, &value)) return false;
                if (value) {
                    if (!bits.read(8, &value)) return false;
                } else {
                    std::uint32_t index = 0;
                    if (!bits.read(set.width, &index) || index >= set.count) return false;
                    value = set.values[index];
                }
            }
            *out = std::uint8_t(value);
            previous = *out;
            return true;
        };

        const unsigned lengthBits = unsigned(8 - factor_);
        const std::uint32_t lengthMask = (1u << lengthBits) - 1;
        while (decoded_.size() < std::size_t(uncompressed_)) {
            std::uint8_t value = 0;
            if (!next(&value)) { failed_ = true; decoded_.clear(); return false; }
            if (value != 0x90) {
                decoded_.push_back(value);
                continue;
            }
            std::uint8_t code = 0;
            if (!next(&code)) { failed_ = true; decoded_.clear(); return false; }
            if (!code) {
                decoded_.push_back(0x90);
                continue;
            }
            std::size_t length = code & lengthMask;
            if (length == lengthMask) {
                std::uint8_t extra = 0;
                if (!next(&extra)) { failed_ = true; decoded_.clear(); return false; }
                length += extra;
            }
            length += 3;
            std::uint8_t lowDistance = 0;
            if (!next(&lowDistance)) { failed_ = true; decoded_.clear(); return false; }
            const std::size_t distance =
                (std::size_t(code >> lengthBits) << 8) + lowDistance + 1;
            if (length > std::size_t(uncompressed_) - decoded_.size()) {
                failed_ = true; decoded_.clear(); return false;
            }
            for (std::size_t i = 0; i < length; ++i) {
                decoded_.push_back(distance > decoded_.size()
                    ? 0 : decoded_[decoded_.size() - distance]);
            }
        }
        decodedReady_ = true;
        return true;
    }

    fs::ByteStorePtr source_;
    std::int64_t offset_;
    std::int64_t compressed_;
    std::int64_t uncompressed_;
    int factor_;
    mutable std::mutex mutex_;
    mutable std::vector<std::uint8_t> decoded_;
    mutable bool decodedReady_ = false;
    mutable bool failed_ = false;
};

// ZIP method 1 ("Shrink") is decoded only when the member is actually read.
// Ordinary stored/deflated members keep their existing zero-copy/prefix paths.
class ZipShrinkStore final : public fs::IByteStore {
public:
    ZipShrinkStore(fs::ByteStorePtr source, std::int64_t offset,
                   std::int64_t compressed, std::int64_t uncompressed)
        : source_(std::move(source)), offset_(offset), compressed_(compressed),
          uncompressed_(uncompressed) {}

    std::int64_t capacity() const override { return uncompressed_; }
    bool cheapRandomAccess() const override { return false; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!source_ || pos < 0 || count <= 0 || pos >= uncompressed_) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ensureDecoded()) return 0;
        const int n = int(std::min<std::int64_t>(count, uncompressed_ - pos));
        std::copy(decoded_.begin() + std::ptrdiff_t(pos),
                  decoded_.begin() + std::ptrdiff_t(pos + n), dst);
        return n;
    }

private:
    class BitReader {
    public:
        explicit BitReader(const std::vector<std::uint8_t>& data) : data_(data) {}
        bool read(int bits, quint16* value) {
            while (available_ < bits) {
                if (position_ >= data_.size()) return false;
                buffer_ |= quint64(data_[position_++]) << available_;
                available_ += 8;
            }
            *value = quint16(buffer_ & ((quint64(1) << bits) - 1));
            buffer_ >>= bits;
            available_ -= bits;
            return true;
        }
    private:
        const std::vector<std::uint8_t>& data_;
        std::size_t position_ = 0;
        quint64 buffer_ = 0;
        int available_ = 0;
    };

    bool ensureDecoded() const {
        if (decodedReady_) return true;
        if (failed_ || compressed_ < 0 || uncompressed_ < 0 || offset_ < 0 ||
            std::uint64_t(compressed_) > std::numeric_limits<std::size_t>::max() ||
            std::uint64_t(uncompressed_) > std::numeric_limits<std::size_t>::max() ||
            offset_ > source_->capacity() - compressed_) {
            failed_ = true;
            return false;
        }
        const std::vector<std::uint8_t> input = source_->readRange(
            offset_, std::size_t(compressed_));
        if (input.size() != std::size_t(compressed_)) { failed_ = true; return false; }

        const int kTableSize = 8192;
        const quint16 kBogusCode = 256;
        const quint16 kCodeMask = 8191;
        const quint16 kFreeCode = 8192;
        const quint16 kHasChild = 16384;
        std::vector<quint16> parent(kTableSize, kFreeCode);
        std::vector<std::uint8_t> value(kTableSize, 0);
        std::vector<std::uint8_t> stack;
        stack.reserve(kTableSize);
        for (int code = 0; code < 256; ++code) {
            value[code] = std::uint8_t(code);
            parent[code] = kBogusCode;
        }

        BitReader reader(input);
        int codeSize = 9;
        quint16 oldCode = 0;
        if (!reader.read(codeSize, &oldCode) || oldCode >= 256) {
            failed_ = true;
            return false;
        }
        decoded_.clear();
        decoded_.reserve(std::size_t(uncompressed_));
        decoded_.push_back(std::uint8_t(oldCode));
        std::uint8_t finalValue = std::uint8_t(oldCode);
        int lastFreeCode = kBogusCode;

        while (decoded_.size() < std::size_t(uncompressed_)) {
            quint16 code = 0;
            if (!reader.read(codeSize, &code)) { failed_ = true; decoded_.clear(); return false; }
            if (code == kBogusCode) {
                quint16 command = 0;
                if (!reader.read(codeSize, &command)) { failed_ = true; decoded_.clear(); return false; }
                if (command == 1) {
                    if (++codeSize > 13) { failed_ = true; decoded_.clear(); return false; }
                } else if (command == 2) {
                    for (int i = kBogusCode + 1; i < kTableSize; ++i) {
                        const int p = parent[i] & kCodeMask;
                        if (parent[i] != kFreeCode && p > kBogusCode)
                            parent[p] = quint16(parent[p] | kHasChild);
                    }
                    for (int i = kBogusCode + 1; i < kTableSize; ++i) {
                        if (parent[i] & kHasChild)
                            parent[i] = quint16(parent[i] & ~kHasChild);
                        else
                            parent[i] = kFreeCode;
                    }
                    lastFreeCode = kBogusCode;
                } else {
                    failed_ = true; decoded_.clear(); return false;
                }
                continue;
            }
            if (code >= kTableSize) { failed_ = true; decoded_.clear(); return false; }

            const quint16 currentCode = code;
            stack.clear();
            if (parent[code] == kFreeCode) {
                stack.push_back(finalValue);
                code = oldCode;
            }
            int guard = 0;
            while (code != kBogusCode) {
                if (code >= kTableSize || ++guard > kTableSize) {
                    failed_ = true; decoded_.clear(); return false;
                }
                if (parent[code] == kFreeCode) {
                    stack.push_back(finalValue);
                    code = oldCode;
                } else {
                    stack.push_back(value[code]);
                    code = quint16(parent[code] & kCodeMask);
                }
            }
            if (stack.empty() || decoded_.size() + stack.size() > std::size_t(uncompressed_)) {
                failed_ = true; decoded_.clear(); return false;
            }
            finalValue = stack.back();
            for (auto it = stack.rbegin(); it != stack.rend(); ++it)
                decoded_.push_back(*it);

            int freeCode = lastFreeCode + 1;
            while (freeCode < kTableSize && parent[freeCode] != kFreeCode) ++freeCode;
            if (freeCode >= kTableSize) { failed_ = true; decoded_.clear(); return false; }
            lastFreeCode = freeCode;
            value[freeCode] = finalValue;
            parent[freeCode] = oldCode;
            oldCode = currentCode;
        }

        if (decoded_.size() != std::size_t(uncompressed_)) {
            failed_ = true; decoded_.clear(); return false;
        }
        decodedReady_ = true;
        return true;
    }

    fs::ByteStorePtr source_;
    std::int64_t offset_;
    std::int64_t compressed_;
    std::int64_t uncompressed_;
    mutable std::mutex mutex_;
    mutable std::vector<std::uint8_t> decoded_;
    mutable bool decodedReady_ = false;
    mutable bool failed_ = false;
};

class ZipCompressedInput final : public compression::InflateInput {
public:
    ZipCompressedInput(const fs::IByteStore& source, std::int64_t offset,
                       std::int64_t length)
        : source_(source), offset_(offset), length_(length), consumed_(0),
          bufferPosition_(0), bufferSize_(0) {}

    bool readByte(std::uint8_t* value) override {
        if (!value || consumed_ >= length_) return false;
        if (bufferPosition_ >= bufferSize_) {
            const std::int64_t remaining = length_ - consumed_;
            const int request = int(std::min<std::int64_t>(remaining, sizeof(buffer_)));
            const int got = source_.read(offset_ + consumed_, buffer_, request);
            if (got <= 0) return false;
            bufferPosition_ = 0;
            bufferSize_ = got;
        }
        *value = buffer_[bufferPosition_++];
        ++consumed_;
        return true;
    }

    std::size_t consumed() const override {
        return consumed_ < 0 ? 0 : static_cast<std::size_t>(consumed_);
    }

private:
    const fs::IByteStore& source_;
    std::int64_t offset_;
    std::int64_t length_;
    std::int64_t consumed_;
    std::uint8_t buffer_[64 * 1024];
    int bufferPosition_;
    int bufferSize_;
};

class ZipDeflateStore final : public fs::IByteStore {
public:
    ZipDeflateStore(fs::ByteStorePtr source, std::int64_t offset,
                    std::int64_t compressed, std::int64_t uncompressed)
        : source_(std::move(source)), offset_(offset), compressed_(compressed),
          uncompressed_(uncompressed) {}

    std::int64_t capacity() const override { return uncompressed_; }
    bool cheapRandomAccess() const override { return false; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!source_ || pos < 0 || count <= 0 || pos >= uncompressed_) return 0;
        const std::int64_t end = pos + std::min<std::int64_t>(count, uncompressed_ - pos);
        if (std::uint64_t(end) > std::numeric_limits<std::size_t>::max()) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ensurePrefix(std::size_t(end))) return 0;
        const int n = int(end - pos);
        std::copy(prefix_.begin() + std::ptrdiff_t(pos),
                  prefix_.begin() + std::ptrdiff_t(pos + n), dst);
        return n;
    }

private:
    bool ensurePrefix(std::size_t wanted) const {
        if (failed_) return false;
        if (prefix_.size() >= wanted) return true;
        if (uncompressed_ < 0 || std::uint64_t(uncompressed_) >
                std::numeric_limits<std::size_t>::max()) {
            failed_ = true;
            return false;
        }
        if (wanted > std::size_t(uncompressed_)) wanted = std::size_t(uncompressed_);
        if (compressed_ < 0 || offset_ < 0 ||
            offset_ > source_->capacity() - compressed_) {
            failed_ = true;
            return false;
        }

        ZipCompressedInput input(*source_, offset_, compressed_);
        std::vector<std::uint8_t> output;
        std::string error;
        const bool ok = wanted == std::size_t(uncompressed_)
            ? compression::inflateRawExact(input, wanted, nullptr, &output, &error)
            : compression::inflateRawPrefix(input, wanted, &output, &error);
        if (!ok) {
            failed_ = true;
            prefix_.clear();
            return false;
        }
        prefix_.swap(output);
        return true;
    }

    fs::ByteStorePtr source_;
    std::int64_t offset_;
    std::int64_t compressed_;
    std::int64_t uncompressed_;
    mutable std::mutex mutex_;
    mutable std::vector<std::uint8_t> prefix_;
    mutable bool failed_ = false;
};

class ZipAdvancedStore final : public fs::IByteStore {
public:
    ZipAdvancedStore(fs::ByteStorePtr source, std::int64_t offset,
                     std::int64_t compressed, std::int64_t uncompressed,
                     quint16 method, quint16 flags, quint32 expectedCrc)
        : source_(std::move(source)), offset_(offset), compressed_(compressed),
          uncompressed_(uncompressed), method_(method), flags_(flags),
          expectedCrc_(expectedCrc) {}

    std::int64_t capacity() const override { return uncompressed_; }
    bool cheapRandomAccess() const override { return false; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!source_ || pos < 0 || count <= 0 || pos >= uncompressed_) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ensureDecoded()) return 0;
        const int size = int(std::min<std::int64_t>(count, uncompressed_ - pos));
        std::copy(decoded_.begin() + std::ptrdiff_t(pos),
                  decoded_.begin() + std::ptrdiff_t(pos + size), dst);
        return size;
    }

private:
    bool ensureDecoded() const {
        if (decodedReady_) return true;
        if (failed_ || compressed_ < 0 || uncompressed_ < 0 || offset_ < 0 ||
            std::uint64_t(compressed_) > std::numeric_limits<std::size_t>::max() ||
            std::uint64_t(uncompressed_) > std::numeric_limits<std::size_t>::max() ||
            offset_ > source_->capacity() - compressed_) {
            failed_ = true;
            return false;
        }
        const std::vector<std::uint8_t> packed = source_->readRange(
            offset_, std::size_t(compressed_));
        if (packed.size() != std::size_t(compressed_)) {
            failed_ = true;
            return false;
        }

        std::vector<std::uint8_t> output;
        bool valid = false;
        if (method_ == 9) {
            valid = compression::inflateRawDeflate64Exact(packed.data(), packed.size(),
                std::size_t(uncompressed_), &output);
        } else if (method_ == 10) {
            valid = decodeDclZip(packed, std::size_t(uncompressed_), &output);
        } else if (method_ == 12) {
            valid = compression::decompressBzip2(packed.data(), packed.size(),
                std::size_t(uncompressed_), false, &output);
        } else if (method_ == 14 && packed.size() >= 9) {
            const std::size_t propertiesSize = le16(packed.data() + 2);
            if (propertiesSize == 5 && propertiesSize <= packed.size() - 4) {
                const std::uint8_t* properties = packed.data() + 4;
                const std::uint8_t* stream = properties + propertiesSize;
                const std::size_t streamSize = packed.size() - 4 - propertiesSize;
                valid = (flags_ & 0x0002u)
                    ? compression::decodeLzmaRawWithEndMarker(stream, streamSize,
                        properties, propertiesSize, std::size_t(uncompressed_), &output)
                    : compression::decodeLzmaRaw(stream, streamSize, properties,
                        propertiesSize, std::size_t(uncompressed_), &output);
            }
        } else if (method_ == 95) {
            valid = XzModule::decodePayload(packed, &output);
        } else if (method_ == 93) {
            valid = ZstdModule::decodePayload(packed, &output);
        } else if (method_ == 98) {
            valid = compression::decodePpmdZip(packed.data(), packed.size(),
                std::size_t(uncompressed_), &output);
        } else if (method_ == 97) {
            valid = compression::decodeWavPackZip(packed.data(), packed.size(),
                std::size_t(uncompressed_), &output);
        } else if (method_ == 94) {
            valid = compression::decodePackMp3(packed.data(), packed.size(),
                std::size_t(uncompressed_), &output);
        } else if (method_ == 96) {
            valid = compression::decodeWinZipJpeg(packed.data(), packed.size(),
                std::size_t(uncompressed_), &output);
        }
        if (!valid || output.size() != std::size_t(uncompressed_) ||
            compression::crc32(output.data(), output.size()) != expectedCrc_) {
            failed_ = true;
            return false;
        }
        decoded_.swap(output);
        decodedReady_ = true;
        return true;
    }

    fs::ByteStorePtr source_;
    std::int64_t offset_;
    std::int64_t compressed_;
    std::int64_t uncompressed_;
    quint16 method_;
    quint16 flags_;
    quint32 expectedCrc_;
    mutable std::mutex mutex_;
    mutable std::vector<std::uint8_t> decoded_;
    mutable bool decodedReady_ = false;
    mutable bool failed_ = false;
};

class ZipExplodeStore final : public fs::IByteStore {
public:
    ZipExplodeStore(fs::ByteStorePtr source, std::int64_t offset,
                    std::int64_t compressed, std::int64_t uncompressed,
                    quint16 flags)
        : source_(std::move(source)), offset_(offset), compressed_(compressed),
          uncompressed_(uncompressed), flags_(flags) {}

    std::int64_t capacity() const override { return uncompressed_; }
    bool cheapRandomAccess() const override { return false; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!source_ || pos < 0 || count <= 0 || pos >= uncompressed_) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ensureDecoded()) return 0;
        const int n = int(std::min<std::int64_t>(count, uncompressed_ - pos));
        std::copy(decoded_.begin() + std::ptrdiff_t(pos),
                  decoded_.begin() + std::ptrdiff_t(pos + n), dst);
        return n;
    }

private:
    class BitReader {
    public:
        explicit BitReader(const std::vector<std::uint8_t>& input) : input_(input) {}
        bool read(int count, unsigned* value) {
            while (available_ < count) {
                if (position_ >= input_.size()) return false;
                bits_ |= quint64(input_[position_++]) << available_;
                available_ += 8;
            }
            *value = unsigned(bits_ & ((quint64(1) << count) - 1));
            bits_ >>= count;
            available_ -= count;
            return true;
        }
    private:
        const std::vector<std::uint8_t>& input_;
        std::size_t position_ = 0;
        quint64 bits_ = 0;
        int available_ = 0;
    };

    struct Huffman {
        int counts[17] = {};
        std::vector<int> symbols;
    };

    static bool readTree(BitReader* bits, int symbolCount, Huffman* tree) {
        unsigned packedCount = 0;
        if (!bits->read(8, &packedCount)) return false;
        const int repetitions = int(packedCount) + 1;
        std::vector<int> lengths;
        lengths.reserve(symbolCount);
        for (int i = 0; i < repetitions; ++i) {
            unsigned packed = 0;
            if (!bits->read(8, &packed)) return false;
            const int run = int(packed >> 4) + 1;
            const int length = int(packed & 15) + 1;
            if (run > symbolCount - int(lengths.size())) return false;
            lengths.insert(lengths.end(), std::size_t(run), length);
        }
        if (int(lengths.size()) != symbolCount) return false;
        int left = 1;
        for (int length : lengths) ++tree->counts[length];
        if (tree->counts[0] == symbolCount) return false;
        for (int length = 1; length <= 16; ++length) {
            left = (left << 1) - tree->counts[length];
            if (left < 0) return false;
        }
        tree->symbols.resize(std::size_t(symbolCount - tree->counts[0]));
        int offsets[17] = {};
        for (int length = 1; length < 16; ++length)
            offsets[length + 1] = offsets[length] + tree->counts[length];
        for (int symbol = 0; symbol < symbolCount; ++symbol) {
            const int length = lengths[std::size_t(symbol)];
            if (length) tree->symbols[std::size_t(offsets[length]++)] = symbol;
        }
        return true;
    }

    static bool decode(BitReader* bits, const Huffman& tree, int* symbol) {
        unsigned code = 0;
        unsigned first = 0;
        int index = 0;
        for (int length = 1; length <= 16; ++length) {
            unsigned bit = 0;
            if (!bits->read(1, &bit)) return false;
            code |= bit ^ 1u;
            const unsigned count = unsigned(tree.counts[length]);
            if (code >= first && code - first < count) {
                const std::size_t slot = std::size_t(index) + code - first;
                if (slot >= tree.symbols.size()) return false;
                *symbol = tree.symbols[slot];
                return true;
            }
            index += int(count);
            first = (first + count) << 1;
            code <<= 1;
        }
        return false;
    }

    bool ensureDecoded() const {
        if (decodedReady_) return true;
        if (failed_ || !source_ || compressed_ < 0 || uncompressed_ < 0 || offset_ < 0 ||
            std::uint64_t(compressed_) > std::numeric_limits<std::size_t>::max() ||
            std::uint64_t(uncompressed_) > std::numeric_limits<std::size_t>::max() ||
            offset_ > source_->capacity() - compressed_) {
            failed_ = true;
            return false;
        }
        const std::vector<std::uint8_t> input = source_->readRange(
            offset_, std::size_t(compressed_));
        if (input.size() != std::size_t(compressed_)) { failed_ = true; return false; }

        BitReader bits(input);
        Huffman literalTree;
        Huffman lengthTree;
        Huffman distanceTree;
        const bool codedLiterals = flags_ & 0x0004;
        const int distanceBits = (flags_ & 0x0002) ? 7 : 6;
        if ((codedLiterals && !readTree(&bits, 256, &literalTree)) ||
            !readTree(&bits, 64, &lengthTree) ||
            !readTree(&bits, 64, &distanceTree)) {
            failed_ = true;
            return false;
        }

        const int minimumLength = codedLiterals ? 3 : 2;
        decoded_.clear();
        decoded_.reserve(std::size_t(uncompressed_));
        while (decoded_.size() < std::size_t(uncompressed_)) {
            unsigned isLiteral = 0;
            if (!bits.read(1, &isLiteral)) { failed_ = true; decoded_.clear(); return false; }
            if (isLiteral) {
                unsigned literal = 0;
                if (codedLiterals) {
                    int value = 0;
                    if (!decode(&bits, literalTree, &value)) {
                        failed_ = true; decoded_.clear(); return false;
                    }
                    literal = unsigned(value);
                } else if (!bits.read(8, &literal)) {
                    failed_ = true; decoded_.clear(); return false;
                }
                decoded_.push_back(std::uint8_t(literal));
                continue;
            }

            unsigned lowDistance = 0;
            if (!bits.read(distanceBits, &lowDistance)) {
                failed_ = true; decoded_.clear(); return false;
            }
            int distanceCode = 0;
            int lengthCode = 0;
            unsigned extraLength = 0;
            if (!decode(&bits, distanceTree, &distanceCode) ||
                !decode(&bits, lengthTree, &lengthCode) ||
                (lengthCode == 63 && !bits.read(8, &extraLength))) {
                failed_ = true; decoded_.clear(); return false;
            }
            const std::size_t distance =
                ((std::size_t(distanceCode) << distanceBits) | lowDistance) + 1;
            const int length = lengthCode + int(extraLength) + minimumLength;
            if (std::size_t(length) > std::size_t(uncompressed_) - decoded_.size()) {
                failed_ = true; decoded_.clear(); return false;
            }
            for (int i = 0; i < length; ++i)
                decoded_.push_back(distance > decoded_.size()
                    ? 0 : decoded_[decoded_.size() - distance]);
        }
        if (decoded_.size() != std::size_t(uncompressed_)) {
            failed_ = true;
            decoded_.clear();
            return false;
        }
        decodedReady_ = true;
        return true;
    }

    fs::ByteStorePtr source_;
    std::int64_t offset_;
    std::int64_t compressed_;
    std::int64_t uncompressed_;
    quint16 flags_;
    mutable std::mutex mutex_;
    mutable std::vector<std::uint8_t> decoded_;
    mutable bool decodedReady_ = false;
    mutable bool failed_ = false;
};

std::int64_t findEocd(const fs::IByteStore& store) {
    if (store.capacity() < 22) return -1;
    const std::int64_t tailSize = std::min<std::int64_t>(store.capacity(), 65535 + 22);
    const std::int64_t tailOffset = store.capacity() - tailSize;
    const QByteArray tail = readArray(store, tailOffset, tailSize);
    if (tail.size() != tailSize) return -1;
    for (int pos = tail.size() - 22; pos >= 0; --pos) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(tail.constData() + pos);
        if (le32(p) != kZipEndCentralDirectory) continue;
        const quint16 comment = le16(p + 20);
        if (pos + 22 + comment == tail.size()) return tailOffset + pos;
    }
    return -1;
}


QString resourcePath(const ResourceEntry& entry) {
    QStringList path = entry.hierarchyPath;
    path.push_back(entry.name);
    return path.join(QLatin1Char('/'));
}

const ResourceEntry* findResource(const QVector<ResourceEntry>& resources,
                                  const QString& wanted) {
    for (const ResourceEntry& entry : resources)
        if (resourcePath(entry).compare(wanted, Qt::CaseInsensitive) == 0)
            return &entry;
    return nullptr;
}

bool hasResource(const QVector<ResourceEntry>& resources, const QString& wanted) {
    return findResource(resources, wanted) != nullptr;
}

QByteArray resourceBytes(const QVector<ResourceEntry>& resources,
                         const QString& wanted,
                         std::int64_t maximum = 2 * 1024 * 1024) {
    const ResourceEntry* entry = findResource(resources, wanted);
    if (!entry) return QByteArray();
    if (!entry->data.isEmpty())
        return entry->data.size() <= maximum ? entry->data : QByteArray();
    if (!entry->content || entry->content->capacity() < 0 ||
        entry->content->capacity() > maximum)
        return QByteArray();
    const std::vector<std::uint8_t> bytes = entry->content->readAll();
    return QByteArray(reinterpret_cast<const char*>(bytes.data()), int(bytes.size()));
}

QString xmlAttribute(const QXmlStreamAttributes& attributes, const QString& name) {
    for (const QXmlStreamAttribute& attribute : attributes)
        if (attribute.name().compare(name, Qt::CaseInsensitive) == 0)
            return attribute.value().toString().trimmed();
    return QString();
}

QString compactMetadata(QString value) {
    value.replace(QLatin1Char('\r'), QLatin1Char(' '));
    value.replace(QLatin1Char('\n'), QLatin1Char(' '));
    value = value.simplified();
    if (value.size() > 160) value = value.left(157) + QStringLiteral("...");
    return value;
}

void appendMetadata(QStringList* values, const QString& label, const QString& value) {
    const QString clean = compactMetadata(value);
    if (!clean.isEmpty()) values->push_back(label + QLatin1Char('=') + clean);
}

ModuleFormat classifyPackage(const QVector<ResourceEntry>& resources,
                             const QString& sourceName) {
    if (ModuleFormatDetector::zipPackageFormatForName(sourceName) ==
            ModuleFormat::ANDROID_APK &&
        hasResource(resources, QStringLiteral("AndroidManifest.xml")))
        return ModuleFormat::ANDROID_APK;
    if (hasResource(resources, QStringLiteral("AppxMetadata/AppxBundleManifest.xml")))
        return ModuleFormat::APPXBUNDLE;
    if (hasResource(resources, QStringLiteral("AppxManifest.xml")) &&
        hasResource(resources, QStringLiteral("[Content_Types].xml"))) {
        return ModuleFormatDetector::zipPackageFormatForName(sourceName) == ModuleFormat::MSIX
            ? ModuleFormat::MSIX : ModuleFormat::APPX;
    }
    if (hasResource(resources, QStringLiteral("AppManifest.xaml")) ||
        hasResource(resources, QStringLiteral("WMAppManifest.xml")))
        return ModuleFormat::XAP;
    return ModuleFormat::ZIP;
}

QString appPackageDescription(const QVector<ResourceEntry>& resources,
                              ModuleFormat format) {
    const QByteArray manifest = resourceBytes(resources, QStringLiteral("AppxManifest.xml"));
    QString name, publisher, version, architecture, displayName;
    QXmlStreamReader xml(manifest);
    xml.setEntityExpansionLimit(1024);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement()) continue;
        if (xml.name().compare(QLatin1String("Identity"), Qt::CaseInsensitive) == 0) {
            name = xmlAttribute(xml.attributes(), QStringLiteral("Name"));
            publisher = xmlAttribute(xml.attributes(), QStringLiteral("Publisher"));
            version = xmlAttribute(xml.attributes(), QStringLiteral("Version"));
            architecture = xmlAttribute(xml.attributes(), QStringLiteral("ProcessorArchitecture"));
        } else if (displayName.isEmpty() &&
                   xml.name().compare(QLatin1String("DisplayName"), Qt::CaseInsensitive) == 0) {
            displayName = xml.readElementText(QXmlStreamReader::IncludeChildElements).trimmed();
        }
    }

    QStringList details;
    appendMetadata(&details, QStringLiteral("Name"), name);
    appendMetadata(&details, QStringLiteral("DisplayName"), displayName);
    appendMetadata(&details, QStringLiteral("Version"), version);
    appendMetadata(&details, QStringLiteral("Architecture"), architecture);
    appendMetadata(&details, QStringLiteral("Publisher"), publisher);
    details.push_back(QStringLiteral("%1 files").arg(resources.size()));
    const QString label = format == ModuleFormat::MSIX
        ? QStringLiteral("Microsoft MSIX application package")
        : QStringLiteral("Microsoft APPX application package");
    return label + QStringLiteral(" — ") + details.join(QStringLiteral("; "));
}

QString appBundleDescription(const QVector<ResourceEntry>& resources) {
    const QByteArray manifest = resourceBytes(
        resources, QStringLiteral("AppxMetadata/AppxBundleManifest.xml"));
    QString name, publisher, version;
    int packageCount = 0;
    QXmlStreamReader xml(manifest);
    xml.setEntityExpansionLimit(1024);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement()) continue;
        if (xml.name().compare(QLatin1String("Identity"), Qt::CaseInsensitive) == 0) {
            name = xmlAttribute(xml.attributes(), QStringLiteral("Name"));
            publisher = xmlAttribute(xml.attributes(), QStringLiteral("Publisher"));
            version = xmlAttribute(xml.attributes(), QStringLiteral("Version"));
        } else if (xml.name().compare(QLatin1String("Package"), Qt::CaseInsensitive) == 0) {
            ++packageCount;
        }
    }

    QStringList details;
    appendMetadata(&details, QStringLiteral("Name"), name);
    appendMetadata(&details, QStringLiteral("Version"), version);
    appendMetadata(&details, QStringLiteral("Publisher"), publisher);
    details.push_back(QStringLiteral("%1 package entries").arg(packageCount));
    details.push_back(QStringLiteral("%1 files").arg(resources.size()));
    return QStringLiteral("Microsoft APPX application bundle — ") +
           details.join(QStringLiteral("; "));
}

QString xapDescription(const QVector<ResourceEntry>& resources) {
    QByteArray manifest = resourceBytes(resources, QStringLiteral("WMAppManifest.xml"));
    if (manifest.isEmpty())
        manifest = resourceBytes(resources, QStringLiteral("AppManifest.xaml"));
    QString title, version, productId, entryAssembly, entryType, runtimeVersion;
    QXmlStreamReader xml(manifest);
    xml.setEntityExpansionLimit(1024);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement()) continue;
        if (xml.name().compare(QLatin1String("Deployment"), Qt::CaseInsensitive) == 0) {
            entryAssembly = xmlAttribute(xml.attributes(), QStringLiteral("EntryPointAssembly"));
            entryType = xmlAttribute(xml.attributes(), QStringLiteral("EntryPointType"));
            runtimeVersion = xmlAttribute(xml.attributes(), QStringLiteral("RuntimeVersion"));
        } else if (xml.name().compare(QLatin1String("App"), Qt::CaseInsensitive) == 0) {
            title = xmlAttribute(xml.attributes(), QStringLiteral("Title"));
            version = xmlAttribute(xml.attributes(), QStringLiteral("Version"));
            productId = xmlAttribute(xml.attributes(), QStringLiteral("ProductID"));
            if (productId.isEmpty())
                productId = xmlAttribute(xml.attributes(), QStringLiteral("ProductId"));
        }
    }

    QStringList details;
    appendMetadata(&details, QStringLiteral("Title"), title);
    appendMetadata(&details, QStringLiteral("Version"), version);
    appendMetadata(&details, QStringLiteral("ProductID"), productId);
    appendMetadata(&details, QStringLiteral("EntryAssembly"), entryAssembly);
    appendMetadata(&details, QStringLiteral("EntryType"), entryType);
    appendMetadata(&details, QStringLiteral("Runtime"), runtimeVersion);
    details.push_back(QStringLiteral("%1 files").arg(resources.size()));
    return QStringLiteral("Silverlight / Windows Phone XAP package — ") +
           details.join(QStringLiteral("; "));
}

QString invalidPackageError(ModuleFormat expected) {
    switch (expected) {
    case ModuleFormat::APPX:
    case ModuleFormat::MSIX:
        return QStringLiteral("Invalid APPX/MSIX package: AppxManifest.xml or [Content_Types].xml is missing");
    case ModuleFormat::APPXBUNDLE:
        return QStringLiteral("Invalid APPXBUNDLE package: AppxMetadata/AppxBundleManifest.xml is missing");
    case ModuleFormat::XAP:
        return QStringLiteral("Invalid XAP package: AppManifest.xaml or WMAppManifest.xml is missing");
    case ModuleFormat::ANDROID_APK:
        return QStringLiteral("Invalid Android APK: AndroidManifest.xml is missing");
    default:
        return QString();
    }
}

bool packageMatchesExpectation(ModuleFormat actual, ModuleFormat expected) {
    if (expected == ModuleFormat::Unknown) return true;
    if (expected == ModuleFormat::APPX || expected == ModuleFormat::MSIX)
        return actual == ModuleFormat::APPX || actual == ModuleFormat::MSIX;
    return actual == expected;
}

}  // namespace

std::unique_ptr<ZipModule> ZipModule::open(const QString& filePath) {
    return open(filePath, filePath);
}

std::unique_ptr<ZipModule> ZipModule::open(const QString& physicalPath,
                                           const QString& logicalName) {
    ModulePtr base = open(storeForFile(physicalPath), logicalName);
    return std::unique_ptr<ZipModule>(static_cast<ZipModule*>(base.release()));
}

std::unique_ptr<ZipModule> ZipModule::open(const QByteArray& data, const QString& logicalName) {
    ModulePtr base = open(std::make_shared<fs::MemoryStore>(
                              reinterpret_cast<const std::uint8_t*>(data.constData()),
                              std::size_t(data.size())),
                          logicalName);
    return std::unique_ptr<ZipModule>(static_cast<ZipModule*>(base.release()));
}

ModulePtr ZipModule::open(const fs::ByteStorePtr& file, const QString& sourceName,
                          const QString& subPath) {
    auto module = peare::makeUnique<ZipModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::ZIP;
    module->info_.description = QStringLiteral("ZIP archive");
    if (!file) {
        module->info_.error = QStringLiteral("Cannot open ZIP archive");
        return ModulePtr(std::move(module));
    }
    module->file_ = file;

    const std::int64_t eocd = findEocd(*file);
    std::uint8_t end[22];
    if (eocd < 0 || !readExact(*file, eocd, end, sizeof(end))) {
        module->info_.error = QStringLiteral("ZIP end of central directory not found");
        return ModulePtr(std::move(module));
    }
    quint64 disk = le16(end + 4);
    quint64 centralDisk = le16(end + 6);
    quint64 entriesOnDisk = le16(end + 8);
    quint64 entries = le16(end + 10);
    quint64 cdSize = le32(end + 12);
    quint64 cdOffset = le32(end + 16);

    const bool needsZip64 = entries == 0xffffu || entriesOnDisk == 0xffffu ||
                            cdSize == 0xffffffffu || cdOffset == 0xffffffffu;
    if (needsZip64) {
        std::uint8_t locator[20];
        if (eocd < 20 || !readExact(*file, eocd - 20, locator, sizeof(locator)) ||
            le32(locator) != 0x07064b50u) {
            module->info_.error = QStringLiteral("Invalid ZIP64 locator");
            return ModulePtr(std::move(module));
        }
        const quint64 zip64Offset = le64(locator + 8);
        std::uint8_t z64[56];
        if (zip64Offset > quint64(file->capacity()) - sizeof(z64) ||
            !readExact(*file, std::int64_t(zip64Offset), z64, sizeof(z64)) ||
            le32(z64) != 0x06064b50u) {
            module->info_.error = QStringLiteral("Invalid ZIP64 end of central directory");
            return ModulePtr(std::move(module));
        }
        disk = le32(z64 + 16);
        centralDisk = le32(z64 + 20);
        entriesOnDisk = le64(z64 + 24);
        entries = le64(z64 + 32);
        cdSize = le64(z64 + 40);
        cdOffset = le64(z64 + 48);
    }
    if (disk || centralDisk || entriesOnDisk != entries) {
        module->info_.error = QStringLiteral("Multi-disk ZIP archives are not supported");
        return ModulePtr(std::move(module));
    }
    if (entries > 1000000 || cdOffset > quint64(file->capacity()) ||
        cdSize > quint64(file->capacity()) - cdOffset) {
        module->info_.error = QStringLiteral("Invalid ZIP central directory range");
        return ModulePtr(std::move(module));
    }

    QStringList explicitDirectories;
    std::int64_t pos = std::int64_t(cdOffset);
    for (quint64 i = 0; i < entries; ++i) {
        std::uint8_t central[46];
        if (!readExact(*file, pos, central, sizeof(central)) ||
            le32(central) != kZipCentralHeader) {
            module->info_.error = QStringLiteral("Invalid ZIP central directory");
            return ModulePtr(std::move(module));
        }
        const quint16 flags = le16(central + 8);
        const quint16 method = le16(central + 10);
        quint64 compressedSize = le32(central + 20);
        quint64 uncompressedSize = le32(central + 24);
        const quint16 nameLen = le16(central + 28);
        const quint16 extraLen = le16(central + 30);
        const quint16 commentLen = le16(central + 32);
        quint64 localOffset = le32(central + 42);
        quint32 diskStart = le16(central + 34);
        const std::int64_t variable = std::int64_t(nameLen) + extraLen + commentLen;
        if (variable > kMaxZipVariableField || pos > file->capacity() - 46 - variable) {
            module->info_.error = QStringLiteral("Truncated ZIP central directory");
            return ModulePtr(std::move(module));
        }
        const QByteArray rawName = readArray(*file, pos + 46, nameLen);
        const QByteArray extra = readArray(*file, pos + 46 + nameLen, extraLen);
        if (rawName.size() != nameLen || extra.size() != extraLen) {
            module->info_.error = QStringLiteral("Truncated ZIP entry metadata");
            return ModulePtr(std::move(module));
        }
        const bool needUncompressed = uncompressedSize == 0xffffffffu;
        const bool needCompressed = compressedSize == 0xffffffffu;
        const bool needOffset = localOffset == 0xffffffffu;
        const bool needDisk = diskStart == 0xffffu;
        if ((needUncompressed || needCompressed || needOffset || needDisk) &&
            !resolveZip64Extra(extra, needUncompressed, needCompressed, needOffset,
                               needDisk, &uncompressedSize, &compressedSize,
                               &localOffset, &diskStart)) {
            module->info_.error = QStringLiteral("Invalid ZIP64 entry extra field");
            return ModulePtr(std::move(module));
        }
        QString fullName = normalizeName(decodeZipName(rawName, flags));
        pos += 46 + variable;
        const bool directory = fullName.endsWith(QLatin1Char('/'));
        while (fullName.endsWith(QLatin1Char('/'))) fullName.chop(1);
        if (fullName.isEmpty()) continue;
        if (directory) {
            QStringList hierarchy;
            QString leaf;
            if (splitSafeName(fullName, &hierarchy, &leaf))
                explicitDirectories.push_back((hierarchy + QStringList{leaf}).join(QLatin1Char('/')));
            continue;
        }
        if (flags & 0x0001) {
            module->info_.error = QStringLiteral("Encrypted ZIP entries are not supported");
            return ModulePtr(std::move(module));
        }
        if (diskStart || localOffset > quint64(std::numeric_limits<std::int64_t>::max()) ||
            compressedSize > quint64(std::numeric_limits<std::int64_t>::max()) ||
            uncompressedSize > quint64(std::numeric_limits<std::int64_t>::max())) {
            module->info_.error = QStringLiteral("ZIP entry exceeds supported size or disk limits");
            return ModulePtr(std::move(module));
        }

        std::uint8_t local[30];
        if (localOffset > quint64(file->capacity()) ||
            !readExact(*file, std::int64_t(localOffset), local, sizeof(local)) ||
            le32(local) != kZipLocalHeader) {
            module->info_.error = QStringLiteral("Invalid ZIP local header");
            return ModulePtr(std::move(module));
        }
        const quint16 localNameLen = le16(local + 26);
        const quint16 localExtraLen = le16(local + 28);
        const std::int64_t localHeaderOffset = std::int64_t(localOffset);
        if (localHeaderOffset > file->capacity() - 30 - localNameLen - localExtraLen) {
            module->info_.error = QStringLiteral("Truncated ZIP local header");
            return ModulePtr(std::move(module));
        }
        const std::int64_t dataOffset = localHeaderOffset + 30 + localNameLen + localExtraLen;
        if (dataOffset > file->capacity() - std::int64_t(compressedSize)) {
            module->info_.error = QStringLiteral("Truncated ZIP file data");
            return ModulePtr(std::move(module));
        }

        fs::ByteStorePtr content;
        if (method == 0) {
            if (compressedSize != uncompressedSize) {
                module->info_.error = QStringLiteral("Invalid stored ZIP entry size");
                return ModulePtr(std::move(module));
            }
            content = std::make_shared<fs::SubStore>(file, dataOffset, uncompressedSize);
        } else if (method == 1) {
            content = std::make_shared<ZipShrinkStore>(file, dataOffset,
                                                       compressedSize, uncompressedSize);
        } else if (method >= 2 && method <= 5) {
            content = std::make_shared<ZipReduceStore>(file, dataOffset,
                compressedSize, uncompressedSize, method - 1);
        } else if (method == 8) {
            content = std::make_shared<ZipDeflateStore>(file, dataOffset,
                                                        compressedSize, uncompressedSize);
        } else if (method == 6) {
            content = std::make_shared<ZipExplodeStore>(file, dataOffset,
                compressedSize, uncompressedSize, flags);
        } else if (method == 9 || method == 10 || method == 12 || method == 14 || method == 93 ||
                   method == 94 || method == 95 || method == 96 || method == 97 || method == 98) {
            content = std::make_shared<ZipAdvancedStore>(file, dataOffset,
                compressedSize, uncompressedSize, method, flags, le32(central + 16));
        } else {
            module->info_.error = QStringLiteral("Unsupported ZIP compression method %1").arg(method);
            return ModulePtr(std::move(module));
        }

        QStringList hierarchy;
        QString leaf;
        if (!splitSafeName(fullName, &hierarchy, &leaf)) continue;
        ResourceEntry entry;
        entry.type = QStringLiteral("ZIP_FILE");
        entry.isEmbeddedFile = true;
        entry.name = leaf;
        entry.language = QStringLiteral("neutral");
        entry.dataOffset = quint64(dataOffset);
        entry.dataSize = quint64(uncompressedSize);
        entry.format = ModuleFormat::ZIP;
        entry.hierarchyPath = hierarchy;
        entry.content = content;
        module->resources_.push_back(std::move(entry));
    }

    const QString requestedDirectory = normalizeName(subPath);
    if (!requestedDirectory.isEmpty() && explicitDirectories.contains(requestedDirectory)) {
        bool hasDescendant = false;
        const QString prefix = requestedDirectory + QLatin1Char('/');
        for (const ResourceEntry& entry : module->resources_) {
            const QString path = resourcePath(entry);
            if (path == requestedDirectory || path.startsWith(prefix)) {
                hasDescendant = true;
                break;
            }
        }
        if (!hasDescendant) {
            module->resources_.clear();
            return ModulePtr(std::move(module));
        }
    }

    for (const QString& path : explicitDirectories) {
        const QString prefix = path + QLatin1Char('/');
        bool hasDescendant = false;
        for (const ResourceEntry& entry : module->resources_) {
            const QString entryPath = resourcePath(entry);
            if (entryPath == path || entryPath.startsWith(prefix)) {
                hasDescendant = true;
                break;
            }
        }
        if (hasDescendant) continue;
        const QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (parts.isEmpty()) continue;
        ResourceEntry entry;
        entry.type = QStringLiteral("ZIP_DIR");
        entry.name = parts.last();
        entry.language = QStringLiteral("neutral");
        entry.format = ModuleFormat::ZIP;
        entry.hierarchyPath = parts;
        entry.hierarchyPath.removeLast();
        entry.content = file;
        entry.isDirectory = true;
        entry.containerSubPath = path;
        module->resources_.push_back(std::move(entry));
    }

    const ModuleFormat expected = ModuleFormatDetector::zipPackageFormatForName(sourceName);
    const ModuleFormat actual = classifyPackage(module->resources_, sourceName);
    if (!packageMatchesExpectation(actual, expected)) {
        module->info_.format = expected;
        module->info_.description = ModuleFormatDetector::formatName(expected);
        module->info_.error = invalidPackageError(expected);
        return ModulePtr(std::move(module));
    }

    module->info_.format = actual;
    if (actual == ModuleFormat::ANDROID_APK)
        module->info_.description = apkSigningDescription(*file, cdOffset);
    if (actual == ModuleFormat::APPX || actual == ModuleFormat::MSIX)
        module->info_.description = appPackageDescription(module->resources_, actual);
    else if (actual == ModuleFormat::APPXBUNDLE)
        module->info_.description = appBundleDescription(module->resources_);
    else if (actual == ModuleFormat::XAP)
        module->info_.description = xapDescription(module->resources_);

    const QString resourceType = actual == ModuleFormat::APPXBUNDLE
        ? QStringLiteral("APP_BUNDLE_FILE")
        : (actual == ModuleFormat::APPX || actual == ModuleFormat::MSIX)
            ? QStringLiteral("APP_PACKAGE_FILE")
            : actual == ModuleFormat::XAP
                ? QStringLiteral("XAP_FILE")
                : actual == ModuleFormat::ANDROID_APK
                    ? QStringLiteral("APK_FILE")
                : QStringLiteral("ZIP_FILE");
    for (ResourceEntry& entry : module->resources_) {
        entry.type = resourceType;
        entry.format = actual;
    }
    return ModulePtr(std::move(module));
}

}  // namespace peare
