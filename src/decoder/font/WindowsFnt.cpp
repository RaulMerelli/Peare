#include "WindowsFnt.h"

#include <limits>
#include <QtEndian>

namespace peare {
namespace font {
namespace {

quint16 readLe16(const QByteArray& data, int offset)
{
    return qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(data.constData() + offset));
}

quint32 readLe32(const QByteArray& data, int offset)
{
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(data.constData() + offset));
}

} // namespace

bool hasWindowsFntHeader(const QByteArray& data)
{
    if (data.size() < 117) return false;
    const quint16 version = readLe16(data, 0);
    const quint32 declaredSize = readLe32(data, 2);
    const quint16 headerSize = version == 0x0100 ? 117 : version == 0x0200 ? 118
                               : version == 0x0300 ? 148 : 0;
    if (!headerSize || declaredSize < headerSize || declaredSize > 64u * 1024u * 1024u ||
        data.size() < headerSize)
        return false;
    const quint16 type = readLe16(data, 66);
    const quint16 height = readLe16(data, 88);
    const quint8 first = quint8(data.at(95));
    const quint8 last = quint8(data.at(96));
    const quint32 faceOffset = readLe32(data, 105);
    const quint32 bitsOffset = readLe32(data, 113);
    return (type & ~quint16(1)) == 0 && height > 0 && height <= 4096 &&
           last >= first && faceOffset >= headerSize && faceOffset < declaredSize &&
           bitsOffset >= headerSize && bitsOffset < declaredSize;
}

} // namespace font
} // namespace peare
