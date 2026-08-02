#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

namespace peare {

enum class PeStorageLayout {
    File,
    LoadedImage
};

struct PeResourceEntry {
    QString type;
    QString name;
    QString language;
    quint32 dataRva = 0;
    quint32 size = 0;
    quint32 codePage = 0;
    QByteArray data;
};

struct PeResourceResult {
    QVector<PeResourceEntry> entries;
    QString error;
    bool isValid() const noexcept { return error.isEmpty(); }
};

class PeResources final {
public:
    static PeResourceResult listFile(const QString& filePath);
    static PeResourceResult listFile(const QByteArray& fileData);
    static PeResourceResult listImage(const QByteArray& loadedImage);

private:
    static PeResourceResult list(const QByteArray& data, PeStorageLayout layout);
};

} // namespace peare
