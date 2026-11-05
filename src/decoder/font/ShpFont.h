#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVector>

namespace peare {
namespace font {

struct ShpGlyph {
    quint16 code = 0;
    QString name;
    QVector<quint16> instructions;
};

struct ShpFont {
    QString name;
    bool compiled = false;
    bool bigFont = false;
    quint8 above = 0;
    quint8 below = 0;
    quint8 modes = 0;
    bool unicode = false;
    quint8 encoding = 0;
    quint8 embeddingType = 0;
    QVector<ShpGlyph> glyphs;
    QHash<quint32, quint16> glyphByUnicode;
};

bool hasShpFontHeader(const QByteArray& data);
bool parseShpFont(const QByteArray& data, ShpFont* font, QString* error = nullptr);
bool mapPackedShpUnicode(ShpFont* font, quint32 codePage, QString* error = nullptr);

} // namespace font
} // namespace peare
