#pragma once

#include "../../../decoder/DecoderTypes.h"
#include "opener/modules/ModuleFormat.h"

#include <QString>

namespace peare {
namespace resources {

class RT_STRING {
public:
    static quint8 ReadByte(const QByteArray& data, qsizetype& offset);
    static quint16 ReadUInt16(const QByteArray& data, qsizetype& offset);
    static QString ReadLenString(const QByteArray& data, qsizetype& offset, int codepage, int len);
    static QString ReadNullTerminatedString(const QByteArray& data, qsizetype& offset, int codepage);

    static QString Get(const QByteArray& data,
                       ModuleFormat headerType,
                       bool isOs2 = false,
                       int baseId = 100);
    static ResourcePreview preview(const ResourceEntry& entry);

private:
    static QString Escape(const QString& value);
};

} // namespace resources
} // namespace peare
