#include "OpenerSession.h"
#include "modules/Compat.h"

#include "modules/ModuleFactory.h"
#include "modules/LzmaModule.h"
#include "modules/Ps2RomdirParser.h"
#include "modules/MsiModule.h"
#include "modules/MsgModule.h"
#include "fs/LinuxRaid.h"
#include "fs/DynamicDisk.h"
#include "fs/AcronisRestoredStore.h"
#include "fs/TibLegacyReader.h"
#include "fs/TibxVolumeStore.h"

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
    case ModuleFormat::XDBF:
    case ModuleFormat::XUIZ:
    case ModuleFormat::LIVE_PIRS:
    case ModuleFormat::CON:
        return ResourcePlatform::Other;
    case ModuleFormat::OS2_PACK:
    case ModuleFormat::WPI:
    case ModuleFormat::OS2_EA:
    case ModuleFormat::HPFS:
        return ResourcePlatform::Os2;
    case ModuleFormat::SZDD:
    case ModuleFormat::CHM:
    case ModuleFormat::KWAJ:
    case ModuleFormat::XBCOMPRESS:
    case ModuleFormat::GZIP:
    case ModuleFormat::XZ:
    case ModuleFormat::BZIP2:
    case ModuleFormat::AR:
    case ModuleFormat::ARJ:
    case ModuleFormat::XAR:
    case ModuleFormat::CPIO:
    case ModuleFormat::UNIX_COMPRESS:
    case ModuleFormat::INTEL_HEX:
    case ModuleFormat::LZIP:
    case ModuleFormat::LZMA:
    case ModuleFormat::ZSTD:
    case ModuleFormat::CAB:
    case ModuleFormat::SEVEN_ZIP:
    case ModuleFormat::ZIP:
    case ModuleFormat::ANDROID_APK:
    case ModuleFormat::TAR:
    case ModuleFormat::ASAR:
    case ModuleFormat::SIEMENS_IMG:
    case ModuleFormat::SIEMENS_ADONIS:
    case ModuleFormat::SIEMENS_FWF:
    case ModuleFormat::ISO9660:
    case ModuleFormat::WIM:
    case ModuleFormat::FAT:
    case ModuleFormat::UDF:
    case ModuleFormat::EXFAT:
    case ModuleFormat::VMDK:
    case ModuleFormat::RAW_DISK:
    case ModuleFormat::ANDROID_SPARSE:
    case ModuleFormat::ANDROID_SUPER:
    case ModuleFormat::DXBC:
    case ModuleFormat::NUP:
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
    case ModuleFormat::CRAMFS:
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
    case ModuleFormat::MAC_RESOURCE_FORK:
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

static void probeResource(ResourceEntry& entry);

OpenerSession::OpenerSession() = default;
OpenerSession::~OpenerSession() = default;
OpenerSession::OpenerSession(OpenerSession&&) noexcept = default;
OpenerSession& OpenerSession::operator=(OpenerSession&&) noexcept = default;

bool OpenerSession::openFile(const QString& filePath, const QString& password, const QByteArray& tibxPrivateKey,
                             const QString& tibxKeyPassphrase)
{
    close();
    return adoptModule(ModuleFactory::openWithPassword(filePath, password, tibxPrivateKey, tibxKeyPassphrase), filePath);
}

bool OpenerSession::openStore(const peare::fs::ByteStorePtr& store, const QString& sourceName,
                              const QString& subPath, const QString& password, const QByteArray& tibxPrivateKey,
                              const QString& tibxKeyPassphrase)
{
    close();
    return adoptModule(ModuleFactory::open(store, sourceName, subPath, password, tibxPrivateKey, tibxKeyPassphrase), sourceName);
}

bool OpenerSession::openParts(const std::vector<std::pair<peare::fs::ByteStorePtr, QString> >& parts,
                              const QString& password, const QByteArray& tibxPrivateKey, const QString& tibxKeyPassphrase)
{
    close();
    return adoptModule(ModuleFactory::openParts(parts, password, tibxPrivateKey, tibxKeyPassphrase), parts.empty() ? QString() : parts[0].second);
}

bool OpenerSession::openBuffer(const QByteArray& data, const QString& sourceName,
                               const QString& password, const QByteArray& tibxPrivateKey, const QString& tibxKeyPassphrase)
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
    ModulePtr module = ModuleFactory::open(temporaryPath, sourceName, password, tibxPrivateKey, tibxKeyPassphrase);
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

    ResourceEntry resolved;
    if (entry->deferredContent) {
        resolved = *entry;
        resolved.content = entry->deferredContent->resolve();
        if (!resolved.content || !resolved.content->validate() || resolved.content->capacity() < 0) {
            resource.error = QStringLiteral("Cannot open NTFS file content");
            return resource;
        }
        resolved.dataSize = static_cast<quint64>(resolved.content->capacity());
        probeResource(resolved);
        if (!resolved.content->validate()) {
            resource.error = QStringLiteral("Cannot read NTFS file content");
            return resource;
        }
        entry = &resolved;
    }

    resource.session = this;
    resource.resourceIndex = resourceIndex;
    // Array-backed resources provide bytes directly; layer-backed resources read on demand.
    resource.payload = entry->data;
    resource.contentStore = entry->content;
    resource.isDirectory = entry->isDirectory;
    resource.subPath = entry->containerSubPath;
    resource.context = contextFor(*entry, info_, resourceIndex);
    if (entry->format == ModuleFormat::SEVEN_ZIP) {
        QStringList path = entry->hierarchyPath;
        path.push_back(entry->name);
        resource.archivePath = path.join(QLatin1Char('/'));
        if (entry->isDirectory && !resource.archivePath.endsWith(QLatin1Char('/')))
            resource.archivePath.append(QLatin1Char('/'));
    }
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

static void probeResource(ResourceEntry& entry)
{
    // Probe whole files deeply and arbitrary embedded resources by strong signatures.
    // This keeps nested detection useful without scanning every resource payload.
        entry.isContainer = false;
        if (entry.isDirectory) {
            entry.isContainer = !entry.containerSubPath.isEmpty();
            return;
        }

        // Probe complete embedded files only; PE structures can resemble nested files.
        // This prevents recursive PE_HEADERS expansion.
        if (!entry.isEmbeddedFile)
            return;
        if (entry.content && !entry.content->cheapRandomAccess() &&
            entry.content->capacity() > 16LL * 1024LL * 1024LL) {
            entry.isContainer = true;
            return;
        }

        bool readFailed = false;
        const auto readHead = [&](std::int64_t count) -> QByteArray {
            if (entry.content) {
                const std::int64_t length = std::min<std::int64_t>(
                    std::max<std::int64_t>(count, 0),
                    std::max<std::int64_t>(entry.content->capacity(), 0));
                QByteArray header(static_cast<int>(length), '\0');
                if (length && !entry.content->readExactlyChecked(0,
                        reinterpret_cast<std::uint8_t*>(header.data()),
                        static_cast<int>(length))) {
                    readFailed = true;
                    return QByteArray();
                }
                return header;
            }
            const int length = static_cast<int>(std::min<std::int64_t>(
                std::max<std::int64_t>(count, 0), entry.data.size()));
            return QByteArray::fromRawData(entry.data.constData(), length);
        };
        const auto readTail4 = [&]() -> QByteArray {
            if (entry.content && entry.content->capacity() >= 4) {
                QByteArray tail(4, '\0');
                if (!entry.content->readExactlyChecked(entry.content->capacity() - 4,
                    reinterpret_cast<std::uint8_t*>(tail.data()), 4)) {
                    readFailed = true;
                    return QByteArray();
                }
                return tail;
            }
            return entry.data.size() >= 4
                ? QByteArray::fromRawData(entry.data.constData() + entry.data.size() - 4, 4)
                : QByteArray();
        };

        const QByteArray prefix = readHead(512);
        if (readFailed) return;
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

        // Check the footer only for IMG candidates because ProSave wraps B000FF data.
        // Preserve the outer IMG container when its payload also matches WinCE.
        const bool imgCandidate = entry.name.endsWith(QStringLiteral(".img"),
                                                       Qt::CaseInsensitive) ||
                                  nestedFormat == ModuleFormat::WINCE_ROM;
        if (entry.isEmbeddedFile && imgCandidate &&
            (!entry.content || entry.content->cheapRandomAccess())) {
            const QByteArray tail = readTail4();
            if (readFailed) return;
            if (tail.size() == 4) {
                const auto* p = reinterpret_cast<const unsigned char*>(tail.constData());
                const quint32 value = quint32(p[0]) | (quint32(p[1]) << 8) |
                                      (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
                if (value == 0x03031998U) nestedFormat = ModuleFormat::SIEMENS_IMG;
            }
        }

        if (nestedFormat == ModuleFormat::Unknown && entry.isEmbeddedFile) {
            const QByteArray header = readHead(0x14000);
            if (readFailed) {
                const ModuleFormat shortFormat = ModuleFormatDetector::detectBuffer(prefix).format;
                if (shortFormat != ModuleFormat::FAT && shortFormat != ModuleFormat::EXFAT)
                    return;
                nestedFormat = shortFormat;
            } else {
                nestedFormat = header.isEmpty() ? ModuleFormat::Unknown
                    : ModuleFormatDetector::detectBuffer(header).format;
            }
        }
        if (nestedFormat == ModuleFormat::Unknown && entry.isEmbeddedFile &&
            LzmaModule::hasExtension(entry.name))
            nestedFormat = ModuleFormat::LZMA;

        // Validate PS2 ROMDIR against the full lazy store, not a truncated prefix.
        // Restrict the scan to plausible image names and bounded file sizes.
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
    for (ResourceEntry& entry : resources_) {
        if (!entry.deferredContent) probeResource(entry);
        if (!entry.content) continue;
        const auto legacy = std::dynamic_pointer_cast<fs::TibLegacyFilteredVolume>(entry.content);
        const auto tibx = std::dynamic_pointer_cast<fs::TibxVolumeStore>(entry.content);
        if (!legacy && (!tibx || entry.type != QStringLiteral("TIBX_VOLUME"))) continue;
        if (legacy) {
            entry.content = std::make_shared<fs::AcronisRestoredStore>(entry.content,
                [legacy](fs::ByteStorePtr* filter) {
                    *filter = legacy->fatClusterFilter();
                    return true;
                });
        } else {
            entry.content = std::make_shared<fs::AcronisRestoredStore>(entry.content,
                [tibx](fs::ByteStorePtr* filter) {
                    const auto result = tibx->openFatClusterFilter(filter);
                    return result == fs::TibxVolumeStore::StreamResult::Found ||
                           result == fs::TibxVolumeStore::StreamResult::NotFound;
                }, true);
        }
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
        const bool pathIncludesEntry = entry.type != QStringLiteral("ZIP_FILE") &&
            entry.type != QStringLiteral("APK_FILE") &&
            !path.isEmpty() && path.last() == entry.name;
        if (pathIncludesEntry)
            path.removeLast();
        const bool selectableContainer =
            entry.isDirectory || entry.isEmbeddedFile || entry.isContainer ||
            pathIncludesEntry ||  // Some modules store the resource leaf in this path.
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
            entry.type == QStringLiteral("WPI_FILE") ||
            entry.type == QStringLiteral("BZIP2_FILE") ||
            entry.type == QStringLiteral("AR_FILE") ||
            entry.type == QStringLiteral("ARJ_FILE") ||
            entry.type == QStringLiteral("ARJ_DIR") ||
            entry.type == QStringLiteral("CPIO_FILE") ||
            entry.type == QStringLiteral("CPIO_SPECIAL") ||
            entry.type == QStringLiteral("UNIX_COMPRESS_FILE") ||
            entry.type == QStringLiteral("LZIP_FILE") ||
            entry.type == QStringLiteral("ZSTD_FILE") ||
            entry.type == QStringLiteral("UUE_FILE") ||
            entry.type == QStringLiteral("LHA_FILE") ||
            entry.type == QStringLiteral("RAR4_FILE") ||
            entry.type == QStringLiteral("RAR5_FILE") ||
            entry.type == QStringLiteral("LHA_DIR") ||
            entry.type == QStringLiteral("LHA_SYMLINK") ||
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
            entry.type == QStringLiteral("APK_FILE") ||
            entry.type == QStringLiteral("APP_PACKAGE_FILE") ||
            entry.type == QStringLiteral("APP_BUNDLE_FILE") ||
            entry.type == QStringLiteral("XAP_FILE") ||
            entry.type == QStringLiteral("XAP_METADATA") ||
            entry.type == QStringLiteral("XAP_ENCRYPTED_ARCHIVE") ||
            entry.type == QStringLiteral("TAR_FILE") ||
            entry.type == QStringLiteral("ASAR_FILE") ||
            entry.type == QStringLiteral("ASAR_LINK") ||
            entry.type == QStringLiteral("PUP_SEGMENT") ||
            entry.type == QStringLiteral("WAD_LUMP") ||
            entry.type == QStringLiteral("WAD_MARKER") ||
            entry.type == QStringLiteral("WAD_MAP_HEADER") ||
            entry.type == QStringLiteral("WAD_MAP_LUMP") ||
            entry.type == QStringLiteral("WAD_PALETTE") ||
            entry.type == QStringLiteral("WAD_PNAMES") ||
            entry.type == QStringLiteral("WAD_TEXTUREX") ||
            entry.type == QStringLiteral("WAD_DOOM_PATCH") ||
            entry.type == QStringLiteral("WAD_DOOM_SPRITE") ||
            entry.type.startsWith(QStringLiteral("WAD2_LUMP_TYPE_")) ||
            entry.type.startsWith(QStringLiteral("WAD3_LUMP_TYPE_")) ||
            entry.type == QStringLiteral("MDF_TRACK") ||
            entry.type == QStringLiteral("PS2_ROM_FILE") ||
            entry.type == QStringLiteral("MSI_STREAM") ||
            entry.type == QStringLiteral("MSG_ATTACHMENT") ||
            entry.type == QStringLiteral("SIEMENS_IMG_FILE") ||
            entry.type == QStringLiteral("SIEMENS_FWF_FILE") ||
            entry.type == QStringLiteral("SIEMENS_FSF_FILE") ||
            entry.type == QStringLiteral("ISO_FILE") ||
            entry.type == QStringLiteral("WIM_FILE") ||
            entry.type == QStringLiteral("TIBX_VOLUME") ||
            entry.type == QStringLiteral("TIBX_FILE") ||
            entry.type == QStringLiteral("FAT_FILE") ||
            entry.type == QStringLiteral("UDF_FILE") ||
            entry.type == QStringLiteral("UFS_FILE") ||
            entry.type == QStringLiteral("EXFAT_FILE") ||
            entry.type == QStringLiteral("EXT_FILE") ||
            entry.type == QStringLiteral("NTFS_FILE") ||
            entry.type == QStringLiteral("XFS_FILE") ||
            entry.type == QStringLiteral("JFS_FILE") ||
            entry.type == QStringLiteral("HPFS_FILE") ||
            entry.type == QStringLiteral("SQUASHFS_FILE") ||
            entry.type == QStringLiteral("CRAMFS_FILE") ||
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
