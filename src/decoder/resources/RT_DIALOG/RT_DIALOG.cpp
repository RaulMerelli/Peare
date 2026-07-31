#include "RT_DIALOG.h"
#include "OS2_RT_DIALOG.h"

namespace peare {
namespace resources {

QString RT_DIALOG::Get(const QByteArray& data, ModuleFormat format, bool isOs2)
{
    if (((format == ModuleFormat::LE || format == ModuleFormat::NE) && isOs2) ||
        format == ModuleFormat::LX)
    {
        // Structure is different for OS/2
        return OS2_RT_DIALOG::Get(data);
    }
    return QString();
}

ResourcePreview RT_DIALOG::preview(const ResourceEntry& entry)
{
    ResourcePreview result;
    result.text = Get(entry.data, entry.format, entry.isOs2);
    if (result.text.startsWith(QStringLiteral("Error decoding OS/2 dialog:"))) {
        result.error = result.text;
        result.text.clear();
    }
    return result;
}

} // namespace resources
} // namespace peare
