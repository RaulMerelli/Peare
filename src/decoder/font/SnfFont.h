#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>
#include <QtGlobal>

namespace peare {
namespace font {

struct SnfGlyphMetric {
    quint32 code = 0;
    qint16 leftBearing = 0;
    qint16 rightBearing = 0;
    qint16 advance = 0;
    qint16 ascent = 0;
    qint16 descent = 0;
};

struct SnfFont {
    bool littleEndian = true;
    quint32 firstCode = 0;
    quint32 lastCode = 0;
    qint32 fontAscent = 0;
    qint32 fontDescent = 0;
    quint32 propertyCount = 0;
    quint32 glyphSlotCount = 0;
    QString name;
    QVector<SnfGlyphMetric> glyphs;
};

bool hasSnfHeader(const QByteArray& data);
bool parseSnf(const QByteArray& data, SnfFont* font, QString* error = nullptr);

} // namespace font
} // namespace peare
