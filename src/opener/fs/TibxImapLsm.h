#pragma once

#include "TibxPageStore.h"

#include <list>
#include <memory>

namespace peare {
namespace fs {

// The page store must outlive this cursor. Do not use a cursor concurrently.
// A null lower bound starts at the first key.
class TibxImapLsm {
public:
    TibxImapLsm(const TibxPageStore& pages, const TibxPageStore::CommittedHeader& header,
                const TibxPageStore::Superblock& itemMap, TibxImapSort mode);

    bool next(const std::vector<std::uint8_t>* lower, bool inclusive,
              TibxPageStore::TreeCell* cell, bool* found, std::string* error) const;

private:
    struct PageData {
        bool leaf;
        std::vector<TibxPageStore::TreeCell> cells;
    };
    struct CachedPage {
        std::uint64_t offset;
        std::size_t size;
        std::shared_ptr<const PageData> data;
    };
    bool loadPage(std::uint64_t offset, std::shared_ptr<const PageData>* page,
                  std::string* error) const;

    const TibxPageStore& pages_;
    TibxPageStore::CommittedHeader header_;
    TibxPageStore::Superblock itemMap_;
    TibxImapSort mode_;
    mutable bool inlineReady_;
    mutable std::vector<TibxPageStore::TreeCell> inlineCells_;
    mutable std::size_t cachedBytes_;
    mutable std::list<CachedPage> cachedPages_;
};

}  // namespace fs
}  // namespace peare
