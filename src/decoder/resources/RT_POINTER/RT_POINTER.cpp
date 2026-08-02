#include "RT_POINTER.h"

#include "../RT_BITMAP/RT_BITMAP.h"

namespace peare {
namespace resources {

QVector<QImage> RT_POINTER::Get(const QByteArray& resData)
{
    // RT_BITMAP is already fully able to handle everything a RT_POINTER may have.
    return RT_BITMAP::get(resData);
}

ResourcePreview RT_POINTER::preview(const ResourceEntry& entry)
{
    ResourcePreview result;
    result.images = Get(entry.data);
    if (!result.images.isEmpty())
        result.image = result.images.first();
    return result;
}

} // namespace resources
} // namespace peare
