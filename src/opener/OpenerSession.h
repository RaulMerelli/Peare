#pragma once

#include "modules/Module.h"

#include <QByteArray>
#include <QString>
#include <QVector>
#include <cstddef>
#include <memory>

class QTemporaryFile;

namespace peare {

enum class ResourcePlatform {
    Unknown,
    Windows,
    Os2,
    Other
};

struct ResourceContext {
    QString sourceName;
    ModuleFormat containerFormat = ModuleFormat::Unknown;
    ResourcePlatform platform = ResourcePlatform::Unknown;
    QString type;
    QString identifier;
    QString language;
    quint32 codePage = 0;
    quint64 dataOffset = 0;
    quint64 dataSize = 0;
    int baseId = 0;
    int resourceIndex = -1;
    bool isContainer = false;  // a header peek recognised an openable format
};

struct ResourceFolder {
    QString type;
    QVector<int> resourceIndices;
};

class OpenerSession;

struct OpenedResource {
    const OpenerSession* session = nullptr;
    int resourceIndex = -1;
    QByteArray payload;
    // Layer-backed content, when the resource is served by the fs stack. It is
    // NOT read here: the bytes are materialised only when the payload is actually
    // requested (peare_resource_get_payload), so opening/enumerating a resource
    // never pays for its content.
    peare::fs::ByteStorePtr contentStore;
    ResourceContext context;
    QString error;
    // Directory-level container: contentStore holds the whole filesystem image and
    // subPath the directory within it. Navigating reopens the image at subPath
    // instead of decoding byte content.
    bool isDirectory = false;
    QString subPath;

    bool isValid() const noexcept { return resourceIndex >= 0 && error.isEmpty(); }
};

class OpenerSession final {
public:
    OpenerSession();
    ~OpenerSession();

    OpenerSession(const OpenerSession&) = delete;
    OpenerSession& operator=(const OpenerSession&) = delete;
    OpenerSession(OpenerSession&&) noexcept;
    OpenerSession& operator=(OpenerSession&&) noexcept;

    bool openFile(const QString& filePath);
    bool openBuffer(const QByteArray& data, const QString& sourceName = QStringLiteral("memory.bin"));
    // Open over a positioned byte source without materialising it (filesystem
    // formats read straight from the source).
    bool openStore(const peare::fs::ByteStorePtr& store, const QString& sourceName,
                   const QString& subPath = QString());
    void close();

    bool isOpen() const noexcept;
    const ModuleInfo& info() const noexcept;
    const QVector<ResourceFolder>& folders() const noexcept;
    bool hasResourceContainer() const noexcept;
    int resourceCount() const noexcept;

    const ResourceEntry* resourceEntry(int resourceIndex) const noexcept;
    OpenedResource openResource(int resourceIndex) const;
    OpenedResource openResource(const QString& folderType,
                                const QString& identifier,
                                const QString& preferredLanguage = {}) const;
    int findResource(const QString& folderType,
                     const QString& identifier,
                     const QString& preferredLanguage = {}) const noexcept;

private:
    bool adoptModule(ModulePtr module, const QString& displayPath);
    void rebuildFolders();

    ModulePtr module_;
    const IResourceContainer* resourceContainer_ = nullptr;
    QVector<ResourceEntry> resources_;
    ModuleInfo info_;
    QVector<ResourceFolder> folders_;
    std::unique_ptr<QTemporaryFile> bufferFile_;
};

QString resourcePlatformName(ResourcePlatform platform);

} // namespace peare
