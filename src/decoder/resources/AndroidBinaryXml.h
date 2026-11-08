#pragma once

#include "../DecoderTypes.h"
#include "../../opener/modules/Module.h"

namespace peare { namespace resources {

bool recognizesAndroidBinaryXml(const QByteArray& data) noexcept;
ResourcePreview decodeAndroidBinaryXml(const ResourceEntry& entry) noexcept;

} }
