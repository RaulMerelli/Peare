#pragma once

#include "peare/peare.h"

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

namespace pearegui {

QString blobToString(const peare_blob& blob);
QString statusText(peare_status status);
QString containerName(peare_container_format format);
QString sanitizeFileName(QString value);
QString uniquePath(const QString& path);
QString originalExtension(const QString& type, const QByteArray& payload);

struct ResourceContext {
    peare_container_format containerFormat = PEARE_CONTAINER_UNKNOWN;
    peare_platform platform = PEARE_PLATFORM_UNKNOWN;
    QString sourceName;
    QString type;
    QString identifier;
    QString language;
    quint32 codepage = 0;
    quint64 dataOffset = 0;
    quint64 dataSize = 0;
};

class Resource final {
public:
    Resource() = default;
    explicit Resource(peare_resource_handle handle);
    ~Resource();
    Resource(Resource&& other) noexcept;
    Resource& operator=(Resource&& other) noexcept;
    Resource(const Resource&) = delete;
    Resource& operator=(const Resource&) = delete;

    bool isValid() const { return handle_ != nullptr; }
    peare_resource_handle handle() const { return handle_; }
    QByteArray payload() const;
    ResourceContext context() const;
    QStringList convertedExtensions() const;
    QVector<QByteArray> convert(const QString& extension) const;

private:
    peare_resource_handle handle_ = nullptr;
};

class Session final {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    bool openFile(const QString& path, QString* error = nullptr);
    bool openBuffer(const QByteArray& data, const QString& sourceName, QString* error = nullptr);
    void close();
    bool isOpen() const { return open_; }
    peare_container_format containerFormat() const;
    size_t folderCount() const;
    QString folderType(size_t folder) const;
    size_t resourceCount(size_t folder) const;
    Resource openResource(size_t folder, size_t resource) const;
    Resource findResource(const QString& type, const QString& identifier, const QString& language = {}) const;

private:
    peare_opener_handle handle_ = nullptr;
    bool open_ = false;
};

struct Preview {
    QVector<QImage> images;
    QVector<QByteArray> pngs;
    QVector<QByteArray> texts;
    QString text;
    QString error;
    bool rawDump = false;
};

Preview decode(const Resource& resource, const Session* session = nullptr);
bool getUnsigned(const Resource& resource, peare_info_id id, quint64* value,
                 const QString& resourceTypeOverride = {});
QImage renderFont(const Resource& resource, const QString& text,
                  quint32 scale, quint32 padding,
                  quint32 foregroundRgba, quint32 backgroundRgba,
                  const QString& resourceTypeOverride = {});

} // namespace pearegui
