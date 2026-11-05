#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

namespace peare {
namespace font {

enum class Format { Bdf, Psf1, Psf2, Pcf, Fzx, Pk, Bmf, MacNfnt, Amiga, AmigaColor, AmigaAnim, Vfont, SunNews, Dwf };

struct KerningPair {
    quint32 left = 0;
    quint32 right = 0;
    int adjustment = 0;
};

struct Glyph {
    quint32 codepoint = 0xffffffffu;
    int width = 0;
    int height = 0;
    int bearingX = 0;
    int bearingY = 0;
    int advance = 0;
    QByteArray bitmap;
    QByteArray pixels;
    QByteArray mask;
    QVector<quint32> palette;
};

struct BitmapFont {
    Format format = Format::Bdf;
    QString name;
    int width = 0;
    int height = 0;
    int ascent = 0;
    int descent = 0;
    bool fixedWidth = false;
    int alphaBits = 0;
    int addSpace = 0;
    int foregroundPaletteIndex = -1;
    QVector<quint32> palette;
    QVector<Glyph> glyphs;
    QVector<KerningPair> kernings;
};

bool hasBdfHeader(const QByteArray& data);
bool hasPsfHeader(const QByteArray& data);
bool hasPcfHeader(const QByteArray& data);
bool hasFzxHeader(const QByteArray& data);
bool hasPkHeader(const QByteArray& data);
bool hasBmfHeader(const QByteArray& data);
bool hasVfontHeader(const QByteArray& data);
bool hasSunNewsFontHeader(const QByteArray& data);
bool parseMacBitmapSfnt(const QByteArray& data, BitmapFont* font);
bool parse(const QByteArray& data, BitmapFont* font, QString* error = nullptr);

} // namespace font
} // namespace peare
