#pragma once

#include <QByteArray>

#include "PeModuleBase.h"

namespace peare {

// A PE stored as a normal file. RVAs are translated through the section table
// and PointerToRawData.
class PeFileModule final : public PeModuleBase {
public:
    static std::unique_ptr<PeFileModule> open(const QString& filePath);
    static std::unique_ptr<PeFileModule> open(const QByteArray& fileData, const QString& logicalName);
};

} // namespace peare
