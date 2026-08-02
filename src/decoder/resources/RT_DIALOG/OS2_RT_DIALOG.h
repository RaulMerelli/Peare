#pragma once

#include <QByteArray>
#include <QString>

namespace peare {
namespace resources {

class OS2_RT_DIALOG {
public:
    static QString Get(const QByteArray& data);
};

} // namespace resources
} // namespace peare
