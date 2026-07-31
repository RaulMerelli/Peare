#include "XbeModule.h"

#include <QCryptographicHash>
#include <QFile>
#include <QStringList>

namespace peare {
namespace {
quint16 le16(const QByteArray& data, qsizetype offset)
{
    if (offset < 0 || offset + 2 > data.size()) return 0;
    const auto* p = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

quint32 le32(const QByteArray& data, qsizetype offset)
{
    if (offset < 0 || offset + 4 > data.size()) return 0;
    const auto* p = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

bool vaToOffset(quint32 address, quint32 baseAddress, qsizetype size, qsizetype* result)
{
    if (!result || address < baseAddress) return false;
    const quint64 offset = quint64(address) - baseAddress;
    if (offset >= quint64(size)) return false;
    *result = qsizetype(offset);
    return true;
}

QString readAsciiZ(const QByteArray& data, qsizetype offset)
{
    if (offset < 0 || offset >= data.size()) return {};
    const qsizetype end = data.indexOf('\0', offset);
    const qsizetype length = end < 0 ? qMin<qsizetype>(64, data.size() - offset)
                                     : qMin<qsizetype>(64, end - offset);
    return QString::fromLatin1(data.constData() + offset, length);
}

QString readUtf16LeZ(const QByteArray& data, qsizetype offset, qsizetype codeUnits)
{
    if (offset < 0 || codeUnits < 0 || quint64(offset) + quint64(codeUnits) * 2u > quint64(data.size())) return {};
    QString result;
    result.reserve(int(codeUnits));
    for (qsizetype i = 0; i < codeUnits; ++i) {
        const qsizetype p = offset + i * 2;
        const ushort value = ushort(uchar(data.at(int(p)))) | (ushort(uchar(data.at(int(p + 1)))) << 8);
        if (value == 0) break;
        result.append(QChar(value));
    }
    return result.trimmed();
}

void appendResource(QVector<ResourceEntry>* resources,
                    ModuleFormat format,
                    const QString& type,
                    const QString& name,
                    quint64 offset,
                    const QByteArray& payload,
                    int baseId)
{
    if (!resources) return;
    ResourceEntry entry;
    entry.type = type;
    entry.name = name;
    entry.language = QStringLiteral("neutral");
    entry.dataOffset = offset;
    entry.dataSize = quint64(payload.size());
    entry.codePage = 0;
    entry.format = format;
    entry.baseId = baseId;
    entry.data = payload;
    resources->push_back(std::move(entry));
}


QString librarySummary(const QByteArray& record)
{
    if (record.size() < 16) return {};
    const QString name = QString::fromLatin1(record.constData(), 8).trimmed();
    const quint16 major = le16(record, 8);
    const quint16 minor = le16(record, 10);
    const quint16 build = le16(record, 12);
    const quint16 flags = le16(record, 14);
    return QStringLiteral("%1 %2.%3.%4 flags=0x%5")
        .arg(name.isEmpty() ? QStringLiteral("(unnamed)") : name)
        .arg(major).arg(minor).arg(build)
        .arg(flags, 4, 16, QLatin1Char('0')).toUpper();
}

void appendLibraryVersion(QVector<ResourceEntry>* resources, const QByteArray& data,
                          quint32 address, quint32 baseAddress, const QString& name, int baseId)
{
    qsizetype offset = 0;
    if (!vaToOffset(address, baseAddress, data.size(), &offset) || offset + 16 > data.size()) return;
    const QByteArray record = data.mid(offset, 16);
    appendResource(resources, ModuleFormat::XBE, QStringLiteral("XBE_LIBRARY_VERSION"),
                   name + QStringLiteral(".bin"), quint64(offset), record, baseId);
    appendResource(resources, ModuleFormat::XBE, QStringLiteral("XBE_LIBRARY_METADATA"),
                   name + QStringLiteral(".txt"), quint64(offset),
                   (librarySummary(record) + QLatin1Char('\n')).toUtf8(), baseId - 1000);
}

struct EmbeddedResourceCandidate {
    qsizetype relativeOffset = 0;
    qsizetype size = 0;
    QString type;
    QString extension;
};

bool isXprMagic(const QByteArray& data, qsizetype offset)
{
    if (offset < 0 || offset + 4 > data.size()) return false;
    return data.mid(offset, 4) == QByteArrayLiteral("XPR0") ||
           data.mid(offset, 4) == QByteArrayLiteral("XPR1") ||
           data.mid(offset, 4) == QByteArrayLiteral("XPR2");
}

QVector<EmbeddedResourceCandidate> findEmbeddedResources(const QByteArray& section)
{
    QVector<EmbeddedResourceCandidate> found;
    for (qsizetype offset = 0; offset + 4 <= section.size(); ++offset) {
        if (isXprMagic(section, offset)) {
            if (offset + 12 > section.size()) continue;
            const quint32 totalSize = le32(section, offset + 4);
            const quint32 headerSize = le32(section, offset + 8);
            if (totalSize < 12 || headerSize < 12 || headerSize > totalSize ||
                quint64(offset) + totalSize > quint64(section.size())) continue;
            EmbeddedResourceCandidate candidate;
            candidate.relativeOffset = offset;
            candidate.size = qsizetype(totalSize);
            candidate.type = QStringLiteral("XPR_PACKAGE");
            candidate.extension = QStringLiteral(".xpr");
            found.push_back(candidate);
            offset += qsizetype(totalSize) - 1;
            continue;
        }
        if (section.mid(offset, 4) == QByteArrayLiteral("DDS ")) {
            if (offset + 128 > section.size() || le32(section, offset + 4) != 124u) continue;
            EmbeddedResourceCandidate candidate;
            candidate.relativeOffset = offset;
            candidate.size = 0;
            candidate.type = QStringLiteral("DDS_IMAGE");
            candidate.extension = QStringLiteral(".dds");
            found.push_back(candidate);
        }
    }

    for (int i = 0; i < found.size(); ++i) {
        if (found[i].size != 0) continue;
        const qsizetype next = i + 1 < found.size() ? found[i + 1].relativeOffset : section.size();
        found[i].size = qMax<qsizetype>(128, next - found[i].relativeOffset);
    }
    return found;
}

QByteArray xprMetadata(const QByteArray& xpr);

void appendEmbeddedSectionResources(QVector<ResourceEntry>* resources, const QByteArray& section,
                                    quint64 sectionFileOffset, const QString& sectionName, int sectionIndex)
{
    const auto candidates = findEmbeddedResources(section);
    int item = 0;
    for (const auto& candidate : candidates) {
        if (candidate.size <= 0 || candidate.relativeOffset < 0 ||
            candidate.relativeOffset + candidate.size > section.size()) continue;
        const QString safeSection = sectionName.isEmpty() ? QStringLiteral("section_%1").arg(sectionIndex) : sectionName;
        const QString name = QStringLiteral("%1_embedded_%2%3").arg(safeSection).arg(item++).arg(candidate.extension);
        const QByteArray payload = section.mid(candidate.relativeOffset, candidate.size);
        appendResource(resources, ModuleFormat::XBE, candidate.type, name,
                       sectionFileOffset + quint64(candidate.relativeOffset), payload,
                       100000 + sectionIndex * 1000 + item);
        if (candidate.type == QStringLiteral("XPR_PACKAGE")) {
            appendResource(resources, ModuleFormat::XBE, QStringLiteral("XPR_METADATA"),
                           name + QStringLiteral(".txt"),
                           sectionFileOffset + quint64(candidate.relativeOffset),
                           xprMetadata(payload), 110000 + sectionIndex * 1000 + item);
        }
    }
}


QString sectionFlagsSummary(quint32 flags)
{
    QStringList names;
    if (flags & 0x00000001u) names << QStringLiteral("writable");
    if (flags & 0x00000002u) names << QStringLiteral("preload");
    if (flags & 0x00000004u) names << QStringLiteral("executable");
    if (flags & 0x00000008u) names << QStringLiteral("inserted-file");
    if (flags & 0x00000010u) names << QStringLiteral("head-page-read-only");
    if (flags & 0x00000020u) names << QStringLiteral("tail-page-read-only");
    return names.isEmpty() ? QStringLiteral("none") : names.join(QStringLiteral(", "));
}

QString readDebugString(const QByteArray& data, quint32 address, quint32 baseAddress, bool unicode)
{
    qsizetype offset = 0;
    if (!vaToOffset(address, baseAddress, data.size(), &offset)) return {};
    return unicode ? readUtf16LeZ(data, offset, qMin<qsizetype>(260, (data.size() - offset) / 2))
                   : readAsciiZ(data, offset);
}

QByteArray xprMetadata(const QByteArray& xpr)
{
    if (xpr.size() < 12 || !isXprMagic(xpr, 0)) return {};
    const QString magic = QString::fromLatin1(xpr.constData(), 4);
    const quint32 totalSize = le32(xpr, 4);
    const quint32 headerSize = le32(xpr, 8);
    const quint32 payloadSize = totalSize >= headerSize ? totalSize - headerSize : 0;
    return QStringLiteral("Magic: %1\nTotal size: %2\nHeader size: %3\nPayload size: %4\n")
        .arg(magic).arg(totalSize).arg(headerSize).arg(payloadSize).toUtf8();
}

QString certificateSummary(quint32 titleId,
                           const QString& title,
                           quint32 allowedMedia,
                           quint32 gameRegion,
                           quint32 discNumber,
                           quint32 version)
{
    QStringList lines;
    lines << QStringLiteral("Title: %1").arg(title.isEmpty() ? QStringLiteral("(unnamed)") : title)
          << QStringLiteral("Title ID: 0x%1").arg(titleId, 8, 16, QLatin1Char('0')).toUpper()
          << QStringLiteral("Allowed media: 0x%1").arg(allowedMedia, 8, 16, QLatin1Char('0')).toUpper()
          << QStringLiteral("Game region: 0x%1").arg(gameRegion, 8, 16, QLatin1Char('0')).toUpper()
          << QStringLiteral("Disc number: %1").arg(discNumber)
          << QStringLiteral("Version: %1").arg(version);
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}
}

std::unique_ptr<XbeModule> XbeModule::open(const QString& filePath)
{
    auto module = std::unique_ptr<XbeModule>(new XbeModule);
    module->info_.filePath = filePath;
    module->info_.format = ModuleFormat::XBE;
    module->info_.description = QStringLiteral("Original Xbox Executable (XBE)");

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) { module->info_.error = file.errorString(); return module; }
    const QByteArray data = file.readAll();
    if (data.size() < 0x178 || data.left(4) != QByteArrayLiteral("XBEH")) {
        module->info_.error = QStringLiteral("Invalid XBE header");
        return module;
    }

    const quint32 baseAddress = le32(data, 0x104);
    const quint32 headerSize = le32(data, 0x108);
    const quint32 imageHeaderSize = le32(data, 0x110);
    const quint32 certificateAddress = le32(data, 0x118);
    const quint32 sectionCount = le32(data, 0x11c);
    const quint32 sectionHeadersAddress = le32(data, 0x120);
    const quint32 initializationFlags = le32(data, 0x124);
    const quint32 encodedEntryPoint = le32(data, 0x128);
    const quint32 tlsAddress = le32(data, 0x12c);
    const quint32 peStackCommit = le32(data, 0x130);
    const quint32 peHeapReserve = le32(data, 0x134);
    const quint32 peHeapCommit = le32(data, 0x138);
    const quint32 peBaseAddress = le32(data, 0x13c);
    const quint32 peImageSize = le32(data, 0x140);
    const quint32 peChecksum = le32(data, 0x144);
    const quint32 peTimestamp = le32(data, 0x148);
    const quint32 debugPathAddress = le32(data, 0x14c);
    const quint32 debugFileAddress = le32(data, 0x150);
    const quint32 debugUnicodeFileAddress = le32(data, 0x154);
    const quint32 encodedKernelThunkAddress = le32(data, 0x158);
    const quint32 nonKernelImportAddress = le32(data, 0x15c);
    const quint32 libraryCount = le32(data, 0x160);
    const quint32 libraryVersionsAddress = le32(data, 0x164);
    const quint32 kernelLibraryAddress = le32(data, 0x168);
    const quint32 xapiLibraryAddress = le32(data, 0x16c);
    const quint32 logoAddress = le32(data, 0x170);
    const quint32 logoSize = le32(data, 0x174);
    module->info_.headerOffset = 0;

    // The 256-byte RSA signature is part of the original XBE image header.
    // Verification requires the appropriate Xbox public key, so the opener
    // exposes the byte-perfect signature without claiming authenticity.
    appendResource(&module->resources_, ModuleFormat::XBE,
                   QStringLiteral("XBE_HEADER_SIGNATURE"), QStringLiteral("header_signature.bin"),
                   4, data.mid(4, 256), -30);

    if (baseAddress == 0 || imageHeaderSize < 0x178 || headerSize < imageHeaderSize || headerSize > quint32(data.size())) {
        module->info_.error = QStringLiteral("Invalid XBE base address or header size");
        return module;
    }
    if (sectionCount > 4096) {
        module->info_.error = QStringLiteral("Unreasonable XBE section count");
        return module;
    }

    {
        constexpr quint32 retailEntryXor = 0xA8FC57ABu;
        constexpr quint32 debugEntryXor = 0x94859D4Bu;
        quint32 entryPoint = encodedEntryPoint ^ retailEntryXor;
        QString variant = QStringLiteral("retail");
        if (entryPoint < baseAddress || entryPoint >= baseAddress + quint32(data.size())) {
            entryPoint = encodedEntryPoint ^ debugEntryXor;
            variant = QStringLiteral("debug");
        }
        const QString debugPath = readDebugString(data, debugPathAddress, baseAddress, false);
        const QString debugFile = readDebugString(data, debugFileAddress, baseAddress, false);
        const QString debugUnicodeFile = readDebugString(data, debugUnicodeFileAddress, baseAddress, true);
        QStringList lines;
        lines << QStringLiteral("Variant: %1").arg(variant)
              << QStringLiteral("Initialization flags: 0x%1").arg(initializationFlags, 8, 16, QLatin1Char('0'))
              << QStringLiteral("Entry point: 0x%1").arg(entryPoint, 8, 16, QLatin1Char('0'))
              << QStringLiteral("PE base: 0x%1").arg(peBaseAddress, 8, 16, QLatin1Char('0'))
              << QStringLiteral("PE image size: %1").arg(peImageSize)
              << QStringLiteral("PE checksum: 0x%1").arg(peChecksum, 8, 16, QLatin1Char('0'))
              << QStringLiteral("PE timestamp: %1").arg(peTimestamp)
              << QStringLiteral("Stack commit: %1").arg(peStackCommit)
              << QStringLiteral("Heap reserve: %1").arg(peHeapReserve)
              << QStringLiteral("Heap commit: %1").arg(peHeapCommit);
        if (!debugPath.isEmpty()) lines << QStringLiteral("Debug path: %1").arg(debugPath);
        if (!debugFile.isEmpty()) lines << QStringLiteral("Debug file: %1").arg(debugFile);
        if (!debugUnicodeFile.isEmpty()) lines << QStringLiteral("Debug Unicode file: %1").arg(debugUnicodeFile);
        appendResource(&module->resources_, ModuleFormat::XBE, QStringLiteral("XBE_IMAGE_METADATA"),
                       QStringLiteral("image_header.txt"), 0,
                       (lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8(), -20);
    }

    qsizetype certificateOffset = 0;
    if (certificateAddress != 0 && vaToOffset(certificateAddress, baseAddress, data.size(), &certificateOffset) &&
        certificateOffset + 0xB0 <= data.size()) {
        const quint32 certificateSize = le32(data, certificateOffset);
        const quint32 titleId = le32(data, certificateOffset + 0x08);
        const QString title = readUtf16LeZ(data, certificateOffset + 0x0c, 40);
        const quint32 allowedMedia = le32(data, certificateOffset + 0x9c);
        const quint32 gameRegion = le32(data, certificateOffset + 0xa0);
        const quint32 discNumber = le32(data, certificateOffset + 0xa8);
        const quint32 version = le32(data, certificateOffset + 0xac);

        if (!title.isEmpty()) module->info_.description += QStringLiteral(" — %1").arg(title);

        const qsizetype available = data.size() - certificateOffset;
        const qsizetype boundedSize = certificateSize >= 0xB0
            ? qMin<qsizetype>(qsizetype(certificateSize), available)
            : qMin<qsizetype>(0xB0, available);
        appendResource(&module->resources_, ModuleFormat::XBE,
                       QStringLiteral("XBE_CERTIFICATE"), QStringLiteral("certificate.bin"),
                       quint64(certificateOffset), data.mid(certificateOffset, boundedSize), -3);
        appendResource(&module->resources_, ModuleFormat::XBE,
                       QStringLiteral("XBE_METADATA"), QStringLiteral("certificate.txt"),
                       quint64(certificateOffset),
                       certificateSummary(titleId, title, allowedMedia, gameRegion, discNumber, version).toUtf8(), -2);
    }

    if (tlsAddress != 0) {
        qsizetype tlsOffset = 0;
        if (vaToOffset(tlsAddress, baseAddress, data.size(), &tlsOffset) && tlsOffset + 24 <= data.size()) {
            const QByteArray tls = data.mid(tlsOffset, 24);
            appendResource(&module->resources_, ModuleFormat::XBE, QStringLiteral("XBE_TLS"),
                           QStringLiteral("tls.bin"), quint64(tlsOffset), tls, -10);
            const QString summary = QStringLiteral(
                "Data start: 0x%1\nData end: 0x%2\nTLS index: 0x%3\nCallbacks: 0x%4\nZero fill: %5\nCharacteristics: 0x%6\n")
                .arg(le32(tls, 0), 8, 16, QLatin1Char('0'))
                .arg(le32(tls, 4), 8, 16, QLatin1Char('0'))
                .arg(le32(tls, 8), 8, 16, QLatin1Char('0'))
                .arg(le32(tls, 12), 8, 16, QLatin1Char('0'))
                .arg(le32(tls, 16))
                .arg(le32(tls, 20), 8, 16, QLatin1Char('0')).toUpper();
            appendResource(&module->resources_, ModuleFormat::XBE, QStringLiteral("XBE_TLS_METADATA"),
                           QStringLiteral("tls.txt"), quint64(tlsOffset), summary.toUtf8(), -11);
        }
    }

    if (libraryCount <= 4096 && libraryCount != 0) {
        qsizetype librariesOffset = 0;
        if (vaToOffset(libraryVersionsAddress, baseAddress, data.size(), &librariesOffset) &&
            quint64(librariesOffset) + quint64(libraryCount) * 16u <= quint64(data.size())) {
            for (quint32 i = 0; i < libraryCount; ++i) {
                const qsizetype offset = librariesOffset + qsizetype(i) * 16;
                const QByteArray record = data.mid(offset, 16);
                const QString label = QStringLiteral("library_%1").arg(i);
                appendResource(&module->resources_, ModuleFormat::XBE, QStringLiteral("XBE_LIBRARY_VERSION"),
                               label + QStringLiteral(".bin"), quint64(offset), record, -100 - int(i));
                appendResource(&module->resources_, ModuleFormat::XBE, QStringLiteral("XBE_LIBRARY_METADATA"),
                               label + QStringLiteral(".txt"), quint64(offset),
                               (librarySummary(record) + QLatin1Char('\n')).toUtf8(), -5000 - int(i));
            }
        }
    }
    appendLibraryVersion(&module->resources_, data, kernelLibraryAddress, baseAddress, QStringLiteral("kernel"), -12);
    appendLibraryVersion(&module->resources_, data, xapiLibraryAddress, baseAddress, QStringLiteral("xapi"), -14);

    if (encodedKernelThunkAddress != 0) {
        constexpr quint32 retailXor = 0x5B6D40B6u;
        constexpr quint32 debugXor = 0xEFB1F152u;
        quint32 thunkAddress = encodedKernelThunkAddress ^ retailXor;
        QString variant = QStringLiteral("retail");
        qsizetype thunkOffset = 0;
        if (!vaToOffset(thunkAddress, baseAddress, data.size(), &thunkOffset)) {
            thunkAddress = encodedKernelThunkAddress ^ debugXor;
            variant = QStringLiteral("debug");
        }
        if (vaToOffset(thunkAddress, baseAddress, data.size(), &thunkOffset)) {
            QStringList imports;
            QByteArray raw;
            for (int i = 0; i < 65536 && thunkOffset + qsizetype(i + 1) * 4 <= data.size(); ++i) {
                const quint32 value = le32(data, thunkOffset + qsizetype(i) * 4);
                if (value == 0) break;
                raw.append(data.mid(thunkOffset + qsizetype(i) * 4, 4));
                imports << QStringLiteral("%1: ordinal %2 (0x%3)")
                               .arg(i).arg(value & 0x7fffffffu)
                               .arg(value, 8, 16, QLatin1Char('0')).toUpper();
            }
            raw.append(QByteArray(4, char(0)));
            appendResource(&module->resources_, ModuleFormat::XBE, QStringLiteral("XBE_KERNEL_THUNKS"),
                           QStringLiteral("kernel_thunks.bin"), quint64(thunkOffset), raw, -16);
            const QString text = QStringLiteral("Variant: %1\nDecoded address: 0x%2\n%3\n")
                .arg(variant).arg(thunkAddress, 8, 16, QLatin1Char('0')).arg(imports.join(QLatin1Char('\n'))).toUpper();
            appendResource(&module->resources_, ModuleFormat::XBE, QStringLiteral("XBE_KERNEL_IMPORTS"),
                           QStringLiteral("kernel_imports.txt"), quint64(thunkOffset), text.toUtf8(), -17);
        }
    }

    if (nonKernelImportAddress != 0) {
        qsizetype importOffset = 0;
        if (vaToOffset(nonKernelImportAddress, baseAddress, data.size(), &importOffset)) {
            const qsizetype size = qMin<qsizetype>(256, data.size() - importOffset);
            appendResource(&module->resources_, ModuleFormat::XBE, QStringLiteral("XBE_NON_KERNEL_IMPORT_DIRECTORY"),
                           QStringLiteral("non_kernel_imports.bin"), quint64(importOffset), data.mid(importOffset, size), -18);
        }
    }

    if (logoAddress != 0 && logoSize != 0) {
        qsizetype logoOffset = 0;
        if (vaToOffset(logoAddress, baseAddress, data.size(), &logoOffset) &&
            quint64(logoOffset) + logoSize <= quint64(data.size())) {
            const QByteArray encodedLogo = data.mid(logoOffset, qsizetype(logoSize));
            appendResource(&module->resources_, ModuleFormat::XBE,
                           QStringLiteral("XBE_LOGO_RLE"), QStringLiteral("logo.xprle"),
                           quint64(logoOffset), encodedLogo, -1);
        }
    }

    qsizetype sectionTableOffset = 0;
    if (!vaToOffset(sectionHeadersAddress, baseAddress, data.size(), &sectionTableOffset) ||
        quint64(sectionTableOffset) + quint64(sectionCount) * 0x38u > quint64(data.size())) {
        module->info_.error = QStringLiteral("Invalid XBE section table");
        return module;
    }

    module->resources_.reserve(module->resources_.size() + int(sectionCount));
    struct SectionRange { quint64 offset; quint64 size; QString name; };
    QVector<SectionRange> sectionRanges;

    for (quint32 index = 0; index < sectionCount; ++index) {
        const qsizetype h = sectionTableOffset + qsizetype(index) * 0x38;
        const quint32 flags = le32(data, h + 0x00);
        const quint32 virtualAddress = le32(data, h + 0x04);
        const quint32 virtualSize = le32(data, h + 0x08);
        const quint32 rawAddress = le32(data, h + 0x0c);
        const quint32 rawSize = le32(data, h + 0x10);
        const quint32 nameAddress = le32(data, h + 0x14);
        const quint32 nameReferenceCount = le32(data, h + 0x18);
        const quint32 headSharedReferenceAddress = le32(data, h + 0x1c);
        const quint32 tailSharedReferenceAddress = le32(data, h + 0x20);
        const QByteArray expectedDigest = data.mid(h + 0x24, 20);

        if (quint64(rawAddress) + rawSize > quint64(data.size())) continue;

        qsizetype nameOffset = 0;
        QString name;
        if (vaToOffset(nameAddress, baseAddress, data.size(), &nameOffset)) name = readAsciiZ(data, nameOffset);
        if (name.isEmpty()) name = QStringLiteral("section_%1").arg(index);

        const QByteArray sectionData = data.mid(qsizetype(rawAddress), qsizetype(rawSize));
        SectionRange range;
        range.offset = rawAddress;
        range.size = rawSize;
        range.name = name;
        sectionRanges.push_back(range);
        const QByteArray actualDigest = QCryptographicHash::hash(sectionData, QCryptographicHash::Sha1);
        const bool digestPresent = expectedDigest != QByteArray(20, char(0));
        const bool digestMatches = digestPresent && expectedDigest == actualDigest;
        const QString digestStatus = !digestPresent ? QStringLiteral("not present")
                                                   : (digestMatches ? QStringLiteral("valid")
                                                                    : QStringLiteral("mismatch"));
        appendResource(&module->resources_, ModuleFormat::XBE,
                       QStringLiteral("XBE_SECTION"), name,
                       rawAddress, sectionData, int(index));
        module->resources_.last().hierarchyPath = QStringList() << name;
        appendResource(&module->resources_, ModuleFormat::XBE,
                       QStringLiteral("XBE_SECTION_DIGEST"), name + QStringLiteral(".sha1"),
                       quint64(h + 0x24), expectedDigest, 210000 + int(index));
        const QString sectionText = QStringLiteral(
            "Name: %1\nFlags: 0x%2 (%3)\nVirtual address: 0x%4\nVirtual size: %5\nRaw offset: 0x%6\nRaw size: %7\nName reference count: %8\nHead shared reference: 0x%9\nTail shared reference: 0x%10\nExpected SHA-1: %11\nActual SHA-1: %12\nDigest status: %13\n")
            .arg(name)
            .arg(flags, 8, 16, QLatin1Char('0')).arg(sectionFlagsSummary(flags))
            .arg(virtualAddress, 8, 16, QLatin1Char('0')).arg(virtualSize)
            .arg(rawAddress, 8, 16, QLatin1Char('0')).arg(rawSize).arg(nameReferenceCount)
            .arg(headSharedReferenceAddress, 8, 16, QLatin1Char('0'))
            .arg(tailSharedReferenceAddress, 8, 16, QLatin1Char('0'))
            .arg(QString::fromLatin1(expectedDigest.toHex()))
            .arg(QString::fromLatin1(actualDigest.toHex()))
            .arg(digestStatus);
        appendResource(&module->resources_, ModuleFormat::XBE,
                       QStringLiteral("XBE_SECTION_METADATA"), name + QStringLiteral(".txt"),
                       quint64(h), sectionText.toUtf8(), 200000 + int(index));
        appendEmbeddedSectionResources(&module->resources_, sectionData, rawAddress, name, int(index));
    }

    for (ResourceEntry& entry : module->resources_) {
        if (entry.type == QStringLiteral("XBE_SECTION")) continue;
        for (const SectionRange& section : sectionRanges) {
            if (entry.dataOffset >= section.offset &&
                entry.dataOffset + entry.dataSize <= section.offset + section.size) {
                entry.hierarchyPath = QStringList() << section.name;
                break;
            }
        }
    }

    return module;
}

} // namespace peare
