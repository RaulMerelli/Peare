#include "OpenerSession.h"
#include "modules/Compat.h"

#include "modules/ModuleFactory.h"
#include "modules/Ps2RomdirParser.h"
#include "modules/MsiModule.h"
#include "modules/MsgModule.h"
#include "fs/LinuxRaid.h"
#include "fs/DynamicDisk.h"

#include <QCryptographicHash>
#include <QHash>
#include <QSet>
#include <QTemporaryFile>
#include <utility>

namespace peare {
namespace {

ResourcePlatform platformFor(const ResourceEntry& entry)
{
    if (entry.isOs2)
        return ResourcePlatform::Os2;

    switch (entry.format) {
    case ModuleFormat::PE:
    case ModuleFormat::WINCE_ROM:
    case ModuleFormat::FFU:
    case ModuleFormat::SIEMENS_FSF:
    case ModuleFormat::APPX:
    case ModuleFormat::MSIX:
    case ModuleFormat::APPXBUNDLE:
    case ModuleFormat::EAPPX:
    case ModuleFormat::EMSIX:
    case ModuleFormat::EAPPXBUNDLE:
    case ModuleFormat::EMSIXBUNDLE:
    case ModuleFormat::XAP:
        return ResourcePlatform::Windows;
    case ModuleFormat::LE:
    case ModuleFormat::LX:
        return ResourcePlatform::Os2;
    case ModuleFormat::NE:
        return ResourcePlatform::Windows;
    case ModuleFormat::XEX:
    case ModuleFormat::XBE:
    case ModuleFormat::XUIZ:
    case ModuleFormat::LIVE_PIRS:
    case ModuleFormat::CON:
        return ResourcePlatform::Other;
    case ModuleFormat::OS2_PACK:
    case ModuleFormat::OS2_EA:
    case ModuleFormat::HPFS:
        return ResourcePlatform::Os2;
    case ModuleFormat::SZDD:
    case ModuleFormat::CAB:
    case ModuleFormat::ZIP:
    case ModuleFormat::TAR:
    case ModuleFormat::SIEMENS_IMG:
    case ModuleFormat::SIEMENS_FWF:
    case ModuleFormat::ISO9660:
    case ModuleFormat::WIM:
    case ModuleFormat::FAT:
    case ModuleFormat::UDF:
    case ModuleFormat::EXFAT:
    case ModuleFormat::VMDK:
    case ModuleFormat::RAW_DISK:
    case ModuleFormat::FLOPPY_IMAGE:
    case ModuleFormat::VHD:
    case ModuleFormat::VDI:
    case ModuleFormat::QCOW:
    case ModuleFormat::QCOW2:
    case ModuleFormat::VHDX:
    case ModuleFormat::SDI:
    case ModuleFormat::XVA:
    case ModuleFormat::SWAP:
    case ModuleFormat::LVM:
    case ModuleFormat::LINUX_RAID:
    case ModuleFormat::DYNAMIC_DISK:
    case ModuleFormat::EXT:
    case ModuleFormat::XFS:
    case ModuleFormat::JFS:
    case ModuleFormat::SQUASHFS:
    case ModuleFormat::HFSPLUS:
    case ModuleFormat::DMG:
    case ModuleFormat::BTRFS:
    case ModuleFormat::RESX:
    case ModuleFormat::CUE_BIN:
    case ModuleFormat::PS3_PUP:
    case ModuleFormat::WAD:
    case ModuleFormat::MTF:
    case ModuleFormat::MDF_MDS:
    case ModuleFormat::PARALLELS_HDD:
    case ModuleFormat::PS2_ROMDIR:
        return ResourcePlatform::Other;
    case ModuleFormat::MSI:
    case ModuleFormat::MSG:
        return ResourcePlatform::Windows;
    case ModuleFormat::NTFS:
    case ModuleFormat::REGISTRY:
    case ModuleFormat::BOOTCONFIG:
        return ResourcePlatform::Windows;
    case ModuleFormat::Unknown:
    case ModuleFormat::DosMZ:
        return ResourcePlatform::Unknown;
    }
    return ResourcePlatform::Unknown;
}



ResourceContext contextFor(const ResourceEntry& entry,
                           const ModuleInfo& moduleInfo,
                           int resourceIndex)
{
    ResourceContext context;
    context.sourceName = moduleInfo.filePath;
    context.containerFormat = entry.format;
    context.platform = platformFor(entry);
    context.type = entry.type;
    context.identifier = entry.name;
    context.language = entry.language;
    context.codePage = entry.codePage;
    context.dataOffset = entry.dataOffset;
    context.dataSize = entry.dataSize;
    context.baseId = entry.baseId;
    context.resourceIndex = resourceIndex;
    context.isContainer = entry.isContainer;
    return context;
}

} // namespace

OpenerSession::OpenerSession() = default;
OpenerSession::~OpenerSession() = default;
OpenerSession::OpenerSession(OpenerSession&&) noexcept = default;
OpenerSession& OpenerSession::operator=(OpenerSession&&) noexcept = default;

bool OpenerSession::openFile(const QString& filePath)
{
    close();
    return adoptModule(ModuleFactory::open(filePath), filePath);
}

bool OpenerSession::openStore(const peare::fs::ByteStorePtr& store, const QString& sourceName,
                              const QString& subPath)
{
    close();
    return adoptModule(ModuleFactory::open(store, sourceName, subPath), sourceName);
}

bool OpenerSession::openBuffer(const QByteArray& data, const QString& sourceName)
{
    close();

    auto temporary = peare::makeUnique<QTemporaryFile>();
    if (!temporary->open()) {
        info_.filePath = sourceName;
        info_.error = temporary->errorString();
        return false;
    }
    if (temporary->write(data) != data.size() || !temporary->flush()) {
        info_.filePath = sourceName;
        info_.error = temporary->errorString();
        return false;
    }

    const QString temporaryPath = temporary->fileName();
    temporary->close();
    ModulePtr module = ModuleFactory::open(temporaryPath, sourceName);
    bufferFile_ = std::move(temporary);
    return adoptModule(std::move(module), sourceName);
}

void OpenerSession::close()
{
    folders_.clear();
    resourceContainer_ = nullptr;
    resources_.clear();
    module_.reset();
    bufferFile_.reset();
    info_ = {};
}

bool OpenerSession::isOpen() const noexcept
{
    return module_ != nullptr && info_.isValid();
}

const ModuleInfo& OpenerSession::info() const noexcept
{
    return info_;
}

const QVector<ResourceFolder>& OpenerSession::folders() const noexcept
{
    return folders_;
}

bool OpenerSession::hasResourceContainer() const noexcept
{
    return resourceContainer_ != nullptr;
}

int OpenerSession::resourceCount() const noexcept
{
    return resources_.size();
}

const ResourceEntry* OpenerSession::resourceEntry(int resourceIndex) const noexcept
{
    if (resourceIndex < 0 || resourceIndex >= resources_.size())
        return nullptr;
    return &resources_.at(resourceIndex);
}

OpenedResource OpenerSession::openResource(int resourceIndex) const
{
    OpenedResource resource;
    const ResourceEntry* entry = resourceEntry(resourceIndex);
    if (!entry) {
        resource.error = QStringLiteral("Invalid resource index");
        return resource;
    }

    resource.session = this;
    resource.resourceIndex = resourceIndex;
    // Array-backed resources carry their (already-in-memory) bytes directly.
    // Layer-backed resources carry only the store pointer; the bytes are read
    // lazily when the payload is requested, so merely opening a resource (or
    // building the sibling list) never reads content.
    resource.payload = entry->data;
    resource.contentStore = entry->content;
    resource.isDirectory = entry->isDirectory;
    resource.subPath = entry->containerSubPath;
    resource.context = contextFor(*entry, info_, resourceIndex);
    return resource;
}

OpenedResource OpenerSession::openResource(const QString& folderType,
                                           const QString& identifier,
                                           const QString& preferredLanguage) const
{
    return openResource(findResource(folderType, identifier, preferredLanguage));
}

int OpenerSession::findResource(const QString& folderType,
                                const QString& identifier,
                                const QString& preferredLanguage) const noexcept
{
    if (resources_.isEmpty())
        return -1;

    const QVector<ResourceEntry>& entries = resources_;
    if (!preferredLanguage.isEmpty()) {
        for (int index = 0; index < entries.size(); ++index) {
            const ResourceEntry& entry = entries.at(index);
            if (entry.type == folderType && entry.name == identifier && entry.language == preferredLanguage)
                return index;
        }
    }
    for (int index = 0; index < entries.size(); ++index) {
        const ResourceEntry& entry = entries.at(index);
        if (entry.type == folderType && entry.name == identifier)
            return index;
    }
    return -1;
}

bool OpenerSession::adoptModule(ModulePtr module, const QString& displayPath)
{
    module_ = std::move(module);
    if (!module_) {
        info_.filePath = displayPath;
        info_.error = QStringLiteral("Module factory returned no module");
        return false;
    }

    info_ = module_->info();
    info_.filePath = displayPath;
    resourceContainer_ = dynamic_cast<const IResourceContainer*>(module_.get());
    resources_.clear();
    if (resourceContainer_)
        resources_ = resourceContainer_->resources();
    // Nested containers are expanded only on demand. Whole files receive the
    // existing deep header probe (needed for ISO/UDF/Btrfs signatures at fixed
    // offsets). Arbitrary executable resources receive only a 512-byte,
    // strong-signature probe, which makes embedded ZIP/FWF/etc. navigable without
    // applying expensive or loose structural heuristics to every RT_* payload.
    for (ResourceEntry& entry : resources_) {
        entry.isContainer = false;
        if (entry.isDirectory) {
            entry.isContainer = true;
            continue;
        }

        // Only complete embedded files are eligible for recursive format
        // detection. PE headers/sections and other structural regions can begin
        // with valid-looking signatures but are not standalone files; probing
        // them caused the PE_HEADERS -> PE_HEADERS infinite expansion loop.
        if (!entry.isEmbeddedFile)
            continue;

        const auto readHead = [&](std::int64_t count) -> QByteArray {
            if (entry.content) {
                const std::vector<std::uint8_t> h = entry.content->readRange(0, count);
                return QByteArray(reinterpret_cast<const char*>(h.data()),
                                  static_cast<int>(h.size()));
            }
            return entry.data.left(static_cast<int>(count));
        };
        const auto readTail4 = [&]() -> QByteArray {
            if (entry.content && entry.content->capacity() >= 4) {
                const std::vector<std::uint8_t> t =
                    entry.content->readRange(entry.content->capacity() - 4, 4);
                return QByteArray(reinterpret_cast<const char*>(t.data()),
                                  static_cast<int>(t.size()));
            }
            return entry.data.size() >= 4 ? entry.data.right(4) : QByteArray();
        };

        const QByteArray prefix = readHead(512);
        ModuleFormat nestedFormat = prefix.isEmpty() ? ModuleFormat::Unknown
            : ModuleFormatDetector::detectNestedBuffer(prefix).format;
        if (nestedFormat == ModuleFormat::ZIP) {
            const ModuleFormat named = ModuleFormatDetector::zipPackageFormatForName(entry.name);
            if (named != ModuleFormat::Unknown) nestedFormat = named;
        }

        if (nestedFormat == ModuleFormat::Unknown && MsiModule::hasCompoundMagic(prefix.left(8))) {
            fs::ByteStorePtr compoundStore = entry.content;
            if (!compoundStore && !entry.data.isEmpty())
                compoundStore = std::make_shared<fs::MemoryStore>(
                    reinterpret_cast<const std::uint8_t*>(entry.data.constData()),
                    std::size_t(entry.data.size()));
            if (compoundStore && MsgModule::isOutlookMessage(compoundStore))
                nestedFormat = ModuleFormat::MSG;
            else if (MsiModule::hasInstallerExtension(entry.name))
                nestedFormat = ModuleFormat::MSI;
        }

        // Siemens ProSave common-file IMG wraps payloads that can begin with a
        // stronger B000FF signature. Probe the footer only for .IMG resources or
        // an already-detected WinCE payload, then let the outer wrapper win. This
        // keeps ordinary ISO directory enumeration at the original one-prefix-
        // read cost while preserving ISO -> folders -> IMG -> NK.bin.
        const bool imgCandidate = entry.name.endsWith(QStringLiteral(".img"),
                                                       Qt::CaseInsensitive) ||
                                  nestedFormat == ModuleFormat::WINCE_ROM;
        if (entry.isEmbeddedFile && imgCandidate &&
            (!entry.content || entry.content->cheapRandomAccess())) {
            const QByteArray tail = readTail4();
            if (tail.size() == 4) {
                const auto* p = reinterpret_cast<const unsigned char*>(tail.constData());
                const quint32 value = quint32(p[0]) | (quint32(p[1]) << 8) |
                                      (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
                if (value == 0x03031998U) nestedFormat = ModuleFormat::SIEMENS_IMG;
            }
        }

        if (nestedFormat == ModuleFormat::Unknown && entry.isEmbeddedFile) {
            const QByteArray header = readHead(0x14000);
            nestedFormat = header.isEmpty() ? ModuleFormat::Unknown
                : ModuleFormatDetector::detectBuffer(header).format;
        }

        // A complete PS2 BIOS stores ROMDIR around 0x2700, but validating that
        // directory also requires checking file ranges against the full 4 MiB
        // image. A prefix-only QByteArray therefore looks truncated and was not
        // marked expandable when it lived inside ZIP/TAR/CAB. Probe the actual
        // lazy store for plausible PS2 image names, preserving lazy decompression.
        if (nestedFormat == ModuleFormat::Unknown && entry.content &&
            entry.isEmbeddedFile && entry.content->capacity() >= 64 &&
            entry.content->capacity() <= 64LL * 1024LL * 1024LL &&
            (entry.name.endsWith(QStringLiteral(".bin"), Qt::CaseInsensitive) ||
             entry.name.endsWith(QStringLiteral(".rom"), Qt::CaseInsensitive) ||
             entry.name.endsWith(QStringLiteral(".img"), Qt::CaseInsensitive))) {
            const quint64 scan = std::min<quint64>(
                quint64(entry.content->capacity()), 16ULL * 1024ULL * 1024ULL);
            if (findPs2Romdir(entry.content, scan) >= 0)
                nestedFormat = ModuleFormat::PS2_ROMDIR;
        }

        if (nestedFormat == ModuleFormat::Unknown && entry.content && entry.isEmbeddedFile &&
            entry.content->cheapRandomAccess() &&
            fs::hasLinuxRaidSuperblock(entry.content))
            nestedFormat = ModuleFormat::LINUX_RAID;
        if (nestedFormat == ModuleFormat::Unknown && entry.content && entry.isEmbeddedFile &&
            entry.content->cheapRandomAccess() &&
            fs::hasDynamicDiskMetadata(entry.content))
            nestedFormat = ModuleFormat::DYNAMIC_DISK;

        entry.isContainer = nestedFormat != ModuleFormat::Unknown;
        if (entry.isContainer)
            entry.format = nestedFormat;
    }
    rebuildFolders();
    return info_.isValid();
}

void OpenerSession::rebuildFolders()
{
    folders_.clear();
    if (resources_.isEmpty())
        return;

    QHash<QString, int> folderIndexByType;
    const QVector<ResourceEntry>& entries = resources_;
    for (int resourceIndex = 0; resourceIndex < entries.size(); ++resourceIndex) {
        const ResourceEntry& entry = entries.at(resourceIndex);
        QStringList path = entry.hierarchyPath;
        const bool selectableContainer =
            entry.isDirectory ||  // filesystem directory (lazy nested listing)
            entry.type == QStringLiteral("PE_MODULE") ||
            entry.type == QStringLiteral("PE_HEADERS") ||
            entry.type == QStringLiteral("PE_SECTION") ||
            entry.type == QStringLiteral("XBE_SECTION") ||
            entry.type == QStringLiteral("NE_HEADERS") ||
            entry.type == QStringLiteral("NE_AREA") ||
            entry.type == QStringLiteral("LE_HEADERS") ||
            entry.type == QStringLiteral("LE_AREA") ||
            entry.type == QStringLiteral("LX_HEADERS") ||
            entry.type == QStringLiteral("LX_AREA") ||
            entry.type == QStringLiteral("XUIZ_CONTAINER") ||
            entry.type == QStringLiteral("XUIZ_FILE") ||
            entry.type == QStringLiteral("WINCE_FILE") ||
            entry.type == QStringLiteral("OS2_PACK_FILE") ||
            entry.type == QStringLiteral("SZDD_FILE") ||
            entry.type == QStringLiteral("RESX_DATA") ||
            entry.type == QStringLiteral("RESX_METADATA") ||
            entry.type == QStringLiteral("RESX_FILE_REF") ||
            entry.type == QStringLiteral("RESX_BINARY") ||
            entry.type == QStringLiteral("RESX_SERIALIZED_OBJECT") ||
            entry.type == QStringLiteral("CUE_SHEET") ||
            entry.type == QStringLiteral("CUE_METADATA") ||
            entry.type == QStringLiteral("CUE_BINARY_FILE") ||
            entry.type == QStringLiteral("CUE_DATA_TRACK") ||
            entry.type == QStringLiteral("CUE_AUDIO_TRACK") ||
            entry.type == QStringLiteral("CAB_FILE") ||
            entry.type == QStringLiteral("ZIP_FILE") ||
            entry.type == QStringLiteral("APP_PACKAGE_FILE") ||
            entry.type == QStringLiteral("APP_BUNDLE_FILE") ||
            entry.type == QStringLiteral("XAP_FILE") ||
            entry.type == QStringLiteral("XAP_METADATA") ||
            entry.type == QStringLiteral("XAP_ENCRYPTED_ARCHIVE") ||
            entry.type == QStringLiteral("TAR_FILE") ||
            entry.type == QStringLiteral("PUP_SEGMENT") ||
            entry.type == QStringLiteral("WAD_LUMP") ||
            entry.type == QStringLiteral("MDF_TRACK") ||
            entry.type == QStringLiteral("PS2_ROM_FILE") ||
            entry.type == QStringLiteral("MSI_STREAM") ||
            entry.type == QStringLiteral("MSG_ATTACHMENT") ||
            entry.type == QStringLiteral("SIEMENS_IMG_FILE") ||
            entry.type == QStringLiteral("SIEMENS_FWF_FILE") ||
            entry.type == QStringLiteral("SIEMENS_FSF_FILE") ||
            entry.type == QStringLiteral("ISO_FILE") ||
            entry.type == QStringLiteral("WIM_FILE") ||
            entry.type == QStringLiteral("FAT_FILE") ||
            entry.type == QStringLiteral("UDF_FILE") ||
            entry.type == QStringLiteral("EXFAT_FILE") ||
            entry.type == QStringLiteral("EXT_FILE") ||
            entry.type == QStringLiteral("NTFS_FILE") ||
            entry.type == QStringLiteral("XFS_FILE") ||
            entry.type == QStringLiteral("JFS_FILE") ||
            entry.type == QStringLiteral("HPFS_FILE") ||
            entry.type == QStringLiteral("SQUASHFS_FILE") ||
            entry.type == QStringLiteral("HFS_FILE") ||
            entry.type == QStringLiteral("BTRFS_FILE") ||
            entry.type == QStringLiteral("LVM_LOGICAL_VOLUME") ||
            entry.type == QStringLiteral("LINUX_RAID_VOLUME") ||
            entry.type == QStringLiteral("LDM_VOLUME") ||
            entry.type == QStringLiteral("DISK_PARTITION");
        if (!selectableContainer && (path.isEmpty() || path.last() != entry.type))
            path.push_back(entry.type);
        const QString type = path.join(QChar(0x1f));
        auto found = folderIndexByType.constFind(type);
        int folderIndex = -1;
        if (found == folderIndexByType.constEnd()) {
            folderIndex = folders_.size();
            folderIndexByType.insert(type, folderIndex);
            ResourceFolder folder;
            folder.type = type;
            folders_.push_back(folder);
        } else {
            folderIndex = found.value();
        }
        folders_[folderIndex].resourceIndices.push_back(resourceIndex);
    }
}

QString resourcePlatformName(ResourcePlatform platform)
{
    switch (platform) {
    case ResourcePlatform::Windows: return QStringLiteral("Windows");
    case ResourcePlatform::Os2: return QStringLiteral("OS/2");
    case ResourcePlatform::Other: return QStringLiteral("Other");
    case ResourcePlatform::Unknown: return QStringLiteral("Unknown");
    }
    return QStringLiteral("Unknown");
}

} // namespace peare
