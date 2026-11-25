#pragma once

#include "TibLegacyFileTree.h"

namespace peare {
namespace fs {

enum class TibLegacyChunkKind { Attributes1, Stream2, Stream4, Metadata5, Metadata6, Hash, End };

struct TibLegacyChunk {
    TibLegacyChunkKind kind;
    ByteStorePtr stream;
    TibLegacyChunkHash hash;
    std::vector<std::uint8_t> metadata;
    std::uint32_t attributes;
    bool directoryAttribute;
    std::uint32_t internalType;
    std::uint32_t structureSize;
};

// Positions are absolute within source; end is exclusive. Stream bytes are
// validated before being returned. Each stream retains the source independently.
class TibLegacyFileChunks {
public:
    TibLegacyFileChunks(ByteStorePtr source, std::int64_t record, std::int64_t end);
    bool next(TibLegacyChunk* chunk, std::string* error = nullptr);
private:
    ByteStorePtr source_;
    std::int64_t position_, end_;
    bool started_, ended_, pending_, failed_;
    TibLegacyChunkHash hash_;
};

// Selects the first type 2 or 4 stream. No stream is manufactured if absent.
bool openTibLegacyPrimaryStream(TibLegacyFileChunks* chunks, ByteStorePtr* stream,
                                std::string* error = nullptr);

enum class TibLegacyItemKind { File, Directory, CharacterDevice, BlockDevice, Fifo, Socket, Symlink };
struct TibLegacyItem {
    TibLegacyItemKind kind;
    ByteStorePtr stream;
    std::uint32_t deviceMajor;
    std::uint32_t deviceMinor;
    std::u16string linkTarget;
};
bool readTibLegacyItem(TibLegacyFileChunks* chunks, TibLegacyItem* item,
                       std::string* error = nullptr);

} // namespace fs
} // namespace peare
