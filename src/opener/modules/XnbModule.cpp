#include "XnbModule.h"

#include "Compat.h"
#include "../compression/peare/Lzx_frontends.h"
#include "../fs/DiscStore.h"

#include <QFileInfo>
#include <algorithm>
#include <climits>
#include <cstdint>
#include <limits>
#include <new>
#include <vector>

namespace peare {
namespace {

constexpr std::uint32_t kMaxReaderCount = 4096;
constexpr std::uint32_t kMaxSharedResources = 1u << 20;
constexpr std::uint32_t kLzxFrameSize = 0x8000;
constexpr std::uint64_t kMaxPreviewPixels = 1u << 24;

std::uint32_t readLe32(const std::uint8_t* data)
{
    return std::uint32_t(data[0]) | (std::uint32_t(data[1]) << 8) |
           (std::uint32_t(data[2]) << 16) | (std::uint32_t(data[3]) << 24);
}

std::uint16_t readBe16(const std::uint8_t* data)
{
    return static_cast<std::uint16_t>((std::uint16_t(data[0]) << 8) | data[1]);
}

void appendLe16(std::vector<std::uint8_t>* data, std::uint16_t value)
{
    data->push_back(static_cast<std::uint8_t>(value));
    data->push_back(static_cast<std::uint8_t>(value >> 8));
}

void appendLe32(std::vector<std::uint8_t>* data, std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8)
        data->push_back(static_cast<std::uint8_t>(value >> shift));
}

bool makeColorTextureBmp(const std::vector<std::uint8_t>& body, std::size_t position,
                         bool legacyV4,
                         std::vector<std::uint8_t>* bmp)
{
    if (!bmp || position > body.size() || body.size() - position < 20) return false;
    const std::uint32_t format = readLe32(body.data() + position);
    const std::uint32_t width = readLe32(body.data() + position + 4);
    const std::uint32_t height = readLe32(body.data() + position + 8);
    const std::uint32_t mipCount = readLe32(body.data() + position + 12);
    if (format != (legacyV4 ? 1u : 0u) || !width || !height || !mipCount || mipCount > 32 ||
        width > kMaxPreviewPixels / height ||
        std::uint64_t(width) * height > kMaxPreviewPixels)
        return false;

    position += 16;
    const std::uint8_t* baseLevel = nullptr;
    std::size_t baseLevelSize = 0;
    for (std::uint32_t level = 0; level < mipCount; ++level) {
        if (body.size() - position < 4) return false;
        const std::uint32_t levelSize = readLe32(body.data() + position);
        position += 4;
        const std::uint32_t levelWidth = std::max(1u, width >> level);
        const std::uint32_t levelHeight = std::max(1u, height >> level);
        const std::uint64_t expectedSize = std::uint64_t(levelWidth) * levelHeight * 4;
        if (expectedSize > std::numeric_limits<std::uint32_t>::max() ||
            levelSize != expectedSize || levelSize > body.size() - position)
            return false;
        if (!level) {
            baseLevel = body.data() + position;
            baseLevelSize = levelSize;
        }
        position += levelSize;
    }

    const std::uint64_t pixelBytes = std::uint64_t(width) * height * 4;
    const std::uint64_t fileBytes = 14 + 56 + pixelBytes;
    if (!baseLevel || baseLevelSize != pixelBytes || fileBytes >
        std::numeric_limits<std::uint32_t>::max())
        return false;

    bmp->clear();
    bmp->reserve(static_cast<std::size_t>(fileBytes));
    bmp->push_back('B');
    bmp->push_back('M');
    appendLe32(bmp, static_cast<std::uint32_t>(fileBytes));
    appendLe16(bmp, 0);
    appendLe16(bmp, 0);
    appendLe32(bmp, 70);
    appendLe32(bmp, 56);
    appendLe32(bmp, width);
    appendLe32(bmp, static_cast<std::uint32_t>(-static_cast<std::int32_t>(height)));
    appendLe16(bmp, 1);
    appendLe16(bmp, 32);
    appendLe32(bmp, 3);
    appendLe32(bmp, static_cast<std::uint32_t>(pixelBytes));
    appendLe32(bmp, 0);
    appendLe32(bmp, 0);
    appendLe32(bmp, 0);
    appendLe32(bmp, 0);
    appendLe32(bmp, 0x00ff0000u);
    appendLe32(bmp, 0x0000ff00u);
    appendLe32(bmp, 0x000000ffu);
    appendLe32(bmp, 0xff000000u);
    for (std::size_t i = 0; i < baseLevelSize; i += 4) {
        bmp->push_back(legacyV4 ? baseLevel[i] : baseLevel[i + 2]);
        bmp->push_back(baseLevel[i + 1]);
        bmp->push_back(legacyV4 ? baseLevel[i + 2] : baseLevel[i]);
        bmp->push_back(baseLevel[i + 3]);
    }
    return true;
}

bool readVarInt(const std::vector<std::uint8_t>& data, std::size_t* position,
                std::uint32_t* value)
{
    if (!position || !value) return false;
    std::uint32_t result = 0;
    for (unsigned shift = 0; shift <= 28; shift += 7) {
        if (*position >= data.size()) return false;
        const std::uint8_t byte = data[(*position)++];
        if (shift == 28 && (byte & 0xf0u)) return false;
        result |= std::uint32_t(byte & 0x7fu) << shift;
        if (!(byte & 0x80u)) {
            *value = result;
            return true;
        }
    }
    return false;
}

bool decompressLz4(const std::uint8_t* input, std::size_t inputSize,
                   std::vector<std::uint8_t>* output, std::size_t outputSize)
{
    if (!output || (inputSize && !input)) return false;
    output->clear();
    output->reserve(outputSize);
    std::size_t in = 0;
    while (in < inputSize) {
        const std::uint8_t token = input[in++];
        std::size_t literalLength = token >> 4;
        if (literalLength == 15) {
            std::uint8_t extension = 255;
            while (extension == 255) {
                if (in >= inputSize) return false;
                extension = input[in++];
                if (literalLength > outputSize || extension > outputSize - literalLength)
                    return false;
                literalLength += extension;
            }
        }
        if (literalLength > inputSize - in || literalLength > outputSize - output->size())
            return false;
        output->insert(output->end(), input + in, input + in + literalLength);
        in += literalLength;
        if (in == inputSize) break;
        if (inputSize - in < 2) return false;
        const std::size_t distance = std::size_t(input[in]) |
                                     (std::size_t(input[in + 1]) << 8);
        in += 2;
        if (!distance || distance > output->size()) return false;
        std::size_t matchLength = (token & 0x0fu) + 4u;
        if ((token & 0x0fu) == 15u) {
            std::uint8_t extension = 255;
            while (extension == 255) {
                if (in >= inputSize) return false;
                extension = input[in++];
                if (matchLength > outputSize || extension > outputSize - matchLength)
                    return false;
                matchLength += extension;
            }
        }
        if (matchLength > outputSize - output->size()) return false;
        for (std::size_t i = 0; i < matchLength; ++i)
            output->push_back((*output)[output->size() - distance]);
    }
    return output->size() == outputSize;
}

bool decompressLzx(const std::uint8_t* input, std::size_t inputSize,
                   std::vector<std::uint8_t>* output, std::size_t outputSize)
{
    if (!output || !input || inputSize == 0 || outputSize == 0) return false;
    struct Block { std::size_t offset; std::size_t packed; std::size_t unpacked; };
    std::vector<Block> blocks;
    std::size_t in = 0;
    std::size_t framedOutput = 0;
    while (in < inputSize) {
        if (inputSize - in < 2) return false;
        std::size_t frameSize = kLzxFrameSize;
        std::size_t blockSize = readBe16(input + in);
        if (input[in] == 0xffu) {
            if (inputSize - in < 5) return false;
            frameSize = readBe16(input + in + 1);
            blockSize = readBe16(input + in + 3);
            in += 5;
        } else {
            in += 2;
        }
        if (!blockSize) break;
        if (!frameSize || frameSize > outputSize - framedOutput ||
            blockSize > inputSize - in)
            return false;
        blocks.push_back({in, blockSize, frameSize});
        in += blockSize;
        framedOutput += frameSize;
    }
    if (framedOutput != outputSize || blocks.empty()) return false;
    output->resize(outputSize);
    peare_lzx_cab_decoder* decoder = nullptr;
    if (peare_lzx_cab_create(1u << 16, &decoder) != PEARE_LZX_OK) return false;
    std::size_t out = 0;
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const Block& block = blocks[i];
        const peare_lzx_status status = peare_lzx_cab_decompress_frame(
            decoder, input + block.offset, block.packed, output->data(),
            output->size(), block.unpacked, i == 0, i + 1 == blocks.size());
        if (status != PEARE_LZX_OK) {
            peare_lzx_cab_destroy(decoder);
            return false;
        }
        out += block.unpacked;
    }
    peare_lzx_cab_destroy(decoder);
    return out == outputSize;
}

bool readStore(const fs::ByteStorePtr& source, std::vector<std::uint8_t>* bytes)
{
    if (!source || !bytes || !source->validate()) return false;
    const std::int64_t capacity = source->capacity();
    if (capacity < 10 || static_cast<std::uint64_t>(capacity) >
            std::numeric_limits<std::uint32_t>::max() ||
        static_cast<std::uint64_t>(capacity) >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
        return false;
    bytes->resize(static_cast<std::size_t>(capacity));
    std::size_t offset = 0;
    while (offset < bytes->size()) {
        const int request = static_cast<int>(std::min<std::size_t>(
            bytes->size() - offset, 4u * 1024u * 1024u));
        const int count = source->read(static_cast<std::int64_t>(offset),
            bytes->data() + offset, request);
        if (count <= 0 || count > request) return false;
        offset += static_cast<std::size_t>(count);
    }
    return true;
}

bool parseString(const std::vector<std::uint8_t>& data, std::size_t* position,
                 QString* value)
{
    std::uint32_t length = 0;
    if (!readVarInt(data, position, &length) || length > INT_MAX ||
        length > data.size() - *position)
        return false;
    *value = QString::fromUtf8(reinterpret_cast<const char*>(data.data() + *position),
                               static_cast<int>(length));
    *position += length;
    return true;
}

QString platformName(char platform)
{
    switch (platform) {
    case 'w': return QStringLiteral("Windows");
    case 'd': return QStringLiteral("DesktopGL");
    case 'x': return QStringLiteral("Xbox 360");
    case 'm': return QStringLiteral("Windows Phone");
    case 'i': return QStringLiteral("iOS");
    case 'a': return QStringLiteral("Android");
    case 'p': return QStringLiteral("PlayStation");
    case 's': return QStringLiteral("Nintendo Switch");
    default: return QStringLiteral("Unknown (%1)").arg(QChar::fromLatin1(platform));
    }
}

}  // namespace

ModulePtr XnbModule::open(const fs::ByteStorePtr& source, const QString& sourceName)
{
    auto module = peare::makeUnique<XnbModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::XNB;
    module->info_.description = QStringLiteral("XNA / MonoGame compiled content");
    try {
        std::vector<std::uint8_t> file;
        if (!readStore(source, &file)) {
            module->info_.error = QStringLiteral("Could not read the XNB file");
            return ModulePtr(std::move(module));
        }
        if (file[0] != 'X' || file[1] != 'N' || file[2] != 'B' ||
            (file[4] != 4 && file[4] != 5) || (file[5] & ~0xc1u) != 0 ||
            ((file[5] & 0xc0u) == 0xc0u) || readLe32(file.data() + 6) != file.size()) {
            module->info_.error = QStringLiteral("Invalid XNB header or declared file length");
            return ModulePtr(std::move(module));
        }

        const bool lzx = (file[5] & 0x80u) != 0;
        const bool lz4 = (file[5] & 0x40u) != 0;
        std::vector<std::uint8_t> body;
        if (lzx || lz4) {
            if (file.size() < 14) {
                module->info_.error = QStringLiteral("Truncated compressed XNB header");
                return ModulePtr(std::move(module));
            }
            const std::uint32_t expandedSize = readLe32(file.data() + 10);
            const std::size_t compressedSize = file.size() - 14;
            const bool ok = lzx
                ? decompressLzx(file.data() + 14, compressedSize, &body, expandedSize)
                : decompressLz4(file.data() + 14, compressedSize, &body, expandedSize);
            if (!ok) {
                module->info_.error = QStringLiteral("XNB compressed content is invalid or unsupported");
                return ModulePtr(std::move(module));
            }
        } else {
            body.assign(file.begin() + 10, file.end());
        }

        std::size_t position = 0;
        std::uint32_t readerCount = 0;
        if (!readVarInt(body, &position, &readerCount) ||
            !readerCount || readerCount > kMaxReaderCount) {
            module->info_.error = QStringLiteral("Invalid XNB content-reader count");
            return ModulePtr(std::move(module));
        }
        QStringList readers;
        readers.reserve(static_cast<int>(readerCount));
        for (std::uint32_t i = 0; i < readerCount; ++i) {
            QString name;
            if (!parseString(body, &position, &name) || body.size() - position < 4) {
                module->info_.error = QStringLiteral("Truncated XNB content-reader table");
                return ModulePtr(std::move(module));
            }
            const std::uint32_t version = readLe32(body.data() + position);
            position += 4;
            readers.append(QStringLiteral("%1 (reader version %2)").arg(name).arg(version));
        }
        std::uint32_t sharedCount = 0;
        std::uint32_t rootReader = 0;
        if (!readVarInt(body, &position, &sharedCount) || sharedCount > kMaxSharedResources ||
            !readVarInt(body, &position, &rootReader) || rootReader > readerCount) {
            module->info_.error = QStringLiteral("Invalid XNB root-reader or shared-resource table");
            return ModulePtr(std::move(module));
        }

        QString rootName = rootReader == 0 ? QStringLiteral("Null")
            : readers.at(static_cast<int>(rootReader - 1));
        const QString metadata = QStringLiteral(
            "XNA / MonoGame compiled content\n"
            "Platform: %1\n"
            "XNB version: %2\n"
            "Compression: %3\n"
            "File size: %4 bytes\n"
            "Expanded content size: %5 bytes\n"
            "Root reader: %6\n"
            "Shared resources: %7\n"
            "Content readers:\n- %8\n")
            .arg(platformName(static_cast<char>(file[3])))
            .arg(file[4])
            .arg(lzx ? QStringLiteral("LZX") : lz4 ? QStringLiteral("LZ4")
                                                    : QStringLiteral("None"))
            .arg(file.size())
            .arg(body.size())
            .arg(rootName)
            .arg(sharedCount)
            .arg(readers.join(QStringLiteral("\n- ")));

        ResourceEntry info;
        info.type = QStringLiteral("XNB_METADATA");
        info.name = QStringLiteral("XNB metadata.txt");
        info.language = QStringLiteral("neutral");
        info.data = metadata.toUtf8();
        info.dataSize = static_cast<quint64>(info.data.size());
        info.format = ModuleFormat::XNB;
        info.isEmbeddedFile = true;
        info.content = std::make_shared<fs::MemoryStore>(
            reinterpret_cast<const std::uint8_t*>(info.data.constData()),
            static_cast<std::size_t>(info.data.size()));
        module->resources_.push_back(std::move(info));

        ResourceEntry asset;
        asset.type = QStringLiteral("XNB_ASSET_DATA");
        asset.name = QFileInfo(sourceName).completeBaseName() + QStringLiteral(".asset.bin");
        asset.language = QStringLiteral("neutral");
        asset.dataSize = static_cast<quint64>(body.size() - position);
        asset.format = ModuleFormat::XNB;
        asset.isEmbeddedFile = true;
        asset.content = std::make_shared<fs::MemoryStore>(body.data() + position,
            body.size() - position);
        module->resources_.push_back(std::move(asset));

        if (rootName.startsWith(QStringLiteral(
                "Microsoft.Xna.Framework.Content.Texture2DReader"))) {
            std::vector<std::uint8_t> textureBmp;
            if (makeColorTextureBmp(body, position, file[4] == 4, &textureBmp)) {
                ResourceEntry texture;
                texture.type = QStringLiteral("XNB_TEXTURE_PREVIEW");
                texture.name = QFileInfo(sourceName).completeBaseName() +
                    QStringLiteral(".texture.bmp");
                texture.language = QStringLiteral("neutral");
                texture.dataSize = static_cast<quint64>(textureBmp.size());
                texture.format = ModuleFormat::XNB;
                texture.isEmbeddedFile = true;
                texture.content = std::make_shared<fs::MemoryStore>(textureBmp.data(),
                    textureBmp.size());
                module->resources_.push_back(std::move(texture));
            }
        }
    } catch (const std::bad_alloc&) {
        module->info_.error = QStringLiteral("Not enough memory to read XNB content");
    } catch (...) {
        module->info_.error = QStringLiteral("Could not parse XNB content");
    }
    return ModulePtr(std::move(module));
}

}  // namespace peare
