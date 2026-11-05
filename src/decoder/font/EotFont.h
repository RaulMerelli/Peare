#pragma once

#include <QByteArray>
#include <QString>

namespace peare {
namespace font {

struct EotFont {
    quint32 version = 0;
    quint32 flags = 0;
    QString familyName;
    QString styleName;
    QByteArray sfntBytes;
};

bool hasEotHeader(const QByteArray& data);
bool parseEot(const QByteArray& data, EotFont* font, QString* error = nullptr);

} // namespace font
} // namespace peare
