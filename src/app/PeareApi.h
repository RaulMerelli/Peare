#pragma once

#include "peare/peare.h"

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QStringList>
#include <QVector>

#include <memory>

namespace pearegui {

QString blobToString(const peare_blob& blob);
QString statusText(peare_status status);
QString containerName(peare_container_format format);
QString sanitizeFileName(QString value);
QString uniquePath(const QString& path);
QString originalExtension(const QString& type, const QByteArray& payload,
                          const QString& identifier = {});

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
    qint64 baseId = -1;
    bool isContainer = false;

    ResourceContext() = default;
    ResourceContext(peare_container_format format, peare_platform resourcePlatform,
                    const QString& resourceSource, const QString& resourceType,
                    const QString& resourceIdentifier, const QString& resourceLanguage,
                    quint32 resourceCodepage, quint64 offset, quint64 size,
                    bool container)
        : containerFormat(format), platform(resourcePlatform), sourceName(resourceSource),
          type(resourceType), identifier(resourceIdentifier), language(resourceLanguage),
          codepage(resourceCodepage), dataOffset(offset), dataSize(size),
          isContainer(container) {}
};

class Session;

class Resource final {
public:
    Resource() = default;
    explicit Resource(peare_resource_handle handle);
    Resource(QByteArray payload, ResourceContext context);
    ~Resource();
    Resource(Resource&& other) noexcept;
    Resource& operator=(Resource&& other) noexcept;
    Resource(const Resource&) = delete;
    Resource& operator=(const Resource&) = delete;

    bool isValid() const { return handle_ != nullptr || rawValid_; }
    peare_resource_handle handle() const { return handle_; }
    QByteArray payload() const;
    ResourceContext context() const;
    QStringList convertedExtensions(const Session* session = nullptr) const;
    QVector<QByteArray> convert(const QString& extension, const Session* session = nullptr) const;

private:
    peare_resource_handle handle_ = nullptr;
    QByteArray rawPayload_;
    ResourceContext rawContext_;
    bool rawValid_ = false;
};

class Session final {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    bool openFile(const QString& path, QString* error = nullptr,
                  const QString& password = QString());
    void close();
    bool isOpen() const { return open_; }
    peare_status lastStatus() const { return lastStatus_; }
    peare_container_format containerFormat() const;
    QString description() const;
    QString password() const { return password_; }
    size_t folderCount() const;
    QString folderType(size_t folder) const;
    size_t resourceCount(size_t folder) const;
    Resource openResource(size_t folder, size_t resource) const;
    Resource findResource(const QString& type, const QString& identifier, const QString& language = {}) const;

    // Open the content of one of this session's resources as a nested child
    // session (a container inside a container). Returns null if it does not open
    // as a navigable container. Uses the unified source+open path.
    std::unique_ptr<Session> openNested(size_t folder, size_t resource) const;

private:
    peare_opener_handle handle_ = nullptr;
    bool open_ = false;
    peare_status lastStatus_ = PEARE_STATUS_NOT_OPEN;
    QString password_;
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
QImage renderFontGlyph(const Resource& resource, quint32 glyphIndex,
                       quint32 scale, quint32 padding,
                       quint32 foregroundRgba, quint32 backgroundRgba,
                       const QString& resourceTypeOverride = {});
QVector<QImage> renderFontGlyphs(const Resource& resource, const QVector<quint32>& glyphIndexes,
                                 quint32 scale, quint32 padding, quint32 foregroundRgba,
                                 quint32 backgroundRgba,
                                 const QString& resourceTypeOverride = {});
QVector<QByteArray> fontCollectionFaces(const Resource& resource);
bool fontSupportsText(const Resource& resource, const QString& text, bool* supported,
                      const QString& resourceTypeOverride = {});
struct FontCodepoints {
    QVector<quint32> values;
    bool usesGlyphIndexes = false;
};
FontCodepoints fontCodepoints(const Resource& resource,
                              const QString& resourceTypeOverride = {});

} // namespace pearegui
