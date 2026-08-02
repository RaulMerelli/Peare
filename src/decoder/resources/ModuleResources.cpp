#include "ModuleResources.h"
#include "RT_ICON/RT_ICON.h"
#include "RT_CURSOR/RT_CURSOR.h"
#include "RT_GROUP_ICON/RT_GROUP_ICON.h"
#include "RT_GROUP_CURSOR/RT_GROUP_CURSOR.h"
#include "RT_BITMAP/RT_BITMAP.h"
#include "RT_POINTER/RT_POINTER.h"
#include "RT_STRING/RT_STRING.h"
#include "RT_DLGINCLUDE/RT_DLGINCLUDE.h"
#include "RT_MESSAGE/RT_MESSAGE.h"
#include "RT_VERSION/RT_VERSION.h"
#include "RT_ACCELERATOR/RT_ACCELERATOR.h"
#include "RT_ACCELTABLE/RT_ACCELTABLE.h"
#include "RT_FONTDIR/RT_FONTDIR.h"
#include "RT_FONT/RT_FONT.h"
#include "RT_DISPLAYINFO/RT_DISPLAYINFO.h"
#include "RT_HELPTABLE/RT_HELPTABLE.h"
#include "RT_HELPSUBTABLE/RT_HELPSUBTABLE.h"
#include "RT_NAMETABLE/RT_NAMETABLE.h"
#include "RT_MENU/RT_MENU.h"
#include "RT_DIALOG/RT_DIALOG.h"
#include "XBE_LOGO_RLE/XBE_LOGO_RLE.h"
#include "FMIM/FMIM.h"
#include "RawDetect.h"
#include "../DecoderRoute.h"

namespace peare { namespace resources {

ResourcePreview ModuleResources::preview(const ResourceEntry& entry,
                                         const IResourceResolver& resolver)
{
    ResourcePreview declared;
    bool attemptedDeclaredDecoder = true;

    switch (DecoderRoute::classify(entry)) {
    case ResourceKind::Icon:
        declared = RT_ICON::preview(entry); break;
    case ResourceKind::Cursor:
        declared = RT_CURSOR::preview(entry); break;
    case ResourceKind::GroupIcon:
        declared = RT_GROUP_ICON::preview(entry, resolver); break;
    case ResourceKind::GroupCursor:
        declared = RT_GROUP_CURSOR::preview(entry, resolver); break;
    case ResourceKind::Bitmap:
        declared = RT_BITMAP::preview(entry); break;
    case ResourceKind::Pointer:
        declared = RT_POINTER::preview(entry); break;
    case ResourceKind::StringTable:
        declared = RT_STRING::preview(entry); break;
    case ResourceKind::DialogInclude:
        declared = RT_DLGINCLUDE::preview(entry); break;
    case ResourceKind::MessageTable:
        declared = RT_MESSAGE::preview(entry); break;
    case ResourceKind::Version:
        declared = RT_VERSION::preview(entry); break;
    case ResourceKind::Accelerator:
        declared = RT_ACCELERATOR::preview(entry); break;
    case ResourceKind::AccelTable:
        declared = RT_ACCELTABLE::preview(entry); break;
    case ResourceKind::FontDirectory:
        declared = RT_FONTDIR::preview(entry); break;
    case ResourceKind::Font:
        declared = RT_FONT::preview(entry); break;
    case ResourceKind::DisplayInfo:
        declared = RT_DISPLAYINFO::preview(entry); break;
    case ResourceKind::HelpTable:
        declared = RT_HELPTABLE::preview(entry); break;
    case ResourceKind::HelpSubTable:
        declared = RT_HELPSUBTABLE::preview(entry); break;
    case ResourceKind::NameTable:
        declared = RT_NAMETABLE::preview(entry); break;
    case ResourceKind::Menu:
        declared = RT_MENU::preview(entry); break;
    case ResourceKind::Dialog:
        declared = RT_DIALOG::preview(entry); break;
    case ResourceKind::XbeLogoRle:
        declared = XBE_LOGO_RLE::preview(entry); break;
    case ResourceKind::Fmim:
        declared = FMIM::preview(entry); break;
    case ResourceKind::Unknown:
        attemptedDeclaredDecoder = false; break;
    }

    if (attemptedDeclaredDecoder &&
        (!declared.images.isEmpty() || !declared.text.isEmpty()) &&
        declared.error.isEmpty())
        return declared;

    // The declared type remains the first choice. If its decoder cannot
    // produce a valid result, continue with payload-based detection as
    // required by the context-first design.
    ResourcePreview detected = RawDetect::Get(entry);
    if (!detected.images.isEmpty() || !detected.text.isEmpty())
        return detected;

    return declared;
}

} }
