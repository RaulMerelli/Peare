#include "FloppyImage.h"

namespace peare {
namespace fs {

FloppyGeometry floppyGeometryForCapacity(std::int64_t capacity)
{
    struct KnownGeometry {
        std::int64_t capacity;
        int cylinders;
        int heads;
        int sectorsPerTrack;
        const char* nominalSize;
    };
    static const KnownGeometry known[] = {
        {  163840, 40, 1,  8, "160 KiB" },
        {  184320, 40, 1,  9, "180 KiB" },
        {  327680, 40, 2,  8, "320 KiB" },
        {  368640, 40, 2,  9, "360 KiB" },
        {  409600, 40, 2, 10, "400 KiB" },
        {  655360, 80, 2,  8, "640 KiB" },
        {  737280, 80, 2,  9, "720 KiB" },
        {  819200, 80, 2, 10, "800 KiB" },
        { 1228800, 80, 2, 15, "1.2 MiB" },
        { 1474560, 80, 2, 18, "1.44 MiB" },
        { 1720320, 80, 2, 21, "1.68 MiB (DMF)" },
        { 2949120, 80, 2, 36, "2.88 MiB" }
    };

    for (const KnownGeometry& item : known) {
        if (item.capacity == capacity) {
            FloppyGeometry result;
            result.capacity = item.capacity;
            result.cylinders = item.cylinders;
            result.heads = item.heads;
            result.sectorsPerTrack = item.sectorsPerTrack;
            result.nominalSize = item.nominalSize;
            return result;
        }
    }
    return {};
}

} // namespace fs
} // namespace peare
