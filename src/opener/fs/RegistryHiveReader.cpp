#include "RegistryHiveReader.h"

#include <algorithm>
#include <cstdio>
#include <iomanip>
#include <sstream>

namespace peare {
namespace fs {
namespace {

const std::int64_t kBinStart = 0x1000;

std::uint16_t le16(const std::uint8_t* p) { return std::uint16_t(p[0] | (p[1] << 8)); }
std::int16_t sle16(const std::uint8_t* p) { return static_cast<std::int16_t>(le16(p)); }
std::uint32_t le32(const std::uint8_t* p) {
    return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) |
           (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
std::int32_t sle32(const std::uint8_t* p) { return static_cast<std::int32_t>(le32(p)); }
std::uint64_t le64(const std::uint8_t* p) {
    return std::uint64_t(le32(p)) | (std::uint64_t(le32(p + 4)) << 32);
}

bool range(std::size_t size, std::size_t off, std::size_t len) {
    return off <= size && len <= size - off;
}

std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos < path.size()) {
        while (pos < path.size() && (path[pos] == '/' || path[pos] == '\\')) ++pos;
        const std::size_t start = pos;
        while (pos < path.size() && path[pos] != '/' && path[pos] != '\\') ++pos;
        if (pos > start) out.push_back(path.substr(start, pos - start));
    }
    return out;
}

std::string latin1(const std::vector<std::uint8_t>& b, std::size_t off, std::size_t len) {
    if (!range(b.size(), off, len)) return std::string();
    return std::string(reinterpret_cast<const char*>(b.data() + off), len);
}

std::string utf16leToUtf8(const std::vector<std::uint8_t>& b) {
    std::string out;
    std::size_t n = b.size();
    while (n >= 2 && b[n - 1] == 0 && b[n - 2] == 0) n -= 2;
    for (std::size_t i = 0; i + 1 < n; i += 2) {
        const std::uint32_t ch = le16(b.data() + i);
        if (ch < 0x80) {
            out.push_back(static_cast<char>(ch));
        } else if (ch < 0x800) {
            out.push_back(static_cast<char>(0xc0 | (ch >> 6)));
            out.push_back(static_cast<char>(0x80 | (ch & 0x3f)));
        } else {
            out.push_back(static_cast<char>(0xe0 | (ch >> 12)));
            out.push_back(static_cast<char>(0x80 | ((ch >> 6) & 0x3f)));
            out.push_back(static_cast<char>(0x80 | (ch & 0x3f)));
        }
    }
    return out;
}

std::string typeName(std::uint32_t type) {
    switch (type) {
    case 0: return "REG_NONE";
    case 1: return "REG_SZ";
    case 2: return "REG_EXPAND_SZ";
    case 3: return "REG_BINARY";
    case 4: return "REG_DWORD";
    case 5: return "REG_DWORD_BIG_ENDIAN";
    case 6: return "REG_LINK";
    case 7: return "REG_MULTI_SZ";
    case 8: return "REG_RESOURCE_LIST";
    case 9: return "REG_FULL_RESOURCE_DESCRIPTOR";
    case 10: return "REG_RESOURCE_REQUIREMENTS_LIST";
    case 11: return "REG_QWORD";
    default: return "REG_UNKNOWN";
    }
}

bool icaseEq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
}

}  // namespace

RegistryHiveReader::RegistryHiveReader(ByteStorePtr store) : store_(std::move(store)) { parse(); }

void RegistryHiveReader::parse() {
    if (!store_ || store_->capacity() < kBinStart + 0x20) {
        error_ = "Registry hive too small";
        return;
    }
    std::vector<std::uint8_t> header = store_->readRange(0, 0x200);
    if (header.size() < 0x200 || std::memcmp(header.data(), "regf", 4) != 0) {
        error_ = "Invalid registry hive signature";
        return;
    }
    rootCell_ = sle32(header.data() + 0x24);
    hiveLength_ = sle32(header.data() + 0x28);
    if (rootCell_ < 0 || hiveLength_ <= 0 ||
        kBinStart + hiveLength_ > store_->capacity() + 0x1000) {
        error_ = "Invalid registry hive header";
        return;
    }
    KeyCell root;
    if (!parseKey(rootCell_, &root)) {
        error_ = "Registry hive root key not found";
        return;
    }
    valid_ = true;
}

RegistryHiveReader::RawCell RegistryHiveReader::rawCell(std::int32_t index) const {
    RawCell out;
    if (!store_ || index < 0 || index >= hiveLength_) return out;
    const std::int64_t cellPos = kBinStart + index;
    std::uint8_t sizeBytes[4] = {};
    store_->readExactly(cellPos, sizeBytes, 4);
    const std::int32_t storedSize = sle32(sizeBytes);
    if (storedSize >= 0) return out;
    const std::int32_t cellSize = -storedSize;
    if (cellSize < 6 || cellPos + cellSize > store_->capacity()) return out;
    out.data = store_->readRange(cellPos + 4, cellSize - 4);
    if (out.data.size() != static_cast<std::size_t>(cellSize - 4)) return RawCell();
    out.size = cellSize;
    out.valid = true;
    return out;
}

bool RegistryHiveReader::parseKey(std::int32_t index, KeyCell* key) const {
    if (!key) return false;
    RawCell cell = rawCell(index);
    if (!cell.valid || cell.data.size() < 0x4c || std::memcmp(cell.data.data(), "nk", 2) != 0)
        return false;
    const std::uint16_t nameLen = le16(cell.data.data() + 0x48);
    if (!range(cell.data.size(), 0x4c, nameLen)) return false;
    key->index = index;
    key->flags = le16(cell.data.data() + 0x02);
    key->subKeyCount = sle32(cell.data.data() + 0x14);
    key->subKeysIndex = sle32(cell.data.data() + 0x1c);
    key->valueCount = sle32(cell.data.data() + 0x24);
    key->valueListIndex = sle32(cell.data.data() + 0x28);
    key->name = latin1(cell.data, 0x4c, nameLen);
    return true;
}

bool RegistryHiveReader::parseValue(std::int32_t index, ValueCell* value) const {
    if (!value) return false;
    RawCell cell = rawCell(index);
    if (!cell.valid || cell.data.size() < 0x14 || std::memcmp(cell.data.data(), "vk", 2) != 0)
        return false;
    const std::uint16_t nameLen = le16(cell.data.data() + 0x02);
    if (!range(cell.data.size(), 0x14, nameLen)) return false;
    value->index = index;
    value->dataLength = sle32(cell.data.data() + 0x04);
    value->dataIndex = sle32(cell.data.data() + 0x08);
    value->type = le32(cell.data.data() + 0x0c);
    value->flags = le16(cell.data.data() + 0x10);
    value->name = (value->flags & 0x0001) ? latin1(cell.data, 0x14, nameLen) : std::string();
    return true;
}

bool RegistryHiveReader::collectSubKeyIndexes(std::int32_t listIndex,
                                              std::vector<std::int32_t>* out,
                                              int depth) const {
    if (!out || depth > 8 || listIndex < 0) return false;
    RawCell cell = rawCell(listIndex);
    if (!cell.valid || cell.data.size() < 4) return false;
    const char a = static_cast<char>(cell.data[0]);
    const char b = static_cast<char>(cell.data[1]);
    const std::uint16_t count = le16(cell.data.data() + 2);
    if ((a == 'l' && (b == 'f' || b == 'h')) || (a == 'i' && b == 'l')) {
        const std::size_t stride = (b == 'f' || b == 'h') ? 8 : 4;
        if (!range(cell.data.size(), 4, std::size_t(count) * stride)) return false;
        for (std::uint16_t i = 0; i < count; ++i)
            out->push_back(sle32(cell.data.data() + 4 + std::size_t(i) * stride));
        return true;
    }
    if (a == 'r' && b == 'i') {
        if (!range(cell.data.size(), 4, std::size_t(count) * 4)) return false;
        for (std::uint16_t i = 0; i < count; ++i) {
            if (!collectSubKeyIndexes(sle32(cell.data.data() + 4 + std::size_t(i) * 4), out,
                                      depth + 1))
                return false;
        }
        return true;
    }
    return false;
}

std::vector<std::int32_t> RegistryHiveReader::valueIndexes(const KeyCell& key) const {
    std::vector<std::int32_t> out;
    if (key.valueCount <= 0 || key.valueListIndex < 0) return out;
    RawCell cell = rawCell(key.valueListIndex);
    if (!cell.valid || !range(cell.data.size(), 0, std::size_t(key.valueCount) * 4)) return out;
    for (std::int32_t i = 0; i < key.valueCount; ++i)
        out.push_back(sle32(cell.data.data() + std::size_t(i) * 4));
    return out;
}

bool RegistryHiveReader::readValueData(const ValueCell& value,
                                       std::vector<std::uint8_t>* out) const {
    if (!out) return false;
    out->clear();
    const bool inlineData = (value.dataLength & static_cast<std::int32_t>(0x80000000)) != 0;
    const std::size_t len =
        static_cast<std::size_t>(value.dataLength & static_cast<std::int32_t>(0x7fffffff));
    if (len == 0) return true;
    if (inlineData) {
        const std::size_t n = std::min<std::size_t>(len, 4);
        out->resize(n);
        for (std::size_t i = 0; i < n; ++i)
            (*out)[i] = static_cast<std::uint8_t>((value.dataIndex >> (8 * i)) & 0xff);
        return true;
    }
    RawCell cell = rawCell(value.dataIndex);
    if (!cell.valid) return false;
    if (cell.data.size() >= 2 && cell.data[0] == 'd' && cell.data[1] == 'b' &&
        cell.data.size() >= 8) {
        const std::uint16_t segments = le16(cell.data.data() + 2);
        const std::int32_t listIndex = sle32(cell.data.data() + 4);
        RawCell list = rawCell(listIndex);
        if (!list.valid || !range(list.data.size(), 0, std::size_t(segments) * 4)) return false;
        for (std::uint16_t i = 0; i < segments && out->size() < len; ++i) {
            RawCell seg = rawCell(sle32(list.data.data() + std::size_t(i) * 4));
            if (!seg.valid) return false;
            const std::size_t n = std::min<std::size_t>(seg.data.size(), len - out->size());
            out->insert(out->end(), seg.data.begin(), seg.data.begin() + static_cast<std::ptrdiff_t>(n));
        }
        return out->size() == len;
    }
    const std::size_t n = std::min<std::size_t>(len, cell.data.size());
    out->assign(cell.data.begin(), cell.data.begin() + static_cast<std::ptrdiff_t>(n));
    return true;
}

bool RegistryHiveReader::keyForPath(const std::string& path, KeyCell* key) const {
    if (!parseKey(rootCell_, key)) return false;
    const std::vector<std::string> parts = splitPath(path);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        std::vector<std::int32_t> children;
        if (!collectSubKeyIndexes(key->subKeysIndex, &children)) return false;
        bool found = false;
        for (std::size_t c = 0; c < children.size(); ++c) {
            KeyCell child;
            if (parseKey(children[c], &child) && icaseEq(child.name, parts[i])) {
                *key = child;
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

std::vector<DiscEntry> RegistryHiveReader::list(const std::string& dirPath) const {
    std::vector<DiscEntry> out;
    if (!valid_) return out;
    KeyCell key;
    if (!keyForPath(dirPath, &key)) return out;
    std::vector<std::int32_t> children;
    if (key.subKeyCount > 0 && collectSubKeyIndexes(key.subKeysIndex, &children)) {
        for (std::size_t i = 0; i < children.size(); ++i) {
            KeyCell child;
            if (parseKey(children[i], &child))
                out.push_back(DiscEntry{child.name, true, 0});
        }
    }
    const std::vector<std::int32_t> values = valueIndexes(key);
    for (std::size_t i = 0; i < values.size(); ++i) {
        ValueCell value;
        if (!parseValue(values[i], &value)) continue;
        std::vector<std::uint8_t> data;
        if (!readValueData(value, &data)) continue;
        const std::string name = value.name.empty() ? "@" : "@" + value.name;
        out.push_back(DiscEntry{name, false, static_cast<std::int64_t>(
                                       valuePayload(value, data).size())});
    }
    std::sort(out.begin(), out.end(), [](const DiscEntry& a, const DiscEntry& b) {
        if (a.isDirectory != b.isDirectory) return a.isDirectory > b.isDirectory;
        return a.name < b.name;
    });
    return out;
}

ByteStorePtr RegistryHiveReader::openFile(const std::string& path) const {
    if (!valid_) return ByteStorePtr();
    const std::vector<std::string> parts = splitPath(path);
    if (parts.empty()) return ByteStorePtr();
    std::string keyPath;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        if (!keyPath.empty()) keyPath += "/";
        keyPath += parts[i];
    }
    KeyCell key;
    if (!keyForPath(keyPath, &key)) return ByteStorePtr();
    std::string wanted = parts.back();
    if (wanted.empty() || wanted[0] != '@') return ByteStorePtr();
    wanted = wanted.size() == 1 ? std::string() : wanted.substr(1);
    const std::vector<std::int32_t> values = valueIndexes(key);
    for (std::size_t i = 0; i < values.size(); ++i) {
        ValueCell value;
        if (!parseValue(values[i], &value) || !icaseEq(value.name, wanted)) continue;
        std::vector<std::uint8_t> data;
        if (!readValueData(value, &data)) return ByteStorePtr();
        const std::string text = valuePayload(value, data);
        return std::make_shared<MemoryStore>(
            reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
    }
    return ByteStorePtr();
}

std::string RegistryHiveReader::valuePayload(const ValueCell& value,
                                             const std::vector<std::uint8_t>& data) const {
    std::ostringstream s;
    s << "type=" << typeName(value.type) << "\n";
    s << "length=" << data.size() << "\n";
    s << "value=";
    if (value.type == 1 || value.type == 2 || value.type == 6) {
        s << utf16leToUtf8(data);
    } else if (value.type == 7) {
        std::string text = utf16leToUtf8(data);
        for (std::size_t i = 0; i < text.size(); ++i) s << (text[i] == '\0' ? '\n' : text[i]);
    } else if (value.type == 4 && data.size() >= 4) {
        s << le32(data.data());
    } else if (value.type == 5 && data.size() >= 4) {
        s << (std::uint32_t(data[0]) << 24 | std::uint32_t(data[1]) << 16 |
              std::uint32_t(data[2]) << 8 | std::uint32_t(data[3]));
    } else if (value.type == 11 && data.size() >= 8) {
        s << le64(data.data());
    } else {
        s << std::hex << std::setfill('0');
        for (std::size_t i = 0; i < data.size(); ++i) {
            if (i) s << ' ';
            s << std::setw(2) << static_cast<unsigned>(data[i]);
        }
    }
    s << "\n";
    return s.str();
}

}  // namespace fs
}  // namespace peare

