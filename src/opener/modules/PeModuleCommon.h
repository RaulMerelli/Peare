#pragma once

#include "Module.h"
#include "PeResources.h"

namespace peare {
namespace detail {

void appendPeResources(QVector<ResourceEntry>& destination,
                       const PeResourceResult& parsed);
void appendPeStructure(QVector<ResourceEntry>& destination,
                       const QByteArray& data, PeStorageLayout layout);

} // namespace detail
} // namespace peare
