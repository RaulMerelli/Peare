#pragma once

#include "PeModuleBase.h"

namespace peare {

// A PE image already laid out as it would be in virtual memory. RVAs are direct
// offsets from the image base and must not be translated through raw sections.
class PeImageModule final : public PeModuleBase {
public:
    static std::unique_ptr<PeImageModule> open(const QByteArray& image,
                                               const QString& logicalName = QString());
    static std::unique_ptr<PeImageModule> openFile(const QString& filePath);
};

} // namespace peare
