#include "RT_DLGINCLUDE.h"

#include "../RT_STRING/RT_STRING.h"

#include <stdexcept>

namespace peare {
namespace resources {

QString RT_DLGINCLUDE::Get(const QByteArray& data)
{
    qsizetype offset = 0;
    return RT_STRING::ReadNullTerminatedString(data, offset, 850);
}

ResourcePreview RT_DLGINCLUDE::preview(const ResourceEntry& entry)
{
    ResourcePreview preview;

    try {
        preview.text = Get(entry.data);
    } catch (const std::exception& error) {
        preview.error = QString::fromUtf8(error.what());
    }

    return preview;
}

} // namespace resources
} // namespace peare
