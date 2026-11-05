#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>
#include <QVector>

namespace peare {
namespace font {

struct MacFondAssociation {
    quint16 pointSize = 0;
    quint16 style = 0;
    qint16 resourceId = 0;
};

struct MacFondFont {
    quint16 flags = 0;
    quint16 familyId = 0;
    quint16 firstCharacter = 0;
    quint16 lastCharacter = 0;
    quint16 version = 0;
    int widthTableCount = 0;
    QVector<MacFondAssociation> associations;
};

bool hasMacFondHeader(const QByteArray& data);
bool parseMacFond(const QByteArray& data, MacFondFont* font, QString* error = nullptr);

} // namespace font
} // namespace peare
