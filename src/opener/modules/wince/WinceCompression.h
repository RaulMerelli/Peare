#pragma once

// Ported from the MIT-licensed CERF project by Yaroslav Kibysh (gweslab).
// See CERF-LICENSE.txt in this directory.

#include <QByteArray>
#include <QtGlobal>

namespace peare {
namespace wince {

QByteArray decompressCe1Lzw(const QByteArray& source, quint32 outputSize);
QByteArray decompressCe3Bin(const QByteArray& source, quint32 outputSize);
QByteArray decompressCeLzx(const QByteArray& source, quint32 outputSize);
QByteArray decompressCeRom(const QByteArray& source, quint32 outputSize);
QByteArray decompressImgfsXpress(const QByteArray& source, quint32 outputSize);

} // namespace wince
} // namespace peare
