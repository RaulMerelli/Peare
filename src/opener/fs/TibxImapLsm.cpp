#include "TibxImapLsm.h"

#include <limits>
#include <set>
#include <utility>

namespace peare {
namespace fs {
namespace {

bool fail(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

std::uint64_t be64(const std::vector<std::uint8_t>& value) {
    std::uint64_t result = 0;
    for (std::uint8_t byte : value) result = (result << 8) | byte;
    return result;
}

bool compare(const std::vector<std::uint8_t>& a, const std::vector<std::uint8_t>& b,
             TibxImapSort mode, int* order, std::string* error) {
    return compareTibxImapKeys(a, b, mode, order) ||
           fail(error, "Invalid TIBX IMAP key");
}

}  // namespace

TibxImapLsm::TibxImapLsm(const TibxPageStore& pages,
                         const TibxPageStore::CommittedHeader& header,
                         const TibxPageStore::Superblock& itemMap, TibxImapSort mode)
    : pages_(pages), header_(header), itemMap_(itemMap), mode_(mode),
      inlineReady_(false), cachedBytes_(0) {}

bool TibxImapLsm::loadPage(std::uint64_t offset, std::shared_ptr<const PageData>* result,
                           std::string* error) const {
    for (auto it = cachedPages_.begin(); it != cachedPages_.end(); ++it) {
        if (it->offset == offset) {
            *result = it->data;
            cachedPages_.splice(cachedPages_.begin(), cachedPages_, it);
            return true;
        }
    }
    TibxPageStore::TreePage page;
    std::shared_ptr<PageData> decoded = std::make_shared<PageData>();
    if (!pages_.readTreePage(offset, &page, error) ||
        !pages_.readTreeCells(page, itemMap_, &decoded->cells, error, &mode_)) return false;
    decoded->leaf = page.leaf;
    if (decoded->leaf) {
        for (std::size_t i = 0; i < decoded->cells.size(); ++i) {
            int order;
            if (!compare(decoded->cells[i].key, decoded->cells[i].key, mode_, &order, error)) return false;
            if (i && (!compare(decoded->cells[i - 1].key, decoded->cells[i].key, mode_, &order, error) || order > 0))
                return fail(error, "Unordered TIBX IMAP leaf keys");
        }
    }
    const std::size_t budget = 8U * 1024U * 1024U;
    std::size_t size = sizeof(PageData);
    bool retain = decoded->cells.capacity() <= (budget - size) / sizeof(TibxPageStore::TreeCell);
    if (retain) {
        size += decoded->cells.capacity() * sizeof(TibxPageStore::TreeCell);
        for (const auto& cell : decoded->cells) {
            if (cell.key.capacity() > budget - size) { retain = false; break; }
            size += cell.key.capacity();
            if (cell.value.capacity() > budget - size) { retain = false; break; }
            size += cell.value.capacity();
        }
    }
    if (retain) {
        while (!cachedPages_.empty() &&
               (cachedPages_.size() >= 16 || cachedBytes_ > budget - size)) {
            cachedBytes_ -= cachedPages_.back().size;
            cachedPages_.pop_back();
        }
        cachedPages_.push_front(CachedPage{offset, size, decoded});
        cachedBytes_ += size;
    }
    *result = std::move(decoded);
    return true;
}

bool TibxImapLsm::next(const std::vector<std::uint8_t>* lower, bool inclusive,
                       TibxPageStore::TreeCell* cell, bool* found, std::string* error) const {
    if (!cell || !found) return fail(error, "Missing TIBX IMAP destination");
    const TibxImapSort mode = mode_;
    if (lower) {
        int order;
        if (!compare(*lower, *lower, mode, &order, error)) return false;
    }
    if (!inlineReady_) {
        std::vector<std::uint8_t> inlineData;
        std::vector<TibxPageStore::TreeCell> cells;
        if (!pages_.decodeInlineTree(header_, itemMap_, &inlineData, error) ||
            !pages_.readInlineCells(inlineData, itemMap_, &cells, error)) return false;
        for (const auto& entry : cells) {
            int order;
            if (!compare(entry.key, entry.key, mode, &order, error)) return false;
        }
        inlineCells_.swap(cells);
        inlineReady_ = true;
    }

    std::vector<std::uint8_t> bound;
    const std::vector<std::uint8_t>* target = lower;
    bool allowEqual = inclusive;
    for (;;) {
        bool have = false;
        TibxPageStore::TreeCell best;
        for (const auto& entry : inlineCells_) {
            int order;
            if (target) {
                if (!compare(entry.key, *target, mode, &order, error)) return false;
                if (order < 0 || (!allowEqual && order == 0)) continue;
            }
            if (have) {
                if (!compare(entry.key, best.key, mode, &order, error)) return false;
                if (order > 0) continue;
            }
            best = entry;
            have = true;
        }

        for (const auto& level : itemMap_.levels) {
            if (level.rootOffset == std::numeric_limits<std::uint64_t>::max()) continue;
            std::vector<std::uint64_t> pending(1, level.rootOffset);
            std::set<std::uint64_t> visited;
            bool levelFound = false;
            TibxPageStore::TreeCell levelBest;
            while (!pending.empty() && !levelFound) {
                const std::uint64_t offset = pending.back();
                pending.pop_back();
                if (!visited.insert(offset).second) return fail(error, "TIBX tree contains a cycle");
                std::shared_ptr<const PageData> page;
                if (!loadPage(offset, &page, error)) return false;
                const std::vector<TibxPageStore::TreeCell>& entries = page->cells;
                if (page->leaf) {
                    std::size_t first = 0, last = entries.size();
                    while (target && first < last) {
                        const std::size_t middle = first + (last - first) / 2;
                        int order;
                        if (!compare(entries[middle].key, *target, mode, &order, error)) return false;
                        if (order < 0 || (!allowEqual && order == 0)) first = middle + 1;
                        else last = middle;
                    }
                    if (first < entries.size()) { levelBest = entries[first]; levelFound = true; }
                } else {
                    for (std::size_t i = entries.size(); i-- > 0;) {
                        if (target) {
                            int order;
                            if (!compare(entries[i].key, *target, mode, &order, error)) return false;
                            if (order < 0 || (!allowEqual && order == 0)) continue;
                        }
                        pending.push_back(be64(entries[i].value));
                    }
                }
            }
            if (levelFound) {
                int order = -1;
                if (have && !compare(levelBest.key, best.key, mode, &order, error)) return false;
                if (!have || order < 0) { best = std::move(levelBest); have = true; }
            }
        }
        if (!have) { *found = false; if (error) error->clear(); return true; }
        if (!best.deleted) { *cell = std::move(best); *found = true; if (error) error->clear(); return true; }
        bound = best.key;
        target = &bound;
        allowEqual = false;
    }
}

}  // namespace fs
}  // namespace peare
