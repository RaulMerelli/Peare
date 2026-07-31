#include "LxModule.h"

#include <QFile>
#include <QHash>

#include <cstring>

#include <utility>

namespace peare {
namespace {

quint16 u16(const QByteArray& data, qsizetype offset)
{
    if (offset < 0 || offset + 2 > data.size()) return 0;
    const auto* p = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

quint32 u32(const QByteArray& data, qsizetype offset)
{
    if (offset < 0 || offset + 4 > data.size()) return 0;
    const auto* p = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint32(p[0]) | (quint32(p[1]) << 8) |
           (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

// Special thanks to EDM/2 Wiki for providing the updated doc:
// https://www.edm2.com/index.php/IBM_OS/2_16/32-bit_Object_Module_Format_(OMF)_and_Linear_eXecutable_Module_Format_(LX)
const QHash<int, QString> lxResourceTypes = {
    {0x01, QStringLiteral("RT_POINTER")},       // mouse pointer shape
    {0x02, QStringLiteral("RT_BITMAP")},        // bitmap
    {0x03, QStringLiteral("RT_MENU")},          // menu template
    {0x04, QStringLiteral("RT_DIALOG")},        // dialog template
    {0x05, QStringLiteral("RT_STRING")},        // string tables
    {0x06, QStringLiteral("RT_FONTDIR")},       // font directory
    {0x07, QStringLiteral("RT_FONT")},          // font
    {0x08, QStringLiteral("RT_ACCELTABLE")},    // accelerator tables
    {0x09, QStringLiteral("RT_RCDATA")},        // binary data
    {0x0A, QStringLiteral("RT_MESSAGE")},       // error message tables
    {0x0B, QStringLiteral("RT_DLGINCLUDE")},    // dialog include file name
    {0x0C, QStringLiteral("RT_VKEYTBL")},       // key to virtual-key tables
    {0x0D, QStringLiteral("RT_KEYTBL")},        // key to UGL tables
    {0x0E, QStringLiteral("RT_CHARTBL")},       // glyph to character tables
    {0x0F, QStringLiteral("RT_DISPLAYINFO")},   // screen display information
    {0x10, QStringLiteral("RT_FKASHORT")},      // function key area short form
    {0x11, QStringLiteral("RT_FKALONG")},       // function key area long form
    {0x12, QStringLiteral("RT_HELPTABLE")},     // Help table for Cary Help manager
    {0x13, QStringLiteral("RT_HELPSUBTABLE")},  // Help subtable for Cary Help manager
    {0x14, QStringLiteral("RT_FDDIR")},         // DBCS unique/font driver directory
    {0x15, QStringLiteral("RT_FD")}             // DBCS unique/font driver
};

// Special thanks to OsFree project for providing the method to extract ITERDATA2 pages.
// This is a direct safe C++ conversion of the C# UnpackMethod2 implementation.
QByteArray unpackMethod2(const QByteArray& srcData, int dstDataSize, bool* success)
{
    QByteArray destData(dstDataSize, '\0');
    int sOf = 0;
    int dOf = 0;

    const auto srcAvail = [&](int n) { return n >= 0 && sOf + n <= srcData.size(); };
    const auto dstAvail = [&](int n) { return n >= 0 && dOf + n <= dstDataSize; };

    while (dOf < dstDataSize) {
        if (!srcAvail(1)) break;
        quint8 b1 = quint8(srcData.at(sOf));
        int backOffset = 0;
        quint8 b2 = 0;

        switch (b1 & 3) {
        case 0:
            if (b1 == 0) {
                if (!srcAvail(2)) { *success = false; return {}; }
                if (quint8(srcData.at(sOf + 1)) == 0) {
                    sOf += 2;
                    break;
                }
                const int count = quint8(srcData.at(sOf + 1));
                if (!srcAvail(3) || !dstAvail(count)) { *success = false; return {}; }
                memset(destData.data() + dOf, quint8(srcData.at(sOf + 2)), size_t(count));
                sOf += 3;
                dOf += count;
            } else {
                const int count = b1 >> 2;
                if (!srcAvail(count + 1) || !dstAvail(count)) { *success = false; return {}; }
                memcpy(destData.data() + dOf, srcData.constData() + sOf + 1, size_t(count));
                dOf += count;
                sOf += count + 1;
            }
            break;

        case 1: {
            if (!srcAvail(2)) { *success = false; return {}; }
            const quint16 word = quint16(quint8(srcData.at(sOf))) |
                                 (quint16(quint8(srcData.at(sOf + 1))) << 8);
            backOffset = word >> 7;
            b2 = quint8(((b1 >> 4) & 7) + 3);
            b1 = quint8((b1 >> 2) & 3);
            sOf += 2;
            if (!srcAvail(b1) || !dstAvail(int(b1) + int(b2)) || dOf + b1 - backOffset < 0) {
                *success = false; return {};
            }
            memcpy(destData.data() + dOf, srcData.constData() + sOf, b1);
            dOf += b1;
            sOf += b1;
            for (int i = 0; i < b2; ++i) destData[dOf + i] = destData[dOf - backOffset + i];
            dOf += b2;
            break;
        }

        case 2: {
            if (!srcAvail(2)) { *success = false; return {}; }
            const quint16 word = quint16(quint8(srcData.at(sOf))) |
                                 (quint16(quint8(srcData.at(sOf + 1))) << 8);
            backOffset = word >> 4;
            b1 = quint8(((b1 >> 2) & 3) + 3);
            if (!dstAvail(b1) || dOf - backOffset < 0) { *success = false; return {}; }
            for (int i = 0; i < b1; ++i) destData[dOf + i] = destData[dOf - backOffset + i];
            dOf += b1;
            sOf += 2;
            break;
        }

        case 3: {
            if (!srcAvail(3)) { *success = false; return {}; }
            const quint16 firstWord = quint16(quint8(srcData.at(sOf))) |
                                      (quint16(quint8(srcData.at(sOf + 1))) << 8);
            const quint16 secondWord = quint16(quint8(srcData.at(sOf + 1))) |
                                       (quint16(quint8(srcData.at(sOf + 2))) << 8);
            b2 = quint8((firstWord >> 6) & 0x3F);
            b1 = quint8((quint8(srcData.at(sOf)) >> 2) & 0x0F);
            backOffset = secondWord >> 4;
            sOf += 3;
            if (!srcAvail(b1) || !dstAvail(int(b1) + int(b2)) || dOf + b1 - backOffset < 0) {
                *success = false; return {};
            }
            memcpy(destData.data() + dOf, srcData.constData() + sOf, b1);
            dOf += b1;
            sOf += b1;
            for (int i = 0; i < b2; ++i) destData[dOf + i] = destData[dOf - backOffset + i];
            dOf += b2;
            break;
        }
        }
    }

    *success = true;
    return destData;
}

// Generated from the docs in the original C# implementation.
QByteArray decompressRlePage(const QByteArray& compressedData, int logicalPageSize)
{
    if (compressedData.isEmpty()) return {};
    QByteArray output;
    output.reserve(logicalPageSize);
    int position = 0;

    while (output.size() < logicalPageSize && position < compressedData.size()) {
        if (compressedData.size() - position < 4) break;
        const quint16 nIter = u16(compressedData, position);
        const quint16 nBytes = u16(compressedData, position + 2);
        position += 4;
        if (nIter == 0 || nBytes == 0) break;
        if (position + nBytes > compressedData.size()) break;
        const QByteArray iteratedData = compressedData.mid(position, nBytes);
        position += nBytes;
        for (quint16 i = 0; i < nIter && output.size() < logicalPageSize; ++i) {
            output.append(iteratedData.left(logicalPageSize - output.size()));
        }
    }
    output.resize(logicalPageSize);
    return output;
}


bool extractResource(const QByteArray& fileBytes, qsizetype lxHeaderOffset,
                     quint32 pageSize, quint32 pageOffsetShift,
                     quint32 dataPagesOffset, quint32 objectTableOffset,
                     quint32 objectPageTableOffset, quint16 objectNum,
                     quint32 offsetInObject, quint32 resourceSize,
                     QByteArray* output, QString* error)
{
    if (objectNum == 0) { *error = QStringLiteral("Invalid object number 0."); return false; }
    const qsizetype objEntryOffset = lxHeaderOffset + objectTableOffset + qsizetype(objectNum - 1) * 24;
    if (objEntryOffset < 0 || objEntryOffset + 24 > fileBytes.size()) {
        *error = QStringLiteral("Object table entry is outside file bounds."); return false;
    }
    const quint32 pageTableIndex = u32(fileBytes, objEntryOffset + 12);
    const quint32 pageTableEntries = u32(fileBytes, objEntryOffset + 16);
    if (pageTableEntries == 0) {
        *error = QStringLiteral("The object associated with the resource contains no pages."); return false;
    }
    if (pageSize == 0) { *error = QStringLiteral("LX page size is zero."); return false; }

    const quint32 startPageIndexInObject = offsetInObject / pageSize;
    quint32 currentOffsetWithinPage = offsetInObject % pageSize;
    QByteArray result(int(resourceSize), '\0');
    quint32 bytesRead = 0;
    quint32 currentPageIndexInObject = startPageIndexInObject;

    while (bytesRead < resourceSize && currentPageIndexInObject < pageTableEntries) {
        const quint64 globalPageEntryIndex = quint64(pageTableIndex) + currentPageIndexInObject;
        if (globalPageEntryIndex == 0) { *error = QStringLiteral("Invalid global page index 0."); return false; }
        const qsizetype pageEntryOffset = lxHeaderOffset + objectPageTableOffset + qsizetype(globalPageEntryIndex - 1) * 8;
        if (pageEntryOffset < 0 || pageEntryOffset + 8 > fileBytes.size()) {
            *error = QStringLiteral("Object page entry is outside file bounds."); return false;
        }

        const quint32 pageDataOffset = u32(fileBytes, pageEntryOffset);
        const quint16 pageDataLengthInFile = u16(fileBytes, pageEntryOffset + 4);
        const quint16 pageFlags = u16(fileBytes, pageEntryOffset + 6);
        QByteArray page;

        if (pageFlags == 0x03) {
            page = QByteArray(int(pageSize), '\0');
        } else if (pageFlags == 0x00 || pageFlags == 0x01 || pageFlags == 0x05) {
            const quint64 physicalOffset = quint64(dataPagesOffset) +
                                           (quint64(pageDataOffset) << pageOffsetShift);
            if (physicalOffset > quint64(fileBytes.size()) ||
                physicalOffset + pageDataLengthInFile > quint64(fileBytes.size())) {
                *error = QStringLiteral("Page data is outside file bounds."); return false;
            }
            const QByteArray block = fileBytes.mid(qsizetype(physicalOffset), pageDataLengthInFile);
            if (pageFlags == 0x00) {
                page = block;
                if (page.size() < int(pageSize)) page.resize(int(pageSize));
            } else if (pageFlags == 0x01) {
                page = decompressRlePage(block, int(pageSize));
            } else {
                bool success = false;
                page = unpackMethod2(block, int(pageSize), &success);
                if (!success) { *error = QStringLiteral("Page decompression with UnpackMethod2 failed."); return false; }
            }
        } else {
            *error = QStringLiteral("Unsupported page type (flags = 0x%1).")
                         .arg(pageFlags, 2, 16, QLatin1Char('0')).toUpper();
            return false;
        }

        const quint32 available = currentOffsetWithinPage < quint32(page.size())
            ? quint32(page.size()) - currentOffsetWithinPage : 0;
        const quint32 bytesToCopy = qMin(resourceSize - bytesRead,
                                         qMin(pageSize - currentOffsetWithinPage, available));
        if (bytesToCopy > 0)
            memcpy(result.data() + bytesRead, page.constData() + currentOffsetWithinPage, bytesToCopy);
        bytesRead += bytesToCopy;
        ++currentPageIndexInObject;
        currentOffsetWithinPage = 0;
    }

    if (bytesRead < resourceSize) {
        *error = QStringLiteral("Warning: Not all resource bytes were read (%1/%2). Missing %3 bytes.")
                     .arg(bytesRead).arg(resourceSize).arg(resourceSize - bytesRead);
    }
    *output = result;
    return true;
}


QByteArray rebuildLxObject(const QByteArray& bytes, qsizetype header, quint32 pageSize,
                           quint32 pageShift, quint32 dataPagesOffset,
                           quint32 pageTableOffset, quint32 pageIndex,
                           quint32 pageCount, quint32 virtualSize)
{
    if (pageSize == 0 || pageIndex == 0 || pageCount == 0)
        return {};
    QByteArray output;
    output.reserve(int(qMin<quint64>(virtualSize, 64u * 1024u * 1024u)));
    for (quint32 i = 0; i < pageCount && quint64(output.size()) < virtualSize; ++i) {
        const qsizetype descriptor = header + pageTableOffset + qsizetype(pageIndex - 1 + i) * 8;
        if (descriptor < 0 || descriptor + 8 > bytes.size())
            return {};
        const quint32 pageOffset = u32(bytes, descriptor);
        const quint16 storedSize = u16(bytes, descriptor + 4);
        const quint16 flags = u16(bytes, descriptor + 6);
        const quint32 wanted = qMin<quint32>(pageSize, virtualSize - quint32(output.size()));
        if (flags == 0x0000) {
            const quint64 offset = quint64(dataPagesOffset) + (quint64(pageOffset) << pageShift);
            const quint32 available = storedSize == 0 ? wanted : qMin<quint32>(storedSize, wanted);
            if (offset + available > quint64(bytes.size()))
                return {};
            output.append(bytes.constData() + qsizetype(offset), int(available));
            if (available < wanted)
                output.append(QByteArray(int(wanted - available), '\0'));
        } else if (flags == 0x0002 || flags == 0x0003) {
            output.append(QByteArray(int(wanted), '\0'));
        } else {
            return {}; // iterated/compressed pages require their dedicated decoder
        }
    }
    return output;
}

void appendLxArea(QVector<ResourceEntry>& destination, const QString& name,
                   const QByteArray& payload, quint64 virtualSize)
{
    if (payload.isEmpty() && virtualSize == 0)
        return;
    ResourceEntry area;
    area.type = QStringLiteral("LX_AREA");
    area.name = name;
    area.hierarchyPath = QStringList() << name;
    area.language = QStringLiteral("neutral");
    area.dataOffset = 0;
    area.dataSize = payload.isEmpty() ? virtualSize : quint64(payload.size());
    area.format = ModuleFormat::LX;
    area.isOs2 = true;
    area.data = payload;
    destination.push_back(std::move(area));
}

void appendLxStructure(QVector<ResourceEntry>& destination, const QByteArray& bytes,
                       qsizetype header, quint32 pageSize, quint32 pageShift,
                       quint32 dataPagesOffset, quint32 objectTableOffset,
                       quint32 objectPageTableOffset, quint32 objectCount)
{
    const qsizetype objectTable = header + objectTableOffset;
    if (objectTable < header || objectTable + qsizetype(objectCount) * 24 > bytes.size())
        return;
    ResourceEntry headers;
    headers.type = QStringLiteral("LX_HEADERS"); headers.name = QStringLiteral("headers");
    headers.hierarchyPath = QStringList() << headers.name; headers.language = QStringLiteral("neutral");
    headers.dataOffset = 0; headers.dataSize = qMin<quint64>(quint64(objectTable + qsizetype(objectCount) * 24), quint64(bytes.size()));
    headers.format = ModuleFormat::LX; headers.isOs2 = true; headers.data = bytes.left(qsizetype(headers.dataSize));
    destination.push_back(std::move(headers));

    QByteArray textBytes;
    QByteArray dataBytes;
    QByteArray bssBytes;
    quint64 textVirtualSize = 0;
    quint64 dataVirtualSize = 0;
    quint64 bssVirtualSize = 0;
    for (quint32 i = 0; i < objectCount; ++i) {
        const qsizetype object = objectTable + qsizetype(i) * 24;
        const quint32 virtualSize = u32(bytes, object);
        const quint32 flags = u32(bytes, object + 8);
        const quint32 pageIndex = u32(bytes, object + 12);
        const quint32 pageCount = u32(bytes, object + 16);
        if ((flags & 0x0008u) != 0)
            continue;
        const bool executable = (flags & 0x0004u) != 0;
        if (pageCount == 0) {
            bssBytes.append(QByteArray(int(qMin<quint32>(virtualSize, 64u * 1024u * 1024u)), '\0'));
            bssVirtualSize += virtualSize;
            continue;
        }
        const QByteArray objectBytes = rebuildLxObject(bytes, header, pageSize, pageShift, dataPagesOffset,
                                        objectPageTableOffset, pageIndex, pageCount, virtualSize);
        if (objectBytes.isEmpty())
            continue;
        if (executable) { textBytes.append(objectBytes); textVirtualSize += virtualSize; }
        else { dataBytes.append(objectBytes); dataVirtualSize += virtualSize; }
    }

    appendLxArea(destination, QStringLiteral(".text"), textBytes, textVirtualSize);
    appendLxArea(destination, QStringLiteral(".data"), dataBytes, dataVirtualSize);
    appendLxArea(destination, QStringLiteral(".bss"), bssBytes, bssVirtualSize);
}

} // namespace

std::unique_ptr<LxModule> LxModule::open(const QString& filePath)
{
    auto module = std::unique_ptr<LxModule>(new LxModule);
    module->info_.filePath = filePath;
    module->info_.format = ModuleFormat::LX;
    module->info_.description = QStringLiteral("Linear Executable (LX)");

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) { module->info_.error = file.errorString(); return module; }
    const QByteArray bytes = file.readAll();
    if (bytes.size() < 0x40 || u16(bytes, 0) != 0x5A4D) {
        module->info_.error = QStringLiteral("The file is not an MZ executable."); return module;
    }
    const quint32 lxHeaderOffset = u32(bytes, 0x3C);
    module->info_.headerOffset = lxHeaderOffset;
    if (lxHeaderOffset + 0xAC > quint32(bytes.size()) || u16(bytes, lxHeaderOffset) != 0x584C) {
        module->info_.error = QStringLiteral("The file is not a Linear Executable (LX)."); return module;
    }

    const quint32 pageSize = u32(bytes, lxHeaderOffset + 0x28);
    const quint32 pageOffsetShift = u32(bytes, lxHeaderOffset + 0x2C);
    const quint32 objectTableOffset = u32(bytes, lxHeaderOffset + 0x40);
    const quint32 objectCount = u32(bytes, lxHeaderOffset + 0x44);
    const quint32 objectPageTableOffset = u32(bytes, lxHeaderOffset + 0x48);
    const quint32 resourceTableOffset = u32(bytes, lxHeaderOffset + 0x50);
    const quint32 resourceCount = u32(bytes, lxHeaderOffset + 0x54);
    const quint32 dataPagesOffset = u32(bytes, lxHeaderOffset + 0x80);

    appendLxStructure(module->resources_, bytes, lxHeaderOffset, pageSize, pageOffsetShift,
                      dataPagesOffset, objectTableOffset, objectPageTableOffset, objectCount);

    if (resourceTableOffset == 0 || resourceCount == 0) return module;
    const qsizetype tableOffset = lxHeaderOffset + resourceTableOffset;
    if (tableOffset < 0 || tableOffset > bytes.size()) {
        module->info_.error = QStringLiteral("Resource table offset is outside file bounds."); return module;
    }

    module->resources_.reserve(int(qMin<quint32>(resourceCount, 65536)));
    for (quint32 i = 0; i < resourceCount; ++i) {
        const qsizetype entryOffset = tableOffset + qsizetype(i) * 14;
        if (entryOffset < 0 || entryOffset + 14 > bytes.size()) continue;
        const quint16 typeId = u16(bytes, entryOffset);
        const quint16 nameId = u16(bytes, entryOffset + 2);
        const quint32 resourceSize = u32(bytes, entryOffset + 4);
        const quint16 objectNum = u16(bytes, entryOffset + 8);
        const quint32 offsetInObject = u32(bytes, entryOffset + 10);

        QByteArray resourceData;
        QString extractionMessage;
        if (!extractResource(bytes, lxHeaderOffset, pageSize, pageOffsetShift,
                             dataPagesOffset, objectTableOffset, objectPageTableOffset,
                             objectNum, offsetInObject, resourceSize,
                             &resourceData, &extractionMessage)) {
            continue;
        }

        ResourceEntry entry;
        entry.type = lxResourceTypes.value(typeId, QStringLiteral("#%1").arg(typeId));
        entry.name = QStringLiteral("#%1").arg(nameId);
        entry.dataOffset = offsetInObject;
        entry.dataSize = resourceSize;
        entry.format = ModuleFormat::LX;
        entry.isOs2 = true;
        entry.baseId = nameId;
        entry.hierarchyPath = QStringList() << QStringLiteral(".rsrc");
        entry.data = std::move(resourceData);
        module->resources_.push_back(std::move(entry));
    }
    return module;
}


} // namespace peare
