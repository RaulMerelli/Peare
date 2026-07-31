#pragma once

#include "../opener/modules/Module.h"
#include "../opener/OpenerSession.h"

namespace peare {

enum class ResourceKind {
    Unknown,
    Icon,
    Cursor,
    GroupIcon,
    GroupCursor,
    Bitmap,
    Pointer,
    StringTable,
    DialogInclude,
    MessageTable,
    Version,
    Accelerator,
    AccelTable,
    FontDirectory,
    Font,
    DisplayInfo,
    HelpTable,
    HelpSubTable,
    NameTable,
    Menu,
    Dialog,
    XbeLogoRle,
    Fmim
};

class DecoderRoute final {
public:
    static ResourceKind classify(const ResourceEntry& entry) noexcept;
    static ResourceKind classify(const OpenedResource& resource) noexcept;
};

} // namespace peare
