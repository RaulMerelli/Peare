#pragma once

#include "../../../decoder/DecoderTypes.h"

#include <QVector>

namespace peare {
namespace resources {

class RT_POINTER {
public:
    static QVector<QImage> Get(const QByteArray& resData);
    static ResourcePreview preview(const ResourceEntry& entry);
};

} // namespace resources
} // namespace peare
