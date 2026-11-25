#include "TibxModule.h"
#include "Compat.h"
#include "../fs/QFileByteStore.h"
#include "../fs/TibxVolumeStore.h"
#include "../compression/TibxEncryptedPem.h"

#include <QDir>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <map>
#include <set>
#include <tuple>
#include <utility>

namespace peare {
namespace {

template <typename T> class SecretGuard {
public:
    explicit SecretGuard(T& value) : value_(value) {}
    ~SecretGuard() {
        if (!value_.empty()) {
            volatile char* p = reinterpret_cast<volatile char*>(&value_[0]);
            for (std::size_t i = 0; i < value_.size(); ++i) p[i] = 0;
        }
    }
private:
    T& value_;
};

class SecretByteArrayGuard {
public:
    explicit SecretByteArrayGuard(QByteArray& value) : value_(value) {}
    ~SecretByteArrayGuard() { value_.fill(0); }
private:
    QByteArray& value_;
};

std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

std::uint64_t be64(const std::uint8_t* p) {
    return (std::uint64_t(be32(p)) << 32) | be32(p + 4);
}

bool canonicalVolumeName(const std::string& name) {
    if (name.compare(0, 4, "disk") != 0) return false;
    std::size_t pos = 4, first = pos;
    while (pos < name.size() && name[pos] >= '0' && name[pos] <= '9') ++pos;
    if (pos == first || name.compare(pos, 5, "/part") != 0) return false;
    pos += 5; first = pos;
    while (pos < name.size() && name[pos] >= '0' && name[pos] <= '9') ++pos;
    return pos == name.size() && pos != first;
}

bool linkedVolumeAliases(const fs::TibxPageStore& pages,
                         const fs::TibxPageStore::CommittedHeader& header,
                         const fs::TibxPageStore::Superblock& itemMap,
                         std::uint32_t fullId, std::uint32_t sliceId,
                         const std::vector<fs::TibxPageStore::Volume>& volumes,
                         std::set<std::string>* aliases) {
    if (!aliases) return false;
    try {
        std::set<std::uint64_t> ids;
        typedef std::tuple<std::string, std::uint64_t, std::uint32_t> VolumeKey;
        std::set<VolumeKey> volumeKeys;
        for (const auto& volume : volumes) {
            volumeKeys.insert(VolumeKey(volume.name, volume.id, volume.sliceId));
            if (volume.name.compare(0, 5, "part/") == 0) ids.insert(volume.id);
        }
        if (ids.empty()) return true;
        std::vector<fs::TibxPageStore::HeaderSection> sections;
        fs::TibxPageStore::Superblock links;
        if (!fs::TibxPageStore::readHeaderSections(header, &sections, nullptr) ||
            sections.size() <= 4 || !sections[4].length ||
            !fs::TibxPageStore::readSuperblock(header, sections[4], &links, nullptr)) return false;
        std::vector<fs::TibxPageStore::ResolvedItem> resolved;
        if (!pages.readResolvedItems(header, itemMap, fullId, sliceId, &resolved, nullptr)) return false;
        std::map<std::string, const fs::TibxPageStore::ResolvedItem*> byName;
        for (const auto& item : resolved)
            if (!byName.insert(std::make_pair(item.name, &item)).second) return false;
        typedef std::pair<std::uint64_t, std::uint32_t> LinkKey;
        std::map<LinkKey, std::set<std::string> > groups;
        for (std::uint64_t id : ids) {
            std::vector<std::uint8_t> prefix(8);
            for (std::size_t i = 0; i < 8; ++i) prefix[i] = std::uint8_t(id >> (56 - 8 * i));
            std::vector<fs::TibxPageStore::TreeCell> cells;
            if (!pages.readTreePrefix(header, links, prefix, &cells, nullptr)) return false;
            for (const auto& cell : cells) {
                if (cell.key.size() < 12 || be64(cell.key.data()) != id) return false;
                if (cell.deleted) continue;
                if (!cell.value.empty()) return false;
                const std::uint32_t version = be32(cell.key.data() + 8);
                if (version < fullId || version > sliceId) continue;
                const std::string name(cell.key.begin() + 12, cell.key.end());
                const auto found = byName.find(name);
                if (found == byName.end()) continue;
                const auto& item = *found->second;
                if (!(item.flags & 2U) || item.sliceId != version || !item.descriptorsValid) continue;
                const fs::TibxPageStore::StreamDescriptor* primary = nullptr;
                for (const auto& descriptor : item.streams) {
                    if (descriptor.selector != 0) continue;
                    if (primary) return false;
                    primary = &descriptor;
                }
                if (primary && primary->id == id && volumeKeys.count(VolumeKey(name, id, version)))
                    groups[LinkKey(id, version)].insert(name);
            }
        }
        std::set<std::string> result;
        const auto sameFilter = [](const fs::TibxPageStore::ResolvedItem& a,
                                   const fs::TibxPageStore::ResolvedItem& b) {
            const fs::TibxPageStore::StreamDescriptor* left = nullptr;
            const fs::TibxPageStore::StreamDescriptor* right = nullptr;
            for (const auto& descriptor : a.streams) if (descriptor.selector == 4) {
                if (left) return false;
                left = &descriptor;
            }
            for (const auto& descriptor : b.streams) if (descriptor.selector == 4) {
                if (right) return false;
                right = &descriptor;
            }
            if (!left || !right) return left == right;
            return left->id == right->id && left->size == right->size && left->flags == right->flags;
        };
        for (const auto& group : groups) {
            for (const auto& name : group.second) {
                if (name.compare(0, 5, "part/") != 0) continue;
                for (const auto& canonical : group.second) {
                    if (canonicalVolumeName(canonical) &&
                        sameFilter(*byName.at(name), *byName.at(canonical))) {
                        result.insert(name);
                        break;
                    }
                }
            }
        }
        aliases->swap(result);
        return true;
    } catch (...) { return false; }
}

struct TibxPart {
    std::uint32_t id;
    std::int64_t start;
    std::int64_t size;
    fs::ByteStorePtr source;
};

class TibxMappedStore final : public fs::IByteStore {
public:
    explicit TibxMappedStore(std::vector<TibxPart> parts) : parts_(std::move(parts)), total_(0) {
        for (const TibxPart& part : parts_) total_ = std::max(total_, part.start + part.size);
    }
    std::int64_t capacity() const override { return total_; }
    bool validate() const override {
        for (const TibxPart& part : parts_)
            if (!part.source || !part.source->validate() || part.source->capacity() != part.size) return false;
        return true;
    }
    bool cheapRandomAccess() const override {
        for (const TibxPart& part : parts_) if (!part.source->cheapRandomAccess()) return false;
        return true;
    }
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (pos < 0 || count <= 0 || pos >= total_) return 0;
        if (!dst) return -1;
        const int wanted = static_cast<int>(std::min<std::int64_t>(count, total_ - pos));
        int done = 0;
        while (done < wanted) {
            const std::int64_t at = pos + done;
            const TibxPart* found = nullptr;
            for (const TibxPart& part : parts_) {
                if (at >= part.start && at - part.start < part.size) { found = &part; break; }
            }
            if (!found) return -1;
            const std::int64_t local = at - found->start;
            const int amount = static_cast<int>(std::min<std::int64_t>(wanted - done, found->size - local));
            const int got = found->source->read(local, dst + done, amount);
            if (got <= 0 || got > amount) return -1;
            done += got;
        }
        return done;
    }
private:
    std::vector<TibxPart> parts_;
    std::int64_t total_;
};

bool tibxVolumeTable(const fs::TibxPageStore::CommittedHeader& header,
                     std::vector<std::pair<std::uint32_t, std::uint64_t> >* entries) {
    std::vector<fs::TibxPageStore::HeaderSection> sections;
    if (!fs::TibxPageStore::readHeaderSections(header, &sections, nullptr) ||
        sections.size() <= 18 || !sections[18].length || sections[18].length % 12) return false;
    const std::uint8_t* data = header.data.data() + sections[18].offset;
    for (std::size_t i = 0; i < sections[18].length / 12; ++i) {
        const std::uint32_t id = be32(data + i * 12);
        const std::uint64_t start = be64(data + i * 12 + 4);
        if ((i == 0 && (id != 0 || start != 0)) ||
            (i && (id <= entries->back().first || start <= entries->back().second))) return false;
        entries->push_back(std::make_pair(id, start));
    }
    return true;
}

fs::ByteStorePtr bootstrapTibxParts(const std::vector<TibxPart>& physical, bool* ioError) {
    *ioError = false;
    if (physical.size() < 2 || physical.front().id != 0) return fs::ByteStorePtr();
    fs::TibxPageStore first(physical.front().source);
    fs::TibxPageStore::Page initial;
    if (!first.readPage(0, &initial, nullptr, ioError) || initial[1] != 1 ||
        std::memcmp(initial.data() + 8, "ARCH", 4) != 0) return fs::ByteStorePtr();

    const TibxPart& last = physical.back();
    fs::TibxPageStore tail(last.source);
    const std::uint64_t tailCount = tail.pageCount();
    for (std::uint64_t page = tailCount; page > 0; --page) {
        fs::TibxPageStore::CommitInfo ci;
        if (!tail.readCommitInfo(page - 1, &ci, ioError)) {
            if (*ioError) return fs::ByteStorePtr();
            continue;
        }
        if ((ci.flags & 2U) || ci.volumeOffset != (page - 1) * fs::TibxPageStore::kPageSize ||
            std::memcmp(ci.archiveId.data(), initial.data() + 40, 16) != 0) continue;
        bool found = false, conflict = false;
        std::uint64_t start = 0;
        for (std::size_t i = 0; i < ci.volumeIds.size(); ++i) {
            if (ci.volumeIds[i] != last.id) continue;
            if (found && start != ci.volumeOffsets[i]) conflict = true;
            start = ci.volumeOffsets[i];
            found = true;
        }
        const std::uint64_t max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
        if (!found || conflict || start < static_cast<std::uint64_t>(physical.front().size) ||
            start > max || static_cast<std::uint64_t>(last.size) > max - start ||
            ci.volumeOffset > max - start || (start + ci.volumeOffset) % fs::TibxPageStore::kPageSize) continue;
        const std::uint64_t baseSize = static_cast<std::uint64_t>(physical.front().size);
        const std::uint64_t lastSize = static_cast<std::uint64_t>(last.size);
        const bool inBase = ci.archiveOffset <= baseSize && ci.archiveSize <= baseSize - ci.archiveOffset;
        const bool inLast = ci.archiveOffset >= start && ci.archiveOffset - start <= lastSize &&
                            ci.archiveSize <= lastSize - (ci.archiveOffset - start);
        if (!ci.archiveSize || (!inBase && !inLast)) continue;

        std::vector<TibxPart> probe;
        probe.push_back(physical.front());
        TibxPart mappedLast = last;
        mappedLast.start = static_cast<std::int64_t>(start);
        probe.push_back(mappedLast);
        fs::ByteStorePtr partial = std::make_shared<TibxMappedStore>(probe);
        fs::TibxPageStore pages(partial);
        fs::TibxPageStore::CommittedHeader header;
        if (!pages.readHeaderAtCommit((start + ci.volumeOffset) / fs::TibxPageStore::kPageSize, &header, ioError)) {
            if (*ioError) return fs::ByteStorePtr();
            continue;
        }
        if (header.offset != ci.archiveOffset || header.sequence != ci.sequence ||
            std::memcmp(header.data.data() + 32, ci.archiveId.data(), 16) != 0) continue;

        std::vector<std::pair<std::uint32_t, std::uint64_t> > table;
        // ARCI supplies a candidate start only; ARCH carries the full volume table.
        // Retention layouts with removed volumes remain unverified on real samples.
        if (!tibxVolumeTable(header, &table) || table.size() != physical.size() || table.back().first != last.id ||
            table.back().second != start) continue;
        std::vector<TibxPart> mapped;
        std::size_t next = 0;
        bool valid = true;
        for (const auto& entry : table) {
            if (!mapped.empty() && entry.second < static_cast<std::uint64_t>(mapped.back().start + mapped.back().size)) {
                valid = false;
                break;
            }
            if (next == physical.size() || physical[next].id != entry.first) { valid = false; break; }
            if (entry.second > max || static_cast<std::uint64_t>(physical[next].size) > max - entry.second ||
                (!mapped.empty() && entry.second < static_cast<std::uint64_t>(mapped.back().start + mapped.back().size))) {
                valid = false;
                break;
            }
            TibxPart part = physical[next++];
            part.start = static_cast<std::int64_t>(entry.second);
            mapped.push_back(part);
        }
        if (valid && next == physical.size()) {
            fs::ByteStorePtr source = std::make_shared<TibxMappedStore>(mapped);
            fs::TibxPageStore complete(source);
            // CRC checks structural consistency, not the identity of each part.
            for (std::size_t i = 1; valid && i < mapped.size(); ++i) {
                fs::TibxPageStore::Page boundary;
                valid = complete.readPage(static_cast<std::uint64_t>(mapped[i].start) /
                                          fs::TibxPageStore::kPageSize, &boundary, nullptr, ioError);
            }
            if (*ioError) return fs::ByteStorePtr();
            if (valid) return source;
        }
    }
    return fs::ByteStorePtr();
}

QStringList splitTibxPath(const QString& path) {
    QStringList result;
    int start = 0;
    int backslashes = 0;
    for (int i = 0; i < path.size(); ++i) {
        if (path[i] == QLatin1Char('\\')) { ++backslashes; continue; }
        if (path[i] == QLatin1Char('/') && !(backslashes & 1)) {
            if (i > start) result.push_back(path.mid(start, i - start));
            start = i + 1;
        }
        backslashes = 0;
    }
    if (start < path.size()) result.push_back(path.mid(start));
    return result;
}

QByteArray passwordDigest(const QByteArray& salt, const QString& password) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(salt);
    QByteArray utf8 = password.toUtf8();
    hash.addData(utf8);
    utf8.fill(0);
    return hash.result();
}

QByteArray privateKeyDigest(const QByteArray& salt, const QByteArray& privateKey) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(salt);
    hash.addData(privateKey);
    return hash.result();
}

QByteArray passphraseDigest(const QByteArray& salt, const QString& passphrase) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(salt);
    const char present = passphrase.isNull() ? 0 : 1;
    hash.addData(&present, 1);
    QByteArray utf8 = passphrase.toUtf8();
    hash.addData(utf8);
    utf8.fill(0);
    return hash.result();
}

struct TibxOpenContext {
    fs::ByteStorePtr source;
    std::int64_t sourceSize;
    std::shared_ptr<fs::TibxVolumeContext> volume;
    fs::TibxPageStore::Superblock itemMap;
    std::vector<fs::TibxPageStore::RecoveryPoint> points;
    QByteArray salt;
    QByteArray passwordHash;
    QByteArray privateKeyHash;
    QByteArray passphraseHash;

    ~TibxOpenContext() { passwordHash.fill(0); privateKeyHash.fill(0); passphraseHash.fill(0); salt.fill(0); }

    bool matches(const QString& password, const QByteArray& privateKey, const QString& passphrase) const {
        QByteArray candidate = passwordDigest(salt, password);
        if (candidate.size() != passwordHash.size()) { candidate.fill(0); return false; }
        unsigned char difference = 0;
        for (int i = 0; i < candidate.size(); ++i)
            difference |= static_cast<unsigned char>(candidate[i]) ^ static_cast<unsigned char>(passwordHash[i]);
        candidate.fill(0);
        candidate = privateKeyDigest(salt, privateKey);
        if (candidate.size() != privateKeyHash.size()) { candidate.fill(0); return false; }
        for (int i = 0; i < candidate.size(); ++i)
            difference |= static_cast<unsigned char>(candidate[i]) ^ static_cast<unsigned char>(privateKeyHash[i]);
        candidate.fill(0);
        candidate = passphraseDigest(salt, passphrase);
        if (candidate.size() != passphraseHash.size()) { candidate.fill(0); return false; }
        for (int i = 0; i < candidate.size(); ++i)
            difference |= static_cast<unsigned char>(candidate[i]) ^ static_cast<unsigned char>(passphraseHash[i]);
        candidate.fill(0);
        return difference == 0;
    }
};

class TibxContextStore final : public fs::IByteStore {
public:
    explicit TibxContextStore(std::shared_ptr<TibxOpenContext> context) : context_(std::move(context)) {}
    std::int64_t capacity() const override { return context_->source->capacity(); }
    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (!validate()) return -1;
        return context_->source->read(pos, dst, count);
    }
    bool validate() const override {
        return context_->source->validate() && context_->source->capacity() == context_->sourceSize;
    }
    bool cheapRandomAccess() const override { return context_->source->cheapRandomAccess(); }
    const std::shared_ptr<TibxOpenContext>& context() const { return context_; }
private:
    std::shared_ptr<TibxOpenContext> context_;
};

}  // namespace

ModulePtr TibxModule::open(const QString& path, const QString& password, const QByteArray& privateKey,
                           const QString& keyPassphrase) {
    const auto failure = [&path](const QString& message) -> ModulePtr {
        auto module = peare::makeUnique<TibxModule>();
        module->info_.filePath = path;
        module->info_.format = ModuleFormat::TIBX;
        module->info_.description = QStringLiteral("Acronis TIBX backup");
        module->info_.error = message;
        return ModulePtr(std::move(module));
    };
    QString primaryPath = path;
    const QFileInfo selected(path);
    auto selectedSource = std::make_shared<fs::QFileByteStore>(path);
    if (!selectedSource->validate()) return failure(QStringLiteral("Cannot open TIBX file"));
    const std::vector<std::uint8_t> head = selectedSource->readRange(0, 12);
    const QRegularExpression partPattern(QStringLiteral("^(.*)-(\\d{4,})$"));
    const QRegularExpressionMatch match = partPattern.match(selected.completeBaseName());
    const QString selectedExtension = selected.suffix().isEmpty() ? QString() : QLatin1Char('.') + selected.suffix();
    if (head.size() < 12 || std::memcmp(head.data() + 8, "ARCH", 4) != 0) {
        if (!match.hasMatch()) return failure(QStringLiteral("Missing first TIBX part"));
        primaryPath = selected.absoluteDir().filePath(match.captured(1) + selectedExtension);
    } else if (match.hasMatch()) {
        const QString candidate = selected.absoluteDir().filePath(match.captured(1) + selectedExtension);
        auto base = std::make_shared<fs::QFileByteStore>(candidate);
        fs::TibxPageStore basePages(base), selectedPages(selectedSource);
        fs::TibxPageStore::Page basePage, selectedPage;
        if (base->validate() && basePages.readPage(0, &basePage, nullptr) &&
            selectedPages.readPage(0, &selectedPage, nullptr) && basePage[1] == 1 &&
            std::memcmp(basePage.data() + 8, "ARCH", 4) == 0 &&
            std::memcmp(basePage.data() + 40, selectedPage.data() + 40, 16) == 0) primaryPath = candidate;
    }
    const QFileInfo info(primaryPath);
    const QString base = info.completeBaseName();
    const QString suffix = info.suffix();
    const QString extension = suffix.isEmpty() ? QString() : QLatin1Char('.') + suffix;
    const QDir dir = info.absoluteDir();
    const QRegularExpression pattern(QStringLiteral("^%1-(\\d{4,})%2$")
        .arg(QRegularExpression::escape(base), QRegularExpression::escape(extension)),
        QRegularExpression::CaseInsensitiveOption);
    QMap<qulonglong, QString> candidates;
    const QStringList names = dir.entryList(QDir::Files);
    bool invalidNumbering = false;
    for (const QString& name : names) {
        const QRegularExpressionMatch match = pattern.match(name);
        if (!match.hasMatch()) continue;
        bool ok = false;
        const qulonglong number = match.captured(1).toULongLong(&ok);
        if (!ok || !number || candidates.contains(number)) { invalidNumbering = true; continue; }
        candidates.insert(number, dir.filePath(name));
    }
    auto first = std::make_shared<fs::QFileByteStore>(primaryPath);
    if (!first->validate()) return failure(QStringLiteral("Cannot open TIBX file"));
    fs::TibxPageStore firstPages(first);
    fs::TibxPageStore::CommittedHeader firstHeader;
    std::vector<fs::TibxPageStore::HeaderSection> firstSections;
    bool ioError = false;
    const bool hasCurrentHeader = firstPages.readCurrentHeader(&firstHeader, &ioError);
    if (ioError) return failure(QStringLiteral("Cannot read TIBX header"));
    if (hasCurrentHeader &&
        fs::TibxPageStore::readHeaderSections(firstHeader, &firstSections, nullptr) &&
        firstSections.size() > 18 && firstSections[18].length == 12) {
        const std::uint8_t* table = firstHeader.data.data() + firstSections[18].offset;
        if (be32(table) == 0 && be64(table + 4) == 0) {
            if (primaryPath != path) return failure(QStringLiteral("Selected file is not a TIBX continuation"));
            return open(first, path, QString(), password, privateKey, keyPassphrase);
        }
    }
    if (invalidNumbering) return failure(QStringLiteral("Invalid TIBX part numbering"));
    std::vector<std::pair<fs::ByteStorePtr, QString> > sources;
    sources.push_back(std::make_pair(first, info.fileName()));
    for (auto it = candidates.cbegin(); it != candidates.cend(); ++it)
        sources.push_back(std::make_pair(std::make_shared<fs::QFileByteStore>(it.value()),
                                         QFileInfo(it.value()).fileName()));
    return openParts(sources, path, password, privateKey, keyPassphrase);
}

ModulePtr TibxModule::openParts(const std::vector<std::pair<fs::ByteStorePtr, QString> >& sources,
                                const QString& name, const QString& password, const QByteArray& privateKey,
                                const QString& keyPassphrase) {
    const auto failure = [&name](const QString& message) -> ModulePtr {
        auto module = peare::makeUnique<TibxModule>();
        module->info_.filePath = name;
        module->info_.format = ModuleFormat::TIBX;
        module->info_.description = QStringLiteral("Acronis TIBX backup");
        module->info_.error = message;
        return ModulePtr(std::move(module));
    };
    if (sources.empty() || !sources[0].first || !sources[0].first->validate())
        return failure(QStringLiteral("Cannot open TIBX first part"));
    const fs::ByteStorePtr first = sources[0].first;
    if (sources.size() == 1) return open(first, name, QString(), password, privateKey, keyPassphrase);
    const QFileInfo firstName(sources[0].second);
    const QString base = firstName.completeBaseName();
    const QString extension = firstName.suffix().isEmpty() ? QString() : QLatin1Char('.') + firstName.suffix();
    if (base.isEmpty() || QRegularExpression(QStringLiteral("-\\d{4,}$")).match(base).hasMatch())
        return failure(QStringLiteral("Ambiguous TIBX first part name"));
    const QRegularExpression pattern(QStringLiteral("^%1-(\\d{4,})%2$")
        .arg(QRegularExpression::escape(base), QRegularExpression::escape(extension)),
        QRegularExpression::CaseInsensitiveOption);
    std::vector<TibxPart> physical;
    const TibxPart basePart = {0, 0, first->capacity(), first};
    if (basePart.size <= 0) return failure(QStringLiteral("Invalid TIBX first part"));
    physical.push_back(basePart);
    std::int64_t total = first->capacity();
    qulonglong previousId = 0;
    for (std::size_t i = 1; i < sources.size(); ++i) {
        const QRegularExpressionMatch match = pattern.match(QFileInfo(sources[i].second).fileName());
        bool ok = false;
        const qulonglong id = match.hasMatch() ? match.captured(1).toULongLong(&ok) : 0;
        if (!ok || id <= previousId || id > std::numeric_limits<std::uint32_t>::max())
            return failure(QStringLiteral("Invalid TIBX part numbering"));
        previousId = id;
        const fs::ByteStorePtr& part = sources[i].first;
        if (!part || !part->validate() || part->capacity() <= 0 ||
            part->capacity() > std::numeric_limits<std::int64_t>::max() - total) {
            return failure(QStringLiteral("Invalid TIBX continuation"));
        }
        const TibxPart entry = {static_cast<std::uint32_t>(id), total, part->capacity(), part};
        physical.push_back(entry);
        total += part->capacity();
    }
    fs::TibxPageStore firstPages(first);
    bool ioError = false;
    bool misaligned = false;
    for (std::size_t i = 1; i < physical.size(); ++i)
        if (physical[i].start % fs::TibxPageStore::kPageSize) misaligned = true;
    {
        fs::ByteStorePtr raw = std::make_shared<TibxMappedStore>(physical);
        fs::TibxPageStore pages(raw);
        fs::TibxPageStore::CommittedHeader header;
        fs::TibxPageStore::Page initial;
        std::vector<std::pair<std::uint32_t, std::uint64_t> > table;
        bool valid = firstPages.readPage(0, &initial, nullptr, &ioError) && initial[1] == 1 &&
            std::memcmp(initial.data() + 8, "ARCH", 4) == 0 &&
            pages.readCommittedHeader(&header, nullptr, &ioError) &&
            std::memcmp(initial.data() + 40, header.data.data() + 32, 16) == 0 &&
            tibxVolumeTable(header, &table) &&
            be64(header.data.data() + 424) >= static_cast<std::uint64_t>(physical.back().start);
        if (ioError) return failure(QStringLiteral("Cannot read TIBX continuation"));
        if (valid) {
            bool matches = table.size() == physical.size();
            for (std::size_t i = 0; matches && i < physical.size(); ++i)
                matches = table[i].first == physical[i].id &&
                          table[i].second == static_cast<std::uint64_t>(physical[i].start);
            // Boundary CRCs validate pages, not individual file identities.
            for (std::size_t i = 1; matches && i < physical.size(); ++i) {
                fs::TibxPageStore::Page boundary;
                matches = pages.readPage(static_cast<std::uint64_t>(physical[i].start) /
                                         fs::TibxPageStore::kPageSize, &boundary, nullptr, &ioError);
            }
            if (ioError) return failure(QStringLiteral("Cannot read TIBX continuation"));
            if (matches) return open(raw, name, QString(), password, privateKey, keyPassphrase);
        }
    }
    fs::ByteStorePtr source = misaligned ? fs::ByteStorePtr() : bootstrapTibxParts(physical, &ioError);
    if (ioError) return failure(QStringLiteral("Cannot read TIBX continuation"));
    if (source) return open(source, name, QString(), password, privateKey, keyPassphrase);
    return failure(QStringLiteral("Cannot verify TIBX continuation table"));
}

ModulePtr TibxModule::open(const fs::ByteStorePtr& source, const QString& name, const QString& subPath,
                           const QString& password, const QByteArray& privateKey, const QString& keyPassphrase) {
    auto module = peare::makeUnique<TibxModule>();
    module->info_.filePath = name;
    module->info_.format = ModuleFormat::TIBX;
    module->info_.description = QStringLiteral("Acronis TIBX backup");
    const auto existing = std::dynamic_pointer_cast<TibxContextStore>(source);
    if (existing && !existing->context()->matches(password, privateKey, keyPassphrase))
        return open(existing->context()->source, name, subPath, password, privateKey, keyPassphrase);
    if (existing && !existing->validate()) {
        module->info_.error = QStringLiteral("Cannot read TIBX source");
        return ModulePtr(std::move(module));
    }
    std::shared_ptr<TibxOpenContext> opened;
    if (existing) {
        opened = existing->context();
    } else {
        fs::TibxPageStore pages(source);
        fs::TibxPageStore::CommittedHeader header;
        std::vector<fs::TibxPageStore::HeaderSection> sections;
        fs::TibxPageStore::Superblock itemMap, dataMap, segmentMap, sliceMap;
        std::string error;
        if (!pages.readCommittedHeader(&header, &error) ||
            !fs::TibxPageStore::readHeaderSections(header, &sections, &error) ||
            sections.size() < 6 ||
            !fs::TibxPageStore::readSuperblock(header, sections[0], &itemMap, &error) ||
            !fs::TibxPageStore::readSuperblock(header, sections[1], &dataMap, &error) ||
            !fs::TibxPageStore::readSuperblock(header, sections[2], &segmentMap, &error) ||
            !fs::TibxPageStore::readSuperblock(header, sections[5], &sliceMap, &error)) {
            module->info_.error = error.empty() ? QStringLiteral("Invalid TIBX header") : QString::fromStdString(error);
            return ModulePtr(std::move(module));
        }
        fs::TibxPageStore::Superblock keyMap;
        std::vector<fs::TibxPageStore::TreeCell> keyCells;
        if (sections.size() > 7 &&
            (!fs::TibxPageStore::readSuperblock(header, sections[7], &keyMap, &error) ||
             !pages.readTree(header, keyMap, &keyCells, &error))) {
            module->info_.error = QString::fromStdString(error);
            return ModulePtr(std::move(module));
        }
        bool passwordProtected = false, publicKeyProtected = false;
        for (const auto& cell : keyCells) {
            if (!cell.deleted && !cell.key.empty() && cell.key[0] == 1 &&
                !cell.value.empty() && cell.value[0] == 1)
                passwordProtected = true;
            if (!cell.deleted && !cell.key.empty() && cell.key[0] == 2 &&
                !cell.value.empty() && cell.value[0] == 2)
                publicKeyProtected = true;
        }
        if (passwordProtected || publicKeyProtected) {
            if (password.isEmpty() && privateKey.isEmpty()) {
                module->info_.error = publicKeyProtected
                    ? (passwordProtected ? QStringLiteral("TIBX password or private key required")
                                         : QStringLiteral("TIBX private key required"))
                    : QStringLiteral("TIBX password required");
                module->info_.openError = publicKeyProtected
                    ? (passwordProtected ? ModuleOpenError::PasswordOrPrivateKeyRequired
                                         : ModuleOpenError::PrivateKeyRequired)
                    : ModuleOpenError::PasswordRequired;
                return ModulePtr(std::move(module));
            }
            bool unlocked = false;
            ModuleOpenError keyError = ModuleOpenError::PrivateKeyInvalidOrCorrupt;
            if (publicKeyProtected && !privateKey.isEmpty()) {
                std::string pem;
                SecretGuard<std::string> pemGuard(pem);
                bool keyReady = true;
                if (privateKey.contains("-----BEGIN ENCRYPTED PRIVATE KEY-----")) {
                    std::vector<std::uint8_t> encrypted(privateKey.constData(), privateKey.constData() + privateKey.size());
                    SecretGuard<std::vector<std::uint8_t>> encryptedGuard(encrypted);
                    QByteArray passUtf8 = keyPassphrase.toUtf8();
                    SecretByteArrayGuard passGuard(passUtf8);
                    std::vector<std::uint8_t> pass(passUtf8.constData(), passUtf8.constData() + passUtf8.size());
                    SecretGuard<std::vector<std::uint8_t>> passBytesGuard(pass);
                    std::vector<std::uint8_t> der;
                    SecretGuard<std::vector<std::uint8_t>> derGuard(der);
                    const fs::TibxEncryptedPemResult result = fs::tibxDecryptEncryptedPem(
                        encrypted, !keyPassphrase.isNull(), pass, &der);
                    if (result == fs::TibxEncryptedPemResult::Success &&
                        der.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max() / 2)) {
                        QByteArray encoded = QByteArray::fromRawData(
                            reinterpret_cast<const char*>(der.data()), static_cast<int>(der.size())).toBase64();
                        SecretByteArrayGuard encodedGuard(encoded);
                        pem.reserve(static_cast<std::size_t>(encoded.size()) + 64);
                        pem = "-----BEGIN PRIVATE KEY-----\n";
                        pem.append(encoded.constData(), static_cast<std::size_t>(encoded.size()));
                        pem += "\n-----END PRIVATE KEY-----\n";
                    } else {
                        keyReady = false;
                        if (result == fs::TibxEncryptedPemResult::PassphraseRequired)
                            keyError = ModuleOpenError::PrivateKeyPassphraseRequired;
                        else if (result == fs::TibxEncryptedPemResult::IncorrectPassphraseOrCorrupt)
                            keyError = ModuleOpenError::PrivateKeyPassphraseInvalid;
                        else if (result == fs::TibxEncryptedPemResult::WorkLimitExceeded)
                            keyError = ModuleOpenError::PrivateKeyKdfLimitExceeded;
                    }
                } else {
                    pem.assign(privateKey.constData(), static_cast<std::size_t>(privateKey.size()));
                }
                if (keyReady) {
                    bool resourceFailure = false;
                    unlocked = pages.unlockPrivateKey(header, keyMap, pem, &error, &resourceFailure);
                    if (resourceFailure) {
                        module->info_.error = QString::fromStdString(error);
                        return ModulePtr(std::move(module));
                    }
                }
            }
            if (!unlocked && passwordProtected && !password.isEmpty())
                unlocked = pages.unlock(header, keyMap, password.toUtf8().toStdString(), &error);
            if (!unlocked) {
                module->info_.error = error.empty() ? QStringLiteral("Invalid TIBX credential") : QString::fromStdString(error);
                module->info_.openError = keyError == ModuleOpenError::PrivateKeyKdfLimitExceeded
                    ? keyError : !password.isEmpty() && passwordProtected
                    ? ModuleOpenError::PasswordInvalidOrCorrupt
                    : (!privateKey.isEmpty() && publicKeyProtected
                        ? keyError
                        : (publicKeyProtected ? ModuleOpenError::PrivateKeyRequired
                                              : ModuleOpenError::PasswordRequired));
                return ModulePtr(std::move(module));
            }
        }
        std::vector<fs::TibxPageStore::RecoveryPoint> points;
        if (!pages.readRecoveryPoints(header, sliceMap, &points, &error)) {
            module->info_.error = QString::fromStdString(error);
            return ModulePtr(std::move(module));
        }
        auto context = std::make_shared<fs::TibxVolumeContext>(source, header, dataMap, segmentMap);
        context->pages = pages;
        opened = std::make_shared<TibxOpenContext>();
        opened->source = source;
        opened->sourceSize = source->capacity();
        opened->volume = context;
        opened->itemMap = std::move(itemMap);
        opened->points = std::move(points);
        opened->salt = QUuid::createUuid().toRfc4122();
        opened->passwordHash = passwordDigest(opened->salt, password);
        opened->privateKeyHash = privateKeyDigest(opened->salt, privateKey);
        opened->passphraseHash = passphraseDigest(opened->salt, keyPassphrase);
    }
    const auto& context = opened->volume;
    const auto& pages = context->pages;
    const auto& header = context->header;
    const auto& itemMap = opened->itemMap;
    const auto& points = opened->points;
    std::string error;
    fs::ByteStorePtr directorySource = existing ? source : std::make_shared<TibxContextStore>(opened);
    const QStringList requested = splitTibxPath(subPath);
    QSet<QString> directories;
    const auto addDirectory = [&](const QString& child) {
        if (directories.contains(child)) return;
        directories.insert(child);
        ResourceEntry entry;
        entry.name = child;
        entry.type = QStringLiteral("TIBX_DIR");
        entry.language = QStringLiteral("neutral");
        entry.format = ModuleFormat::TIBX;
        entry.content = directorySource;
        entry.isDirectory = true;
        entry.containerSubPath = subPath.isEmpty() ? child : subPath + QLatin1Char('/') + child;
        module->resources_.push_back(std::move(entry));
    };
    if (points.size() > 1 && requested.isEmpty()) {
        for (const fs::TibxPageStore::RecoveryPoint& point : points) {
            if (!point.hidden) addDirectory(QString::number(point.sliceId));
        }
        return ModulePtr(std::move(module));
    }
    if (points.size() == 1 && points[0].hidden && requested.isEmpty())
        return ModulePtr(std::move(module));
    bool directoryExists = subPath.isEmpty();
    for (const fs::TibxPageStore::RecoveryPoint& point : points) {
        QStringList parent = requested;
        if (points.size() > 1) {
            if (parent.isEmpty() || parent.takeFirst() != QString::number(point.sliceId)) continue;
            if (parent.isEmpty()) directoryExists = true;
        }
        const QString parentPath = parent.join(QLatin1Char('/'));
        std::vector<fs::TibxPageStore::Volume> volumes;
        std::vector<fs::TibxPageStore::FileItem> files;
        bool fileDirectoryExists = false;
        if (!pages.readDirectoryItems(header, itemMap, point.fullId, point.sliceId,
                                      parentPath.toUtf8().toStdString(), &volumes,
                                      &files, &fileDirectoryExists, &error)) {
            module->info_.error = QString::fromStdString(error);
            module->resources_.clear();
            return ModulePtr(std::move(module));
        }
        directoryExists = directoryExists || fileDirectoryExists;
        const auto recoveryVolumes =
            std::make_shared<const std::vector<fs::TibxPageStore::Volume> >(volumes);
        const auto recoveryItems = std::make_shared<fs::TibxRecoveryItems>(
            context, itemMap, point.fullId, point.sliceId);
        std::set<std::string> linkedAliases;
        if (parentPath.isEmpty() || parentPath == QStringLiteral("part"))
            linkedVolumeAliases(pages, header, itemMap, point.fullId, point.sliceId, volumes, &linkedAliases);
        for (const fs::TibxPageStore::Volume& volume : volumes) {
            if (linkedAliases.count(volume.name)) continue;
            if (volume.size > std::uint64_t(std::numeric_limits<std::int64_t>::max()) ||
                volume.name.size() > std::size_t(std::numeric_limits<int>::max())) {
                module->info_.error = QStringLiteral("TIBX volume is too large");
                module->resources_.clear();
                return ModulePtr(std::move(module));
            }
            const QByteArray rawName(volume.name.data(), int(volume.name.size()));
            const QString decodedName = QString::fromUtf8(rawName);
            if (rawName.contains('\0') || decodedName.toUtf8() != rawName) {
                module->info_.error = QStringLiteral("Unsupported TIBX volume name encoding");
                module->resources_.clear();
                return ModulePtr(std::move(module));
            }
            QStringList path = splitTibxPath(decodedName);
            if (path.isEmpty()) {
                module->info_.error = QStringLiteral("Invalid TIBX volume name");
                module->resources_.clear();
                return ModulePtr(std::move(module));
            }
            if (path.size() <= parent.size()) continue;
            bool matches = true;
            for (int i = 0; i < parent.size(); ++i) if (path[i] != parent[i]) { matches = false; break; }
            if (!matches) continue;
            directoryExists = true;
            if (path.size() > parent.size() + 1) {
                addDirectory(path[parent.size()]);
                continue;
            }
            ResourceEntry entry;
            entry.name = path.takeLast();
            entry.type = QStringLiteral("TIBX_VOLUME");
            entry.language = QStringLiteral("neutral");
            entry.dataSize = volume.size;
            entry.content = std::make_shared<fs::TibxVolumeStore>(
                context, volume.id, static_cast<std::int64_t>(volume.size), volume.sliceId,
                recoveryVolumes, recoveryItems, volume.name);
            entry.isEmbeddedFile = true;
            module->resources_.push_back(std::move(entry));
        }
        for (const fs::TibxPageStore::FileItem& file : files) {
            if (file.size > std::uint64_t(std::numeric_limits<std::int64_t>::max()) ||
                file.name.size() > std::size_t(std::numeric_limits<int>::max())) {
                module->info_.error = QStringLiteral("TIBX file is too large");
                module->resources_.clear();
                return ModulePtr(std::move(module));
            }
            const QByteArray rawName(file.name.data(), int(file.name.size()));
            const QString decodedName = QString::fromUtf8(rawName);
            if (rawName.contains('\0') || decodedName.toUtf8() != rawName) {
                module->info_.error = QStringLiteral("Unsupported TIBX file name encoding");
                module->resources_.clear();
                return ModulePtr(std::move(module));
            }
            QStringList path = splitTibxPath(decodedName);
            if (path.isEmpty()) {
                module->info_.error = QStringLiteral("Invalid TIBX file name");
                module->resources_.clear();
                return ModulePtr(std::move(module));
            }
            if (file.directory) {
                addDirectory(path.last());
                continue;
            }
            ResourceEntry entry;
            entry.name = path.takeLast();
            entry.type = QStringLiteral("TIBX_FILE");
            entry.language = QStringLiteral("neutral");
            entry.dataSize = file.size;
            entry.content = std::make_shared<fs::TibxVolumeStore>(
                context, file.id, static_cast<std::int64_t>(file.size), file.sliceId,
                recoveryVolumes, recoveryItems);
            entry.isEmbeddedFile = true;
            module->resources_.push_back(std::move(entry));
        }
    }
    if (!directoryExists) module->info_.error = QStringLiteral("TIBX directory not found");
    else if (subPath.isEmpty() && module->resources_.isEmpty())
        module->info_.error = QStringLiteral("No supported TIBX volumes found");
    return ModulePtr(std::move(module));
}

}  // namespace peare
