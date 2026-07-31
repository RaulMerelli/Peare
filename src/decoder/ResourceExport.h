#pragma once

#include "DecoderTypes.h"

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QImage>

namespace peare {

struct ResourceFileFormat {
    QString extension;
    QString description;
    QString mimeType;
};

struct ConvertedResourceFile {
    QString fileName;
    QByteArray data;
};

class ResourceConversion final {
public:
    static QStringList availableExtensions(const ResourcePreview& preview);
    static QVector<ConvertedResourceFile> convert(const ResourcePreview& preview,
                                                   const QString& extension,
                                                   const QString& baseName);
};

class ResourceFormatDetector final {
public:
    static ResourceFileFormat detect(const QString& resourceType, const QByteArray& data);
};

QString sanitizeFileName(const QString& value);
QString uniquePath(const QString& path);

} // namespace peare
