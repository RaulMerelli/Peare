#include "TibxVolumeStore.h"
#include "PartialPartitionTable.h"

#include <algorithm>
#include <array>
#include <new>
#include <set>
#include <stdexcept>
#include <utility>

namespace peare {
namespace fs {

namespace {

// Recognize only the names emitted by the documented AddSlice path.
bool sliceName(const std::string& name, std::string* disk, bool* partition) {
    if (name.compare(0, 4, "disk") != 0) return false;
    std::size_t pos = 4, start = pos;
    while (pos < name.size() && name[pos] >= '0' && name[pos] <= '9') ++pos;
    if (pos == start || (pos - start > 1 && name[start] == '0')) return false;
    *disk = name.substr(0, pos);
    if (pos == name.size()) { *partition = false; return true; }
    if (name.compare(pos, 5, "/part") != 0) return false;
    pos += 5; start = pos;
    while (pos < name.size() && name[pos] >= '0' && name[pos] <= '9') ++pos;
    if (pos == start || pos != name.size() || (pos - start > 1 && name[start] == '0')) return false;
    *partition = true;
    return true;
}

bool field(const std::vector<TibxMetadataField>& fields, std::uint16_t tag,
           std::size_t width, std::vector<std::uint8_t>* value) {
    bool found = false;
    return getTibxItemMetadataField(fields, tag, width, value, &found) && found;
}
std::uint64_t little(const std::vector<std::uint8_t>& bytes) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) value |= std::uint64_t(bytes[i]) << (8 * i);
    return value;
}
class MetadataGetters {
public:
    explicit MetadataGetters(const std::vector<TibxMetadataField>& fields) : fields_(fields) {}

    bool get(std::uint16_t tag, std::size_t width, bool* present = nullptr, bool zero = false) {
        std::vector<std::uint8_t> value;
        bool found = false;
        if (!getTibxItemMetadataField(fields_, tag, width, &value, &found)) return false;
        if (present) *present = found;
        if (found) {
            if (zero && std::find_if(value.begin(), value.end(), [](std::uint8_t x) { return x != 0; }) != value.end())
                return false;
            consumed_.push_back(tag);
        }
        return true;
    }

    bool number(std::uint16_t tag, std::size_t width, std::uint64_t fallback,
                std::uint64_t* result) {
        if (!result) return false;
        std::vector<std::uint8_t> value;
        bool found = false;
        if (!getTibxItemMetadataField(fields_, tag, width, &value, &found)) return false;
        *result = found ? little(value) : fallback;
        if (found) consumed_.push_back(tag);
        return true;
    }

    bool text(std::uint16_t tag, std::size_t capacity) {
        const TibxMetadataField* f = find(tag);
        if (!f) return true;
        if (!f->bytes.empty()) {
            const std::size_t n = f->bytes[0], remaining = f->bytes.size() - 1;
            if (n > capacity || remaining < n || remaining > 2 * n) return false;
        }
        consumed_.push_back(tag);
        return true;
    }

    bool present(std::uint16_t tag) const { return find(tag) != nullptr; }
    bool findValue(std::uint16_t tag, const TibxMetadataField** value) {
        *value = find(tag);
        if (*value) consumed_.push_back(tag);
        return true;
    }
    bool done() const { return finishTibxItemMetadata(fields_, consumed_); }
private:
    const TibxMetadataField* find(std::uint16_t tag) const {
        for (const auto& f : fields_) if (f.tag == tag) return &f;
        return nullptr;
    }
    const std::vector<TibxMetadataField>& fields_;
    std::vector<std::uint16_t> consumed_;
};

bool containerFields(MetadataGetters* g) {
    if (!g->get(32768, 8) || !g->get(158, 0) || !g->get(262, 0)) return false;
    const TibxMetadataField* changesId = nullptr;
    if (!g->findValue(32787, &changesId)) return false;
    // Consumers of these variable-sized fields are not yet specified.
    return !g->present(252) && !g->present(253) && !g->present(254);
}

bool diskMetadata(const std::vector<TibxMetadataField>& fields) {
    MetadataGetters g(fields);
    bool alternate = false;
    if (!g.get(105, 0, &alternate) || alternate || !containerFields(&g)) return false;
    const std::uint16_t u32[] = {73, 2, 85, 86, 87, 32773};
    const std::uint16_t u64[] = {18, 7};
    const std::uint16_t u8[] = {74, 75, 76};
    for (auto tag : u32) if (!g.get(tag, 4)) return false;
    for (auto tag : u64) if (!g.get(tag, 8)) return false;
    for (auto tag : u8) if (!g.get(tag, 1)) return false;
    if (!g.get(88, 63) || !g.get(32774, 16) || !g.get(32792, 16) ||
        !g.get(128, 0) || !g.text(129, 255) || !g.text(32789, 255) ||
        !g.get(152, 0) || !g.get(32788, 0)) return false;
    const std::uint16_t choices[] = {77, 78, 79, 80, 81, 82, 83, 84, 149, 150, 151,
                                     32793, 32794, 32795, 32796, 32797, 32798, 32801};
    for (auto tag : choices) {
        bool present = false;
        if (!g.get(tag, 0, &present)) return false;
        if (present) break;
    }
    bool shortcut = false;
    if (!g.get(32800, 0, &shortcut)) return false;
    const TibxMetadataField* variable = nullptr;
    if (!shortcut) {
        if (!g.findValue(32791, &variable)) return false;
        if (variable && variable->bytes.empty()) shortcut = true;
        if (!shortcut) {
            if (!g.get(32790, 0, &shortcut)) return false;
            if (!shortcut && !g.get(32785, 0)) return false;
        }
    }
    if (!g.present(32792)) {
        if (!g.findValue(32791, &variable)) return false;
        if (variable && !variable->bytes.empty() && !g.get(32791, 16)) return false;
    }
    return g.done();
}

struct PartitionMetadataInfo {
    std::uint16_t selector = 0;
    std::array<std::uint8_t, 9> byteFields = {};
    std::array<std::uint32_t, 3> wordFields = {};
};

bool partitionMetadata(const std::vector<TibxMetadataField>& fields, PartitionMetadataInfo* info) {
    if (!info) return false;
    PartitionMetadataInfo result;
    MetadataGetters g(fields);
    if (!containerFields(&g)) return false;
    const std::uint16_t types[] = {9, 137, 10, 132, 224, 12, 11, 193, 213, 130, 13,
                                   251, 235, 101, 14, 15, 206, 16, 232, 236, 242,
                                   249, 248, 256, 244, 243};
    std::uint16_t filesystem = 0;
    for (auto tag : types) {
        bool present = false;
        if (!g.get(tag, 0, &present)) return false;
        if (present) { filesystem = tag; break; }
    }
    if (filesystem != 9 && filesystem != 13 && filesystem != 14 && filesystem != 15 &&
        filesystem != 206 && filesystem != 224) return false;
    const std::uint16_t flags[] = {23, 24, 25, 26, 92, 133, 134, 135, 139, 114, 131,
                                   225, 226, 227, 228, 229, 230, 245, 255, 259, 260,
                                   261, 267, 268, 271, 272, 273, 274};
    for (auto tag : flags) if (!g.get(tag, 1, nullptr, true)) return false;
    if (filesystem == 224 && !g.get(243, 1, nullptr, true)) return false;
    const std::uint16_t markers[] = {8, 95, 103, 104, 115, 116, 117, 118, 270, 53};
    const std::uint16_t u8[] = {28, 119};
    const std::uint16_t u32[] = {2, 3, 22, 27, 29, 30, 31, 33};
    const std::uint16_t u64[] = {5, 17, 18, 19, 20, 21, 32, 36, 37, 38, 39, 40, 41,
                                  42, 43, 44, 45, 46, 47, 48, 91, 100, 108, 109, 110,
                                  111, 112, 113, 200, 32804, 32805, 32806, 32807, 32808};
    for (auto tag : markers) if (!g.get(tag, 0)) return false;
    for (auto tag : u8) if (!g.get(tag, 1)) return false;
    for (auto tag : u32) if (!g.get(tag, 4)) return false;
    for (auto tag : u64) if (!g.get(tag, 8)) return false;
    if (!g.get(35, 16)) return false;
    bool field203 = false;
    if (!g.get(203, 510, &field203) || (!field203 && !g.get(34, 64))) return false;
    if (filesystem == 224) {
        if (!g.get(49, 8) || !g.get(51, 4) || !g.get(50, 0) || !g.get(231, 0)) return false;
    } else if (filesystem == 14 || filesystem == 15) {
        if (!g.get(54, 32) || !g.get(55, 1)) return false;
    } else if (filesystem == 13) {
        bool extended = false;
        if (!g.get(102, 64, &extended)) return false;
        if (extended) {
            if (!g.get(32803, 40)) return false;
        } else {
            if (!g.get(52, 8)) return false;
            const auto number = [&fields](std::uint16_t tag, std::size_t width,
                                          std::uint64_t fallback, std::uint64_t* out) -> bool {
                bool found = false; std::vector<std::uint8_t> bytes;
                if (!getTibxItemMetadataField(fields, tag, width, &bytes, &found)) return false;
                *out = found ? little(bytes) : fallback;
                return true;
            };
            std::uint64_t t2, t3, t32, t47, t52;
            if (!number(2, 4, 0, &t2) || !number(3, 4, 1, &t3) ||
                !number(32, 8, 0, &t32) || !number(47, 8, 0, &t47) ||
                !number(52, 8, 0, &t52)) return false;
            const std::uint32_t product = static_cast<std::uint32_t>(t2 * t3);
            if (!product) return false;
            std::uint64_t threshold = t47;
            if (t32) {
                const std::uint64_t extra = ((t32 - 1) / product) / 8;
                if (threshold == UINT64_MAX || extra > UINT64_MAX - threshold - 1) return false;
                threshold += 1 + extra;
            }
            if (t52 <= threshold) return false;
        }
    }
    const std::uint16_t selectors[] = {162, 163, 164, 165, 166, 167, 237, 263, 257};
    std::uint16_t selected = 0;
    for (auto tag : selectors) {
        bool present = false;
        if (!g.get(tag, 0, &present)) return false;
        if (present) { selected = tag; break; }
    }
    if (selected != 163 && selected != 165) return false;
    result.selector = selected;
    if (!g.get(90, 0) || !g.get(56, 0) || !g.get(57, 0)) return false;
    bool field58 = false;
    if (!g.get(58, 0, &field58) || (!field58 && !g.get(59, 0))) return false;
    if (!g.get(60, 1)) return false;
    if (selected == 163) {
        for (std::uint16_t tag = 61; tag <= 68; ++tag) {
            std::uint64_t value = 0;
            if (!g.number(tag, 1, 127, &value)) return false;
            result.byteFields[tag - 61] = static_cast<std::uint8_t>(value);
        }
        std::uint64_t value = 0;
        if (!g.number(107, 1, 127, &value)) return false;
        result.byteFields[8] = static_cast<std::uint8_t>(value);
        for (std::uint16_t tag = 69; tag <= 71; ++tag) {
            if (!g.number(tag, 4, 65535, &value)) return false;
            result.wordFields[tag - 69] = static_cast<std::uint32_t>(value);
        }
    } else {
        for (std::uint16_t tag = 61; tag <= 68; ++tag) if (!g.get(tag, 1)) return false;
        for (std::uint16_t tag = 69; tag <= 71; ++tag) if (!g.get(tag, 4)) return false;
        if (!g.get(107, 1)) return false;
    }
    if (!g.get(106, 16) || !g.get(159, 8) || !g.get(7, 8) ||
        !g.text(129, 255)) return false;
    if (selected == 165 && (!g.get(182, 16) || !g.get(183, 8) || !g.text(181, 36))) return false;
    const TibxMetadataField* variable = nullptr;
    if (!g.findValue(32802, &variable)) return false;
    if (!g.done()) return false;
    *info = result;
    return true;
}

} // namespace

TibxVolumeContext::TibxVolumeContext(ByteStorePtr source, TibxPageStore::CommittedHeader h,
                                     TibxPageStore::Superblock d, TibxPageStore::Superblock s)
    : pages(source), header(std::move(h)), dataMap(std::move(d)),
      segmentMap(std::move(s)), source_(std::move(source)),
      sourceSize_(source_ ? source_->capacity() : -1) {}

bool TibxVolumeContext::validate() const {
    return source_ && sourceSize_ >= 0 && source_->validate() && source_->capacity() == sourceSize_;
}

std::shared_ptr<const std::vector<TibxPageStore::Extent> >
TibxVolumeContext::loadExtents(std::uint64_t volumeId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto existing = extents_.find(volumeId);
    if (existing != extents_.end()) return existing->second;
    auto extents = std::make_shared<std::vector<TibxPageStore::Extent> >();
    if (!pages.readExtents(header, dataMap, volumeId, extents.get(), nullptr)) return nullptr;
    extents_.insert(std::make_pair(volumeId, extents));
    return extents;
}

TibxRecoveryItems::TibxRecoveryItems(std::shared_ptr<TibxVolumeContext> context,
                                     TibxPageStore::Superblock itemMap,
                                     std::uint32_t fullId, std::uint32_t sliceId)
    : context_(std::move(context)), itemMap_(std::move(itemMap)), fullId_(fullId), sliceId_(sliceId) {}

std::shared_ptr<const std::vector<TibxPageStore::ResolvedItem> >
TibxRecoveryItems::load(std::string* error) const {
    const auto result = snapshot(error);
    if (!result) return nullptr;
    return std::shared_ptr<const std::vector<TibxPageStore::ResolvedItem> >(result, &result->items());
}

std::shared_ptr<const TibxRecoverySnapshot> TibxRecoveryItems::snapshot(std::string* error) const {
    if (!context_ || !context_->validate()) { if (error) *error = "Invalid TIBX recovery context"; return nullptr; }
    try {
        auto result = std::make_shared<TibxRecoverySnapshot>();
        result->context_ = context_;
        result->fullId_ = fullId_;
        result->sliceId_ = sliceId_;
        if (!context_->pages.readResolvedItems(context_->header, itemMap_, fullId_, sliceId_, &result->items_, error))
            return nullptr;
        if (!context_->validate()) { if (error) *error = "TIBX recovery source became invalid"; return nullptr; }
        return result;
    } catch (const std::bad_alloc&) {
        if (error) *error = "TIBX recovery item allocation failed";
    } catch (const std::length_error&) {
        if (error) *error = "TIBX recovery item size is not representable";
    }
    return nullptr;
}

bool TibxRecoveryItems::matches(const TibxRecoverySnapshot& snapshot) const {
    return context_ && context_ == snapshot.context_ && fullId_ == snapshot.fullId_ && sliceId_ == snapshot.sliceId_;
}

TibxVolumeStore::TibxVolumeStore(std::shared_ptr<TibxVolumeContext> context,
                                 std::uint64_t volumeId, std::int64_t length,
                                 std::uint32_t itemSlice,
                                 std::shared_ptr<const std::vector<TibxPageStore::Volume> > recoveryVolumes,
                                 std::shared_ptr<const TibxRecoveryItems> recoveryItems,
                                 std::string itemName)
    : context_(std::move(context)), volumeId_(volumeId), length_(length),
      itemSlice_(itemSlice), recoveryVolumes_(std::move(recoveryVolumes)),
      recoveryItems_(std::move(recoveryItems)), itemName_(std::move(itemName)), failed_(false) {}

std::shared_ptr<const std::vector<TibxPageStore::Volume> > TibxVolumeStore::recoveryVolumes() const {
    return recoveryVolumes_;
}

ByteStorePtr TibxVolumeStore::openRecoveryVolume(std::size_t index) const {
    if (!context_ || !recoveryVolumes_ || index >= recoveryVolumes_->size()) return nullptr;
    const TibxPageStore::Volume& volume = (*recoveryVolumes_)[index];
    if (volume.size > static_cast<std::uint64_t>(INT64_MAX)) return nullptr;
    return std::make_shared<TibxVolumeStore>(context_, volume.id,
        static_cast<std::int64_t>(volume.size), volume.sliceId, recoveryVolumes_, recoveryItems_, volume.name);
}

std::shared_ptr<const std::vector<TibxPageStore::ResolvedItem> > TibxVolumeStore::recoveryItems(std::string* error) const {
    if (!recoveryItems_) { if (error) *error = "Missing TIBX recovery items"; return nullptr; }
    return recoveryItems_->load(error);
}

std::shared_ptr<const TibxRecoverySnapshot> TibxVolumeStore::recoverySnapshot(std::string* error) const {
    if (!recoveryItems_) { if (error) *error = "Missing TIBX recovery items"; return nullptr; }
    return recoveryItems_->snapshot(error);
}

TibxVolumeStore::StreamResult TibxVolumeStore::openRecoveryItemStream(
        const std::string& itemName, std::uint32_t itemSlice, std::uint8_t selector,
        ByteStorePtr* stream, std::string* error) const {
    if (error) error->clear();
    if (!stream) return StreamResult::Invalid;
    stream->reset();
    if (!context_ || !recoveryItems_) return StreamResult::Invalid;
    const auto snapshot = recoverySnapshot(error);
    if (!snapshot) return StreamResult::Unavailable;
    std::size_t found = snapshot->items().size();
    for (std::size_t i = 0; i < snapshot->items().size(); ++i) {
        const auto& candidate = snapshot->items()[i];
        if (candidate.name != itemName || candidate.sliceId != itemSlice) continue;
        if (found != snapshot->items().size()) return StreamResult::Ambiguous;
        found = i;
    }
    if (found == snapshot->items().size()) return StreamResult::NotFound;
    return openRecoveryItemStream(snapshot, found, selector, stream, error);
}

TibxVolumeStore::StreamResult TibxVolumeStore::openRecoveryItemStream(
        const std::shared_ptr<const TibxRecoverySnapshot>& snapshot, std::size_t itemIndex,
        std::uint8_t selector, ByteStorePtr* stream, std::string* error) const {
    if (error) error->clear();
    if (!stream) return StreamResult::Invalid;
    stream->reset();
    if (!context_ || !recoveryItems_ || !snapshot || !recoveryItems_->matches(*snapshot) ||
        itemIndex >= snapshot->items().size()) return StreamResult::Invalid;
    if (!context_->validate()) return StreamResult::Unavailable;
    const TibxPageStore::ResolvedItem& item = snapshot->items()[itemIndex];
    if (!item.descriptorsValid) return StreamResult::Invalid;
    const TibxPageStore::StreamDescriptor* selected = nullptr;
    for (const auto& descriptor : item.streams) {
        if (descriptor.selector != selector) continue;
        if (selected) return StreamResult::Ambiguous;
        selected = &descriptor;
    }
    if (!selected) return StreamResult::NotFound;
    if (selected->size > static_cast<std::uint64_t>(INT64_MAX)) return StreamResult::Invalid;
    *stream = std::make_shared<TibxVolumeStore>(context_, selected->id,
        static_cast<std::int64_t>(selected->size), item.sliceId, recoveryVolumes_, recoveryItems_,
        selector == 0 ? item.name : std::string());
    return StreamResult::Found;
}

TibxVolumeStore::StreamResult TibxVolumeStore::openFatClusterFilter(ByteStorePtr* stream) const {
    if (!stream) return StreamResult::Invalid;
    stream->reset();
    if (itemName_.empty()) return StreamResult::NotFound;
    const auto snapshot = recoverySnapshot();
    if (!snapshot) return StreamResult::Unavailable;
    std::size_t found = snapshot->items().size();
    for (std::size_t i = 0; i < snapshot->items().size(); ++i) {
        const auto& item = snapshot->items()[i];
        if (item.name != itemName_ || item.sliceId != itemSlice_) continue;
        if (found != snapshot->items().size()) return StreamResult::Ambiguous;
        found = i;
    }
    if (found == snapshot->items().size()) return StreamResult::Unavailable;
    const auto& item = snapshot->items()[found];
    if (!item.descriptorsValid) return StreamResult::Invalid;
    unsigned primaryCount = 0;
    for (const auto& descriptor : item.streams) {
        if (descriptor.selector != 0) continue;
        ++primaryCount;
        if (descriptor.id != volumeId_ || length_ < 0 ||
            descriptor.size != static_cast<std::uint64_t>(length_)) return StreamResult::Invalid;
    }
    if (primaryCount != 1) return StreamResult::Invalid;
    return openRecoveryItemStream(snapshot, found, 4, stream);
}

TibxVolumeStore::StreamResult TibxVolumeStore::readRecoveryItemMetadata(
        const std::shared_ptr<const TibxRecoverySnapshot>& snapshot, std::size_t itemIndex,
        TibxMetadataDomain domain, std::vector<TibxMetadataField>* fields, std::string* error) const {
    if (error) error->clear();
    if (!fields) return StreamResult::Invalid;
    fields->clear();
    ByteStorePtr stream;
    const StreamResult selected = openRecoveryItemStream(snapshot, itemIndex, 1, &stream, error);
    if (selected != StreamResult::Found) return selected;
    if (!readTibxItemMetadata(stream, domain, fields, error)) return StreamResult::Invalid;
    return StreamResult::Found;
}

PartitionResolveResult TibxVolumeStore::openWofSibling(
        const PartitionIdentity& target, ByteStorePtr* stream) const {
    if (!stream) return PartitionResolveResult::Unavailable;
    stream->reset();
    if (target.scheme != PartitionScheme::Gpt && target.scheme != PartitionScheme::Mbr)
        return PartitionResolveResult::Unknown;
    try {
        const auto snapshot = recoverySnapshot();
        if (!snapshot) return PartitionResolveResult::Unavailable;
        struct Candidate { std::size_t index; std::string disk; bool partition; };
        std::vector<Candidate> candidates;
        for (std::size_t i = 0; i < snapshot->items().size(); ++i) {
            std::string disk; bool partition = false;
            if (sliceName(snapshot->items()[i].name, &disk, &partition))
                candidates.push_back(Candidate{i, disk, partition});
        }
        struct Disk { std::string name; ByteStorePtr source; std::vector<std::uint8_t> identity; };
        std::vector<Disk> disks;
        for (const Candidate& c : candidates) {
            if (c.partition) continue;
            ByteStorePtr primary;
            std::vector<TibxMetadataField> fields;
            if (openRecoveryItemStream(snapshot, c.index, 0, &primary) != StreamResult::Found ||
                readRecoveryItemMetadata(snapshot, c.index, TibxMetadataDomain::Disk, &fields) != StreamResult::Found ||
                !diskMetadata(fields)) return PartitionResolveResult::Unavailable;
            std::vector<std::uint8_t> length, marker, guid;
            const std::uint16_t identityTag = target.scheme == PartitionScheme::Gpt ? 32774 : 32773;
            const std::size_t identityWidth = target.scheme == PartitionScheme::Gpt ? 16 : 4;
            if (!field(fields, 18, 8, &length) || !little(length) ||
                !field(fields, 74, 1, &marker) || !little(marker) ||
                !field(fields, identityTag, identityWidth, &guid)) return PartitionResolveResult::Unavailable;
            std::vector<std::uint8_t> sectorField;
            bool hasSector = false;
            if (!getTibxItemMetadataField(fields, 2, 4, &sectorField, &hasSector))
                return PartitionResolveResult::Unavailable;
            disks.push_back(Disk{c.disk, primary, guid});
        }
        bool sourceFound = false;
        ByteStorePtr selected;
        struct Tables { PartialMbrTable mbr; PartialGptTable gpt; };
        std::map<std::pair<std::string, std::uint32_t>, Tables> tables;
        for (const Candidate& c : candidates) {
            if (!c.partition) continue;
            const Disk* disk = nullptr;
            for (const Disk& d : disks) if (d.name == c.disk) {
                if (disk) return PartitionResolveResult::Ambiguous;
                disk = &d;
            }
            if (!disk) return PartitionResolveResult::Unavailable;
            ByteStorePtr primary;
            std::vector<TibxMetadataField> fields;
            PartitionMetadataInfo decoded;
            if (openRecoveryItemStream(snapshot, c.index, 0, &primary) != StreamResult::Found ||
                readRecoveryItemMetadata(snapshot, c.index, TibxMetadataDomain::Partition, &fields) != StreamResult::Found ||
                !partitionMetadata(fields, &decoded)) return PartitionResolveResult::Unavailable;
            std::vector<std::uint8_t> sector, start, length, guid;
            if (!field(fields, 2, 4, &sector) ||
                !field(fields, 17, 8, &start) || !field(fields, 18, 8, &length))
                return PartitionResolveResult::Unavailable;
            if (target.scheme == PartitionScheme::Gpt && !field(fields, 106, 16, &guid))
                return PartitionResolveResult::Unavailable;
            const std::uint32_t sectorSize = static_cast<std::uint32_t>(little(sector));
            const auto key = std::make_pair(disk->name, sectorSize);
            auto table = tables.find(key);
            if (table == tables.end()) {
                PartialMbrTable mbr; PartialGptTable gpt;
                readPartialPartitionTables(disk->source, sectorSize, &mbr,
                                           target.scheme == PartitionScheme::Gpt ? &gpt : nullptr);
                if (mbr.status != PartialTableStatus::Valid) return PartitionResolveResult::Unavailable;
                if (target.scheme == PartitionScheme::Mbr) {
                    if (mbr.protective || !mbr.diskSignature ||
                        little(disk->identity) != mbr.diskSignature) return PartitionResolveResult::Unavailable;
                } else if (!mbr.protective || mbr.hasExtended || !mbr.entries.empty() ||
                           gpt.status != PartialTableStatus::Valid ||
                           !std::equal(disk->identity.begin(), disk->identity.end(), gpt.diskGuid.begin())) {
                    return PartitionResolveResult::Unavailable;
                }
                std::set<std::array<std::uint8_t, 16> > guids;
                std::vector<std::pair<std::uint64_t, std::uint64_t> > ranges;
                for (const auto& entry : gpt.entries) {
                    if (!guids.insert(entry.partitionGuid).second)
                        return PartitionResolveResult::Ambiguous;
                    ranges.push_back(std::make_pair(entry.firstLba, entry.firstLba + entry.sectorCount));
                }
                std::sort(ranges.begin(), ranges.end());
                for (std::size_t i = 1; i < ranges.size(); ++i)
                    if (ranges[i].first < ranges[i - 1].second)
                        return PartitionResolveResult::Ambiguous;
                table = tables.insert(std::make_pair(key, Tables{std::move(mbr), std::move(gpt)})).first;
            }
            const std::uint64_t first = little(start), sectors = little(length);
            if (!sectorSize || !sectors || first > std::uint64_t(INT64_MAX) / sectorSize ||
                sectors > std::uint64_t(INT64_MAX) / sectorSize ||
                primary->capacity() != static_cast<std::int64_t>(sectors * sectorSize))
                return PartitionResolveResult::Unavailable;
            const PartialPartitionEntry* matched = nullptr;
            const auto& entries = target.scheme == PartitionScheme::Gpt
                ? table->second.gpt.entries : table->second.mbr.entries;
            for (const PartialPartitionEntry& p : entries) {
                if (target.scheme == PartitionScheme::Gpt) {
                    if (!std::equal(guid.begin(), guid.end(), p.partitionGuid.begin())) continue;
                } else if (p.firstLba != first || p.sectorCount != sectors) continue;
                if (matched) return PartitionResolveResult::Ambiguous;
                matched = &p;
            }
            if (!matched || matched->firstLba != first || matched->sectorCount != sectors)
                return PartitionResolveResult::Unavailable;
            const auto& item = snapshot->items()[c.index];
            for (const auto& descriptor : item.streams) if (descriptor.selector == 0 &&
                descriptor.id == volumeId_ && item.sliceId == itemSlice_) {
                if (sourceFound) return PartitionResolveResult::Ambiguous;
                sourceFound = true;
            }
            const bool isTarget = target.scheme == PartitionScheme::Gpt
                ? table->second.gpt.diskGuid == target.diskGuid && matched->partitionGuid == target.partitionGuid
                : first <= std::uint64_t(INT64_MAX) / sectorSize &&
                  table->second.mbr.diskSignature == target.mbrDiskSignature &&
                  first * sectorSize == target.startOffset;
            if (isTarget) {
                if (selected) return PartitionResolveResult::Ambiguous;
                selected = primary;
            }
        }
        if (!sourceFound) return PartitionResolveResult::Unavailable;
        if (!selected) return PartitionResolveResult::NotFound;
        if (!context_->validate() || !selected->validate()) return PartitionResolveResult::Unavailable;
        *stream = selected;
        return PartitionResolveResult::Found;
    } catch (...) { return PartitionResolveResult::Unavailable; }
}

TibxVolumeStore::StreamResult TibxVolumeStore::readRecoveryItemMetadata(
        const std::string& itemName, std::uint32_t itemSlice, TibxMetadataDomain domain,
        std::vector<TibxMetadataField>* fields, std::string* error) const {
    if (error) error->clear();
    if (!fields) return StreamResult::Invalid;
    fields->clear();
    ByteStorePtr stream;
    const StreamResult selected = openRecoveryItemStream(itemName, itemSlice, 1, &stream, error);
    if (selected != StreamResult::Found) return selected;
    if (!readTibxItemMetadata(stream, domain, fields, error)) return StreamResult::Invalid;
    return StreamResult::Found;
}

std::int64_t TibxVolumeStore::capacity() const { return length_; }

int TibxVolumeStore::read(std::int64_t pos, std::uint8_t* dst, int count) const {
    if (!dst || pos < 0 || count <= 0 || pos >= length_ || !validate()) return 0;
    const int amount = static_cast<int>(std::min<std::int64_t>(
        std::min<std::int64_t>(count, length_ - pos), 1024 * 1024));
    try {
        const auto extents = context_->loadExtents(volumeId_);
        if (!extents) { failed_.store(true); return 0; }
        std::vector<std::uint8_t> pending(static_cast<std::size_t>(amount));
        if (!context_->pages.readVolumeRange(context_->header, context_->segmentMap,
                                             *extents, volumeId_, itemSlice_,
                                             static_cast<std::uint64_t>(pos), pending.data(),
                                             pending.size(), nullptr) || !validate()) {
            failed_.store(true);
            return 0;
        }
        std::copy(pending.begin(), pending.end(), dst);
        return amount;
    } catch (const std::bad_alloc&) {
        failed_.store(true);
        return 0;
    } catch (const std::length_error&) {
        failed_.store(true);
        return 0;
    }
}

bool TibxVolumeStore::validate() const {
    return context_ && context_->validate() && length_ >= 0 && !failed_.load() && bool(context_->loadExtents(volumeId_));
}

}  // namespace fs
}  // namespace peare
