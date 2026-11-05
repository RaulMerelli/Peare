#include "WinGdiSfntRenderer.h"

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <limits>

namespace peare {
namespace font {
namespace {

class GdiFontContext
{
public:
    GdiFontContext(const SfntFont& font, int scale)
    {
        if (font.sfntBytes.isEmpty() || font.sfntBytes.size() > 64 * 1024 * 1024 ||
            font.familyName.isEmpty())
            return;

        DWORD fontCount = 0;
        fontResource_ = AddFontMemResourceEx(
            const_cast<char*>(font.sfntBytes.constData()),
            DWORD(font.sfntBytes.size()), nullptr, &fontCount);
        if (!fontResource_ || fontCount == 0)
            return;

        dc_ = CreateCompatibleDC(nullptr);
        if (!dc_)
            return;

        const wchar_t* family = reinterpret_cast<const wchar_t*>(font.familyName.utf16());
        font_ = CreateFontW(-16 * scale, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, family);
        if (!font_)
            return;

        previousFont_ = SelectObject(dc_, font_);
        if (!previousFont_ || previousFont_ == HGDI_ERROR) {
            previousFont_ = nullptr;
            return;
        }
        valid_ = true;
    }

    ~GdiFontContext()
    {
        if (dc_ && previousFont_)
            SelectObject(dc_, previousFont_);
        if (font_)
            DeleteObject(font_);
        if (dc_)
            DeleteDC(dc_);
        if (fontResource_)
            RemoveFontMemResourceEx(fontResource_);
    }

    HDC dc() const { return dc_; }
    bool valid() const { return valid_; }

private:
    HANDLE fontResource_ = nullptr;
    HDC dc_ = nullptr;
    HFONT font_ = nullptr;
    HGDIOBJ previousFont_ = nullptr;
    bool valid_ = false;
};

QRgb blendPixel(QRgb foreground, QRgb background, int coverage)
{
    const double mask = double(qBound(0, coverage, 255)) / 255.0;
    const double frontAlpha = double(qAlpha(foreground)) / 255.0 * mask;
    const double backAlpha = double(qAlpha(background)) / 255.0 * (1.0 - mask);
    const double alpha = frontAlpha + backAlpha * (1.0 - frontAlpha);
    if (alpha <= 0.0)
        return qRgba(0, 0, 0, 0);

    const double backWeight = backAlpha * (1.0 - frontAlpha);
    const int red = qRound((qRed(foreground) * frontAlpha +
                             qRed(background) * backWeight) / alpha);
    const int green = qRound((qGreen(foreground) * frontAlpha +
                               qGreen(background) * backWeight) / alpha);
    const int blue = qRound((qBlue(foreground) * frontAlpha +
                              qBlue(background) * backWeight) / alpha);
    return qRgba(red, green, blue, qRound(alpha * 255.0));
}

QImage renderGlyph(GdiFontContext& context, quint32 glyphIndex, int scale, int padding,
                   QRgb foreground, QRgb background, QString* error)
{
    if (!context.valid() || scale < 1 || scale > 16 || padding < 0 || padding > 4096) {
        if (error) *error = QStringLiteral("Windows could not initialize the TrueType renderer.");
        return {};
    }

    MAT2 transform{};
    transform.eM11.value = 1;
    transform.eM22.value = 1;
    GLYPHMETRICS metrics{};
    const UINT format = GGO_GRAY8_BITMAP | GGO_GLYPH_INDEX;
    const DWORD byteCount = GetGlyphOutlineW(context.dc(), glyphIndex, format, &metrics,
                                              0, nullptr, &transform);
    if (byteCount == GDI_ERROR || metrics.gmBlackBoxX > 16384 ||
        metrics.gmBlackBoxY > 16384 || byteCount > 16u * 1024u * 1024u) {
        if (error) *error = QStringLiteral("Windows could not rasterize the TrueType glyph.");
        return {};
    }

    const int glyphWidth = int(metrics.gmBlackBoxX);
    const int glyphHeight = int(metrics.gmBlackBoxY);
    const int width = qMax(1, glyphWidth + 2 * padding);
    const int height = qMax(1, glyphHeight + 2 * padding);
    if (quint64(width) * quint64(height) > 64u * 1024u * 1024u) {
        if (error) *error = QStringLiteral("TrueType glyph image exceeds render limits.");
        return {};
    }

    QByteArray bitmap(int(byteCount), '\0');
    if (byteCount && GetGlyphOutlineW(context.dc(), glyphIndex, format, &metrics,
                                      byteCount, bitmap.data(), &transform) == GDI_ERROR) {
        if (error) *error = QStringLiteral("Windows could not read the TrueType glyph bitmap.");
        return {};
    }

    QImage image(width, height, QImage::Format_ARGB32);
    image.fill(background);
    const int rowBytes = (glyphWidth + 3) & ~3;
    for (int y = 0; y < glyphHeight; ++y) {
        for (int x = 0; x < glyphWidth; ++x) {
            const int offset = y * rowBytes + x;
            if (offset >= bitmap.size())
                break;
            const int gray = qMin(64, int(quint8(bitmap.at(offset))));
            image.setPixel(padding + x, padding + y,
                            blendPixel(foreground, background, gray * 255 / 64));
        }
    }
    return image;
}

} // namespace

QImage renderSfntTextWithGdi(const SfntFont& font, const QString& text, int scale,
                             int padding, QRgb foreground, QRgb background,
                             QString* error)
{
    if (scale < 1 || scale > 16 || padding < 0 || padding > 4096 || text.size() > 4096) {
        if (error) *error = QStringLiteral("Outline font render limits exceeded.");
        return {};
    }

    GdiFontContext context(font, scale);
    if (!context.valid()) {
        if (error) *error = QStringLiteral("Windows could not initialize the TrueType renderer.");
        return {};
    }

    const QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    TEXTMETRICW metrics{};
    if (!GetTextMetricsW(context.dc(), &metrics)) {
        if (error) *error = QStringLiteral("Windows could not read TrueType font metrics.");
        return {};
    }
    const int lineHeight = qMax(1, int(metrics.tmHeight + metrics.tmExternalLeading));
    int widest = 0;
    for (const QString& line : lines) {
        SIZE size{};
        if (!GetTextExtentPoint32W(context.dc(),
                reinterpret_cast<LPCWSTR>(line.utf16()), line.size(), &size)) {
            if (error) *error = QStringLiteral("Windows could not measure TrueType text.");
            return {};
        }
        widest = qMax(widest, int(size.cx));
    }

    const quint64 imageWidth = quint64(qMax(1, widest)) + 2u * quint64(padding);
    const quint64 imageHeight = quint64(lineHeight) * quint64(lines.size()) +
                                2u * quint64(padding);
    if (imageWidth > 16384 || imageHeight > 16384 ||
        imageWidth * imageHeight > 64u * 1024u * 1024u) {
        if (error) *error = QStringLiteral("Outline font render dimensions exceed limits.");
        return {};
    }

    const int width = int(imageWidth);
    const int height = int(imageHeight);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(context.dc(), &info, DIB_RGB_COLORS,
                                      &pixels, nullptr, 0);
    if (!bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        if (error) *error = QStringLiteral("Windows could not allocate the TrueType preview.");
        return {};
    }
    HGDIOBJ previousBitmap = SelectObject(context.dc(), bitmap);
    if (!previousBitmap || previousBitmap == HGDI_ERROR) {
        DeleteObject(bitmap);
        if (error) *error = QStringLiteral("Windows could not prepare the TrueType preview.");
        return {};
    }

    const int stride = width * 4;
    std::memset(pixels, 0, size_t(stride) * size_t(height));
    SetBkMode(context.dc(), TRANSPARENT);
    SetTextColor(context.dc(), RGB(255, 255, 255));
    for (int index = 0; index < lines.size(); ++index) {
        if (!TextOutW(context.dc(), padding, padding + index * lineHeight,
                      reinterpret_cast<LPCWSTR>(lines.at(index).utf16()),
                      lines.at(index).size())) {
            SelectObject(context.dc(), previousBitmap);
            DeleteObject(bitmap);
            if (error) *error = QStringLiteral("Windows could not draw TrueType text.");
            return {};
        }
    }
    GdiFlush();

    QImage image(width, height, QImage::Format_ARGB32);
    const auto* source = static_cast<const uchar*>(pixels);
    for (int y = 0; y < height; ++y) {
        const auto* row = reinterpret_cast<const QRgb*>(source + y * stride);
        for (int x = 0; x < width; ++x) {
            const QRgb pixel = row[x];
            const int coverage = qMax(qRed(pixel), qMax(qGreen(pixel), qBlue(pixel)));
            image.setPixel(x, y, blendPixel(foreground, background, coverage));
        }
    }
    SelectObject(context.dc(), previousBitmap);
    DeleteObject(bitmap);
    return image;
}

QImage renderSfntGlyphWithGdi(const SfntFont& font, quint32 glyphIndex, int scale,
                              int padding, QRgb foreground, QRgb background,
                              QString* error)
{
    if (glyphIndex >= font.glyphCount) {
        if (error) *error = QStringLiteral("Outline glyph render limits exceeded.");
        return {};
    }
    GdiFontContext context(font, scale);
    return renderGlyph(context, glyphIndex, scale, padding, foreground, background, error);
}

QVector<QImage> renderSfntGlyphsWithGdi(const SfntFont& font,
                                        const QVector<quint32>& glyphIndexes,
                                        int scale, int padding, QRgb foreground,
                                        QRgb background, QString* error)
{
    if (glyphIndexes.size() > 8192 || scale < 1 || scale > 16 ||
        padding < 0 || padding > 4096) {
        if (error) *error = QStringLiteral("Font glyph batch exceeds the render limit.");
        return {};
    }
    GdiFontContext context(font, scale);
    if (!context.valid()) {
        if (error) *error = QStringLiteral("Windows could not initialize the TrueType renderer.");
        return {};
    }
    QVector<QImage> images;
    images.reserve(glyphIndexes.size());
    quint64 totalPixels = 0;
    for (quint32 index : glyphIndexes) {
        QImage image = renderGlyph(context, index, scale, padding, foreground, background, error);
        if (image.isNull())
            return {};
        totalPixels += quint64(image.width()) * quint64(image.height());
        if (totalPixels > 16u * 1024u * 1024u) {
            if (error) *error = QStringLiteral("Font glyph batch exceeds the image memory limit.");
            return {};
        }
        images.push_back(std::move(image));
    }
    return images;
}

} // namespace font
} // namespace peare
#endif
