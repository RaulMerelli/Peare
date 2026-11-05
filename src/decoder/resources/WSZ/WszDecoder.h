#pragma once

#include <QByteArray>
#include <QString>

namespace peare {
namespace resources {

// WSZ structure reference: https://github.com/tdebaets/wmp-wsz-format.
// The decoder does not depend on Windows Media Player or its registry data.
class WszDecoder final
{
public:
    static bool TryDecode(const QByteArray& data, QString& decoded);

private:
    static bool LooksLikeWsz(const QByteArray& data);
};

} // namespace resources
} // namespace peare
