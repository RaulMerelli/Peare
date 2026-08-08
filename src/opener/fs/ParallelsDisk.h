#pragma once
#include <string>
#include "DiscStore.h"
namespace peare { namespace fs {
ByteStorePtr openParallelsDisk(const ByteStorePtr& file, std::string* error);
} }
