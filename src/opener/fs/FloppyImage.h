#pragma once

#include <cstdint>
#include <string>

namespace peare {
namespace fs {

struct FloppyGeometry {
    std::int64_t capacity = 0;
    int cylinders = 0;
    int heads = 0;
    int sectorsPerTrack = 0;
    int bytesPerSector = 512;
    std::string nominalSize;

    bool valid() const noexcept {
        return capacity > 0 && cylinders > 0 && heads > 0 &&
               sectorsPerTrack > 0 && bytesPerSector > 0;
    }
};

// Recognises common raw PC-compatible floppy image capacities. These formats
// store sectors consecutively and carry no separate geometry header.
FloppyGeometry floppyGeometryForCapacity(std::int64_t capacity);

} // namespace fs
} // namespace peare
