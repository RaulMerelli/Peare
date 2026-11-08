#include "AmigaFsReader.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <unordered_set>
#include <utility>

namespace peare {
namespace fs {
namespace {

const std::uint32_t kBlockSize = 512;
const std::uint32_t kHeader = 2;
const std::uint32_t kData = 8;
const std::uint32_t kList = 16;
const std::uint32_t kRoot = 1;
const std::uint32_t kUserDir = 2;
const std::uint32_t kFile = 0xFFFFFFFDu;
const std::uint32_t kLinkFile = 0xFFFFFFFCu;
const std::uint32_t kLinkDir = 4;
const std::uint32_t kHashSlots = 72;
const std::uint32_t kHashOffset = 24;
const std::uint32_t kExtensionOffset = 504;
const std::uint32_t kMaxBlocks = 1u << 24;

std::uint32_t readBe32(const std::uint8_t* p)
{
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) |
           (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}

bool blockChecksumValid(const std::vector<std::uint8_t>& block)
{
    if (block.size() != kBlockSize) return false;
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i < block.size(); i += 4)
        sum += readBe32(block.data() + i);
    return sum == 0;
}

std::string latin1ToUtf8(const std::uint8_t* bytes, std::size_t length)
{
    std::string out;
    for (std::size_t i = 0; i < length; ++i) {
        const std::uint8_t c = bytes[i];
        if (c < 0x80) out.push_back(static_cast<char>(c));
        else {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

bool equalName(const std::string& left, const std::string& right)
{
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        unsigned char a = static_cast<unsigned char>(left[i]);
        unsigned char b = static_cast<unsigned char>(right[i]);
        if (a >= 'a' && a <= 'z') a = static_cast<unsigned char>(a - 'a' + 'A');
        if (b >= 'a' && b <= 'z') b = static_cast<unsigned char>(b - 'a' + 'A');
        if (a != b) return false;
    }
    return true;
}

struct Segment {
    std::uint32_t block;
    std::uint16_t offset;
    std::uint16_t length;
};

class AmigaFileStore final : public IByteStore {
public:
    AmigaFileStore(ByteStorePtr image, std::vector<Segment> segments, std::uint64_t size)
        : image_(std::move(image)), segments_(std::move(segments)), size_(size) {}

    std::int64_t capacity() const override { return static_cast<std::int64_t>(size_); }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override
    {
        if (!image_ || !dst || pos < 0 || count <= 0 || std::uint64_t(pos) >= size_) return 0;
        const int wanted = static_cast<int>(std::min<std::uint64_t>(
            static_cast<std::uint64_t>(count), size_ - std::uint64_t(pos)));
        int produced = 0;
        std::uint64_t fileOffset = 0;
        for (const Segment& segment : segments_) {
            const std::uint64_t segmentEnd = fileOffset + segment.length;
            const std::uint64_t requestStart = static_cast<std::uint64_t>(pos);
            const std::uint64_t requestEnd = requestStart + wanted;
            if (requestStart < segmentEnd && requestEnd > fileOffset) {
                const std::uint64_t overlapStart = std::max(requestStart, fileOffset);
                const std::uint64_t overlapEnd = std::min(requestEnd, segmentEnd);
                const std::uint32_t within = static_cast<std::uint32_t>(overlapStart - fileOffset);
                const int amount = static_cast<int>(overlapEnd - overlapStart);
                const std::int64_t sourceOffset =
                    std::int64_t(segment.block) * kBlockSize + segment.offset + within;
                const int got = image_->read(sourceOffset, dst + produced, amount);
                if (got != amount) return produced + std::max(got, 0);
                produced += got;
            }
            fileOffset = segmentEnd;
            if (produced == wanted) break;
        }
        return produced;
    }

private:
    ByteStorePtr image_;
    std::vector<Segment> segments_;
    std::uint64_t size_;
};

}  // namespace

AmigaFsReader::AmigaFsReader(ByteStorePtr image) : image_(std::move(image))
{
    parse();
}

bool AmigaFsReader::readBlock(std::uint32_t block,
                              std::vector<std::uint8_t>* bytes) const
{
    if (!bytes || block >= blockCount_) return false;
    bytes->resize(kBlockSize);
    return image_->read(std::int64_t(block) * kBlockSize, bytes->data(), kBlockSize) ==
           static_cast<int>(kBlockSize);
}

void AmigaFsReader::parse()
{
    if (!image_ || image_->capacity() < 901120 ||
        image_->capacity() % kBlockSize != 0) {
        error_ = "Not an Amiga floppy image";
        return;
    }
    const std::int64_t blocks = image_->capacity() / kBlockSize;
    if (blocks > kMaxBlocks) {
        error_ = "Amiga volume exceeds the supported block limit";
        return;
    }
    blockCount_ = static_cast<std::uint32_t>(blocks);
    std::uint8_t boot[12];
    if (image_->read(0, boot, sizeof(boot)) != sizeof(boot) ||
        std::memcmp(boot, "DOS", 3) != 0 || (boot[3] & 0xF8) != 0) {
        error_ = "Amiga DOS boot signature not found";
        return;
    }
    fastFileSystem_ = (boot[3] & 1) != 0;
    rootBlock_ = readBe32(boot + 8);
    std::vector<std::uint8_t> root;
    const auto validRoot = [this](std::uint32_t candidate,
                                  std::vector<std::uint8_t>* bytes) {
        return candidate < blockCount_ && readBlock(candidate, bytes) &&
               readBe32(bytes->data()) == kHeader &&
               readBe32(bytes->data() + 508) == kRoot && blockChecksumValid(*bytes);
    };
    if (!validRoot(rootBlock_, &root)) {
        const std::uint32_t middle = blockCount_ / 2;
        if (!validRoot(middle, &root)) {
            error_ = "Invalid Amiga root block";
            return;
        }
        rootBlock_ = middle;
    }
    if (rootBlock_ >= blockCount_) {
        error_ = "Invalid Amiga root block";
        return;
    }
    const std::uint32_t slots = readBe32(root.data() + 12);
    if (slots == 0 || slots > kHashSlots) {
        error_ = "Invalid Amiga root hash table";
        return;
    }
    friendly_ = fastFileSystem_ ? "Amiga Fast File System (FFS)"
                               : "Amiga Old File System (OFS)";
    valid_ = true;
}

bool AmigaFsReader::readObject(std::uint32_t block, Object* object) const
{
    if (!object) return false;
    std::vector<std::uint8_t> bytes;
    if (!readBlock(block, &bytes) || readBe32(bytes.data()) != kHeader ||
        readBe32(bytes.data() + 4) != block || !blockChecksumValid(bytes)) return false;
    const std::uint32_t nameLength = bytes[432];
    if (nameLength > 30) return false;
    object->block = block;
    object->type = readBe32(bytes.data() + 508);
    object->name = latin1ToUtf8(bytes.data() + 433, nameLength);
    object->size = object->type == kFile ? readBe32(bytes.data() + 324) : 0;
    return !object->name.empty() || block == rootBlock_;
}

std::vector<AmigaFsReader::Object> AmigaFsReader::directory(std::uint32_t block) const
{
    std::vector<Object> entries;
    std::vector<std::uint8_t> bytes;
    if (!readBlock(block, &bytes)) return entries;
    const std::uint32_t secType = readBe32(bytes.data() + 508);
    if ((block == rootBlock_ && secType != kRoot) ||
        (block != rootBlock_ && secType != kUserDir) || !blockChecksumValid(bytes))
        return entries;
    const std::uint32_t slots = block == rootBlock_ ? readBe32(bytes.data() + 12) : kHashSlots;
    if (slots == 0 || slots > kHashSlots) return entries;
    std::unordered_set<std::uint32_t> visited;
    for (std::uint32_t i = 0; i < slots; ++i) {
        std::uint32_t child = readBe32(bytes.data() + kHashOffset + i * 4);
        while (child != 0 && visited.insert(child).second && visited.size() <= blockCount_) {
            Object object;
            if (!readObject(child, &object)) break;
            if (object.type == kFile || object.type == kUserDir ||
                object.type == kLinkFile || object.type == kLinkDir)
                entries.push_back(object);
            std::vector<std::uint8_t> record;
            if (!readBlock(child, &record)) break;
            child = readBe32(record.data() + 496);
        }
    }
    return entries;
}

bool AmigaFsReader::findDirectory(const std::string& path, std::uint32_t* block) const
{
    if (!block) return false;
    *block = rootBlock_;
    std::size_t start = 0;
    while (start < path.size()) {
        while (start < path.size() && path[start] == '/') ++start;
        if (start == path.size()) break;
        const std::size_t end = path.find('/', start);
        const std::string part = path.substr(start, end == std::string::npos
                                                       ? std::string::npos : end - start);
        bool found = false;
        for (const Object& child : directory(*block)) {
            if (child.type == kUserDir && equalName(child.name, part)) {
                *block = child.block;
                found = true;
                break;
            }
        }
        if (!found) return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

std::vector<DiscEntry> AmigaFsReader::list(const std::string& dirPath) const
{
    std::vector<DiscEntry> result;
    std::uint32_t block = 0;
    if (!valid_ || !findDirectory(dirPath, &block)) return result;
    for (const Object& object : directory(block)) {
        if (object.type == kFile || object.type == kUserDir) {
            result.emplace_back(object.name, object.type == kUserDir, object.size);
        }
    }
    return result;
}

std::vector<std::uint32_t> AmigaFsReader::dataBlocks(const Object& object) const
{
    std::vector<std::uint32_t> result;
    if (object.type != kFile || object.size == 0) return result;
    std::vector<std::uint8_t> header;
    if (!readBlock(object.block, &header)) return {};
    std::uint32_t current = object.block;
    std::unordered_set<std::uint32_t> visited;
    while (current != 0 && visited.insert(current).second && visited.size() <= blockCount_) {
        std::vector<std::uint8_t> bytes;
        if (!readBlock(current, &bytes)) return {};
        const std::uint32_t expectedType = current == object.block ? kHeader : kList;
        if (readBe32(bytes.data()) != expectedType ||
            (current != object.block && readBe32(bytes.data() + 500) != object.block) ||
            !blockChecksumValid(bytes)) return {};
        const std::uint32_t used = readBe32(bytes.data() + 8);
        if (used > kHashSlots || (current == object.block && readBe32(bytes.data() + 508) != kFile))
            return {};
        for (std::uint32_t i = 0; i < used; ++i) {
            const std::uint32_t data = readBe32(bytes.data() + kHashOffset +
                                                (kHashSlots - 1 - i) * 4);
            if (data == 0 || data >= blockCount_) return {};
            result.push_back(data);
        }
        current = readBe32(bytes.data() + kExtensionOffset);
    }
    if (current != 0) return {};
    const std::uint64_t perBlock = fastFileSystem_ ? kBlockSize : kBlockSize - 24;
    const std::uint64_t required = (std::uint64_t(object.size) + perBlock - 1) / perBlock;
    if (result.size() < required) return {};
    result.resize(static_cast<std::size_t>(required));
    return result;
}

ByteStorePtr AmigaFsReader::openFile(const std::string& path) const
{
    if (!valid_) return nullptr;
    const std::size_t slash = path.find_last_of('/');
    const std::string parent = slash == std::string::npos ? std::string() : path.substr(0, slash);
    const std::string leaf = slash == std::string::npos ? path : path.substr(slash + 1);
    std::uint32_t directoryBlock = 0;
    if (!findDirectory(parent, &directoryBlock)) return nullptr;
    Object file;
    bool found = false;
    for (const Object& item : directory(directoryBlock)) {
        if (item.type == kFile && equalName(item.name, leaf)) {
            file = item;
            found = true;
            break;
        }
    }
    if (!found) return nullptr;
    const std::vector<std::uint32_t> blocks = dataBlocks(file);
    const std::uint32_t payload = fastFileSystem_ ? kBlockSize : kBlockSize - 24;
    std::vector<Segment> segments;
    std::uint64_t remaining = file.size;
    for (std::size_t i = 0; i < blocks.size() && remaining; ++i) {
        std::uint16_t offset = 0;
        std::uint32_t length = std::min<std::uint64_t>(payload, remaining);
        if (!fastFileSystem_) {
            std::vector<std::uint8_t> data;
            if (!readBlock(blocks[i], &data) || readBe32(data.data()) != kData ||
                readBe32(data.data() + 4) != file.block ||
                readBe32(data.data() + 8) != i + 1 || readBe32(data.data() + 12) < length)
                return nullptr;
            offset = 24;
            length = std::min<std::uint32_t>(readBe32(data.data() + 12),
                                             static_cast<std::uint32_t>(remaining));
        }
        segments.push_back({blocks[i], offset, static_cast<std::uint16_t>(length)});
        remaining -= length;
    }
    if (remaining != 0) return nullptr;
    return std::make_shared<AmigaFileStore>(image_, std::move(segments), file.size);
}

}  // namespace fs
}  // namespace peare
