#pragma once
#include "../../../decoder/DecoderTypes.h"
namespace peare { namespace resources {
class RT_CURSOR {
public:
    static QVector<QImage> decode(const QByteArray& data);
    static ResourcePreview preview(const ResourceEntry& entry);
};
} }
