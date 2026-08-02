#pragma once

#include "opener/modules/Module.h"

namespace peare {
namespace resources {

class OS2_RT_MENU {
public:
    static QString Get(const QByteArray& data, const ResourceEntry& entry);

private:
    static quint8 ReadByte(const QByteArray& data, qsizetype& offset);
    static quint16 ReadUInt16(const QByteArray& data, qsizetype& offset);
    static void ParseMenu(const QByteArray& data, qsizetype& offset, QString& output,
                          int indentLevel, bool isSubMenu = false);
};

} // namespace resources
} // namespace peare
