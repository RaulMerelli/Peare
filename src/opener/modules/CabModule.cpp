#include "CabModule.h"
#include "Compat.h"
#include "DeflateDecoder.h"
#include "../compression/QuantumCabDecoder.h"

#include "peare/lzx_frontends.h"
#include "../fs/QFileByteStore.h"

#include <QStringList>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace peare {
namespace {

const std::size_t kCabHeaderSize = 36;

struct CabBlock {
    quint32 dataOffset = 0;
    quint16 compressedSize = 0;
    quint16 uncompressedSize = 0;
};

struct CabFolder {
    quint32 dataOffset = 0;
    quint16 dataBlocks = 0;
    quint16 compression = 0;
    quint8 dataReserveBytes = 0;
    quint32 uncompressedSize = 0;
    std::vector<CabBlock> blocks;
};

struct CabFile {
    quint32 size = 0;
    quint32 folderOffset = 0;
    quint16 folderIndex = 0;
    quint16 attribs = 0;
    QString name;
};

quint16 le16(const std::vector<std::uint8_t>& data, std::size_t offset, bool* ok = nullptr) {
    const bool valid = offset <= data.size() && data.size() - offset >= 2;
    if (ok) *ok = valid;
    if (!valid) return 0;
    const std::uint8_t* p = data.data() + offset;
    return quint16(p[0]) | (quint16(p[1]) << 8);
}

quint32 le32(const std::vector<std::uint8_t>& data, std::size_t offset, bool* ok = nullptr) {
    const bool valid = offset <= data.size() && data.size() - offset >= 4;
    if (ok) *ok = valid;
    if (!valid) return 0;
    const std::uint8_t* p = data.data() + offset;
    return quint32(p[0]) | (quint32(p[1]) << 8) |
           (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}

QString safeCabName(QString name, int index) {
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (name.startsWith(QLatin1Char('/'))) name.remove(0, 1);
    const QStringList parts = name.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString leaf = parts.isEmpty() ? QString() : parts.last();
    static const QString forbidden = QStringLiteral("<>:\"/\\|?*");
    for (int i = 0; i < leaf.size(); ++i) {
        if (leaf.at(i).unicode() < 0x20 || forbidden.contains(leaf.at(i)))
            leaf[i] = QLatin1Char('_');
    }
    while (leaf.endsWith(QLatin1Char(' ')) || leaf.endsWith(QLatin1Char('.'))) leaf.chop(1);
    return leaf.isEmpty() ? QStringLiteral("cab_file_%1.bin").arg(index, 3, 10, QLatin1Char('0'))
                          : leaf;
}

QStringList hierarchyForCabName(QString name) {
    name.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (name.startsWith(QLatin1Char('/'))) name.remove(0, 1);
    QStringList parts = name.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (!parts.isEmpty()) parts.removeLast();
    return parts;
}

struct IndependentMszipBlock {
    bool decoded = false;
};

bool decodeIndependentMszipBlocks(const std::vector<std::uint8_t>& cab,
                                  const CabFolder& folder,
                                  std::vector<IndependentMszipBlock>* blocks,
                                  std::vector<std::uint8_t>* output) {
    if (!blocks || folder.uncompressedSize < 1024 * 1024 ||
        folder.uncompressedSize > 64 * 1024 * 1024 || folder.dataBlocks < 8 || !output)
        return false;
    const unsigned available = std::thread::hardware_concurrency();
    const unsigned workerCount = std::min<unsigned>(32, available);
    if (workerCount < 2)
        return false;

    try {
        blocks->clear();
        blocks->resize(folder.blocks.size());
        std::vector<std::size_t> outputOffsets(folder.blocks.size());
        std::size_t outputOffset = 0;
        for (std::size_t i = 0; i < folder.blocks.size(); ++i) {
            outputOffsets[i] = outputOffset;
            outputOffset += folder.blocks[i].uncompressedSize;
        }
        if (quint64(outputOffset) != folder.uncompressedSize)
            return false;
        output->resize(outputOffset);
        std::uint8_t* outputData = output->data();
        std::atomic<std::size_t> next(0);
        std::atomic<bool> failed(false);
        const auto worker = [&]() {
            try {
                for (;;) {
                    const std::size_t index = next.fetch_add(1);
                    if (index >= folder.blocks.size() || failed.load())
                        return;
                    const CabBlock& block = folder.blocks[index];
                    const std::uint8_t* data = cab.data() + block.dataOffset;
                    if (block.compressedSize < 2 || data[0] != 'C' || data[1] != 'K')
                        continue;
                    IndependentMszipBlock& result = (*blocks)[index];
                    std::uint8_t emptyOutput = 0;
                    std::uint8_t* destination = block.uncompressedSize
                        ? outputData + outputOffsets[index]
                        : &emptyOutput;
                    std::string error;
                    result.decoded = compression::inflateRawExactToBuffer(
                        data + 2, block.compressedSize - 2,
                        block.uncompressedSize, destination, &error);
                }
            } catch (...) {
                failed.store(true);
            }
        };

        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        try {
            for (unsigned i = 0; i < workerCount; ++i)
                workers.push_back(std::thread(worker));
        } catch (...) {
            failed.store(true);
        }
        for (std::thread& thread : workers)
            if (thread.joinable()) thread.join();
        return !failed.load();
    } catch (...) {
        blocks->clear();
        return false;
    }
}

bool decodeFolder(const std::vector<std::uint8_t>& cab, const CabFolder& folder,
                  std::vector<std::uint8_t>* decoded,
                  QString* error) {
    decoded->clear();
    decoded->reserve(folder.uncompressedSize);
    std::size_t pos = std::size_t(folder.dataOffset);
    const quint16 type = folder.compression & 0x000f;
    const quint16 lzxWindowBits = (folder.compression >> 8) & 0x001f;
    const quint8 quantumWindowBits = quint8(folder.compression >> 8);
    compression::QuantumCabDecoder quantum;
    if (type == 2 && !quantum.configure(quantumWindowBits)) {
        *error = QStringLiteral("Invalid CAB Quantum window size");
        return false;
    }
    std::vector<IndependentMszipBlock> independentBlocks;
    const bool attemptedIndependent = type == 1 &&
        decodeIndependentMszipBlocks(cab, folder, &independentBlocks, decoded);
    if (type == 1 && !attemptedIndependent)
        decoded->resize(folder.uncompressedSize);
    std::vector<std::uint8_t> lzxStream;
    quint32 lzxExpected = 0;
    std::size_t outputOffset = 0;
    for (quint16 i = 0; i < folder.dataBlocks; ++i) {
        if (pos + 8 + folder.dataReserveBytes > cab.size()) {
            *error = QStringLiteral("Truncated CAB data block");
            return false;
        }
        const quint16 cbData = le16(cab, pos + 4);
        const quint16 cbUncomp = le16(cab, pos + 6);
        pos += 8 + folder.dataReserveBytes;
        if (pos + cbData > cab.size()) {
            *error = QStringLiteral("Truncated CAB compressed block");
            return false;
        }
        const std::uint8_t* block = cab.data() + pos;
        pos += cbData;
        const std::uint8_t* plainData = nullptr;
        std::size_t plainSize = 0;
        if (type == 3) {
            lzxStream.insert(lzxStream.end(), block, block + cbData);
            if (lzxExpected + cbUncomp < lzxExpected) return false;
            lzxExpected += cbUncomp;
            continue;
        } else if (type == 0) {
            if (cbData != cbUncomp) {
                *error = QStringLiteral("Invalid CAB uncompressed block size");
                return false;
            }
            plainData = block;
            plainSize = cbUncomp;
        } else if (type == 1) {
            if (cbData < 2 || block[0] != 'C' || block[1] != 'K') {
                *error = QStringLiteral("Invalid CAB MSZIP block signature");
                return false;
            }
            const IndependentMszipBlock* independent = attemptedIndependent
                ? &independentBlocks[i] : nullptr;
            if (independent && independent->decoded) {
                outputOffset += cbUncomp;
                continue;
            } else {
                const std::size_t historySize = std::min<std::size_t>(outputOffset, 32768);
                const std::uint8_t* history = historySize
                    ? decoded->data() + outputOffset - historySize
                    : nullptr;
                std::uint8_t emptyOutput = 0;
                std::uint8_t* destination = cbUncomp
                    ? decoded->data() + outputOffset
                    : &emptyOutput;
                if (!compression::inflateRawExactToBuffer(
                        block + 2, cbData - 2, cbUncomp, history,
                        std::size_t(historySize), destination)) {
                    *error = QStringLiteral("CAB MSZIP decompression failed");
                    return false;
                }
                outputOffset += cbUncomp;
                continue;
            }
        } else if (type == 2) {
            std::vector<std::uint8_t> quantumBlock;
            if (!quantum.decodeBlock(block, cbData, cbUncomp, i != 0,
                                     &quantumBlock)) {
                *error = QStringLiteral("CAB Quantum decompression failed");
                return false;
            }
            decoded->insert(decoded->end(), quantumBlock.begin(), quantumBlock.end());
        } else {
            *error = QStringLiteral("Unsupported CAB compression type %1").arg(type);
            return false;
        }
        if (plainSize)
            decoded->insert(decoded->end(), plainData, plainData + plainSize);
    }
    if (type == 3) {
        if (lzxWindowBits < 15 || lzxWindowBits > 21) {
            *error = QStringLiteral("Invalid CAB LZX window size");
            return false;
        }
        decoded->resize(lzxExpected);
        peare_lzx_cab_decoder* lzx = nullptr;
        peare_lzx_status status = peare_lzx_cab_create(size_t(1u) << lzxWindowBits, &lzx);
        if (status == PEARE_LZX_OK)
            status = peare_lzx_cab_decompress(lzx, lzxStream.data(),
                                              lzxStream.size(), decoded->data(),
                                              decoded->size());
        peare_lzx_cab_destroy(lzx);
        if (status != PEARE_LZX_OK) {
            *error = QStringLiteral("CAB LZX decompression failed");
            decoded->clear();
            return false;
        }
    } else if (type == 1 && quint64(outputOffset) != folder.uncompressedSize) {
        *error = QStringLiteral("CAB folder output size mismatch");
        decoded->clear();
        return false;
    } else if (type == 2 && quint64(decoded->size()) != folder.uncompressedSize) {
        *error = QStringLiteral("CAB folder output size mismatch");
        decoded->clear();
        return false;
    }
    return true;
}

bool measureFolder(const std::vector<std::uint8_t>& cab, CabFolder* folder,
                   QString* error) {
    const quint16 type = folder->compression & 0x000f;
    const quint16 lzxWindowBits = (folder->compression >> 8) & 0x001f;
    const quint8 quantumWindowBits = quint8(folder->compression >> 8);
    if (type != 0 && type != 1 && type != 2 && type != 3) {
        *error = QStringLiteral("Unsupported CAB compression type %1").arg(type);
        return false;
    }
    if (type == 2 && (quantumWindowBits < 10 || quantumWindowBits > 21)) {
        *error = QStringLiteral("Invalid CAB Quantum window size");
        return false;
    }
    if (type == 3 && (lzxWindowBits < 15 || lzxWindowBits > 21)) {
        *error = QStringLiteral("Invalid CAB LZX window size");
        return false;
    }

    folder->blocks.clear();
    folder->blocks.reserve(folder->dataBlocks);
    std::size_t pos = std::size_t(folder->dataOffset);
    quint64 total = 0;
    for (quint16 i = 0; i < folder->dataBlocks; ++i) {
        if (pos + 8 + folder->dataReserveBytes > cab.size()) {
            *error = QStringLiteral("Truncated CAB data block");
            return false;
        }
        const quint16 cbData = le16(cab, pos + 4);
        const quint16 cbUncomp = le16(cab, pos + 6);
        pos += 8 + folder->dataReserveBytes;
        if (pos + cbData > cab.size()) {
            *error = QStringLiteral("Truncated CAB compressed block");
            return false;
        }
        if (type == 0 && cbData != cbUncomp) {
            *error = QStringLiteral("Invalid CAB uncompressed block size");
            return false;
        }
        total += cbUncomp;
        if (total > std::numeric_limits<quint32>::max() ||
            pos > std::numeric_limits<quint32>::max()) {
            *error = QStringLiteral("CAB folder size exceeds its format field");
            return false;
        }
        CabBlock block;
        block.dataOffset = quint32(pos);
        block.compressedSize = cbData;
        block.uncompressedSize = cbUncomp;
        folder->blocks.push_back(block);
        pos += cbData;
    }
    folder->uncompressedSize = quint32(total);
    return true;
}

class CabFolderStore final : public fs::IByteStore {
public:
    CabFolderStore(std::shared_ptr<const std::vector<std::uint8_t>> cab, CabFolder folder)
        : cab_(std::move(cab)), folder_(folder) {}

    std::int64_t capacity() const override { return folder_.uncompressedSize; }

    int read(std::int64_t pos, std::uint8_t* dst, int count) const override {
        if (pos < 0 || count <= 0 || pos >= folder_.uncompressedSize || !dst)
            return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!decodeAttempted_) {
            decodeAttempted_ = true;
            decodeSucceeded_ = decodeFolder(*cab_, folder_, &decoded_, &error_);
        }
        if (!decodeSucceeded_)
            return 0;
        const std::size_t offset = static_cast<std::size_t>(pos);
        const std::size_t available = decoded_.size() - offset;
        const std::size_t amount = std::min<std::size_t>(static_cast<std::size_t>(count), available);
        std::memcpy(dst, decoded_.data() + offset, amount);
        return static_cast<int>(amount);
    }

private:
    std::shared_ptr<const std::vector<std::uint8_t>> cab_;
    CabFolder folder_;
    mutable std::mutex mutex_;
    mutable bool decodeAttempted_ = false;
    mutable bool decodeSucceeded_ = false;
    mutable std::vector<std::uint8_t> decoded_;
    mutable QString error_;
};

}  // namespace

std::unique_ptr<CabModule> CabModule::open(const QString& filePath) {
    auto source = std::make_shared<fs::QFileByteStore>(filePath);
    if (!source->validate()) {
        auto module = peare::makeUnique<CabModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::CAB;
        module->info_.error = QStringLiteral("Could not read CAB archive");
        return module;
    }
    return open(source, filePath);
}

std::unique_ptr<CabModule> CabModule::open(const fs::ByteStorePtr& source,
                                           const QString& logicalName) {
    if (!source || source->capacity() < 0 ||
        std::uint64_t(source->capacity()) > std::numeric_limits<std::size_t>::max()) {
        auto module = peare::makeUnique<CabModule>();
        module->info_.filePath = logicalName;
        module->info_.format = ModuleFormat::CAB;
        module->info_.error = QStringLiteral("Invalid CAB byte source");
        return module;
    }
    return open(source->readAll(), logicalName);
}

std::unique_ptr<CabModule> CabModule::open(const QByteArray& data,
                                           const QString& logicalName) {
    return open(std::vector<std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(data.constData()),
        reinterpret_cast<const std::uint8_t*>(data.constData()) + data.size()), logicalName);
}

std::unique_ptr<CabModule> CabModule::open(std::vector<std::uint8_t> data,
                                           const QString& logicalName) {
    auto module = peare::makeUnique<CabModule>();
    module->info_.filePath = logicalName;
    module->info_.format = ModuleFormat::CAB;
    module->info_.description = QStringLiteral("Microsoft Cabinet (CAB) archive");

    if (data.size() < kCabHeaderSize || std::memcmp(data.data(), "MSCF", 4) != 0) {
        module->info_.error = QStringLiteral("Invalid CAB signature or truncated header");
        return module;
    }
    const quint32 cbCabinet = le32(data, 8);
    const quint32 coffFiles = le32(data, 16);
    const quint16 cFolders = le16(data, 26);
    const quint16 cFiles = le16(data, 28);
    const quint16 flags = le16(data, 30);
    if (cbCabinet > data.size() || coffFiles >= data.size() || cFolders == 0) {
        module->info_.error = QStringLiteral("Invalid CAB header fields");
        return module;
    }
    std::size_t pos = kCabHeaderSize;
    quint8 folderReserve = 0;
    quint8 dataReserve = 0;
    if (flags & 0x0004) {
        if (pos + 4 > data.size()) {
            module->info_.error = QStringLiteral("Truncated CAB reserve header");
            return module;
        }
        const quint16 headerReserve = le16(data, pos);
        folderReserve = data[pos + 2];
        dataReserve = data[pos + 3];
        pos += 4 + headerReserve;
    }

    auto cabinet = std::make_shared<const std::vector<std::uint8_t>>(std::move(data));
    std::vector<CabFolder> folders;
    folders.reserve(cFolders);
    for (quint16 i = 0; i < cFolders; ++i) {
        if (pos + 8 > cabinet->size()) {
            module->info_.error = QStringLiteral("Truncated CAB folder table");
            return module;
        }
        CabFolder folder;
        folder.dataOffset = le32(*cabinet, pos);
        folder.dataBlocks = le16(*cabinet, pos + 4);
        folder.compression = le16(*cabinet, pos + 6);
        folder.dataReserveBytes = dataReserve;
        folders.push_back(folder);
        pos += 8 + folderReserve;
    }

    std::vector<CabFile> files;
    pos = std::size_t(coffFiles);
    for (quint16 i = 0; i < cFiles; ++i) {
        if (pos + 16 > cabinet->size()) {
            module->info_.error = QStringLiteral("Truncated CAB file table");
            return module;
        }
        CabFile file;
        file.size = le32(*cabinet, pos);
        file.folderOffset = le32(*cabinet, pos + 4);
        file.folderIndex = le16(*cabinet, pos + 8);
        file.attribs = le16(*cabinet, pos + 14);
        pos += 16;
        const auto terminator = std::find(cabinet->begin() + std::ptrdiff_t(pos),
                                          cabinet->end(), std::uint8_t(0));
        if (terminator == cabinet->end()) {
            module->info_.error = QStringLiteral("CAB filename is not NUL-terminated");
            return module;
        }
        const std::size_t nameLength = std::size_t(terminator - (cabinet->begin() +
                                                                 std::ptrdiff_t(pos)));
        if (nameLength > std::size_t(std::numeric_limits<int>::max())) {
            module->info_.error = QStringLiteral("CAB filename is too long");
            return module;
        }
        file.name = QString::fromLocal8Bit(
            reinterpret_cast<const char*>(cabinet->data() + pos), int(nameLength));
        pos += nameLength + 1;
        if (file.folderIndex >= folders.size()) {
            module->info_.error = QStringLiteral("CAB file references invalid folder");
            return module;
        }
        files.push_back(file);
    }

    std::vector<peare::fs::ByteStorePtr> folderStores;
    folderStores.reserve(folders.size());
    for (std::size_t i = 0; i < folders.size(); ++i) {
        QString error;
        if (!measureFolder(*cabinet, &folders[i], &error)) {
            module->info_.error = error;
            return module;
        }
        folderStores.push_back(std::make_shared<CabFolderStore>(cabinet, folders[i]));
    }

    for (std::size_t i = 0; i < files.size(); ++i) {
        const CabFile& file = files[i];
        const CabFolder& folder = folders[file.folderIndex];
        if (file.folderOffset > folder.uncompressedSize ||
            file.size > folder.uncompressedSize - file.folderOffset) {
            module->info_.error = QStringLiteral("CAB file range exceeds folder");
            return module;
        }
        ResourceEntry entry;
        entry.type = QStringLiteral("CAB_FILE");
        entry.isEmbeddedFile = true;
        entry.name = safeCabName(file.name, int(i));
        entry.language = QStringLiteral("neutral");
        entry.dataOffset = file.folderOffset;
        entry.dataSize = file.size;
        entry.format = ModuleFormat::CAB;
        entry.hierarchyPath = hierarchyForCabName(file.name);
        entry.content = std::make_shared<peare::fs::SubStore>(
            folderStores[file.folderIndex], file.folderOffset, file.size);
        module->resources_.push_back(std::move(entry));
    }
    return module;
}

}  // namespace peare
