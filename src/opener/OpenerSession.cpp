#include "OpenerSession.h"
#include "modules/Compat.h"

#include "modules/ModuleFactory.h"

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
        return ResourcePlatform::Os2;
    case ModuleFormat::SZDD:
    case ModuleFormat::SIEMENS_IMG:
    case ModuleFormat::SIEMENS_FWF:
        return ResourcePlatform::Other;
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

bool OpenerSession::openStore(const peare::fs::ByteStorePtr& store, const QString& sourceName)
{
    close();
    return adoptModule(ModuleFactory::open(store, sourceName), sourceName);
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
    // Nested containers are no longer expanded eagerly here. Every module exposes
    // only its own resources; a resource that is itself a container is opened on
    // demand by the consumer (peare_resource_get_source + peare_opener_open) when
    // it is navigated into. The is-container hint below marks such resources.
    // Cheap is-container hint: only for whole embedded files the module declared,
    // peek the first 4 KiB (from the lazy store or the array) and see whether a
    // known openable format is recognised. Sub-resources are skipped, so this
    // stays fast even for resource-heavy modules; ISO files read 4 KiB via mmap.
    for (ResourceEntry& entry : resources_) {
        entry.isContainer = false;
        if (!entry.isEmbeddedFile)
            continue;
        QByteArray header;
        if (entry.content) {
            const std::vector<std::uint8_t> h = entry.content->readRange(0, 4096);
            header = QByteArray(reinterpret_cast<const char*>(h.data()), static_cast<int>(h.size()));
        } else {
            header = entry.data.left(4096);
        }
        const ModuleFormatInfo detected =
            header.isEmpty() ? ModuleFormatInfo{} : ModuleFormatDetector::detectBuffer(header);
        entry.isContainer = detected.format != ModuleFormat::Unknown;
        if (entry.isContainer)
            // Report the resource's *own* recognised format (e.g. PE) rather than
            // the containing module's (e.g. ISO 9660), so a selected nested file
            // is labelled by what it actually is.
            entry.format = detected.format;
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
            entry.type == QStringLiteral("OS2_PACK_FILE") ||
            entry.type == QStringLiteral("SZDD_FILE") ||
            entry.type == QStringLiteral("SIEMENS_IMG_FILE") ||
            entry.type == QStringLiteral("SIEMENS_FWF_FILE") ||
            entry.type == QStringLiteral("ISO_FILE") ||
            entry.type == QStringLiteral("WIM_FILE");
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
