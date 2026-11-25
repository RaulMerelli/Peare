#pragma once

#include "TibFileIndex.h"
#include <array>
#include <memory>

namespace peare {
namespace fs {

struct TibFileItem {
    std::u16string path;
    bool directory;
    bool deleted;
    std::uint64_t itemOffset;
    std::uint64_t logicalSize;
    bool providerRequired;
};

bool parseTibFileItem(const TibFileIndexEntry& entry, const TibFileMetadataVersion& metadataVersion,
                      TibFileItem* item, std::string* error);
struct TibFileStream {
    std::u16string name;
    ByteStorePtr content;
};

class TibDedupProvider {
public:
    virtual ~TibDedupProvider() {}
    virtual bool contains(const std::array<std::uint8_t, 16>& hash) const = 0;
    virtual bool read(const std::array<std::uint8_t, 16>& hash,
                      std::vector<std::uint8_t>* data) const = 0;
};

std::shared_ptr<TibDedupProvider> openTibDedupProvider(
    const std::vector<std::pair<ByteStorePtr, std::int64_t> >& stores,
    std::string* error, const std::vector<std::uint8_t>& key = std::vector<std::uint8_t>());

bool openTibFileStreams(const ByteStorePtr& source, std::uint64_t dataOffset,
                        std::uint64_t itemOffset, std::uint64_t logicalSize,
                        const std::vector<std::uint64_t>& itemOffsets,
                        std::vector<TibFileStream>* streams, std::string* error,
                        const std::shared_ptr<TibDedupProvider>& provider = std::shared_ptr<TibDedupProvider>());

} // namespace fs
} // namespace peare
