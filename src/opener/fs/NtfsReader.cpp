#include "NtfsReader.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace peare {
namespace fs {
namespace {

std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}
std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
std::uint64_t le64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(le32(p)) | (static_cast<std::uint64_t>(le32(p + 4)) << 32);
}

// Little-endian variable-length integer used in data runs. `sign` extends the
// most-significant bit (the run-offset delta is signed; the length is not).
std::int64_t readVarInt(const std::uint8_t* p, int n, bool sign) {
    std::int64_t v = 0;
    for (int i = 0; i < n; ++i) v |= static_cast<std::int64_t>(p[i]) << (8 * i);
    if (sign && n > 0 && (p[n - 1] & 0x80))
        v |= -(std::int64_t(1) << (8 * n));  // sign-extend
    return v;
}

std::string utf16leToUtf8(const std::uint8_t* p, int chars) {
    std::string out;
    for (int i = 0; i < chars; ++i) {
        const unsigned int ch = le16(p + i * 2);
        if (ch < 0x80) {
            out.push_back(static_cast<char>(ch));
        } else if (ch < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (ch >> 6)));
            out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (ch >> 12)));
            out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
        }
    }
    return out;
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

void splitPath(const std::string& path, std::vector<std::string>* out) {
    std::string cur;
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!cur.empty()) { out->push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out->push_back(cur);
}

const std::uint32_t kAttrData = 0x80;
const std::uint32_t kAttrIndexRoot = 0x90;
const std::uint32_t kAttrIndexAllocation = 0xA0;

}  // namespace

NtfsReader::NtfsReader(ByteStorePtr disc) : disc_(std::move(disc)) {
    try { parse(); }
    catch (...) { if (error_.empty()) error_ = "NTFS parse error"; valid_ = false; }
}

void NtfsReader::applyFixup(std::vector<std::uint8_t>& buf) {
    if (buf.size() < 8) return;
    const std::uint16_t usnOffset = le16(buf.data() + 0x04);
    const std::uint16_t usnCount = le16(buf.data() + 0x06);
    if (static_cast<std::size_t>(usnOffset) + static_cast<std::size_t>(usnCount) * 2 > buf.size()) return;
    for (std::uint16_t i = 1; i < usnCount; ++i) {
        const std::size_t tail = static_cast<std::size_t>(i) * 512 - 2;
        if (tail + 2 > buf.size()) break;
        buf[tail] = buf[usnOffset + 2 * i];
        buf[tail + 1] = buf[usnOffset + 2 * i + 1];
    }
}

std::vector<NtfsReader::Run> NtfsReader::parseRunlist(const std::uint8_t* p, std::size_t len) const {
    std::vector<Run> runs;
    std::size_t pos = 0;
    std::int64_t lcn = 0;
    while (pos < len && p[pos] != 0) {
        const int lenSize = p[pos] & 0x0F;
        const int offSize = (p[pos] >> 4) & 0x0F;
        if (lenSize == 0 || pos + 1 + lenSize + offSize > len) break;
        const std::int64_t runLen = readVarInt(p + pos + 1, lenSize, false);
        Run r;
        r.length = runLen;
        if (offSize == 0) {
            r.lcn = -1;  // sparse
        } else {
            lcn += readVarInt(p + pos + 1 + lenSize, offSize, true);
            r.lcn = lcn;
        }
        runs.push_back(r);
        pos += 1 + lenSize + offSize;
    }
    return runs;
}

ByteStorePtr NtfsReader::runsStore(const std::vector<Run>& runs, std::uint64_t realSize) const {
    std::vector<ByteStorePtr> parts;
    for (const Run& r : runs) {
        const std::int64_t bytes = r.length * bytesPerCluster_;
        if (r.lcn < 0)
            parts.push_back(std::make_shared<ZeroStore>(bytes));
        else
            parts.push_back(std::make_shared<SubStore>(
                disc_, r.lcn * static_cast<std::int64_t>(bytesPerCluster_), bytes));
    }
    ByteStorePtr concat = std::make_shared<ConcatStore>(std::move(parts));
    const std::int64_t n =
        std::min<std::int64_t>(concat->capacity(), static_cast<std::int64_t>(realSize));
    return std::make_shared<SubStore>(concat, 0, n);
}

NtfsReader::Record NtfsReader::parseRecord(std::vector<std::uint8_t>& buf) const {
    Record rec;
    if (buf.size() < 0x30 || std::memcmp(buf.data(), "FILE", 4) != 0) return rec;
    applyFixup(buf);
    const std::uint16_t flags = le16(buf.data() + 0x16);
    rec.isDirectory = (flags & 0x0002) != 0;
    std::size_t focus = le16(buf.data() + 0x14);  // first attribute offset

    while (focus + 4 <= buf.size()) {
        const std::uint32_t type = le32(buf.data() + focus);
        if (type == 0xFFFFFFFFu) break;
        if (focus + 0x10 > buf.size()) break;
        const std::uint32_t length = le32(buf.data() + focus + 0x04);
        if (length < 0x10 || focus + length > buf.size()) break;
        const std::uint8_t* a = buf.data() + focus;

        Attr attr;
        attr.type = type;
        attr.nonResident = a[0x08] != 0;
        const std::uint8_t nameLen = a[0x09];
        const std::uint16_t nameOff = le16(a + 0x0A);
        attr.flags = le16(a + 0x0C);
        if (nameLen && static_cast<std::uint32_t>(nameOff) + static_cast<std::uint32_t>(nameLen) * 2 <= length)
            attr.name = utf16leToUtf8(a + nameOff, nameLen);

        if (!attr.nonResident) {
            const std::uint32_t dataLen = le32(a + 0x10);
            const std::uint16_t dataOff = le16(a + 0x14);
            if (dataOff + dataLen <= length) {
                attr.residentData.assign(a + dataOff, a + dataOff + dataLen);
                attr.realSize = dataLen;
            }
        } else {
            attr.realSize = le64(a + 0x30);  // real (used) size
            const std::uint16_t runsOff = le16(a + 0x20);
            if (runsOff < length)
                attr.runs = parseRunlist(a + runsOff, length - runsOff);
        }
        rec.attrs.push_back(std::move(attr));
        focus += length;
    }
    rec.valid = true;
    return rec;
}

NtfsReader::Record NtfsReader::readRecord(std::uint64_t index) const {
    Record rec;
    if (!mftStore_) return rec;
    std::vector<std::uint8_t> buf =
        mftStore_->readRange(static_cast<std::int64_t>(index) * mftRecordSize_, mftRecordSize_);
    if (static_cast<std::uint32_t>(buf.size()) < mftRecordSize_) return rec;
    return parseRecord(buf);
}

const NtfsReader::Attr* NtfsReader::findAttr(const Record& rec, std::uint32_t type,
                                             const std::string& name) const {
    for (const Attr& a : rec.attrs)
        if (a.type == type && a.name == name) return &a;
    return nullptr;
}

std::vector<std::uint8_t> NtfsReader::attrBytes(const Attr& a) const {
    if (!a.nonResident) return a.residentData;
    return runsStore(a.runs, a.realSize)->readAll();
}

void NtfsReader::parseIndexNode(const std::uint8_t* node, std::size_t nodeLen,
                                std::size_t entriesOffset, std::vector<DirEntry>& out) const {
    std::size_t pos = entriesOffset;
    while (pos + 0x10 <= nodeLen) {
        const std::uint8_t* e = node + pos;
        const std::uint16_t entryLen = le16(e + 0x08);
        const std::uint16_t keyLen = le16(e + 0x0A);
        const std::uint16_t flags = le16(e + 0x0C);
        if (entryLen < 0x10 || pos + entryLen > nodeLen) break;
        if (flags & 0x02) break;  // End entry: no key
        if (keyLen >= 0x42 && pos + 0x10 + keyLen <= nodeLen) {
            const std::uint64_t ref = le64(e) & 0x0000FFFFFFFFFFFFull;  // MFT index
            const std::uint8_t* key = e + 0x10;  // $FILE_NAME
            const std::uint32_t fnFlags = le32(key + 0x38);
            const std::uint64_t realSize = le64(key + 0x30);
            const std::uint8_t nameLen = key[0x40];
            const std::uint8_t nameSpace = key[0x41];
            if (nameSpace != 2 && 0x42 + nameLen * 2 <= keyLen) {  // skip DOS 8.3 names
                DirEntry de;
                de.name = utf16leToUtf8(key + 0x42, nameLen);
                de.mftRef = ref;
                de.isDirectory = (fnFlags & 0x10000000u) != 0;
                de.size = realSize;
                // Hide NTFS metafiles ($...) and the directory self/parent links.
                if (!de.name.empty() && de.name[0] != '$' && de.name != "." && de.name != "..")
                    out.push_back(de);
            }
        }
        pos += entryLen;
    }
}

std::vector<NtfsReader::DirEntry> NtfsReader::readDirectory(const Record& rec) const {
    std::vector<DirEntry> out;
    const Attr* root = findAttr(rec, kAttrIndexRoot, "$I30");
    if (!root) return out;
    const std::vector<std::uint8_t> rb = root->residentData;  // $INDEX_ROOT is resident
    if (rb.size() < 0x20) return out;
    // IndexHeader at 0x10; entries at 0x10 + OffsetToFirstEntry.
    const std::uint32_t offFirst = le32(rb.data() + 0x10);
    parseIndexNode(rb.data(), rb.size(), 0x10 + offFirst, out);

    // Large directories keep further entries in $INDEX_ALLOCATION (INDX blocks).
    const Attr* alloc = findAttr(rec, kAttrIndexAllocation, "$I30");
    if (alloc) {
        const std::uint32_t indexBlockSize = le32(rb.data() + 0x08);
        if (indexBlockSize >= 0x18) {
            std::vector<std::uint8_t> content = attrBytes(*alloc);
            for (std::size_t off = 0; off + indexBlockSize <= content.size(); off += indexBlockSize) {
                std::vector<std::uint8_t> block(content.begin() + off,
                                                content.begin() + off + indexBlockSize);
                if (std::memcmp(block.data(), "INDX", 4) != 0) continue;
                applyFixup(block);
                const std::uint32_t bOffFirst = le32(block.data() + 0x18);  // header at 0x18
                parseIndexNode(block.data(), block.size(), 0x18 + bOffFirst, out);
            }
        }
    }
    return out;
}

bool NtfsReader::resolvePath(const std::string& path, std::uint64_t* mftRef) const {
    std::uint64_t current = 5;  // root directory record
    std::vector<std::string> parts;
    splitPath(path, &parts);
    for (const std::string& comp : parts) {
        Record rec = readRecord(current);
        if (!rec.valid || !rec.isDirectory) return false;
        const std::vector<DirEntry> entries = readDirectory(rec);
        const std::string want = toLower(comp);
        std::uint64_t next = 0;
        bool found = false;
        for (const DirEntry& e : entries) {
            if (toLower(e.name) == want) { next = e.mftRef; found = true; break; }
        }
        if (!found) return false;
        current = next;
    }
    *mftRef = current;
    return true;
}

void NtfsReader::parse() {
    std::vector<std::uint8_t> bpb = disc_->readRange(0, 512);
    if (bpb.size() < 512 || std::memcmp(bpb.data() + 3, "NTFS    ", 8) != 0) {
        error_ = "Not an NTFS volume";
        return;
    }
    const std::uint16_t bytesPerSector = le16(bpb.data() + 0x0B);
    const std::uint8_t secPerClus = bpb[0x0D];
    if (bytesPerSector == 0 || secPerClus == 0) { error_ = "Invalid NTFS BPB"; return; }
    bytesPerCluster_ = static_cast<std::uint32_t>(bytesPerSector) * secPerClus;
    const std::int64_t mftLcn = static_cast<std::int64_t>(le64(bpb.data() + 0x30));
    const std::int8_t rawRec = static_cast<std::int8_t>(bpb[0x40]);
    mftRecordSize_ = rawRec >= 0 ? static_cast<std::uint32_t>(rawRec) * bytesPerCluster_
                                 : (1u << (-rawRec));
    if (mftRecordSize_ < 42 || mftRecordSize_ > (1u << 20)) { error_ = "Invalid MFT record size"; return; }

    // Bootstrap: read $MFT (record 0) directly, then use its $DATA runlist as the
    // positioned store for every other record.
    std::vector<std::uint8_t> rec0 =
        disc_->readRange(mftLcn * static_cast<std::int64_t>(bytesPerCluster_), mftRecordSize_);
    if (static_cast<std::uint32_t>(rec0.size()) < mftRecordSize_) { error_ = "Truncated MFT"; return; }
    Record mft = parseRecord(rec0);
    const Attr* mftData = findAttr(mft, kAttrData, std::string());
    if (!mftData || !mftData->nonResident || mftData->runs.empty()) {
        error_ = "Cannot locate $MFT data runs";
        return;
    }
    mftStore_ = runsStore(mftData->runs, mftData->realSize);
    valid_ = true;
}

std::vector<DiscEntry> NtfsReader::list(const std::string& dirPath) const {
    std::vector<DiscEntry> out;
    if (!valid_) return out;
    std::uint64_t ref = 0;
    if (!resolvePath(dirPath, &ref)) return out;
    Record rec = readRecord(ref);
    if (!rec.valid || !rec.isDirectory) return out;
    for (const DirEntry& e : readDirectory(rec)) {
        DiscEntry d;
        d.name = e.name;
        d.isDirectory = e.isDirectory;
        d.length = e.isDirectory ? 0 : static_cast<std::int64_t>(e.size);
        out.push_back(d);
    }
    return out;
}

ByteStorePtr NtfsReader::openFile(const std::string& path) const {
    if (!valid_) return nullptr;
    std::uint64_t ref = 0;
    if (!resolvePath(path, &ref)) return nullptr;
    Record rec = readRecord(ref);
    if (!rec.valid || rec.isDirectory) return nullptr;
    const Attr* data = findAttr(rec, kAttrData, std::string());
    if (!data) return nullptr;
    // Compressed data (LZNT1) is not decoded yet; expose nothing rather than junk.
    if (data->flags & 0x0001) return nullptr;
    if (!data->nonResident)
        return std::make_shared<MemoryStore>(data->residentData);
    return runsStore(data->runs, data->realSize);
}

}  // namespace fs
}  // namespace peare
