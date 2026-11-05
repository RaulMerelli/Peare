#pragma once

#include <QByteArray>
#include <QVector>
#include <QString>

namespace peare {
namespace font {

enum class SfntFormat { TrueType, OpenType, Woff, Woff2 };

struct SfntFont {
    SfntFormat format = SfntFormat::TrueType;
    QString familyName;
    QString styleName;
    quint16 unitsPerEm = 0;
    quint16 glyphCount = 0;
    qint16 ascent = 0;
    qint16 descent = 0;
    quint16 averageAdvance = 0;
    quint16 maximumAdvance = 0;
    bool fixedWidth = false;
    bool hasCff2 = false;
    bool hasTrueTypeOutlines = false;
    QVector<quint32> characterMap;
    QByteArray sfntBytes;
};

bool identifySfnt(const QByteArray& data, SfntFormat* format);
bool hasTtcHeader(const QByteArray& data);
bool parseTtcCollection(const QByteArray& data, QVector<SfntFont>* fonts,
                        QString* error = nullptr);
bool parseSfnt(const QByteArray& data, SfntFont* font, QString* error = nullptr);

} // namespace font
} // namespace peare
