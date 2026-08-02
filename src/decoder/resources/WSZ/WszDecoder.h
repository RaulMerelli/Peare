#pragma once

#include <QByteArray>
#include <QString>

namespace peare {
namespace resources {

// WSZ structure based on the format research by Tim De Baets:
// https://github.com/tdebaets/wmp-wsz-format
// This C++/Qt implementation was written for Peare and does not depend on WMP,
// its COM type library, or registry entries.
class WszDecoder final
{
public:
    static bool TryDecode(const QByteArray& data, QString& decoded);

private:
    static bool LooksLikeWsz(const QByteArray& data);
};

} // namespace resources
} // namespace peare
