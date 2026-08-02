#pragma once

#include "../Module.h"

#include <QByteArray>
#include <QString>
#include <QVector>

namespace peare {
namespace stfs {

struct ParsedFile {
    QString path;
    quint64 dataOffset = 0;
    QByteArray data;
};

struct ParsedContainer {
    bool valid = false;
    QString signature;
    QString description;
    QString metadataText;
    QVector<ParsedFile> files;
    QVector<ParsedFile> icons;
    QString error;
};

ParsedContainer parse(const QByteArray& data, const QString& expectedSignature = {});
void populateResources(const ParsedContainer& parsed,
                       const QByteArray& originalData,
                       const QString& logicalName,
                       ModuleFormat format,
                       QVector<ResourceEntry>* resources);

} // namespace stfs
} // namespace peare
