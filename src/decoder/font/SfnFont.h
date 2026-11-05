#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>
#include <QVector>

namespace peare {
namespace font {

enum class SfnFragmentType {
    Contour,
    Bitmap,
    Pixmap,
    Other
};

struct SfnPoint {
    quint8 x = 0;
    quint8 y = 0;
};

struct SfnContourCommand {
    quint8 type = 0;
    SfnPoint end;
    SfnPoint control1;
    SfnPoint control2;
};

struct SfnFragment {
    SfnFragmentType type = SfnFragmentType::Other;
    quint16 width = 0;
    quint16 height = 0;
    QVector<SfnContourCommand> commands;
    QByteArray pixels;
};

struct SfnLayer {
    quint8 x = 0;
    quint8 y = 0;
    quint32 fragment = 0;
    qint16 color = -1;
};

struct SfnGlyph {
    quint32 codepoint = 0;
    quint8 width = 0;
    quint8 height = 0;
    quint8 advanceX = 0;
    quint8 advanceY = 0;
    quint8 overlap = 0;
    QVector<SfnLayer> layers;
};

struct SfnColor {
    quint8 red = 0;
    quint8 green = 0;
    quint8 blue = 0;
    quint8 alpha = 0;
};

struct SfnFont {
    QString uniqueName;
    QString familyName;
    QString subfamilyName;
    QString version;
    QString designer;
    QString license;
    quint8 type = 0;
    quint8 width = 0;
    quint8 height = 0;
    quint8 baseline = 0;
    quint8 underline = 0;
    QVector<SfnColor> colors;
    QVector<SfnFragment> fragments;
    QVector<SfnGlyph> glyphs;
    QHash<quint32, int> glyphByCodepoint;
};

bool hasSfnHeader(const QByteArray& data);
bool parseSfnCollection(const QByteArray& data, QVector<QByteArray>* fonts,
                        QString* error = nullptr);
bool parseSfnFont(const QByteArray& data, SfnFont* font, QString* error = nullptr);

} // namespace font
} // namespace peare
