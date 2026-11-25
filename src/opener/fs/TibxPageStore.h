#pragma once

#include "DiscStore.h"
#include "TibxImapComparator.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace peare {
namespace fs {

class TibxPageStore {
public:
    static const std::size_t kPageSize = 4096;
    typedef std::array<std::uint8_t, kPageSize> Page;

    struct CommittedHeader {
        std::uint64_t offset;
        std::uint64_t sequence;
        std::vector<std::uint8_t> data;
    };

    struct HeaderSection {
        std::size_t offset;
        std::size_t length;
    };

    struct CommitInfo {
        std::uint64_t archiveOffset;
        std::uint32_t archiveSize;
        std::uint64_t sequence;
        std::uint64_t previousOffset;
        std::uint64_t volumeOffset;
        std::uint32_t flags;
        std::array<std::uint8_t, 16> archiveId;
        std::array<std::uint32_t, 4> volumeIds;
        std::array<std::uint64_t, 4> volumeOffsets;
    };

    struct TreeLevel {
        std::uint64_t rootOffset;
        std::uint64_t pageCount;
        std::uint32_t itemCount;
    };

    struct Superblock {
        std::uint32_t keyLength;
        std::uint32_t valueLength;
        std::uint8_t inlineEncoding;
        std::uint16_t inlineCount;
        std::size_t inlineOffset;
        std::size_t inlineLength;
        std::vector<TreeLevel> levels;
    };

    struct TreeCell {
        std::vector<std::uint8_t> key;
        std::vector<std::uint8_t> value;
        bool deleted;
    };

    struct TreePage {
        bool leaf;
        std::uint16_t cellCount;
        std::vector<std::uint8_t> data;
    };

    struct Extent {
        std::uint64_t volumeId;
        std::uint64_t sourceOffset;
        std::uint32_t length;
        std::uint32_t sliceId;
        std::uint64_t extentId;
        std::uint64_t segmentId;
        std::uint16_t segmentIndex;
    };

    struct RecoveryPoint {
        std::uint32_t sliceId;
        std::uint32_t fullId;
        bool hidden;
        std::array<std::uint8_t, 16> id;
        std::uint64_t created;
        std::uint64_t finished;
    };

    struct StreamDescriptor {
        std::uint8_t selector;
        std::uint8_t flags;
        std::uint64_t id;
        std::uint64_t size;
    };

    struct ResolvedItem {
        std::string name;
        std::uint32_t sliceId;
        std::uint16_t flags;
        std::uint8_t type;
        bool descriptorsValid;
        std::vector<StreamDescriptor> streams;
    };

    struct Volume {
        std::uint64_t id;
        std::uint64_t size;
        std::string name;
        std::uint32_t sliceId;
    };

    struct FileItem {
        std::uint64_t id;
        std::uint64_t size;
        std::string name;
        bool directory;
        std::uint32_t sliceId;
    };

    explicit TibxPageStore(ByteStorePtr source);
    std::uint64_t pageCount() const;
    bool readPage(std::uint64_t index, Page* page, std::string* error, bool* ioError = nullptr) const;
    bool readCommitInfo(std::uint64_t index, CommitInfo* info, bool* ioError = nullptr) const;
    bool readHeaderAtCommit(std::uint64_t index, CommittedHeader* header, bool* ioError = nullptr) const;
    bool readCurrentHeader(CommittedHeader* header, bool* ioError = nullptr) const;
    bool readCommittedHeader(CommittedHeader* header, std::string* error, bool* ioError = nullptr) const;
    static bool readHeaderSections(const CommittedHeader& header,
                                   std::vector<HeaderSection>* sections,
                                   std::string* error);
    static bool readSuperblock(const CommittedHeader& header, const HeaderSection& section,
                               Superblock* superblock, std::string* error);
    bool decodeInlineTree(const CommittedHeader& header, const Superblock& superblock,
                          std::vector<std::uint8_t>* data, std::string* error) const;
    bool unlock(const CommittedHeader& header, const Superblock& keyMap,
                const std::string& password, std::string* error);
    bool unlockPrivateKey(const CommittedHeader& header, const Superblock& keyMap,
                          const std::string& privateKeyPem, std::string* error,
                          bool* resourceFailure = nullptr);
    static bool readInlineCells(const std::vector<std::uint8_t>& data,
                                const Superblock& superblock, std::vector<TreeCell>* cells,
                                std::string* error);
    bool readTreePage(std::uint64_t offset, TreePage* page, std::string* error) const;
    static bool readTreeCells(const TreePage& page, const Superblock& superblock,
                              std::vector<TreeCell>* cells, std::string* error,
                              const TibxImapSort* imapSort = nullptr);
    bool findTreeCell(const CommittedHeader& header, const Superblock& superblock,
                      const std::vector<std::uint8_t>& key, TreeCell* cell,
                      bool* found, std::string* error) const;
    bool readTree(const CommittedHeader& header, const Superblock& superblock,
                  std::vector<TreeCell>* cells, std::string* error) const;
    bool readTreePrefix(const CommittedHeader& header, const Superblock& superblock,
                        const std::vector<std::uint8_t>& prefix,
                        std::vector<TreeCell>* cells, std::string* error) const;
    bool findNextTreeCell(const CommittedHeader& header, const Superblock& superblock,
                          const std::vector<std::uint8_t>& lower,
                          TreeCell* cell, bool* found, std::string* error) const;
    bool readDirectoryItems(const CommittedHeader& header, const Superblock& itemMap,
                            std::uint32_t fullId, std::uint32_t sliceId, const std::string& path,
                            std::vector<Volume>* volumes, std::vector<FileItem>* files,
                            bool* exists, std::string* error) const;
    bool readResolvedItems(const CommittedHeader& header, const Superblock& itemMap,
                           std::uint32_t fullId, std::uint32_t sliceId,
                           std::vector<ResolvedItem>* items, std::string* error) const;
    bool readExtents(const CommittedHeader& header, const Superblock& dataMap, std::uint64_t volumeId,
                     std::vector<Extent>* extents, std::string* error) const;
    bool readRecoveryPoints(const CommittedHeader& header, const Superblock& sliceMap,
                            std::vector<RecoveryPoint>* points, std::string* error) const;
    bool readSegment(const CommittedHeader& header, const Superblock& segmentMap,
                     std::uint64_t segmentId,
                     std::shared_ptr<const std::vector<std::uint8_t>>* data,
                     std::string* error) const;
    bool readVolumeRange(const CommittedHeader& header, const Superblock& segmentMap,
                         const std::vector<Extent>& extents, std::uint64_t volumeId,
                         std::uint32_t itemSlice, std::uint64_t offset,
                         std::uint8_t* output, std::size_t count, std::string* error) const;

private:
    struct SegmentCache;
    bool readCommit(std::uint64_t index, CommittedHeader* header, bool requireCurrent = false,
                    bool* ioError = nullptr) const;
    ByteStorePtr source_;
    std::shared_ptr<SegmentCache> segmentCache_;
    std::vector<std::uint8_t> encryptionKey_;
    std::uint8_t encryptionAlgorithm_ = 0;
};

}  // namespace fs
}  // namespace peare
