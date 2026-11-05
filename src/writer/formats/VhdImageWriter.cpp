#include "VhdImageWriter.h"

#include <QtEndian>

#include <algorithm>
#include <cstring>
#include <limits>
#include <set>

namespace peare {
namespace writer {
namespace {

const qint64 kSector = 512;
const quint32 kUnallocated = 0xffffffffU;

quint32 footerChecksum(const char* bytes, int length, int checksumOffset) {
    quint32 sum = 0;
    for (int i = 0; i < length; ++i)
        if (i < checksumOffset || i >= checksumOffset + 4)
            sum += static_cast<unsigned char>(bytes[i]);
    return ~sum;
}

bool validChecksum(const QByteArray& bytes, int checksumOffset) {
    return bytes.size() >= checksumOffset + 4 &&
        qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(bytes.constData() + checksumOffset)) ==
            footerChecksum(bytes.constData(), bytes.size(), checksumOffset);
}

}  // namespace

VhdImageWriter::VhdImageWriter(QFile* file, bool writable, QString* error)
    : file_(file), writable_(writable) {
    valid_ = parse(error);
}

bool VhdImageWriter::readFile(qint64 offset, char* data, qint64 length) const {
    if (!file_ || offset < 0 || length < 0 || offset > file_->size() ||
        length > file_->size() - offset || !file_->seek(offset)) return false;
    qint64 done = 0;
    while (done < length) {
        const qint64 count = file_->read(data + done, length - done);
        if (count <= 0) return false;
        done += count;
    }
    return true;
}

bool VhdImageWriter::writeFile(qint64 offset, const char* data, qint64 length) {
    if (!file_ || !writable_ || offset < 0 || length < 0 || !file_->seek(offset))
        return false;
    qint64 done = 0;
    while (done < length) {
        const qint64 count = file_->write(data + done, length - done);
        if (count <= 0) return false;
        done += count;
    }
    return true;
}

bool VhdImageWriter::parse(QString* error) {
    auto fail = [&](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (!file_ || file_->size() < 2 * kSector || file_->size() % kSector)
        return fail(QStringLiteral("Truncated or unaligned VHD image."));
    QByteArray frontFooter(int(kSector), Qt::Uninitialized);
    QByteArray endFooter(int(kSector), Qt::Uninitialized);
    if (!readFile(0, frontFooter.data(), frontFooter.size()) ||
        !readFile(file_->size() - kSector, endFooter.data(), endFooter.size()) ||
        std::memcmp(frontFooter.constData(), "conectix", 8) != 0 ||
        std::memcmp(endFooter.constData(), "conectix", 8) != 0 ||
        frontFooter != endFooter || !validChecksum(frontFooter, 64) ||
        qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(frontFooter.constData() + 12)) != 0x00010000U ||
        qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(frontFooter.constData() + 60)) != 3)
        return fail(QStringLiteral("Only consistent, checksum-valid dynamic VHD images are supported."));
    const quint64 diskSize = qFromBigEndian<quint64>(
        reinterpret_cast<const uchar*>(frontFooter.constData() + 48));
    const quint64 footerDataOffset = qFromBigEndian<quint64>(
        reinterpret_cast<const uchar*>(frontFooter.constData() + 16));
    if (!diskSize || diskSize > 2ULL * 1024 * 1024 * 1024 * 1024 ||
        diskSize % kSector || diskSize > quint64((std::numeric_limits<qint64>::max)()) ||
        footerDataOffset != kSector)
        return fail(QStringLiteral("Invalid dynamic VHD size or header pointer."));
    diskSize_ = qint64(diskSize);
    headerOffset_ = qint64(footerDataOffset);
    QByteArray header(1024, Qt::Uninitialized);
    if (!readFile(headerOffset_, header.data(), header.size()) ||
        std::memcmp(header.constData(), "cxsparse", 8) != 0 || !validChecksum(header, 36) ||
        qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(header.constData() + 24)) != 0x00010000U)
        return fail(QStringLiteral("Invalid VHD dynamic header or checksum."));
    const quint64 tableOffset = qFromBigEndian<quint64>(
        reinterpret_cast<const uchar*>(header.constData() + 16));
    const quint64 headerDataOffset = qFromBigEndian<quint64>(
        reinterpret_cast<const uchar*>(header.constData() + 8));
    const quint32 tableEntries = qFromBigEndian<quint32>(
        reinterpret_cast<const uchar*>(header.constData() + 28));
    const quint32 blockSize = qFromBigEndian<quint32>(
        reinterpret_cast<const uchar*>(header.constData() + 32));
    if (!blockSize || blockSize % kSector || (blockSize & (blockSize - 1)) ||
        blockSize < 64 * 1024 || blockSize > 64 * 1024 * 1024)
        return fail(QStringLiteral("Invalid dynamic VHD block size."));
    const quint64 neededBlocks = (diskSize - 1) / blockSize + 1;
    if (headerDataOffset != (std::numeric_limits<quint64>::max)() ||
        !tableEntries || tableEntries > 1000000 || neededBlocks != tableEntries ||
        tableOffset % kSector ||
        tableOffset < quint64(headerOffset_ + header.size()) ||
        tableOffset > quint64(file_->size()) ||
        quint64(tableEntries) * 4 > quint64(file_->size()) - tableOffset ||
        neededBlocks > quint64((std::numeric_limits<int>::max)()))
        return fail(QStringLiteral("Invalid dynamic VHD BAT geometry."));
    blockSize_ = blockSize;
    requiredBlocks_ = qint64(neededBlocks);
    tableOffset_ = qint64(tableOffset);
    bitmapSize_ = ((blockSize_ / kSector + 7) / 8 + kSector - 1) / kSector * kSector;
    if (bitmapSize_ <= 0 || bitmapSize_ > blockSize_)
        return fail(QStringLiteral("Invalid dynamic VHD bitmap size."));

    QByteArray rawTable(int(requiredBlocks_ * 4), Qt::Uninitialized);
    if (!readFile(tableOffset_, rawTable.data(), rawTable.size()))
        return fail(QStringLiteral("Cannot read dynamic VHD BAT."));
    bat_.resize(static_cast<std::size_t>(requiredBlocks_));
    bitmaps_.resize(static_cast<std::size_t>(requiredBlocks_));
    std::vector<std::pair<quint64, quint64>> extents;
    extents.push_back({0, kSector});
    extents.push_back({quint64(headerOffset_), quint64(headerOffset_ + header.size())});
    extents.push_back({tableOffset_, tableOffset + quint64(tableEntries) * 4});
    extents.push_back({quint64(file_->size() - kSector), quint64(file_->size())});
    for (qint64 i = 0; i < requiredBlocks_; ++i) {
        const quint32 entry = qFromBigEndian<quint32>(
            reinterpret_cast<const uchar*>(rawTable.constData() + i * 4));
        bat_[static_cast<std::size_t>(i)] = entry;
        if (entry == kUnallocated) continue;
        const quint64 start = quint64(entry) * kSector;
        const quint64 extentLength = quint64(bitmapSize_ + blockSize_);
        if (start % kSector || start > quint64(file_->size()) ||
            extentLength > quint64(file_->size()) - start)
            return fail(QStringLiteral("Dynamic VHD block lies outside the image."));
        extents.push_back({start, start + extentLength});
        QByteArray bitmap(int(bitmapSize_), Qt::Uninitialized);
        if (!readFile(qint64(start), bitmap.data(), bitmap.size()))
            return fail(QStringLiteral("Truncated dynamic VHD block bitmap."));
        bitmaps_[static_cast<std::size_t>(i)] = bitmap;
    }
    std::sort(extents.begin(), extents.end());
    for (size_t i = 1; i < extents.size(); ++i)
        if (extents[i].first < extents[i - 1].second)
            return fail(QStringLiteral("Dynamic VHD metadata and data blocks overlap."));
    valid_ = true;
    return true;
}

bool VhdImageWriter::read(qint64 offset, char* data, qint64 length) const {
    if (!valid_ || !data || offset < 0 || length < 0 || offset > diskSize_ ||
        length > diskSize_ - offset) return false;
    qint64 done = 0;
    while (done < length) {
        const qint64 position = offset + done;
        const qint64 block = position / blockSize_;
        const qint64 inBlock = position % blockSize_;
        const qint64 sector = inBlock / kSector;
        const qint64 inSector = inBlock % kSector;
        const qint64 count = std::min(length - done, kSector - inSector);
        const auto changed = changedBlocks_.find(block);
        if (changed != changedBlocks_.end()) {
            std::memcpy(data + done, changed->second.data() + inBlock,
                        static_cast<std::size_t>(count));
        } else if (block >= requiredBlocks_ ||
                   bat_[static_cast<std::size_t>(block)] == kUnallocated ||
                   !(static_cast<unsigned char>(bitmaps_[static_cast<std::size_t>(block)].at(
                       int(sector / 8))) & (1U << (7 - (sector & 7))))) {
            std::memset(data + done, 0, static_cast<std::size_t>(count));
        } else {
            const qint64 host = qint64(bat_[static_cast<std::size_t>(block)]) * kSector +
                bitmapSize_ + inBlock;
            if (!readFile(host, data + done, count)) return false;
        }
        done += count;
    }
    return true;
}

bool VhdImageWriter::write(qint64 offset, const char* data, qint64 length) {
    if (!valid_ || !writable_ || !data || offset < 0 || length < 0 ||
        offset > diskSize_ || length > diskSize_ - offset) return false;
    qint64 done = 0;
    while (done < length) {
        const qint64 position = offset + done;
        const qint64 block = position / blockSize_;
        const qint64 inBlock = position % blockSize_;
        const qint64 count = std::min(length - done, blockSize_ - inBlock);
        if (bat_[static_cast<std::size_t>(block)] != kUnallocated) {
            const qint64 bitmapOffset = qint64(bat_[static_cast<std::size_t>(block)]) * kSector;
            const qint64 dataOffset = bitmapOffset + bitmapSize_ + inBlock;
            if (!writeFile(dataOffset, data + done, count)) return false;
            QByteArray& bitmap = bitmaps_[static_cast<std::size_t>(block)];
            auto& changedBytes = changedBitmapBytes_[block];
            const qint64 firstSector = inBlock / kSector;
            const qint64 lastSector = (inBlock + count - 1) / kSector;
            for (qint64 sector = firstSector; sector <= lastSector; ++sector) {
                const qint64 byteIndex = sector / 8;
                const unsigned char mask = static_cast<unsigned char>(1U << (7 - (sector & 7)));
                const unsigned char oldValue = static_cast<unsigned char>(bitmap.at(int(byteIndex)));
                if (!(oldValue & mask)) {
                    bitmap[int(byteIndex)] = char(oldValue | mask);
                    changedBytes.insert(byteIndex);
                }
            }
            done += count;
            continue;
        }
        auto changed = changedBlocks_.find(block);
        if (changed == changedBlocks_.end()) {
            std::vector<char> bytes(static_cast<std::size_t>(blockSize_), 0);
            const qint64 validLength = std::min(blockSize_, diskSize_ - block * blockSize_);
            if (!read(block * blockSize_, bytes.data(), validLength)) return false;
            changed = changedBlocks_.insert(std::make_pair(block, std::move(bytes))).first;
        }
        std::memcpy(changed->second.data() + inBlock, data + done,
                    static_cast<std::size_t>(count));
        done += count;
    }
    return true;
}

bool VhdImageWriter::finish(QString* error) {
    if (!valid_ || !writable_ || !file_) {
        if (error) *error = QStringLiteral("VHD writer is not active.");
        return false;
    }
    qint64 appendOffset = file_->size();
    QByteArray bitmap(int(bitmapSize_), '\0');
    for (const auto& changed : changedBitmapBytes_) {
        const qint64 bitmapOffset = qint64(bat_[static_cast<std::size_t>(changed.first)]) * kSector;
        for (qint64 byteIndex : changed.second) {
            const char value = bitmaps_[static_cast<std::size_t>(changed.first)].at(int(byteIndex));
            if (!writeFile(bitmapOffset + byteIndex, &value, 1)) {
                if (error) *error = file_->errorString();
                return false;
            }
        }
    }
    for (const auto& changed : changedBlocks_) {
        const qint64 block = changed.first;
        const qint64 validBytes = std::min(blockSize_, diskSize_ - block * blockSize_);
        const qint64 sectors = (validBytes + kSector - 1) / kSector;
        std::fill(bitmap.begin(), bitmap.end(), '\0');
        for (qint64 sector = 0; sector < sectors; ++sector)
            bitmap[int(sector / 8)] = char(static_cast<unsigned char>(bitmap.at(int(sector / 8))) |
                                           (1U << (7 - (sector & 7))));
        if (appendOffset < 0 || appendOffset % kSector ||
            appendOffset / kSector > (std::numeric_limits<quint32>::max)() ||
            appendOffset > (std::numeric_limits<qint64>::max)() - bitmapSize_ - blockSize_) {
            if (error) *error = QStringLiteral("Dynamic VHD allocation exceeds BAT limits.");
            return false;
        }
        if (!writeFile(appendOffset, bitmap.constData(), bitmap.size()) ||
            !writeFile(appendOffset + bitmapSize_, changed.second.data(), blockSize_)) {
            if (error) *error = file_->errorString();
            return false;
        }
        char encoded[4];
        qToBigEndian<quint32>(quint32(appendOffset / kSector), reinterpret_cast<uchar*>(encoded));
        if (!writeFile(tableOffset_ + block * 4, encoded, sizeof(encoded))) {
            if (error) *error = file_->errorString();
            return false;
        }
        bat_[static_cast<std::size_t>(block)] = quint32(appendOffset / kSector);
        bitmaps_[static_cast<std::size_t>(block)] = bitmap;
        appendOffset += bitmapSize_ + blockSize_;
    }
    if (!changedBlocks_.empty()) {
        QByteArray footer(int(kSector), Qt::Uninitialized);
        if (!readFile(0, footer.data(), footer.size()) ||
            !writeFile(appendOffset, footer.constData(), footer.size())) {
            if (error) *error = file_->errorString().isEmpty()
                ? QStringLiteral("Could not write the updated VHD footer.") : file_->errorString();
            return false;
        }
    }
    if (!file_->flush()) {
        if (error) *error = file_->errorString();
        return false;
    }
    changedBlocks_.clear();
    changedBitmapBytes_.clear();
    return true;
}

}  // namespace writer
}  // namespace peare
