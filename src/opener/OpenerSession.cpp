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


QStringList normalizedEmbeddedPath(const ResourceEntry& parent,
                                   const ResourceEntry& child)
{
    QStringList prefix = parent.hierarchyPath;
    if (prefix.isEmpty() || prefix.last() != parent.name)
        prefix.push_back(parent.name);

    QStringList suffix = child.hierarchyPath;
    while (!suffix.isEmpty() && suffix.first() == parent.name)
        suffix.removeFirst();
    prefix.append(suffix);
    return prefix;
}

bool isStructuralEntry(const ResourceEntry& entry)
{
    // Header/area entries are byte ranges belonging to the current module, not
    // independent embedded files. Synthetic whole-container roots are likewise
    // navigation nodes and must not reopen the file below itself.
    return entry.type.endsWith(QStringLiteral("_HEADERS")) ||
           entry.type.endsWith(QStringLiteral("_AREA")) ||
           entry.type == QStringLiteral("XUIZ_CONTAINER") ||
           entry.type == QStringLiteral("STFS_CONTAINER");
}

ModuleFormat appendEmbeddedResources(QVector<ResourceEntry>& destination,
                                     const ResourceEntry& parent,
                                     int depth,
                                     QSet<QByteArray>& ancestry)
{
    if (depth >= 8 || parent.data.isEmpty() || isStructuralEntry(parent))
        return ModuleFormat::Unknown;

    const QByteArray digest = QCryptographicHash::hash(parent.data, QCryptographicHash::Sha256);
    if (ancestry.contains(digest))
        return ModuleFormat::Unknown;

    ModulePtr embedded = ModuleFactory::open(parent.data, parent.name);
    if (!embedded || !embedded->info().isValid())
        return ModuleFormat::Unknown;

    const ModuleFormat currentFormat = embedded->info().format;
    const auto* container = dynamic_cast<const IResourceContainer*>(embedded.get());
    if (!container)
        return currentFormat;

    ancestry.insert(digest);
    const QVector<ResourceEntry>& children = container->resources();
    for (const ResourceEntry& source : children) {
        // Do not reproduce a module's synthetic self-wrapper below the file node.
        if (source.data == parent.data &&
            (source.name == parent.name || source.type.endsWith(QStringLiteral("_MODULE")) ||
             source.type.endsWith(QStringLiteral("_CONTAINER"))))
            continue;

        ResourceEntry child = source;
        child.hierarchyPath = normalizedEmbeddedPath(parent, source);
        const int childIndex = destination.size();
        destination.push_back(child);
        const ModuleFormat childFormat =
            appendEmbeddedResources(destination, child, depth + 1, ancestry);
        if (childFormat != ModuleFormat::Unknown)
            destination[childIndex].format = childFormat;
    }
    ancestry.remove(digest);
    return currentFormat;
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
    if (entry->content) {
        // Layer-backed: materialise this resource's bytes on demand, now that it
        // is actually being opened (never at enumeration). The common ABI sees
        // an ordinary payload and cannot tell it came from a lazy fs stack.
        const std::vector<std::uint8_t> bytes = entry->content->readAll();
        resource.payload = QByteArray(reinterpret_cast<const char*>(bytes.data()),
                                      static_cast<int>(bytes.size()));
    } else {
        resource.payload = entry->data;
    }
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
    if (resourceContainer_) {
        const QVector<ResourceEntry>& original = resourceContainer_->resources();
        resources_.reserve(original.size());
        for (const ResourceEntry& entry : original) {
            const int entryIndex = resources_.size();
            resources_.push_back(entry);
            QSet<QByteArray> ancestry;
            const ModuleFormat currentFormat =
                appendEmbeddedResources(resources_, entry, 0, ancestry);
            if (currentFormat != ModuleFormat::Unknown)
                resources_[entryIndex].format = currentFormat;
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
            entry.type == QStringLiteral("ISO_FILE");
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
