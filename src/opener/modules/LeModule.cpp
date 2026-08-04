#include "LeModule.h"

#include <QFile>
#include <QHash>

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

// Copied from LxResourceTypes. It may differ.
const QHash<int, QString> leResourceTypes = {
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


// This method is quite buggy but mostly works, so it is a start I guess.
// This format is so little documented (especially LE_OBJECT_PAGE_TABLE_ENTRY, where online docs were describing fields as unknown or with wrong sizes...)
// that I'm already happy by having it at least somehow open.
bool extractResourceLe(const QByteArray& fileBytes, qsizetype leHeaderOffset,
                       quint32 memoryPageSize, quint32 dataPagesOffsetFromTopOfFile,
                       quint32 objectTableOffset, quint32 objectPageMapOffset,
                       quint16 objectNum, quint32 offsetInObject, quint32 resourceSize,
                       QByteArray* result, QString* message)
{
    if (objectNum == 0) {
        *message = QStringLiteral("Invalid object number 0.");
        return false;
    }

    // Handle Object Table Entry
    const qsizetype objEntryOffset = leHeaderOffset + objectTableOffset + qsizetype(objectNum - 1) * 24;
    if (objEntryOffset < 0 || objEntryOffset + 24 > fileBytes.size()) {
        *message = QStringLiteral("Object table entry is outside file bounds.");
        return false;
    }

    const quint32 pageTableIndex = u32(fileBytes, objEntryOffset + 12);
    const quint32 pageTableEntries = u32(fileBytes, objEntryOffset + 16);

    if (pageTableEntries == 0) {
        *message = QStringLiteral("The object associated with the resource contains no entries.");
        return false;
    }

    // LE_OBJECT_PAGE_TABLE_ENTRY is exactly four bytes in the C# implementation.
    const qsizetype pageDescriptorEntryOffset = leHeaderOffset + objectPageMapOffset +
                                                qsizetype(pageTableIndex - 1) * 4;
    if (pageDescriptorEntryOffset < 0 || pageDescriptorEntryOffset + 4 > fileBytes.size()) {
        *message = QStringLiteral("Object page descriptor is outside file bounds.");
        return false;
    }

    const quint8 globalPageIndex = quint8(fileBytes.at(pageDescriptorEntryOffset + 2));
    const quint8 pageFlags = quint8(fileBytes.at(pageDescriptorEntryOffset + 3));
    if (globalPageIndex == 0 || memoryPageSize == 0) {
        *message = QStringLiteral("Invalid LE page descriptor.");
        return false;
    }

    const quint64 currentPageDataOffset = quint64(dataPagesOffsetFromTopOfFile) +
                                          quint64(globalPageIndex - 1) * memoryPageSize;
    if (currentPageDataOffset > quint64(fileBytes.size())) {
        *message = QStringLiteral("Calculated page data offset is outside file bounds.");
        return false;
    }

    // We take data also over the page end, as resources can overflow to next pages.
    // This might be not the correct way to do it, but for Legal Physical Pages seems to be working fine...
    const QByteArray page = fileBytes.mid(qsizetype(currentPageDataOffset));

    if (pageFlags == 0x00) // Legal Physical Page
    {
        if (quint64(offsetInObject) + resourceSize > quint64(page.size())) {
            *message = QStringLiteral("Resource data (offset %1, size %2) exceeds page bounds (%3).")
                           .arg(offsetInObject).arg(resourceSize).arg(page.size());
            return false;
        }

        // Extract the resource bytes from the page
        *result = page.mid(qsizetype(offsetInObject), qsizetype(resourceSize));
        *message = QStringLiteral("LE resource found and extracted successfully.\nSize: %1 bytes.\r\n")
                       .arg(resourceSize);
        return true;
    }

    *message = QStringLiteral("The page associated with the resource is not a legal physical page.");
    return false;
}


QByteArray rebuildLeObject(const QByteArray& bytes, qsizetype header, quint32 pageSize,
                           quint32 dataPagesOffset, quint32 objectPageMapOffset,
                           quint32 pageIndex, quint32 pageCount, quint32 virtualSize)
{
    if (pageSize == 0 || pageIndex == 0 || pageCount == 0)
        return {};
    QByteArray output;
    output.reserve(int(qMin<quint64>(virtualSize, 64u * 1024u * 1024u)));
    for (quint32 i = 0; i < pageCount && quint64(output.size()) < virtualSize; ++i) {
        const qsizetype descriptor = header + objectPageMapOffset + qsizetype(pageIndex - 1 + i) * 4;
        if (descriptor < 0 || descriptor + 4 > bytes.size())
            return {};
        const auto* p = reinterpret_cast<const uchar*>(bytes.constData() + descriptor);
        const quint32 physicalPage = quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16);
        const quint8 flags = p[3];
        const quint32 wanted = qMin<quint32>(pageSize, virtualSize - quint32(output.size()));
        if (flags == 0x00 && physicalPage != 0) {
            const quint64 offset = quint64(dataPagesOffset) + quint64(physicalPage - 1) * pageSize;
            if (offset + wanted > quint64(bytes.size()))
                return {};
            output.append(bytes.constData() + qsizetype(offset), int(wanted));
        } else if (flags == 0x02 || flags == 0x03) {
            output.append(QByteArray(int(wanted), '\0'));
        } else {
            return {}; // iterated/compressed LE pages are not reconstructed speculatively
        }
    }
    return output;
}

void appendLeArea(QVector<ResourceEntry>& destination, const QString& name,
                   const QByteArray& payload, quint64 virtualSize)
{
    if (payload.isEmpty() && virtualSize == 0)
        return;
    ResourceEntry area;
    area.type = QStringLiteral("LE_AREA");
    area.name = name;
    area.hierarchyPath = QStringList() << name;
    area.language = QStringLiteral("neutral");
    area.dataOffset = 0;
    area.dataSize = payload.isEmpty() ? virtualSize : quint64(payload.size());
    area.format = ModuleFormat::LE;
    area.isOs2 = true;
    area.data = payload;
    destination.push_back(std::move(area));
}

void appendLeStructure(QVector<ResourceEntry>& destination, const QByteArray& bytes,
                       qsizetype header, quint32 pageSize, quint32 dataPagesOffset,
                       quint32 objectTableOffset, quint32 objectPageMapOffset,
                       quint32 objectCount)
{
    const qsizetype objectTable = header + objectTableOffset;
    if (objectTable < header || objectTable + qsizetype(objectCount) * 24 > bytes.size())
        return;
    ResourceEntry headers;
    headers.type = QStringLiteral("LE_HEADERS"); headers.name = QStringLiteral("headers");
    headers.hierarchyPath = QStringList() << headers.name; headers.language = QStringLiteral("neutral");
    headers.dataOffset = 0; headers.dataSize = qMin<quint64>(quint64(objectTable + qsizetype(objectCount) * 24), quint64(bytes.size()));
    headers.format = ModuleFormat::LE; headers.isOs2 = true; headers.data = bytes.left(qsizetype(headers.dataSize));
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
        const QByteArray objectBytes = rebuildLeObject(bytes, header, pageSize, dataPagesOffset, objectPageMapOffset,
                                        pageIndex, pageCount, virtualSize);
        if (objectBytes.isEmpty())
            continue;
        if (executable) { textBytes.append(objectBytes); textVirtualSize += virtualSize; }
        else { dataBytes.append(objectBytes); dataVirtualSize += virtualSize; }
    }

    appendLeArea(destination, QStringLiteral(".text"), textBytes, textVirtualSize);
    appendLeArea(destination, QStringLiteral(".data"), dataBytes, dataVirtualSize);
    appendLeArea(destination, QStringLiteral(".bss"), bssBytes, bssVirtualSize);
}

} // namespace

std::unique_ptr<LeModule> LeModule::open(const QString& filePath)
{
    auto module = std::unique_ptr<LeModule>(new LeModule);
    module->info_.filePath = filePath;
    module->info_.format = ModuleFormat::LE;
    module->info_.description = QStringLiteral("Linear Executable (LE)");

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) { module->info_.error = file.errorString(); return module; }
    const QByteArray fileBytes = file.readAll();

    quint32 leHeaderOffset = 0;
    if (fileBytes.size() >= 2 && u16(fileBytes, 0) == 0x454C) {
        leHeaderOffset = 0;
    } else {
        if (fileBytes.size() < 0x40 || u16(fileBytes, 0) != 0x5A4D) {
            module->info_.error = QStringLiteral("The file is neither an MZ-wrapped nor a stubless LE executable.");
            return module;
        }
        leHeaderOffset = u32(fileBytes, 0x3C);
    }

    module->info_.headerOffset = leHeaderOffset;
    if (leHeaderOffset + 172 > quint32(fileBytes.size()) || u16(fileBytes, leHeaderOffset) != 0x454C) {
        module->info_.error = QStringLiteral("The file is not a Linear Executable (LE).");
        return module;
    }

    const quint32 memoryPageSize = u32(fileBytes, leHeaderOffset + 0x28);
    const quint32 objectTableOffset = u32(fileBytes, leHeaderOffset + 0x40);
    const quint32 objectCount = u32(fileBytes, leHeaderOffset + 0x44);
    const quint32 objectPageMapOffset = u32(fileBytes, leHeaderOffset + 0x48);
    const quint32 resourceTableOffset = u32(fileBytes, leHeaderOffset + 0x50);
    const quint32 resourceEntryCount = u32(fileBytes, leHeaderOffset + 0x54);
    const quint32 dataPagesOffsetFromTopOfFile = u32(fileBytes, leHeaderOffset + 0x80);

    appendLeStructure(module->resources_, fileBytes, leHeaderOffset, memoryPageSize,
                      dataPagesOffsetFromTopOfFile, objectTableOffset, objectPageMapOffset, objectCount);

    if (resourceTableOffset == 0 || resourceEntryCount == 0) return module;

    const qsizetype tableOffset = leHeaderOffset + resourceTableOffset;
    if (tableOffset < 0 || tableOffset > fileBytes.size()) {
        module->info_.error = QStringLiteral("Resource table offset is outside file bounds.");
        return module;
    }

    module->resources_.reserve(int(qMin<quint32>(resourceEntryCount, 65536)));
    for (quint32 i = 0; i < resourceEntryCount; ++i) {
        const qsizetype entryOffset = tableOffset + qsizetype(i) * 14; // Each entry is 14 bytes
        if (entryOffset + 14 > fileBytes.size() || entryOffset < 0) continue;

        const quint16 typeId = u16(fileBytes, entryOffset);
        const quint16 nameId = u16(fileBytes, entryOffset + 2);
        const quint32 resourceSize = u32(fileBytes, entryOffset + 4);
        const quint16 objectNum = u16(fileBytes, entryOffset + 8);
        const quint32 offsetInObject = u32(fileBytes, entryOffset + 10);

        QByteArray resourceData;
        QString extractionMessage;
        if (!extractResourceLe(fileBytes, leHeaderOffset, memoryPageSize,
                               dataPagesOffsetFromTopOfFile, objectTableOffset,
                               objectPageMapOffset, objectNum, offsetInObject,
                               resourceSize, &resourceData, &extractionMessage)) {
            continue;
        }

        ResourceEntry entry;
        entry.type = leResourceTypes.value(typeId, QStringLiteral("#%1").arg(typeId));
        entry.name = QStringLiteral("#%1").arg(nameId);
        entry.dataOffset = offsetInObject;
        entry.dataSize = resourceSize;
        entry.format = ModuleFormat::LE;
        entry.isOs2 = true;
        entry.baseId = nameId;
        entry.hierarchyPath = QStringList() << QStringLiteral(".rsrc");
        entry.data = std::move(resourceData);
        module->resources_.push_back(std::move(entry));
    }

    return module;
}


} // namespace peare
