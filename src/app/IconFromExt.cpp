#include "IconFromExt.h"

#include <QDir>
#include <QFileIconProvider>
#include <QFileInfo>
#include <QImage>
#include <QPixmap>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace {
QString normalizedExtension(QString extension)
{
    extension = extension.trimmed();
    if (extension.startsWith(QLatin1Char('.')))
        extension.remove(0, 1);
    return extension;
}

#ifdef Q_OS_WIN
QIcon windowsShellIcon(const QString& path, DWORD attributes, bool useAttributes)
{
    SHFILEINFOW fileInfo{};
    UINT flags = SHGFI_ICON | SHGFI_SMALLICON;
    if (useAttributes)
        flags |= SHGFI_USEFILEATTRIBUTES;
    if (!SHGetFileInfoW(reinterpret_cast<LPCWSTR>(path.utf16()), attributes,
                        &fileInfo, sizeof(fileInfo), flags) || !fileInfo.hIcon)
        return {};

    ICONINFO iconInfo{};
    QImage image;
    if (GetIconInfo(fileInfo.hIcon, &iconInfo) && iconInfo.hbmColor) {
        BITMAP bitmap{};
        if (GetObjectW(iconInfo.hbmColor, sizeof(bitmap), &bitmap) == sizeof(bitmap)) {
            const int width = bitmap.bmWidth;
            const int height = bitmap.bmHeight < 0 ? -bitmap.bmHeight : bitmap.bmHeight;
            image = QImage(width, height, QImage::Format_ARGB32);
            BITMAPINFO bitmapInfo{};
            bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmapInfo.bmiHeader.biWidth = width;
            bitmapInfo.bmiHeader.biHeight = -height;
            bitmapInfo.bmiHeader.biPlanes = 1;
            bitmapInfo.bmiHeader.biBitCount = 32;
            bitmapInfo.bmiHeader.biCompression = BI_RGB;
            HDC screen = GetDC(nullptr);
            const int copied = screen ? GetDIBits(
                screen, iconInfo.hbmColor, 0, height, image.bits(), &bitmapInfo,
                DIB_RGB_COLORS) : 0;
            if (screen)
                ReleaseDC(nullptr, screen);
            if (copied != height)
                image = QImage();
            else {
                bool hasAlpha = false;
                for (int y = 0; y < height && !hasAlpha; ++y) {
                    const QRgb* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
                    for (int x = 0; x < width; ++x) {
                        if (qAlpha(row[x]) != 0) {
                            hasAlpha = true;
                            break;
                        }
                    }
                }
                if (!hasAlpha && iconInfo.hbmMask) {
                    const int maskStride = ((width + 31) / 32) * 4;
                    QByteArray mask(maskStride * height, '\0');
                    struct MonoBitmapInfo {
                        BITMAPINFOHEADER header;
                        RGBQUAD colors[2];
                    } maskInfo{};
                    maskInfo.header.biSize = sizeof(BITMAPINFOHEADER);
                    maskInfo.header.biWidth = width;
                    maskInfo.header.biHeight = -height;
                    maskInfo.header.biPlanes = 1;
                    maskInfo.header.biBitCount = 1;
                    maskInfo.header.biCompression = BI_RGB;
                    maskInfo.header.biClrUsed = 2;
                    screen = GetDC(nullptr);
                    const int maskCopied = screen ? GetDIBits(
                        screen, iconInfo.hbmMask, 0, height, mask.data(),
                        reinterpret_cast<BITMAPINFO*>(&maskInfo), DIB_RGB_COLORS) : 0;
                    if (screen)
                        ReleaseDC(nullptr, screen);
                    if (maskCopied == height) {
                        for (int y = 0; y < height; ++y) {
                            QRgb* row = reinterpret_cast<QRgb*>(image.scanLine(y));
                            const auto* bits = reinterpret_cast<const uchar*>(mask.constData()) +
                                y * maskStride;
                            for (int x = 0; x < width; ++x) {
                                const bool transparent = (bits[x / 8] & (0x80 >> (x % 8))) != 0;
                                row[x] = qRgba(qRed(row[x]), qGreen(row[x]), qBlue(row[x]),
                                               transparent ? 0 : 255);
                            }
                        }
                    } else {
                        image = QImage();
                    }
                }
            }
        }
    }
    if (iconInfo.hbmColor)
        DeleteObject(iconInfo.hbmColor);
    if (iconInfo.hbmMask)
        DeleteObject(iconInfo.hbmMask);
    DestroyIcon(fileInfo.hIcon);
    return image.isNull() ? QIcon() : QIcon(QPixmap::fromImage(image));
}
#endif
}

QIcon IconFromExt::get(const QString& extension)
{
    const QString suffix = normalizedExtension(extension);
#ifdef Q_OS_WIN
    if (!suffix.isEmpty()) {
        const QIcon icon = windowsShellIcon(QStringLiteral("file.%1").arg(suffix),
                                            FILE_ATTRIBUTE_NORMAL, true);
        if (!icon.isNull())
            return icon;
    }
#endif
    QFileIconProvider provider;
    if (suffix.isEmpty())
        return provider.icon(QFileIconProvider::File);

    // The file doesn't need to exist. QFileIconProvider uses its suffix to ask
    // the platform for the associated icon, just like SHGFI_USEFILEATTRIBUTES.
    return provider.icon(QFileInfo(QStringLiteral("file.%1").arg(suffix)));
}

QIcon IconFromExt::getFolder()
{
#ifdef Q_OS_WIN
    const QIcon icon = windowsShellIcon(QStringLiteral("folder"),
                                        FILE_ATTRIBUTE_DIRECTORY, true);
    if (!icon.isNull())
        return icon;
#endif
    QFileIconProvider provider;
    return provider.icon(QFileIconProvider::Folder);
}

QIcon IconFromExt::getFile(const QString& filePath)
{
#ifdef Q_OS_WIN
    const QIcon icon = windowsShellIcon(filePath, FILE_ATTRIBUTE_NORMAL, false);
    if (!icon.isNull())
        return icon;
#endif
    QFileIconProvider provider;
    return provider.icon(QFileInfo(filePath));
}
