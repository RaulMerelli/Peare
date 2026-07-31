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
    ResourceContext context;
    QString error;

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
