#include "UdfReader.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

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

bool inRange(const std::vector<std::uint8_t>& v, std::size_t off, std::size_t need) {
    return off + need <= v.size();
}

// Append a Unicode code unit (BMP) to a UTF-8 string.
void appendUtf8(std::string& out, unsigned int ch) {
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

// Decode an OSTA compressed-unicode d-string (== UdfUtilities.ReadDCharacters).
std::string readDChars(const std::uint8_t* p, std::size_t len) {
    std::string out;
    if (len == 0) return out;
    const std::uint8_t alg = p[0];
    std::size_t pos = 1;
    if (alg == 16) {
        while (pos + 1 < len) {
            const unsigned int ch = (static_cast<unsigned int>(p[pos]) << 8) | p[pos + 1];
            appendUtf8(out, ch);
            pos += 2;
        }
        if (pos < len) appendUtf8(out, p[pos]);
    } else {  // alg 8 (and lenient fallback): one byte per character (Latin-1)
        for (; pos < len; ++pos) appendUtf8(out, p[pos]);
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

}  // namespace

UdfReader::UdfReader(ByteStorePtr disc) : disc_(std::move(disc)) {
    try {
        parse();
    } catch (const std::exception& e) {
        valid_ = false;
        error_ = e.what();
    } catch (...) {
        valid_ = false;
        error_ = "UDF parse error";
    }
}

bool UdfReader::probeSectorSize(std::uint32_t size) const {
    const std::int64_t at = static_cast<std::int64_t>(256) * size;
    if (disc_->capacity() < at + 16) return false;
    std::vector<std::uint8_t> tag = disc_->readRange(at, 16);
    if (tag.size() < 16) return false;
    // Anchor Volume Descriptor Pointer (tag id 2) whose tag location == 256.
    return le16(tag.data()) == 2 && le32(tag.data() + 12) == 256;
}

void UdfReader::parse() {
    // Probe the physical sector size (most common first), like DiscUtils.
    const std::uint32_t candidates[] = {2048, 512, 4096, 1024};
    bool found = false;
    for (std::uint32_t s : candidates) {
        if (probeSectorSize(s)) { sectorSize_ = s; found = true; break; }
    }
    if (!found) { error_ = "Not a UDF volume (no anchor at sector 256)"; return; }

    // Anchor at sector 256 -> main volume descriptor sequence location.
    std::vector<std::uint8_t> avdp =
        disc_->readRange(static_cast<std::int64_t>(256) * sectorSize_, sectorSize_);
    if (avdp.size() < 24) { error_ = "Truncated UDF anchor"; return; }
    const std::uint32_t mainVds = le32(avdp.data() + 20);  // ExtentDescriptor.Location

    // Walk the volume descriptor sequence.
    std::map<std::uint16_t, std::pair<std::uint32_t, std::uint32_t>> physical;  // num -> (start,len)
    std::uint32_t logicalBlockSize = sectorSize_;
    std::vector<std::uint16_t> mapPartitionNumbers;  // Type1 maps, in order
    bool haveLvd = false;
    Icb fsdIcb;

    std::uint32_t sector = mainVds;
    for (int guard = 0; guard < 256; ++guard, ++sector) {
        std::vector<std::uint8_t> d =
            disc_->readRange(static_cast<std::int64_t>(sector) * sectorSize_, sectorSize_);
        if (d.size() < 16) break;
        const std::uint16_t tagId = le16(d.data());
        if (tagId == 0) break;                 // not a valid descriptor
        if (tagId == 8) break;                 // TerminatingDescriptor

        if (tagId == 5 && inRange(d, 196, 0)) {  // PartitionDescriptor
            const std::uint16_t number = le16(d.data() + 22);
            const std::uint32_t start = le32(d.data() + 188);
            const std::uint32_t length = le32(d.data() + 192);
            physical[number] = std::make_pair(start, length);
        } else if (tagId == 6) {               // LogicalVolumeDescriptor
            haveLvd = true;
            logicalBlockSize = le32(d.data() + 212);
            // FileSetDescriptor location: LongAD in LogicalVolumeContentsUse[248..].
            fsdIcb.length = le32(d.data() + 248);
            fsdIcb.logicalBlock = le32(d.data() + 252);
            fsdIcb.partition = le16(d.data() + 256);
            const std::uint32_t numMaps = le32(d.data() + 268);
            std::size_t off = 440;
            for (std::uint32_t i = 0; i < numMaps && off + 2 <= d.size(); ++i) {
                const std::uint8_t type = d[off];
                const std::uint8_t mapLen = d[off + 1];
                if (type == 1) {
                    if (off + 6 > d.size()) break;
                    mapPartitionNumbers.push_back(le16(d.data() + off + 4));
                    off += 6;
                } else {
                    // Type 2 (Metadata/Sparable/Virtual) not supported yet.
                    error_ = "Unsupported UDF partition map type (metadata/sparable/virtual)";
                    return;
                }
                (void)mapLen;
            }
        }
        // Other descriptors (Primary/ImplementationUse/UnallocatedSpace) ignored.
    }

    if (!haveLvd) { error_ = "UDF logical volume descriptor not found"; return; }

    // Build logical partitions from the Type 1 maps.
    partitions_.clear();
    for (std::uint16_t pn : mapPartitionNumbers) {
        auto it = physical.find(pn);
        if (it == physical.end()) {
            error_ = "UDF partition map references unknown physical partition";
            return;
        }
        LogicalPart lp;
        lp.blockSize = logicalBlockSize;
        lp.content = std::make_shared<SubStore>(
            disc_, static_cast<std::int64_t>(it->second.first) * sectorSize_,
            static_cast<std::int64_t>(it->second.second) * sectorSize_);
        partitions_.push_back(lp);
    }
    if (partitions_.empty()) { error_ = "UDF volume has no usable partitions"; return; }

    // File Set Descriptor -> root directory ICB (LongAD at offset 400).
    std::vector<std::uint8_t> fsd = readExtent(fsdIcb);
    if (fsd.size() < 416) { error_ = "UDF file set descriptor truncated"; return; }
    rootIcb_.length = le32(fsd.data() + 400);
    rootIcb_.logicalBlock = le32(fsd.data() + 404);
    rootIcb_.partition = le16(fsd.data() + 408);

    valid_ = true;
}

std::vector<std::uint8_t> UdfReader::readExtent(const Icb& icb) const {
    if (icb.partition >= partitions_.size()) return {};
    const LogicalPart& part = partitions_[icb.partition];
    const std::int64_t pos = static_cast<std::int64_t>(icb.logicalBlock) * part.blockSize;
    return part.content->readRange(pos, icb.length);
}

UdfReader::Node UdfReader::readNode(const Icb& icb) const {
    Node node;
    std::vector<std::uint8_t> d = readExtent(icb);
    if (d.size() < 36) return node;
    const std::uint16_t tagId = le16(d.data());
    // InformationControlBlock at offset 16: fileType at +11, flags at +18.
    const std::uint8_t fileType = d[16 + 11];
    const std::uint16_t icbFlags = le16(d.data() + 16 + 18);
    node.allocationType = icbFlags & 0x3;
    node.isDirectory = (fileType == 4);
    node.partition = icb.partition;

    std::size_t eaLenOff, adLenOff, adBase;
    if (tagId == 266) {          // ExtendedFileEntry
        if (d.size() < 216) return node;
        node.informationLength = le64(d.data() + 56);
        eaLenOff = 208; adLenOff = 212; adBase = 216;
    } else if (tagId == 261) {   // FileEntry
        if (d.size() < 176) return node;
        node.informationLength = le64(d.data() + 56);
        eaLenOff = 168; adLenOff = 172; adBase = 176;
    } else {
        return node;             // unsupported ICB type
    }

    const std::uint32_t eaLen = le32(d.data() + eaLenOff);
    const std::uint32_t adLen = le32(d.data() + adLenOff);
    const std::size_t adStart = adBase + eaLen;
    if (adStart + adLen <= d.size()) {
        node.allocationDescriptors.assign(d.begin() + static_cast<std::ptrdiff_t>(adStart),
                                          d.begin() + static_cast<std::ptrdiff_t>(adStart + adLen));
    }
    node.valid = true;
    return node;
}

ByteStorePtr UdfReader::nodeContent(const Node& node) const {
    const std::int64_t infoLen = static_cast<std::int64_t>(node.informationLength);

    if (node.allocationType == 3) {  // Embedded: content is the AD area itself.
        auto mem = std::make_shared<MemoryStore>(node.allocationDescriptors);
        const std::int64_t n = std::min<std::int64_t>(infoLen, mem->capacity());
        return std::make_shared<SubStore>(mem, 0, n);
    }

    std::vector<ByteStorePtr> parts;
    const std::vector<std::uint8_t>& ad = node.allocationDescriptors;

    if (node.allocationType == 0) {          // Short allocation descriptors (8 bytes)
        const LogicalPart& part = partitions_[node.partition];
        for (std::size_t i = 0; i + 8 <= ad.size(); i += 8) {
            const std::uint32_t raw = le32(ad.data() + i);
            const std::uint32_t len = raw & 0x3FFFFFFF;
            const std::uint32_t flags = (raw >> 30) & 0x3;
            const std::uint32_t loc = le32(ad.data() + i + 4);
            if (len == 0) break;
            const std::int64_t start = static_cast<std::int64_t>(loc) * part.blockSize;
            if (flags == 0) {
                parts.push_back(std::make_shared<SubStore>(part.content, start, len));
            } else {
                parts.push_back(std::make_shared<ZeroStore>(len));  // not recorded -> zeros
            }
        }
    } else if (node.allocationType == 1) {   // Long allocation descriptors (16 bytes)
        for (std::size_t i = 0; i + 16 <= ad.size(); i += 16) {
            const std::uint32_t len = le32(ad.data() + i);
            if (len == 0) break;
            const std::uint32_t loc = le32(ad.data() + i + 4);
            const std::uint16_t pn = le16(ad.data() + i + 8);
            if (pn >= partitions_.size()) break;
            const LogicalPart& part = partitions_[pn];
            const std::int64_t start = static_cast<std::int64_t>(loc) * part.blockSize;
            parts.push_back(std::make_shared<SubStore>(part.content, start, len));
        }
    } else {
        return std::make_shared<ZeroStore>(infoLen);  // extended descriptors unsupported
    }

    ByteStorePtr concat = std::make_shared<ConcatStore>(std::move(parts));
    const std::int64_t n = std::min<std::int64_t>(infoLen, concat->capacity());
    return std::make_shared<SubStore>(concat, 0, n);
}

std::vector<DiscEntry> UdfReader::readDirectory(const Node& node,
                                                std::vector<Icb>* childIcbs) const {
    std::vector<DiscEntry> out;
    if (!node.valid || !node.isDirectory) return out;
    std::vector<std::uint8_t> content = nodeContent(node)->readAll();

    std::size_t pos = 0;
    while (pos + 38 <= content.size()) {
        const std::uint8_t* p = content.data() + pos;
        const std::uint8_t chars = p[18];
        const std::uint8_t nameLen = p[19];
        // FileLocation LongAD at +20: length +20, logicalBlock +24, partition +28.
        Icb child;
        child.length = le32(p + 20);
        child.logicalBlock = le32(p + 24);
        child.partition = le16(p + 28);
        const std::uint16_t iuLen = le16(p + 36);
        const std::size_t nameOff = pos + 38 + iuLen;
        std::string name;
        if (nameOff + nameLen <= content.size()) {
            name = readDChars(content.data() + nameOff, nameLen);
        }
        const std::size_t recSize = (38 + iuLen + nameLen + 3) & ~std::size_t(3);
        pos += recSize;

        // Skip deleted (0x04) and parent ".." (0x08) entries.
        if ((chars & (0x04 | 0x08)) != 0) continue;
        if (name.empty()) continue;

        DiscEntry e;
        e.name = name;
        e.isDirectory = (chars & 0x02) != 0;
        if (!e.isDirectory) {
            Node cn = readNode(child);
            e.length = static_cast<std::int64_t>(cn.informationLength);
        }
        out.push_back(e);
        if (childIcbs) childIcbs->push_back(child);
    }
    return out;
}

bool UdfReader::resolve(const std::string& path, Icb* out, bool* isDir) const {
    std::vector<std::string> parts;
    splitPath(path, &parts);
    Icb cur = rootIcb_;
    bool curIsDir = true;
    for (const std::string& comp : parts) {
        Node node = readNode(cur);
        if (!node.valid || !node.isDirectory) return false;
        std::vector<Icb> icbs;
        std::vector<DiscEntry> entries = readDirectory(node, &icbs);
        const std::string want = toLower(comp);
        bool matched = false;
        for (std::size_t i = 0; i < entries.size(); ++i) {
            if (toLower(entries[i].name) == want) {
                cur = icbs[i];
                curIsDir = entries[i].isDirectory;
                matched = true;
                break;
            }
        }
        if (!matched) return false;
    }
    *out = cur;
    *isDir = curIsDir;
    return true;
}

std::vector<DiscEntry> UdfReader::list(const std::string& dirPath) const {
    if (!valid_) return {};
    Icb icb;
    bool isDir = false;
    if (!resolve(dirPath, &icb, &isDir) || !isDir) return {};
    Node node = readNode(icb);
    return readDirectory(node, nullptr);
}

ByteStorePtr UdfReader::openFile(const std::string& path) const {
    if (!valid_) return nullptr;
    Icb icb;
    bool isDir = false;
    if (!resolve(path, &icb, &isDir) || isDir) return nullptr;
    Node node = readNode(icb);
    if (!node.valid) return nullptr;
    return nodeContent(node);
}

}  // namespace fs
}  // namespace peare
