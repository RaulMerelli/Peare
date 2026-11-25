#pragma once

#include "DiscStore.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace peare {
namespace fs {

enum class TibxMetadataDomain { Partition, Disk };

struct TibxMetadataField {
    std::uint16_t tag;
    std::vector<std::uint8_t> bytes;
};

// All fields retain their serialized bytes. Domain does not imply that a
// particular field is consumed or has a particular width.
bool readTibxItemMetadata(const ByteStorePtr& source, TibxMetadataDomain domain,
                          std::vector<TibxMetadataField>* fields,
                          std::string* error = nullptr);

// Returns success with found=false for an absent tag. A present value is
// checked and padded only to the width requested by its actual consumer.
bool getTibxItemMetadataField(const std::vector<TibxMetadataField>& fields,
                              std::uint16_t tag, std::size_t width,
                              std::vector<std::uint8_t>* value, bool* found,
                              std::string* error = nullptr);

// A caller marks every mandatory field it actually handles; unknown optional
// fields do not prevent completion.
bool finishTibxItemMetadata(const std::vector<TibxMetadataField>& fields,
                            const std::vector<std::uint16_t>& handled,
                            std::string* error = nullptr);

} // namespace fs
} // namespace peare
