#include "SwapModule.h"
#include "Compat.h"

#include <QFile>

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace peare {
namespace {

const std::int64_t kPageSize = 4096;
const char kMagic1[] = "SWAP-SPACE";
const char kMagic2[] = "SWAPSPACE2";

std::uint32_t le32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

QString label(const std::vector<std::uint8_t>& header) {
    int n = 0;
    while (n < 16 && header[0x41c + n] != 0) ++n;
    return QString::fromUtf8(reinterpret_cast<const char*>(header.data() + 0x41c), n);
}

bool isSwapMagic(const std::uint8_t* p) {
    return std::memcmp(p, kMagic1, 10) == 0 || std::memcmp(p, kMagic2, 10) == 0;
}

fs::ByteStorePtr storeForFile(const QString& path) {
    auto holder = std::make_shared<QFile>(path);
    if (!holder->open(QIODevice::ReadOnly)) return nullptr;
    const qint64 size = holder->size();
    uchar* mapped = size > 0 ? holder->map(0, size) : nullptr;
    if (mapped)
        return std::make_shared<fs::ExternalStore>(
            reinterpret_cast<const std::uint8_t*>(mapped), std::int64_t(size),
            std::static_pointer_cast<void>(holder));
    const QByteArray bytes = holder->readAll();
    return std::make_shared<fs::MemoryStore>(
        reinterpret_cast<const std::uint8_t*>(bytes.constData()), std::size_t(bytes.size()));
}

}  // namespace

ModulePtr SwapModule::open(const fs::ByteStorePtr& file, const QString& sourceName) {
    auto module = peare::makeUnique<SwapModule>();
    module->info_.filePath = sourceName;
    module->info_.format = ModuleFormat::SWAP;
    module->info_.description = QStringLiteral("Linux swap");

    if (!file || file->capacity() < kPageSize) {
        module->info_.error = QStringLiteral("Truncated Linux swap header");
        return ModulePtr(std::move(module));
    }
    std::vector<std::uint8_t> header = file->readRange(0, kPageSize);
    if (header.size() < kPageSize || !isSwapMagic(header.data() + kPageSize - 10)) {
        module->info_.error = QStringLiteral("Invalid Linux swap signature");
        return ModulePtr(std::move(module));
    }
    const std::uint32_t version = le32(header.data() + 0x400);
    const std::uint32_t lastPage = le32(header.data() + 0x404);
    const std::uint32_t badPages = le32(header.data() + 0x408);
    const QString volume = label(header);
    QString desc = QStringLiteral("Linux swap v%1, %2 pages").arg(version).arg(lastPage);
    if (!volume.isEmpty())
        desc += QStringLiteral(", label \"%1\"").arg(volume);
    if (badPages)
        desc += QStringLiteral(", %1 bad pages").arg(badPages);
    module->info_.description = desc;
    return ModulePtr(std::move(module));
}

ModulePtr SwapModule::open(const QString& filePath) {
    fs::ByteStorePtr file = storeForFile(filePath);
    if (!file) {
        auto module = peare::makeUnique<SwapModule>();
        module->info_.filePath = filePath;
        module->info_.format = ModuleFormat::SWAP;
        module->info_.error = QStringLiteral("Cannot open file");
        return ModulePtr(std::move(module));
    }
    return open(file, filePath);
}

}  // namespace peare
