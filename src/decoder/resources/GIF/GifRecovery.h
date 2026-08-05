#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>

namespace peare {
namespace resources {

class GifRecovery final {
public:
    static bool LooksLike(const QByteArray& data) noexcept;
    static bool TryDecode(const QByteArray& data, QImage& image,
                          QString* description = nullptr) noexcept;
};

} // namespace resources
} // namespace peare
