#include "NeModule.h"

#include <QFile>
#include <QHash>
#include <QStringList>

#include <utility>

namespace peare {
namespace {

quint16 u16(const QByteArray& data, qsizetype offset)
{
    if (offset < 0 || offset + 2 > data.size())
        return 0;

    const auto* bytes = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint16(bytes[0]) | (quint16(bytes[1]) << 8);
}

quint32 u32(const QByteArray& data, qsizetype offset)
{
    if (offset < 0 || offset + 4 > data.size())
        return 0;

    const auto* bytes = reinterpret_cast<const uchar*>(data.constData() + offset);
    return quint32(bytes[0]) |
           (quint32(bytes[1]) << 8) |
           (quint32(bytes[2]) << 16) |
           (quint32(bytes[3]) << 24);
}

// Windows resource types
// Based on Win1 and Win2 prgref and WINMOD.C for Win3 by Matt Pietrek, 1992
// Win1: https://www.os2museum.com/files/docs/win10sdk/windows-1.03-sdk-prgref-1986.pdf
// Win2: https://www.os2museum.com/files/docs/win20sdk/windows-2.0-sdk-prgref-1987.pdf
const QHash<int, QString> windowsNeResourceTypes = {
    {0x01, QStringLiteral("RT_CURSOR")},
    {0x02, QStringLiteral("RT_BITMAP")},
    {0x03, QStringLiteral("RT_ICON")},
    {0x04, QStringLiteral("RT_MENU")},
    {0x05, QStringLiteral("RT_DIALOG")},
    {0x06, QStringLiteral("RT_STRING")},
    {0x07, QStringLiteral("RT_FONTDIR")}, // FONTDIR do not exist in Win1 prgref, but it works?! Was it undocumented?
    {0x08, QStringLiteral("RT_FONT")},
    {0x09, QStringLiteral("RT_ACCELERATOR")},
    {0x0A, QStringLiteral("RT_RCDATA")},
    {0x0B, QStringLiteral("RT_MESSAGETABLE")}, // In WinMod by Matt Pietrek, 1992, File: WINMOD.C this is ErrorTable
    {0x0C, QStringLiteral("RT_GROUP_CURSOR")},
    {0x0D, QStringLiteral("RT_UNKNOWN(13)")},
    {0x0E, QStringLiteral("RT_GROUP_ICON")},
    {0x0F, QStringLiteral("RT_NAMETABLE")},
    {0x10, QStringLiteral("RT_VERSION")},
    {257,  QStringLiteral("RT_DRV_RAW")} // Not found in any docs, this is an addition by me, as 257 is always found only in drv files
};

// OS/2 resource types
const QHash<int, QString> os2NeResourceTypes = {
    {1, QStringLiteral("RT_POINTER")},
    {2, QStringLiteral("RT_BITMAP")},
    {3, QStringLiteral("RT_MENU")},
    {4, QStringLiteral("RT_DIALOG")},
    {5, QStringLiteral("RT_STRING")},
    {6, QStringLiteral("RT_FONTDIR")},
    {7, QStringLiteral("RT_FONT")},
    {8, QStringLiteral("RT_ACCELTABLE")},
    {9, QStringLiteral("RT_RCDATA")},
    {10, QStringLiteral("RT_MESSAGE")},
    {11, QStringLiteral("RT_DLGINCLUDE")},
    {12, QStringLiteral("RT_VKEYTBL")},
    {13, QStringLiteral("RT_KEYTBL")},
    {14, QStringLiteral("RT_CHARTBL")},
    {15, QStringLiteral("RT_DISPLAYINFO")},
    {16, QStringLiteral("RT_FKASHORT")},
    {17, QStringLiteral("RT_FKALONG")},
    {18, QStringLiteral("RT_HELPTABLE")},
    {19, QStringLiteral("RT_HELPSUBTABLE")},
    {20, QStringLiteral("RT_FDDIR")},
    {21, QStringLiteral("RT_FD")}
};

enum class VersionType {
    Unknown,
    OS2,
    Windows,
    MSDOS4,
    Win386,
    IBMMPN
};

bool looksLikeOs2NeResources(const QByteArray& data, qsizetype neHeaderOffset)
{
    if (neHeaderOffset < 0 || neHeaderOffset + 0x38 > data.size())
        return false;

    const quint16 segmentCount = u16(data, neHeaderOffset + 0x1C);
    const quint16 segmentTableOffset = u16(data, neHeaderOffset + 0x22);
    const quint16 resourceTableOffset = u16(data, neHeaderOffset + 0x24);
    const quint16 resourceTableSize = u16(data, neHeaderOffset + 0x26);
    const quint16 alignShift = u16(data, neHeaderOffset + 0x32);
    const quint16 resourceCount = u16(data, neHeaderOffset + 0x34);

    if (resourceCount == 0 || resourceCount > segmentCount || alignShift > 16)
        return false;
    if (resourceTableSize < resourceCount * 4u)
        return false;

    const qsizetype tablePos = neHeaderOffset + resourceTableOffset;
    const qsizetype tableEnd = tablePos + resourceTableSize;
    const qsizetype segmentPos = neHeaderOffset + segmentTableOffset;
    if (tablePos < neHeaderOffset || tableEnd > data.size() ||
        segmentPos < neHeaderOffset || segmentPos + qsizetype(segmentCount) * 8 > data.size())
        return false;

    int knownTypes = 0;
    for (quint16 i = 0; i < resourceCount; ++i) {
        const qsizetype pos = tablePos + qsizetype(i) * 4;
        const quint16 type = u16(data, pos);
        const quint16 name = u16(data, pos + 2);
        if (type == 0 || name == 0)
            return false;
        if (type >= 1 && type <= 21)
            ++knownTypes;
    }

    // OS/2 resource segments are the final ne_cres entries in the segment table.
    const quint16 firstResourceSegment = segmentCount - resourceCount;
    for (quint16 i = 0; i < resourceCount; ++i) {
        const qsizetype pos = segmentPos + qsizetype(firstResourceSegment + i) * 8;
        const quint16 sector = u16(data, pos);
        const quint16 flags = u16(data, pos + 4);
        if (sector == 0 || (flags & 0x0010u) == 0) // NSMOVE: resource/data segment
            return false;
    }

    return knownTypes * 2 >= resourceCount;
}

bool looksLikeWindowsNeResources(const QByteArray& data, qsizetype neHeaderOffset)
{
    if (neHeaderOffset < 0 || neHeaderOffset + 0x28 > data.size())
        return false;
    const quint16 resourceOffset = u16(data, neHeaderOffset + 0x24);
    const quint16 residentOffset = u16(data, neHeaderOffset + 0x26);
    const qsizetype tablePos = neHeaderOffset + resourceOffset;
    const qsizetype tableEnd = residentOffset > resourceOffset
        ? neHeaderOffset + residentOffset : data.size();
    if (tablePos + 2 > data.size() || tableEnd > data.size() || tablePos >= tableEnd)
        return false;

    const quint16 alignShift = u16(data, tablePos);
    if (alignShift > 16)
        return false;
    if (tablePos + 4 > tableEnd)
        return false;

    const quint16 firstType = u16(data, tablePos + 2);
    if (firstType == 0)
        return true; // valid empty Windows resource table
    if (tablePos + 10 > tableEnd)
        return false;
    const quint16 count = u16(data, tablePos + 4);
    return count > 0 && tablePos + 10 + qsizetype(count) * 12 <= tableEnd;
}

VersionType detectUnknownNeTarget(const QByteArray& data, qsizetype neHeaderOffset)
{
    if (neHeaderOffset + 0x2C > data.size())
        return VersionType::Unknown;

    // Number of refs to imported modules: ne_cmod.
    const quint16 moduleCount = u16(data, neHeaderOffset + 0x1E);

    // Faithful to the C# implementation: an unknown-target NE with no
    // imported modules is treated as Windows. Linker version is not used.
    if (moduleCount == 0)
        return VersionType::Windows;

    // Module Reference Table: ne_modtab
    const quint16 moduleTableOffset = u16(data, neHeaderOffset + 0x28);

    // Imported Names Table: ne_imptab
    const quint16 importedNamesOffset = u16(data, neHeaderOffset + 0x2A);

    const qsizetype moduleTablePosition = neHeaderOffset + moduleTableOffset;
    const qsizetype importedNamesPosition = neHeaderOffset + importedNamesOffset;

    const QStringList os2Modules = {
        QStringLiteral("DOSCALLS"), QStringLiteral("KBDCALLS"),
        QStringLiteral("VIOCALLS"), QStringLiteral("MOUCALLS"),
        QStringLiteral("NLS"), QStringLiteral("QUECALLS")
    };
    const QStringList windowsModules = {
        QStringLiteral("GDI"), QStringLiteral("KERNEL")
    };

    for (quint16 i = 0; i < moduleCount; ++i) {
        const qsizetype referencePosition = moduleTablePosition + qsizetype(i) * 2;
        if (referencePosition + 2 > data.size())
            break;

        const quint16 nameOffset = u16(data, referencePosition);
        const qsizetype namePosition = importedNamesPosition + nameOffset;
        if (namePosition >= data.size())
            continue;

        const quint8 nameLength = quint8(data.at(namePosition));
        if (nameLength == 0 || namePosition + 1 + nameLength > data.size())
            continue;

        const QString moduleName = QString::fromLatin1(data.constData() + namePosition + 1,
                                                       nameLength).toUpper();
        if (os2Modules.contains(moduleName))
            return VersionType::OS2;
        if (windowsModules.contains(moduleName))
            return VersionType::Windows;
    }

    return VersionType::Unknown;
}

VersionType getNeVersionType(const QByteArray& data, qsizetype neHeaderOffset)
{
    if (neHeaderOffset + 0x37 > data.size())
        return VersionType::Unknown;

    const quint8 targetOS = quint8(data.at(neHeaderOffset + 0x36));
    switch (targetOS) {
    case 0x00:
        return detectUnknownNeTarget(data, neHeaderOffset);
    case 0x01:
        return VersionType::OS2;
    case 0x02:
        return VersionType::Windows;
    case 0x03:
        return VersionType::MSDOS4;
    case 0x04:
        return VersionType::Win386;
    case 0x05:
        return VersionType::IBMMPN;
    default:
        return VersionType::Unknown;
    }
}

QString versionDescription(VersionType versionType)
{
    switch (versionType) {
    case VersionType::OS2:
        return QStringLiteral("NE (New Executable for OS/2)");
    case VersionType::Windows:
        return QStringLiteral("NE (New Executable for Windows)");
    case VersionType::MSDOS4:
        return QStringLiteral("NE (New Executable for MS-DOS 4.x)");
    case VersionType::Win386:
        return QStringLiteral("NE (New Executable for Windows 386)");
    case VersionType::IBMMPN:
        return QStringLiteral("NE (New Executable for IBM Microkernel Personality Neutral)");
    case VersionType::Unknown:
    default:
        return QStringLiteral("NE (New Executable for unknown OS)");
    }
}

QString readTypeName(const QByteArray& data, qsizetype resourceTablePos,
                     quint16 typeValue, qsizetype safeParsingEnd)
{
    const qsizetype nameOffset = resourceTablePos + typeValue;
    if (nameOffset < resourceTablePos || nameOffset >= safeParsingEnd || nameOffset >= data.size())
        return QStringLiteral("#[InvalidTypeOffset:0x%1]").arg(typeValue, 0, 16).toUpper();

    if (nameOffset + 1 > data.size() || nameOffset + 1 > safeParsingEnd)
        return QStringLiteral("#[TypeLenOutOfBound:0x%1]").arg(nameOffset, 0, 16).toUpper();

    const quint8 nameLen = quint8(data.at(nameOffset));
    if (nameLen == 0)
        return QString();

    if (nameLen > 127 || nameOffset + 1 + nameLen > data.size() ||
        nameOffset + 1 + nameLen > safeParsingEnd) {
        return QStringLiteral("#[TypeCorruptedName:0x%1 @ 0x%2]")
            .arg(nameLen, 0, 16).arg(nameOffset, 0, 16).toUpper();
    }

    return QString::fromLatin1(data.constData() + nameOffset + 1, nameLen);
}

QString readResourceName(const QByteArray& data, qsizetype resourceTablePos,
                         quint16 idField, qsizetype safeParsingEnd)
{
    const qsizetype nameOffset = resourceTablePos + idField;
    if (nameOffset < resourceTablePos || nameOffset >= safeParsingEnd || nameOffset >= data.size())
        return QStringLiteral("#[InvalidResourceOffset:0x%1]").arg(idField, 0, 16).toUpper();

    if (nameOffset + 1 > data.size() || nameOffset + 1 > safeParsingEnd)
        return QStringLiteral("#[ResLenOutOfBound:0x%1]").arg(nameOffset, 0, 16).toUpper();

    const quint8 nameLen = quint8(data.at(nameOffset));
    if (nameLen == 0)
        return QString();

    if (nameLen > 127 || nameOffset + 1 + nameLen > data.size() ||
        nameOffset + 1 + nameLen > safeParsingEnd) {
        return QStringLiteral("#[ResCorruptedName:0x%1 @ 0x%2]")
            .arg(nameLen, 0, 16).arg(nameOffset, 0, 16).toUpper();
    }

    return QString::fromLatin1(data.constData() + nameOffset + 1, nameLen);
}


void appendNeArea(QVector<ResourceEntry>& destination, const QString& name,
                  const QByteArray& payload, quint64 firstOffset, bool os2)
{
    if (payload.isEmpty())
        return;
    ResourceEntry area;
    area.type = QStringLiteral("NE_AREA");
    area.name = name;
    area.hierarchyPath = QStringList() << name;
    area.language = QStringLiteral("neutral");
    area.dataOffset = firstOffset;
    area.dataSize = quint64(payload.size());
    area.format = ModuleFormat::NE;
    area.isOs2 = os2;
    area.data = payload;
    destination.push_back(std::move(area));
}

void appendNeStructure(QVector<ResourceEntry>& destination, const QByteArray& data,
                       qsizetype neHeaderOffset, bool os2, quint16 os2ResourceCount)
{
    const quint16 segmentCount = u16(data, neHeaderOffset + 0x1C);
    const quint16 segmentTableOffset = u16(data, neHeaderOffset + 0x22);
    const quint16 alignShift = u16(data, neHeaderOffset + 0x32);
    const qsizetype segmentTable = neHeaderOffset + segmentTableOffset;
    if (segmentTable < neHeaderOffset || segmentTable + qsizetype(segmentCount) * 8 > data.size())
        return;

    ResourceEntry headers;
    headers.type = QStringLiteral("NE_HEADERS");
    headers.name = QStringLiteral("headers");
    headers.hierarchyPath = QStringList() << headers.name;
    headers.language = QStringLiteral("neutral");
    headers.dataOffset = 0;
    headers.dataSize = qMin<quint64>(quint64(segmentTable + qsizetype(segmentCount) * 8), quint64(data.size()));
    headers.format = ModuleFormat::NE;
    headers.isOs2 = os2;
    headers.data = data.left(qsizetype(headers.dataSize));
    destination.push_back(std::move(headers));

    QByteArray textBytes;
    QByteArray dataBytes;
    quint64 textOffset = 0;
    quint64 dataOffset = 0;
    bool haveTextOffset = false;
    bool haveDataOffset = false;

    const quint16 firstOs2ResourceSegment = os2 && os2ResourceCount <= segmentCount
        ? quint16(segmentCount - os2ResourceCount) : segmentCount;
    for (quint16 i = 0; i < segmentCount; ++i) {
        if (os2 && i >= firstOs2ResourceSegment)
            continue;
        const qsizetype entryOffset = segmentTable + qsizetype(i) * 8;
        const quint16 sector = u16(data, entryOffset);
        quint64 length = u16(data, entryOffset + 2);
        const quint16 flags = u16(data, entryOffset + 4);
        if (sector == 0)
            continue;
        if (length == 0)
            length = 0x10000;
        const quint64 fileOffset = quint64(sector) << alignShift;
        if (fileOffset >= quint64(data.size()))
            continue;
        const quint64 available = qMin<quint64>(length, quint64(data.size()) - fileOffset);
        const QByteArray bytes = data.mid(qsizetype(fileOffset), qsizetype(available));
        const bool dataSegment = (flags & 0x0001u) != 0;
        if (dataSegment) {
            if (!haveDataOffset) { dataOffset = fileOffset; haveDataOffset = true; }
            dataBytes.append(bytes);
        } else {
            if (!haveTextOffset) { textOffset = fileOffset; haveTextOffset = true; }
            textBytes.append(bytes);
        }
    }

    appendNeArea(destination, QStringLiteral(".text"), textBytes, textOffset, os2);
    appendNeArea(destination, QStringLiteral(".data"), dataBytes, dataOffset, os2);
}

} // namespace

std::unique_ptr<NeModule> NeModule::open(const QString& filePath)
{
    auto module = std::unique_ptr<NeModule>(new NeModule);
    module->info_.filePath = filePath;
    module->info_.format = ModuleFormat::NE;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        module->info_.error = file.errorString();
        return module;
    }
    const QByteArray fileBytes = file.readAll();

    // verify MZ Header (DOS Stub) ---
    if (fileBytes.size() < 0x1A) {
        module->info_.error = QStringLiteral("File too short for a valid MZ header.");
        return module;
    }

    // get NE Header Offset
    if (fileBytes.size() < 0x40) {
        module->info_.error = QStringLiteral("File too short for a valid NE header.");
        return module;
    }

    const qsizetype neHeaderOffset = qsizetype(u32(fileBytes, 0x3C));
    module->info_.headerOffset = quint64(neHeaderOffset);
    if (neHeaderOffset < 0 || neHeaderOffset + 64 > fileBytes.size()) {
        module->info_.error = QStringLiteral("NE header offset out of range (0x%1).")
                                  .arg(neHeaderOffset, 0, 16).toUpper();
        return module;
    }

    // verify NE signature
    if (neHeaderOffset + 1 >= fileBytes.size() ||
        fileBytes.at(neHeaderOffset) != 'N' || fileBytes.at(neHeaderOffset + 1) != 'E') {
        module->info_.error = QStringLiteral("File is not an NE executable ('NE' signature not found at correct offset).");
        return module;
    }

    // get target OS
    if (neHeaderOffset + 0x36 + 1 > fileBytes.size()) {
        module->info_.error = QStringLiteral("File too short to determine target OS from NE header.");
        return module;
    }

    const quint8 osType = quint8(fileBytes.at(neHeaderOffset + 0x36)); // cannot be always trusted. 0x00 may be OS/2 os MT/MS-DOS
    const VersionType versionType = getNeVersionType(fileBytes, neHeaderOffset);
    const bool isOS2 = versionType == VersionType::OS2;
    const bool isWindows = versionType == VersionType::Windows;
    module->info_.description = versionDescription(versionType);

    appendNeStructure(module->resources_, fileBytes, neHeaderOffset, isOS2,
                      isOS2 ? u16(fileBytes, neHeaderOffset + 0x34) : 0);

    if (isWindows) {
        // get Windows resource table offset
        if (neHeaderOffset + 0x24 + 2 > fileBytes.size()) {
            module->info_.error = QStringLiteral("Resource table offset out of range or file too short for complete NE header.");
            return module;
        }

        const quint16 resourceTableOffset = u16(fileBytes, neHeaderOffset + 0x24);
        const qsizetype resourceTablePos = neHeaderOffset + resourceTableOffset;
        if (resourceTablePos + 2 > fileBytes.size()) {
            module->info_.error = QStringLiteral("Resource table offset points out of file bounds (or too close to end).");
            return module;
        }

        const quint16 alignShift = u16(fileBytes, resourceTablePos);
        qsizetype currentParsePos = resourceTablePos + 2; // starts after AlignmentShiftCount

        qsizetype safeParsingEnd = fileBytes.size();
        if (neHeaderOffset + 0x26 + 2 <= fileBytes.size()) {
            const quint16 residentNamesTableOffset = u16(fileBytes, neHeaderOffset + 0x26);
            if (residentNamesTableOffset > 0) {
                const qsizetype potentialNextTablePos = neHeaderOffset + residentNamesTableOffset;
                if (potentialNextTablePos > currentParsePos && potentialNextTablePos < fileBytes.size())
                    safeParsingEnd = potentialNextTablePos;
            }
        }

        // resource table parsing loop for Windows
        while (true) {
            if (currentParsePos + 2 > fileBytes.size() || currentParsePos + 2 > safeParsingEnd)
                break;

            const quint16 typeID = u16(fileBytes, currentParsePos);
            if (typeID == 0)
                break;

            const bool isNamedType = (typeID & 0x8000) == 0;
            const quint16 typeValue = quint16(typeID & 0x7FFF);

            QString typeName;
            if (isNamedType) {
                typeName = readTypeName(fileBytes, resourceTablePos, typeValue, safeParsingEnd);
            } else {
                typeName = windowsNeResourceTypes.value(typeValue, QStringLiteral("#%1").arg(typeValue));
            }

            // read ResourceCount and advance currentParsePos
            if (currentParsePos + 8 > fileBytes.size() || currentParsePos + 8 > safeParsingEnd)
                break;

            const quint16 resourceCount = u16(fileBytes, currentParsePos + 2);
            currentParsePos += 8; // Advance the pointer after the resource type header

            // Resource Parsing Loop for this Type
            for (quint16 i = 0; i < resourceCount; ++i) {
                if (currentParsePos + 12 > fileBytes.size() || currentParsePos + 12 > safeParsingEnd)
                    break;

                const quint16 offsetUnits = u16(fileBytes, currentParsePos);
                const quint16 lengthUnits = u16(fileBytes, currentParsePos + 2);
                const quint16 idField = u16(fileBytes, currentParsePos + 6); // ResourceID is at offset 6 in NE_Resource

                QString resourceName;
                if ((idField & 0x8000) == 0)
                    resourceName = readResourceName(fileBytes, resourceTablePos, idField, safeParsingEnd);
                else
                    resourceName = QStringLiteral("#%1").arg(idField & 0x7FFF);

                const quint64 resourceOffset = quint64(offsetUnits) << alignShift;
                const quint64 resourceLength = quint64(lengthUnits) << alignShift;

                ResourceEntry entry;
                entry.type = typeName;
                entry.name = resourceName;
                entry.language = QStringLiteral("neutral");
                entry.dataOffset = resourceOffset;
                entry.dataSize = resourceLength;
                entry.format = ModuleFormat::NE;
                entry.isOs2 = false;
                entry.hierarchyPath = QStringList() << QStringLiteral(".rsrc");
                if (typeName == QStringLiteral("RT_STRING") && (idField & 0x8000))
                    entry.baseId = ((idField & 0x7FFF) - 1) * 16;
                if (resourceOffset + resourceLength <= quint64(fileBytes.size()))
                    entry.data = fileBytes.mid(qsizetype(resourceOffset), qsizetype(resourceLength));

                module->resources_.push_back(std::move(entry));
                currentParsePos += 12; // Advance the pointer after the resource header
            }
        }
    } else if (isOS2) {
        // Get Resource Table Offset for OS/2
        // For a valid OS/2 NE, the offset ne_rsrctab (0x24) points to a table of (etype, ename) pairs.
        if (neHeaderOffset + 0x24 + 2 > fileBytes.size()) {
            module->info_.error = QStringLiteral("Resource table offset out of range or file too short for complete NE header (OS/2).");
            return module;
        }

        const quint16 os2ResourceTableOffset = u16(fileBytes, neHeaderOffset + 0x24);
        const qsizetype resourceTablePos = neHeaderOffset + os2ResourceTableOffset;

        // After obtaining resourceTablePos
        if (neHeaderOffset + 0x26 + 2 > fileBytes.size()) {
            module->info_.error = QStringLiteral("Resource table size field out of range (OS/2).");
            return module;
        }

        const quint16 resourceTableSize = u16(fileBytes, neHeaderOffset + 0x26);
        const qsizetype safeParsingEnd = resourceTablePos + resourceTableSize;

        if (resourceTablePos < neHeaderOffset || resourceTablePos >= fileBytes.size())
            return module;

        // For OS/2, the resource table is a sequence of (etype, ename) pairs, each 4 bytes.
        // The loop ends when either etype or ename is 0.
        const quint16 numResources = u16(fileBytes, neHeaderOffset + 0x34);
        qsizetype currentParsePos = resourceTablePos;

        const quint16 alignShift = u16(fileBytes, neHeaderOffset + 0x32);
        const quint16 segmentCount = u16(fileBytes, neHeaderOffset + 0x1C);
        const qsizetype segmentTablePos = neHeaderOffset + u16(fileBytes, neHeaderOffset + 0x22);

        for (quint16 i = 0; i < numResources; ++i) {
            if (currentParsePos + 4 > fileBytes.size() || currentParsePos + 4 > safeParsingEnd)
                break;

            const quint16 typeId = u16(fileBytes, currentParsePos);
            const quint16 nameId = u16(fileBytes, currentParsePos + 2);
            if (typeId == 0)
                break;

            const int segmentIndex = int(segmentCount) - int(numResources) + int(i);
            const qsizetype segmentEntryPos = segmentTablePos + qsizetype(segmentIndex) * 8;

            ResourceEntry entry;
            entry.type = os2NeResourceTypes.value(typeId, QStringLiteral("#%1").arg(typeId));
            entry.name = QStringLiteral("#%1").arg(nameId);
            entry.language = QStringLiteral("neutral");
            entry.format = ModuleFormat::NE;
            entry.isOs2 = true;
            entry.hierarchyPath = QStringList() << QStringLiteral(".rsrc");
            if (entry.type == QStringLiteral("RT_STRING"))
                entry.baseId = (int(nameId) - 1) * 16;

            if (segmentIndex >= 0 && segmentEntryPos + 8 <= fileBytes.size()) {
                const quint64 resourceOffset = quint64(u16(fileBytes, segmentEntryPos)) << alignShift;
                quint64 resourceLength = u16(fileBytes, segmentEntryPos + 2);
                if (resourceLength == 0)
                    resourceLength = 0x10000;

                entry.dataOffset = resourceOffset;
                entry.dataSize = resourceLength;
                if (resourceOffset + resourceLength <= quint64(fileBytes.size()))
                    entry.data = fileBytes.mid(qsizetype(resourceOffset), qsizetype(resourceLength));
            }

            // OS/2 NE stores resource data in segments. A segment whose encoded
            // length is zero is exactly 64 KiB; resources larger than 64 KiB are
            // emitted as consecutive resource-table entries with the same type and
            // name. Merge only when all continuation invariants match, so genuine
            // duplicate resources remain distinct.
            const bool previousWasFullSegment =
                segmentIndex > 0 &&
                segmentEntryPos >= segmentTablePos + 8 &&
                u16(fileBytes, segmentEntryPos - 8 + 2) == 0;

            bool mergedContinuation = false;
            if (previousWasFullSegment && !module->resources_.isEmpty()) {
                ResourceEntry &previous = module->resources_.last();
                const bool sameIdentity = previous.type == entry.type && previous.name == entry.name;
                const bool physicallyContiguous =
                    previous.dataOffset + previous.dataSize == entry.dataOffset;

                if (sameIdentity && physicallyContiguous && !previous.data.isEmpty() && !entry.data.isEmpty()) {
                    previous.data.append(entry.data);
                    previous.dataSize += entry.dataSize;
                    mergedContinuation = true;
                }
            }

            if (!mergedContinuation)
                module->resources_.push_back(std::move(entry));

            currentParsePos += 4; // Move to the next entry (etype + ename)
        }
    } else {
        module->info_.error = QStringLiteral("Unsupported NE target OS type: 0x%1. Only Windows and OS/2 are supported for resource parsing.")
                                  .arg(osType, 2, 16, QChar('0')).toUpper();
    }

    return module;
}


} // namespace peare
