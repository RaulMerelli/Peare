#include "ExtVolumeUpdate.h"

#include <ext4.h>
#include <ext4_crc32.h>
#include <ext4_balloc.h>
#include <ext4_extent.h>
#include <ext4_inode.h>
#include <ext4_mkfs.h>
#include <ext4_super.h>
#include <ext4_trans.h>

#include <QFile>
#include <QDateTime>
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QSysInfo>
#include <QThread>
#include <QUuid>
#include <QtEndian>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <new>
#include <limits>
#include <map>
#include <set>
#include <vector>

namespace {

static_assert(offsetof(struct ext4_sblock, encoding) == 636,
              "Unexpected Ext4 encoding field offset");
static_assert(offsetof(struct ext4_sblock, orphan_file_inum) == 640,
              "Unexpected Ext4 orphan-file field offset");

const char kDeviceName[] = "peare-ext";
const char kJournalDeviceName[] = "peare-ext-journal";
const char kMountPoint[] = "/peare/";
const quint32 kAllowedCompat = EXT4_FCOM_DIR_PREALLOC |
    EXT4_FCOM_IMAGIC_INODES |
    EXT4_FCOM_HAS_JOURNAL | EXT4_FCOM_EXT_ATTR |
    EXT4_FCOM_RESIZE_INODE | EXT4_FCOM_DIR_INDEX |
    EXT4_FCOM_SPARSE_SUPER2 | EXT4_FCOM_FAST_COMMIT |
    EXT4_FCOM_STABLE_INODES |
    EXT4_FCOM_ORPHAN_FILE;
const quint32 kAllowedIncompat = EXT4_FINCOM_FILETYPE | EXT4_FINCOM_META_BG |
    EXT4_FINCOM_RECOVER | EXT4_FINCOM_EXTENTS | EXT4_FINCOM_64BIT |
    EXT4_FINCOM_FLEX_BG | EXT4_FINCOM_CSUM_SEED | EXT4_FINCOM_LARGEDIR |
    EXT4_FINCOM_INLINE_DATA | EXT4_FINCOM_EA_INODE | EXT4_FINCOM_MMP |
    EXT4_FINCOM_CASEFOLD | EXT4_FINCOM_ENCRYPT;
const quint32 kAllowedReadOnly = EXT4_FRO_COM_SPARSE_SUPER |
    EXT4_FRO_COM_LARGE_FILE | EXT4_FRO_COM_HUGE_FILE |
    EXT4_FRO_COM_GDT_CSUM | EXT4_FRO_COM_DIR_NLINK |
    EXT4_FRO_COM_EXTRA_ISIZE | EXT4_FRO_COM_METADATA_CSUM |
    EXT4_FRO_COM_BIGALLOC | EXT4_FRO_COM_VERITY |
    EXT4_FRO_COM_QUOTA | EXT4_FRO_COM_PROJECT |
    EXT4_FRO_COM_SHARED_BLOCKS;
const qint64 kIoBlockSize = 512;
const quint16 kExt4EncodingUtf8_12_1 = 1;
const size_t kCopyBufferSize = 64 * 1024;
const quint32 kOrphanBlockMagic = 0x0b10ca04;
const quint32 kMaxOrphanFileBlocks = 512;
const quint32 kMmpMagic = 0x004d4d50;
const quint32 kMmpSeqClean = 0xff4d4d50;
const quint32 kMmpSeqFsck = 0xe24d4d50;
const quint32 kMmpSeqMax = 0xe24d4d4f;
const quint32 kMmpMinCheckInterval = 5;
const qint64 kMmpRecordSize = 1024;
const qint64 kMmpChecksumOffset = 1020;
const quint32 kFsverityBlockSize = 4096;
const quint32 kExtEncryptedInodeFlag = 0x00000800;
const quint32 kFsverityDigestSize = 32;
const quint32 kFsverityDescriptorSize = 256;
const quint32 kFsverityMetadataStartAlignment = 65536;
const quint32 kExtUserQuotaMagic = 0xd9c01f11;
const quint32 kExtGroupQuotaMagic = 0xd9c01927;
const quint32 kExtProjectQuotaMagic = 0xd9c03f14;
const quint32 kExtProjectInheritFlag = 0x20000000;
const quint32 kExtImmutableInodeFlag = 0x00000010;
const quint32 kExtAppendInodeFlag = 0x00000020;
const quint32 kExtUnsupportedMutationFlags = 0x00000001 | 0x00000004 |
    0x00000100 | 0x00000200 | 0x00000800 | 0x00004000 |
    0x00400000 | 0x80000000;
const quint32 kExtKnownInodeFlags = 0x00000001 | 0x00000002 | 0x00000004 |
    0x00000008 | 0x00000010 | 0x00000020 | 0x00000040 | 0x00000080 |
    0x00000100 | 0x00000200 | 0x00000400 | 0x00000800 | 0x00001000 |
    0x00002000 | 0x00004000 | 0x00008000 | 0x00010000 | 0x00020000 |
    0x00040000 | 0x00080000 | 0x00100000 | 0x00200000 | 0x00400000 |
    0x02000000 | 0x10000000 | 0x20000000 | 0x40000000 | 0x80000000;
std::mutex g_extMutex;

bool writeExt4Bytes(ext4_file* file, const char* data, size_t size) {
    while (size) {
        size_t written = 0;
        const int result = ext4_fwrite(file, data, size, &written);
        if (result != EOK || !written || written > size) return false;
        data += written;
        size -= written;
    }
    return true;
}

bool writeExt4Zeros(ext4_file* file, quint64 size) {
    std::array<char, 64 * 1024> zeros = {};
    while (size) {
        const size_t count = size_t(qMin<quint64>(size, zeros.size()));
        if (!writeExt4Bytes(file, zeros.data(), count)) return false;
        size -= count;
    }
    return true;
}

bool buildFsverityMetadata(ext4_file* file, quint64 fileSize,
                           quint32 blockSize,
                           QByteArray* tree, QByteArray* descriptor) {
    if (!file || !tree || !descriptor || blockSize < 1024 ||
        blockSize > 65536 || blockSize % kFsverityDigestSize ||
        fileSize > quint64(INT_MAX) * 128)
        return false;
    const quint32 hashesPerBlock = blockSize / kFsverityDigestSize;
    quint64 dataBlocks = (fileSize + blockSize - 1) / blockSize;
    if (dataBlocks > quint64(INT_MAX) / kFsverityDigestSize) return false;

    std::vector<QByteArray> hashes;
    hashes.reserve(size_t(dataBlocks));
    for (quint64 offset = 0; offset < fileSize;
         offset += blockSize) {
        QByteArray block(int(blockSize), '\0');
        const size_t wanted = size_t(qMin<quint64>(
            fileSize - offset, blockSize));
        size_t read = 0;
        if (ext4_fread(file, block.data(), wanted, &read) != EOK ||
            read != wanted)
            return false;
        hashes.push_back(QCryptographicHash::hash(
            block, QCryptographicHash::Sha256));
    }

    std::vector<QByteArray> levels;
    while (hashes.size() > 1) {
        const size_t parentCount =
            (hashes.size() + hashesPerBlock - 1) / hashesPerBlock;
        if (parentCount > size_t(INT_MAX) / blockSize)
            return false;
        QByteArray level;
        level.reserve(int(parentCount * blockSize));
        std::vector<QByteArray> parents;
        parents.reserve(parentCount);
        for (size_t first = 0; first < hashes.size();
             first += hashesPerBlock) {
            QByteArray block(int(blockSize), '\0');
            const size_t count = qMin<size_t>(
                hashesPerBlock, hashes.size() - first);
            for (size_t index = 0; index < count; ++index)
                std::memcpy(block.data() +
                    index * kFsverityDigestSize, hashes[first + index].constData(),
                    kFsverityDigestSize);
            level.append(block);
            parents.push_back(QCryptographicHash::hash(
                block, QCryptographicHash::Sha256));
        }
        levels.push_back(level);
        hashes.swap(parents);
    }

    tree->clear();
    quint64 treeSize = 0;
    for (const QByteArray& level : levels) {
        treeSize += quint64(level.size());
        if (treeSize > INT_MAX) return false;
    }
    tree->reserve(int(treeSize));
    for (auto level = levels.crbegin(); level != levels.crend(); ++level)
        tree->append(*level);

    descriptor->fill('\0', int(kFsverityDescriptorSize));
    (*descriptor)[0] = 1;
    (*descriptor)[1] = 1;
    quint32 logBlockSize = 0;
    for (quint32 value = blockSize; value > 1; value >>= 1)
        ++logBlockSize;
    (*descriptor)[2] = char(logBlockSize);
    qToLittleEndian<quint64>(fileSize,
        reinterpret_cast<uchar*>(descriptor->data() + 8));
    if (fileSize) {
        if (hashes.size() != 1 ||
            hashes.front().size() != int(kFsverityDigestSize))
            return false;
        std::memcpy(descriptor->data() + 16, hashes.front().constData(),
                    kFsverityDigestSize);
    }
    return true;
}

bool writeFsverityMetadata(const QByteArray& path, quint64 fileSize,
                           QString* error) {
    struct ext4_sblock* sb = nullptr;
    if (ext4_get_sblock(kMountPoint, &sb) != EOK || !sb ||
        !(ext4_get32(sb, features_read_only) & EXT4_FRO_COM_VERITY)) {
        if (error) *error = QStringLiteral(
            "The Ext volume does not have fs-verity enabled.");
        return false;
    }
    const quint64 filesystemBlockSize = ext4_sb_get_block_size(sb);
    if (!filesystemBlockSize || filesystemBlockSize > 65536) {
        if (error) *error = QStringLiteral(
            "The Ext block size is not supported for fs-verity.");
        return false;
    }
    const quint32 blockSize = quint32(qMin<quint64>(
        kFsverityBlockSize, filesystemBlockSize));

    ext4_file input = {};
    if (ext4_fopen(&input, path.constData(), "rb") != EOK) {
        if (error) *error = QStringLiteral(
            "Ext could not read the file for fs-verity.");
        return false;
    }
    if (input.fsize != fileSize) {
        ext4_fclose(&input);
        if (error) *error = QStringLiteral(
            "The Ext file size changed before fs-verity was built.");
        return false;
    }
    QByteArray tree;
    QByteArray descriptor;
    const bool metadataBuilt = buildFsverityMetadata(
        &input, fileSize, blockSize, &tree, &descriptor);
    const int inputClose = ext4_fclose(&input);
    if (!metadataBuilt || inputClose != EOK) {
        if (error) *error = QStringLiteral(
            "Peare could not build the fs-verity Merkle tree.");
        return false;
    }

    const quint64 alignment = kFsverityMetadataStartAlignment;
    if (fileSize > std::numeric_limits<quint64>::max() - alignment) {
        if (error) *error = QStringLiteral("The Ext file is too large for fs-verity.");
        return false;
    }
    const quint64 metadataStart =
        (fileSize + alignment - 1) & ~(alignment - 1);
    if (metadataStart > std::numeric_limits<quint64>::max() -
            quint64(tree.size()) - blockSize - kFsverityDescriptorSize - 4) {
        if (error) *error = QStringLiteral("The Ext file is too large for fs-verity.");
        return false;
    }
    const quint64 descriptorPosition =
        (metadataStart + quint64(tree.size()) + blockSize - 1) /
        blockSize * blockSize;
    const quint64 descriptorEnd = descriptorPosition + kFsverityDescriptorSize;
    const quint64 descriptorSizePosition =
        (descriptorEnd + 4 + blockSize - 1) / blockSize * blockSize - 4;
    const quint64 fileEnd = descriptorSizePosition + 4;
    if (fileEnd > quint64(std::numeric_limits<int64_t>::max())) {
        if (error) *error = QStringLiteral("The Ext file is too large for fs-verity.");
        return false;
    }

    ext4_file output = {};
    if (ext4_fopen(&output, path.constData(), "r+b") != EOK) {
        if (error) *error = QStringLiteral(
            "Ext could not open the file to store fs-verity metadata.");
        return false;
    }
    bool written = ext4_fseek(&output, 0, SEEK_END) == EOK &&
        writeExt4Zeros(&output, metadataStart - fileSize) &&
        writeExt4Bytes(&output, tree.constData(), size_t(tree.size()));
    const quint64 treeEnd = metadataStart + quint64(tree.size());
    written = written && writeExt4Zeros(&output, descriptorPosition - treeEnd) &&
        writeExt4Bytes(&output, descriptor.constData(),
                       size_t(descriptor.size()));
    written = written && writeExt4Zeros(
        &output, descriptorSizePosition - descriptorEnd);
    const quint32 descriptorSize = qToLittleEndian<quint32>(
        kFsverityDescriptorSize);
    written = written && writeExt4Bytes(
        &output, reinterpret_cast<const char*>(&descriptorSize),
        sizeof(descriptorSize));
    const int outputClose = ext4_fclose(&output);
    if (!written || outputClose != EOK ||
        ext4_fset_verity(path.constData(), fileSize) != EOK) {
        if (error) *error = QStringLiteral(
            "Ext could not commit the fs-verity metadata.");
        return false;
    }
    return true;
}

quint32 mmpChecksum(const QByteArray& record, quint32 seed) {
    return ext4_crc32c(seed, record.constData(),
                       uint32_t(kMmpChecksumOffset));
}

void setMmpChecksum(QByteArray* record, quint32 seed, bool enabled) {
    if (!enabled || !record || record->size() < kMmpRecordSize) return;
    qToLittleEndian<quint32>(
        mmpChecksum(*record, seed),
        reinterpret_cast<uchar*>(record->data() + kMmpChecksumOffset));
}

bool hasExtFeatures(const peare_ext_io& io, bool* ext3 = nullptr,
                    bool* ext4 = nullptr, bool* orphanFileOutput = nullptr,
                    bool* externalJournalOutput = nullptr) {
    if (!io.context || !io.read || !io.write || io.size < 2048 ||
        io.size % kIoBlockSize) return false;
    unsigned char superblock[1024];
    if (!io.read(io.context, 1024, superblock, sizeof(superblock))) return false;
    const quint16 magic = qFromLittleEndian<quint16>(superblock + 56);
    const quint16 state = qFromLittleEndian<quint16>(superblock + 58);
    quint64 blocks = qFromLittleEndian<quint32>(superblock + 4);
    const quint32 logBlockSize = qFromLittleEndian<quint32>(superblock + 24);
    const quint32 logClusterSize = qFromLittleEndian<quint32>(superblock + 28);
    const quint32 blocksPerGroup = qFromLittleEndian<quint32>(superblock + 32);
    const quint32 inodesPerGroup = qFromLittleEndian<quint32>(superblock + 40);
    const quint32 firstDataBlock = qFromLittleEndian<quint32>(superblock + 20);
    const quint32 revision = qFromLittleEndian<quint32>(superblock + 76);
    const quint16 inodeSize = revision
        ? qFromLittleEndian<quint16>(superblock + 88) : 128;
    const quint16 rawDescriptorSize =
        qFromLittleEndian<quint16>(superblock + 254);
    const quint16 descriptorSize = rawDescriptorSize
        ? rawDescriptorSize : EXT4_MIN_BLOCK_GROUP_DESCRIPTOR_SIZE;
    const quint32 compat = qFromLittleEndian<quint32>(superblock + 92);
    const quint32 incompat = qFromLittleEndian<quint32>(superblock + 96);
    const quint32 readOnly = qFromLittleEndian<quint32>(superblock + 100);
    const quint32 lastOrphan = qFromLittleEndian<quint32>(superblock + 232);
    const quint32 orphanFileInode = qFromLittleEndian<quint32>(superblock + 640);
    const quint16 encoding = qFromLittleEndian<quint16>(superblock +
        offsetof(struct ext4_sblock, encoding));
    const quint16 encodingFlags = qFromLittleEndian<quint16>(superblock +
        offsetof(struct ext4_sblock, encoding_flags));
    const quint32 journalInode = qFromLittleEndian<quint32>(superblock +
        offsetof(struct ext4_sblock, journal_inode_number));
    const quint32 journalDevice = qFromLittleEndian<quint32>(superblock +
        offsetof(struct ext4_sblock, journal_dev));
    const quint32 backupGroup0 = qFromLittleEndian<quint32>(superblock +
        offsetof(struct ext4_sblock, backup_bgs));
    const quint32 backupGroup1 = qFromLittleEndian<quint32>(superblock +
        offsetof(struct ext4_sblock, backup_bgs) + sizeof(quint32));
    const quint8 checksumType = superblock[
        offsetof(struct ext4_sblock, checksum_type)];
    const quint32 inodeCount = qFromLittleEndian<quint32>(superblock);
    if (incompat & EXT4_FINCOM_64BIT)
        blocks |= quint64(qFromLittleEndian<quint32>(superblock + 336)) << 32;
    const bool hasJournal = (compat & EXT4_FCOM_HAS_JOURNAL) != 0;
    const bool needsJournalRecovery =
        (incompat & EXT4_FINCOM_RECOVER) != 0;
    const quint64 blockSize = quint64(1024) << logBlockSize;
    const bool sparseSuper = (readOnly & EXT4_FRO_COM_SPARSE_SUPER) != 0;
    const bool resizeInode = (compat & EXT4_FCOM_RESIZE_INODE) != 0;
    const bool metadataChecksum =
        (readOnly & EXT4_FRO_COM_METADATA_CSUM) != 0;
    const bool groupChecksum = (readOnly & EXT4_FRO_COM_GDT_CSUM) != 0;
    const bool checksumSeed = (incompat & EXT4_FINCOM_CSUM_SEED) != 0;
    const bool sparseSuper2 = (compat & EXT4_FCOM_SPARSE_SUPER2) != 0;
    const bool orphanFile = (compat & EXT4_FCOM_ORPHAN_FILE) != 0;
    const bool casefold = (incompat & EXT4_FINCOM_CASEFOLD) != 0;
    const bool bigalloc = (readOnly & EXT4_FRO_COM_BIGALLOC) != 0;
    const quint32 clusterBits = logClusterSize >= logBlockSize
        ? logClusterSize - logBlockSize : 31;
    const quint64 clusterRatio = clusterBits < 31
        ? (quint64(1) << clusterBits) : 0;
    const quint64 bitmapGroupBits = bigalloc && clusterRatio
        ? (quint64(blocksPerGroup) + clusterRatio - 1) / clusterRatio
        : blocksPerGroup;
    const bool validClusterLayout = bigalloc
        ? clusterRatio && blocksPerGroup % clusterRatio == 0 &&
          bitmapGroupBits <= blockSize * 8
        : logClusterSize == logBlockSize;
    if (magic != 0xef53 ||
        (state != 1 && (!needsJournalRecovery || (state & 0x0006))) ||
        (needsJournalRecovery && !hasJournal) || lastOrphan ||
        (readOnly & EXT4_FRO_COM_ORPHAN_PRESENT) ||
        (orphanFile != (orphanFileInode != 0)) ||
        (orphanFileInode >= inodeCount) || logBlockSize > 6 ||
        revision > 1 || inodeSize < 128 || inodeSize > blockSize ||
        (inodeSize & (inodeSize - 1)) != 0 ||
        (rawDescriptorSize &&
         rawDescriptorSize < EXT4_MIN_BLOCK_GROUP_DESCRIPTOR_SIZE) ||
        descriptorSize < EXT4_MIN_BLOCK_GROUP_DESCRIPTOR_SIZE ||
        descriptorSize > EXT4_MAX_BLOCK_GROUP_DESCRIPTOR_SIZE ||
        ((incompat & EXT4_FINCOM_64BIT) && descriptorSize < 64) ||
        (resizeInode && !sparseSuper) ||
        (metadataChecksum && groupChecksum) ||
        (checksumSeed && !metadataChecksum) ||
        !blocks || !blocksPerGroup || !inodesPerGroup ||
        !validClusterLayout ||
        (!bigalloc && blocksPerGroup > blockSize * 8) ||
        inodesPerGroup > blockSize * 8 ||
        firstDataBlock != (blockSize == 1024 && !bigalloc ? 1U : 0U) ||
        (compat & ~kAllowedCompat) || (incompat & ~kAllowedIncompat) ||
        (readOnly & ~kAllowedReadOnly) ||
        (casefold && (encoding != kExt4EncodingUtf8_12_1 || encodingFlags)) ||
        (hasJournal && ((journalInode && journalInode >= inodeCount) ||
                        (journalInode && journalDevice) ||
                        (!journalInode && !journalDevice))) ||
        (!hasJournal && (journalInode || journalDevice))) return false;
    if (metadataChecksum && checksumType != EXT4_CHECKSUM_CRC32C) return false;
    if (blockSize > EXT4_MAX_BLOCK_SIZE ||
        quint64(blocks) > quint64(io.size) / blockSize) return false;
    if (sparseSuper2) {
        const quint64 firstBlock = firstDataBlock;
        const quint64 groupCount =
            (quint64(blocks) - firstBlock + blocksPerGroup - 1) /
            blocksPerGroup;
        if ((backupGroup0 && backupGroup0 >= groupCount) ||
            (backupGroup1 && backupGroup1 >= groupCount) ||
            (backupGroup0 && backupGroup0 == backupGroup1)) return false;
    } else if (backupGroup0 || backupGroup1) {
        return false;
    }
    if (ext3) *ext3 = hasJournal;
    if (ext4) *ext4 = orphanFile ||
                      (incompat & (EXT4_FINCOM_EXTENTS |
                                   EXT4_FINCOM_64BIT |
                                   EXT4_FINCOM_FLEX_BG)) != 0 ||
                      (readOnly & (EXT4_FRO_COM_METADATA_CSUM |
                                   EXT4_FRO_COM_HUGE_FILE |
                                   EXT4_FRO_COM_DIR_NLINK |
                                   EXT4_FRO_COM_EXTRA_ISIZE)) != 0;
    if (orphanFileOutput) *orphanFileOutput = orphanFile;
    if (externalJournalOutput) *externalJournalOutput =
        hasJournal && journalDevice != 0 && journalInode == 0;
    return true;
}

bool normalizeExtPath(const QByteArray& utf8, QString* path, QString* error) {
    if (!path || utf8.isEmpty()) {
        if (error) *error = QStringLiteral("A non-empty Ext entry path is required.");
        return false;
    }
    const QString name = QString::fromUtf8(utf8.constData(), utf8.size());
    if (name.isEmpty() || name.toUtf8() != utf8 || name.contains(QChar(0)) ||
        name.startsWith(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) ||
        (name.size() > 1 && name.at(1) == QLatin1Char(':'))) {
        if (error) *error = QStringLiteral("The Ext entry path is invalid UTF-8 or absolute.");
        return false;
    }
    const QStringList parts = name.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    for (const QString& part : parts) {
        const QByteArray encoded = part.toUtf8();
        if (part.isEmpty() || part == QStringLiteral(".") ||
            part == QStringLiteral("..") || encoded.size() > 255) {
            if (error) *error = QStringLiteral("The Ext entry path contains an invalid component.");
            return false;
        }
    }
    *path = name;
    return true;
}

QString extPath(const QString& path) {
    return QString::fromLatin1(kMountPoint) + path;
}

bool extEncryptionEnabled(struct ext4_fs* fs) {
    return fs && (ext4_get32(&fs->sb, features_incompatible) &
                  EXT4_FINCOM_ENCRYPT);
}

bool inodeIsEncrypted(const struct ext4_inode* inode) {
    return inode &&
        (ext4_inode_get_flags(const_cast<struct ext4_inode*>(inode)) &
         kExtEncryptedInodeFlag);
}

bool checkInodeMutationFlags(const struct ext4_inode* inode, QString* error) {
    if (!inode) {
        if (error) *error = QStringLiteral("Ext could not inspect inode flags.");
        return false;
    }
    const quint32 flags = ext4_inode_get_flags(
        const_cast<struct ext4_inode*>(inode));
    if (flags & kExtImmutableInodeFlag) {
        if (error) *error = QStringLiteral("Ext cannot modify an immutable inode.");
        return false;
    }
    if (flags & kExtAppendInodeFlag) {
        if (error) *error = QStringLiteral("Ext cannot modify an append-only inode.");
        return false;
    }
    if (flags & kExtUnsupportedMutationFlags) {
        if (error) *error = QStringLiteral(
            "Ext cannot modify an inode with unsupported data or deletion flags.");
        return false;
    }
    if (flags & ~kExtKnownInodeFlags) {
        if (error) *error = QStringLiteral("Ext cannot modify an inode with unknown flags.");
        return false;
    }
    return true;
}

bool checkPathMutationFlags(struct ext4_fs* fs, const QByteArray& path,
                            QString* error) {
    struct ext4_inode inode = {};
    uint32_t inodeNumber = 0;
    if (!fs || ext4_raw_inode_fill(path.constData(), &inodeNumber, &inode) != EOK) {
        if (error) *error = QStringLiteral(
            "Ext could not inspect inode flags before mutation.");
        return false;
    }
    return checkInodeMutationFlags(&inode, error);
}

bool rejectEncryptedInode(struct ext4_fs* fs, const QByteArray& path,
                          QString* error) {
    if (!extEncryptionEnabled(fs)) return false;
    struct ext4_inode inode = {};
    uint32_t inodeNumber = 0;
    if (ext4_raw_inode_fill(path.constData(), &inodeNumber, &inode) != EOK) {
        if (error) *error = QStringLiteral(
            "Ext cannot inspect the inode encryption state before mutation.");
        return true;
    }
    if (!inodeIsEncrypted(&inode))
        return false;
    if (error) *error = QStringLiteral(
        "Ext cannot modify an encrypted inode without its fscrypt key.");
    return true;
}

bool inheritInodeFlags(struct ext4_fs* fs, const QByteArray& parentPath,
                       uint32_t childNumber, bool childDirectory) {
    struct ext4_inode parent = {};
    uint32_t parentNumber = 0;
    QByteArray resolvedParent = parentPath;
    if (resolvedParent == QByteArray(kMountPoint).chopped(1))
        resolvedParent = kMountPoint;
    if (ext4_raw_inode_fill(resolvedParent.constData(), &parentNumber,
                            &parent) != EOK)
        return false;
    struct ext4_inode_ref parentRef = {};
    if (ext4_fs_get_inode_ref(fs, parentNumber, &parentRef) != EOK) return false;
    const quint32 parentFlags = ext4_inode_get_flags(parentRef.inode);
    const quint32 projectFeature = ext4_get32(&fs->sb, features_read_only) &
        EXT4_FRO_COM_PROJECT;
    const bool inheritProject = projectFeature &&
        (parentFlags & kExtProjectInheritFlag);
    quint32 projectId = 0;
    if (inheritProject) {
        if (ext4_get16(parentRef.inode, extra_isize) < 32) {
            ext4_fs_put_inode_ref(&parentRef);
            return false;
        }
        const uchar* field = reinterpret_cast<const uchar*>(parentRef.inode) +
            offsetof(struct ext4_inode, version_hi) + 4;
        projectId = qFromLittleEndian<quint32>(field);
    }
    if (ext4_fs_put_inode_ref(&parentRef) != EOK) return false;
    quint32 inheritedFlags = parentFlags &
        (0x00000002 | 0x00000008 | 0x00000040 | 0x00000080 |
         0x00000400 | 0x00008000 | 0x00010000 | 0x02000000);
    if (childDirectory) {
        inheritedFlags |= parentFlags & (kExtProjectInheritFlag | 0x40000000);
    } else {
        inheritedFlags &= ~0x00010000;
    }
    if (!inheritProject && !inheritedFlags) return true;
    struct ext4_inode_ref childRef = {};
    if (ext4_fs_get_inode_ref(fs, childNumber, &childRef) != EOK) return false;
    if (inheritProject && ext4_get16(childRef.inode, extra_isize) < 32) {
        ext4_fs_put_inode_ref(&childRef);
        return false;
    }
    if (inheritProject) {
        uchar* field = reinterpret_cast<uchar*>(childRef.inode) +
            offsetof(struct ext4_inode, version_hi) + 4;
        qToLittleEndian<quint32>(projectId, field);
    }
    ext4_inode_set_flags(childRef.inode,
        ext4_inode_get_flags(childRef.inode) | inheritedFlags);
    childRef.dirty = true;
    return ext4_fs_put_inode_ref(&childRef) == EOK;
}

bool ensureExtParents(struct ext4_fs* fs, const QString& path, QString* error) {
    const bool encryptionEnabled = extEncryptionEnabled(fs);
    if (encryptionEnabled) {
        struct ext4_inode root = {};
        uint32_t rootNumber = 0;
        const QByteArray rootPath(kMountPoint);
        if (ext4_raw_inode_fill(rootPath.constData(), &rootNumber, &root) != EOK ||
            inodeIsEncrypted(&root)) {
            if (error) *error = QStringLiteral(
                "Ext cannot update a path below an encrypted directory without its key.");
            return false;
        }
        if (!checkInodeMutationFlags(&root, error)) return false;
    } else {
        if (!checkPathMutationFlags(fs, QByteArray(kMountPoint), error))
            return false;
    }
    int slash = path.indexOf(QLatin1Char('/'));
    while (slash >= 0) {
        const QByteArray parent = extPath(path.left(slash)).toUtf8();
        const int exists = ext4_inode_exist(parent.constData(), EXT4_DE_DIR);
        if (exists == EOK) {
            struct ext4_inode inode = {};
            uint32_t inodeNumber = 0;
            if (ext4_raw_inode_fill(parent.constData(), &inodeNumber, &inode) != EOK ||
                (encryptionEnabled && inodeIsEncrypted(&inode))) {
                if (error) *error = QStringLiteral(
                    "Ext cannot update a path below an encrypted directory without its key.");
                return false;
            }
            if (!checkInodeMutationFlags(&inode, error)) return false;
        }
        if (exists != EOK) {
            if (encryptionEnabled) {
                if (error) *error = QStringLiteral(
                    "Ext cannot create missing parent directories on an ENCRYPT volume without a key.");
                return false;
            }
            if (ext4_dir_mk(parent.constData()) != EOK) {
                if (error) *error = QStringLiteral("Ext could not create a parent directory.");
                return false;
            }
            struct ext4_inode created = {};
            uint32_t createdNumber = 0;
            const QByteArray parentParent = parent.left(parent.lastIndexOf('/'));
            if (ext4_raw_inode_fill(parent.constData(), &createdNumber,
                                    &created) != EOK ||
                !inheritInodeFlags(fs, parentParent, createdNumber, true)) {
                if (error) *error = QStringLiteral(
                    "Ext could not inherit the parent project ID.");
                return false;
            }
        }
        slash = path.indexOf(QLatin1Char('/'), slash + 1);
    }
    return true;
}

bool isEmptyDirectory(const QByteArray& path) {
    ext4_dir directory = {};
    if (ext4_dir_open(&directory, path.constData()) != EOK) return false;
    bool empty = true;
    const ext4_direntry* entry = nullptr;
    while ((entry = ext4_dir_entry_next(&directory)) != nullptr) {
        const QByteArray name(reinterpret_cast<const char*>(entry->name),
                              entry->name_length);
        if (name != QByteArray(".") && name != QByteArray("..")) {
            empty = false;
            break;
        }
    }
    const int closeResult = ext4_dir_close(&directory);
    return empty && closeResult == EOK;
}

int hasExternalXattr(struct ext4_fs* fs, uint32_t inode) {
    struct ext4_inode_ref ref = {};
    if (!fs || !inode || ext4_fs_get_inode_ref(fs, inode, &ref) != EOK)
        return -1;
    const int result = ext4_fs_check_xattr_block(&ref);
    if (ext4_fs_put_inode_ref(&ref) != EOK) return -1;
    return result == EOK ? 0 : -1;
}

bool validateOrphanFile(struct ext4_fs* fs, QString* failure = nullptr) {
    auto reject = [failure](const QString& message) {
        if (failure) *failure = message;
        return false;
    };
    if (!fs || !ext4_sb_feature_com(&fs->sb, EXT4_FCOM_ORPHAN_FILE))
        return true;

    const uint32_t inodeNumber = ext4_get32(&fs->sb, orphan_file_inum);
    struct ext4_inode_ref inodeRef = {};
    if (!inodeNumber || inodeNumber >= ext4_get32(&fs->sb, inodes_count))
        return reject(QStringLiteral("The orphan inode number is invalid."));
    const int inodeReadResult = ext4_fs_get_inode_ref(fs, inodeNumber, &inodeRef);
    if (inodeReadResult != EOK)
        return reject(QStringLiteral("The orphan inode could not be read (%1).")
            .arg(inodeReadResult));

    const uint32_t blockSize = ext4_sb_get_block_size(&fs->sb);
    const uint64_t size = ext4_inode_get_size(&fs->sb, inodeRef.inode);
    const uint64_t blockCount = size / blockSize;
    bool valid = ext4_fs_check_inode(&inodeRef) == EOK &&
                 ext4_inode_type(&fs->sb, inodeRef.inode) ==
                     EXT4_INODE_MODE_FILE &&
                 ext4_inode_get_links_cnt(inodeRef.inode) == 1 && size &&
                 size % blockSize == 0 && blockCount <= kMaxOrphanFileBlocks;
    if (!valid) {
        ext4_fs_put_inode_ref(&inodeRef);
        return reject(QStringLiteral("The orphan inode metadata is invalid."));
    }
    const bool metadataChecksum = ext4_sb_feature_ro_com(
        &fs->sb, EXT4_FRO_COM_METADATA_CSUM);
    const uint32_t entriesSize = blockSize - 8;
    uint32_t inodeSeed = ext4_sb_get_csum_seed(&fs->sb);
    const uint32_t inodeNumberLe = qToLittleEndian<quint32>(inodeNumber);
    const uint32_t generationLe = qToLittleEndian<quint32>(
        ext4_inode_get_generation(inodeRef.inode));
    inodeSeed = ext4_crc32c(inodeSeed, &inodeNumberLe, sizeof(inodeNumberLe));
    inodeSeed = ext4_crc32c(inodeSeed, &generationLe, sizeof(generationLe));

    QByteArray data(int(blockSize), '\0');
    for (uint64_t logical = 0; valid && logical < blockCount; ++logical) {
        ext4_fsblk_t physical = 0;
        struct ext4_block block = {};
        if (ext4_fs_get_inode_dblk_idx(&inodeRef, ext4_lblk_t(logical),
                                       &physical, true) != EOK ||
            !physical || physical >= ext4_sb_get_blocks_cnt(&fs->sb) ||
            ext4_block_get(fs->bdev, &block, physical) != EOK) {
            if (failure) *failure = QStringLiteral(
                "Orphan block %1 cannot be read from its extent.").arg(logical);
            valid = false;
            break;
        }
        memcpy(data.data(), block.data, blockSize);
        if (ext4_block_set(fs->bdev, &block) != EOK) {
            valid = false;
            break;
        }

        const quint32 magic = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar*>(data.constData() + entriesSize));
        if (magic != kOrphanBlockMagic ||
            !std::all_of(data.constData(), data.constData() + entriesSize,
                         [](char value) { return value == 0; })) {
            if (failure) *failure = QStringLiteral(
                "Orphan block %1 has invalid entries or a tail marker.").arg(logical);
            valid = false;
            break;
        }
        if (metadataChecksum) {
            const quint64 physicalLe = qToLittleEndian<quint64>(physical);
            uint32_t checksum = ext4_crc32c(inodeSeed, &physicalLe,
                                            sizeof(physicalLe));
            checksum = ext4_crc32c(checksum, data.constData(), entriesSize);
            const quint32 stored = qFromLittleEndian<quint32>(
                reinterpret_cast<const uchar*>(data.constData() + entriesSize + 4));
            valid = checksum == stored;
            if (!valid && failure) *failure = QStringLiteral(
                "Orphan block %1 has an invalid checksum.").arg(logical);
        }
    }

    if (ext4_fs_put_inode_ref(&inodeRef) != EOK) {
        if (valid && failure) *failure = QStringLiteral(
            "The orphan inode could not be released cleanly.");
        valid = false;
    }
    return valid;
}

int checkPathXattrs(struct ext4_fs* fs, const QByteArray& path, bool directory,
                    bool recursive, std::set<uint32_t>* visited) {
    if (!visited) return -1;
    if (directory) {
        struct DirectoryFrame {
            ext4_dir directory = {};
            QByteArray path;
            bool opened = false;

            ~DirectoryFrame() {
                if (opened) ext4_dir_close(&directory);
            }

            bool close() {
                if (!opened) return true;
                opened = false;
                return ext4_dir_close(&directory) == EOK;
            }
        };
        auto openDirectory = [&](const QByteArray& currentPath, int* result)
            -> std::unique_ptr<DirectoryFrame> {
            auto frame = std::unique_ptr<DirectoryFrame>(new (std::nothrow) DirectoryFrame);
            if (!frame || ext4_dir_open(&frame->directory, currentPath.constData()) != EOK) {
                *result = -1;
                return nullptr;
            }
            frame->opened = true;
            frame->path = currentPath;
            const uint32_t inode = frame->directory.f.inode;
            if (!visited->insert(inode).second) {
                frame->close();
                *result = -1;
                return nullptr;
            }
            *result = hasExternalXattr(fs, inode);
            struct ext4_inode raw = {};
            uint32_t inodeNumber = 0;
            if (*result == 0 && ext4_raw_inode_fill(currentPath.constData(),
                    &inodeNumber, &raw) != EOK)
                *result = -1;
            if (*result == 0 && !checkInodeMutationFlags(&raw, nullptr))
                *result = 3;
            if (*result == 0 && extEncryptionEnabled(fs) &&
                inodeIsEncrypted(&raw)) *result = 2;
            if (*result != 0) {
                frame->close();
                return nullptr;
            }
            return frame;
        };

        int result = 0;
        auto root = openDirectory(path, &result);
        if (!root) return result;
        if (!recursive) return root->close() ? 0 : -1;

        std::vector<std::unique_ptr<DirectoryFrame>> stack;
        stack.push_back(std::move(root));
        while (result == 0 && !stack.empty()) {
            DirectoryFrame& frame = *stack.back();
            const ext4_direntry* entry = ext4_dir_entry_next(&frame.directory);
            if (!entry) {
                if (!frame.close()) result = -1;
                stack.pop_back();
                continue;
            }
            const QByteArray leaf(reinterpret_cast<const char*>(entry->name),
                                  entry->name_length);
            if (leaf == QByteArray(".") || leaf == QByteArray("..")) continue;
            const QByteArray childPath = frame.path + "/" + leaf;
            if (entry->inode_type == EXT4_DE_DIR) {
                auto child = openDirectory(childPath, &result);
                if (child) stack.push_back(std::move(child));
            } else {
                struct ext4_inode childInode = {};
                uint32_t childNumber = 0;
                if (ext4_raw_inode_fill(childPath.constData(), &childNumber,
                                        &childInode) != EOK) {
                    result = -1;
                    continue;
                }
                result = hasExternalXattr(fs, childNumber);
                if (result == 0 && !checkInodeMutationFlags(&childInode, nullptr))
                    result = 3;
                if (result == 0 && extEncryptionEnabled(fs) &&
                    inodeIsEncrypted(&childInode))
                    result = 2;
            }
        }

        while (!stack.empty()) {
            if (!stack.back()->close() && result == 0)
                result = -1;
            stack.pop_back();
        }
        return result;
    }

    struct ext4_inode inode = {};
    uint32_t inodeNumber = 0;
    if (ext4_raw_inode_fill(path.constData(), &inodeNumber, &inode) != EOK)
        return -1;
    if (extEncryptionEnabled(fs) && inodeIsEncrypted(&inode)) return 2;
    if (!checkInodeMutationFlags(&inode, nullptr)) return 3;
    return hasExternalXattr(fs, inodeNumber);
}

struct QuotaUsage { quint64 bytes = 0; quint64 inodes = 0; };
struct QuotaRecord {
    quint32 id = 0;
    quint64 offset = 0;
};

bool addQuotaUsage(std::map<quint32, QuotaUsage>* usage,
                   quint32 id, quint64 bytes) {
    QuotaUsage& current = (*usage)[id];
    if (current.bytes > UINT64_MAX - bytes ||
        current.inodes == UINT64_MAX)
        return false;
    current.bytes += bytes;
    ++current.inodes;
    return true;
}

bool quotaBytes(struct ext4_fs* fs, struct ext4_inode_ref* ref,
                quint64 offset, char* data, quint64 size, bool write) {
    const quint32 blockSize = ext4_sb_get_block_size(&fs->sb);
    while (size) {
        const ext4_lblk_t logical = ext4_lblk_t(offset / blockSize);
        const quint32 within = quint32(offset % blockSize);
        const quint32 count = quint32(qMin<quint64>(size, blockSize - within));
        ext4_fsblk_t physical = 0;
        if (ext4_fs_get_inode_dblk_idx(ref, logical, &physical, false) != EOK ||
            !physical)
            return false;
        struct ext4_block block = {};
        if (ext4_trans_block_get(fs->bdev, &block, physical) != EOK)
            return false;
        if (write) {
            std::memcpy(block.data + within, data, count);
            if (ext4_trans_set_block_dirty(block.buf) != EOK) return false;
        } else {
            std::memcpy(data, block.data + within, count);
        }
        data += count;
        offset += count;
        size -= count;
    }
    return true;
}

bool quotaTree(struct ext4_fs* fs, struct ext4_inode_ref* ref,
               quint32 blockCount, quint32 depth, quint32 blockId,
               quint32 entrySize, std::set<quint32>* visited,
               std::vector<QuotaRecord>* records) {
    if (!blockId || blockId >= blockCount || !visited->insert(blockId).second)
        return false;
    QByteArray block(1024, '\0');
    if (!quotaBytes(fs, ref, quint64(blockId) * 1024,
                    block.data(), 1024, false))
        return false;
    if (depth < 3) {
        for (quint32 index = 0; index < 256; ++index) {
            const quint32 child = qFromLittleEndian<quint32>(
                reinterpret_cast<const uchar*>(block.constData() + index * 4));
            if (child && !quotaTree(fs, ref, blockCount, depth + 1, child,
                                    entrySize, visited, records))
                return false;
        }
        return true;
    }
    const quint32 capacity = (1024 - 16) / entrySize;
    for (quint32 index = 0; index < 256; ++index) {
        const quint32 dataBlock = qFromLittleEndian<quint32>(
            reinterpret_cast<const uchar*>(block.constData() + index * 4));
        if (!dataBlock) continue;
        if (dataBlock >= blockCount) return false;
        if (!visited->insert(dataBlock).second) continue;
        QByteArray data(1024, '\0');
        if (!quotaBytes(fs, ref, quint64(dataBlock) * 1024,
                        data.data(), 1024, false))
            return false;
        const quint16 count = qFromLittleEndian<quint16>(
            reinterpret_cast<const uchar*>(data.constData() + 8));
        if (count > capacity) return false;
        for (quint32 slot = 0; slot < capacity; ++slot) {
            QByteArray entry(int(entrySize), '\0');
            const quint64 position = quint64(dataBlock) * 1024 + 16 +
                quint64(slot) * entrySize;
            std::memcpy(entry.data(), data.constData() + 16 +
                        slot * entrySize, entrySize);
            bool empty = true;
            for (char value : entry) empty = empty && value == 0;
            if (!empty) records->push_back({qFromLittleEndian<quint32>(
                reinterpret_cast<const uchar*>(entry.constData())), position});
        }
    }
    return true;
}

bool updateQuotaFile(struct ext4_fs* fs, quint32 inodeNumber,
                     quint32 expectedMagic,
                     const std::map<quint32, QuotaUsage>& usage,
                     int* failureStage) {
    *failureStage = 1;
    struct ext4_inode_ref ref = {};
    if (!inodeNumber || ext4_fs_get_inode_ref(fs, inodeNumber, &ref) != EOK)
        return false;
    const quint64 fileSize = ext4_inode_get_size(&fs->sb, ref.inode);
    const quint32 mode = ext4_inode_get_mode(&fs->sb, ref.inode);
    QByteArray header(32, '\0');
    if ((mode & EXT4_INODE_MODE_TYPE_MASK) != EXT4_INODE_MODE_FILE ||
        fileSize < 2048 ||
        !quotaBytes(fs, &ref, 0, header.data(), header.size(), false)) {
        *failureStage = 2;
        ext4_fs_put_inode_ref(&ref);
        return false;
    }
    const quint32 version = qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar*>(header.constData() + 4));
    const quint32 magic = qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar*>(header.constData()));
    const quint32 blockCount = qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar*>(header.constData() + 20));
    if (magic != expectedMagic || version > 1 || blockCount < 2 ||
        quint64(blockCount) * 1024 > fileSize) {
        *failureStage = 3;
        ext4_fs_put_inode_ref(&ref);
        return false;
    }
    const bool revision1 = version == 1;
    const quint32 entrySize = revision1 ? 72 : 40;
    std::set<quint32> visited;
    std::vector<QuotaRecord> records;
    if (!quotaTree(fs, &ref, blockCount, 0, 1, entrySize,
                   &visited, &records)) {
        *failureStage = 4;
        ext4_fs_put_inode_ref(&ref);
        return false;
    }
    std::map<quint32, quint64> recordOffsets;
    for (const QuotaRecord& record : records) {
        if (!recordOffsets.emplace(record.id, record.offset).second) {
            *failureStage = 5;
            ext4_fs_put_inode_ref(&ref);
            return false;
        }
    }
    for (const auto& item : usage) {
        if (!recordOffsets.count(item.first)) {
            *failureStage = 6;
            ext4_fs_put_inode_ref(&ref);
            return false;
        }
    }
    for (const QuotaRecord& record : records) {
        const auto found = usage.find(record.id);
        const QuotaUsage current = found == usage.end()
            ? QuotaUsage{} : found->second;
        const quint32 inodeOffset = revision1 ? 24 : 12;
        const quint32 spaceOffset = revision1 ? 48 : 24;
        QByteArray encoded(16, '\0');
        if (revision1) {
            qToLittleEndian<quint64>(current.inodes,
                reinterpret_cast<uchar*>(encoded.data()));
            qToLittleEndian<quint64>(current.bytes,
                reinterpret_cast<uchar*>(encoded.data() + 8));
        } else {
            if (current.inodes > UINT32_MAX) {
                *failureStage = 7;
                ext4_fs_put_inode_ref(&ref);
                return false;
            }
            qToLittleEndian<quint32>(quint32(current.inodes),
                reinterpret_cast<uchar*>(encoded.data()));
            qToLittleEndian<quint64>(current.bytes,
                reinterpret_cast<uchar*>(encoded.data() + 4));
        }
        const quint32 countSize = revision1 ? 8 : 4;
        if (!quotaBytes(fs, &ref, record.offset + inodeOffset,
                encoded.data(), countSize, true) ||
            !quotaBytes(fs, &ref, record.offset + spaceOffset,
                encoded.data() + countSize, 8, true)) {
            *failureStage = 8;
            ext4_fs_put_inode_ref(&ref);
            return false;
        }
    }
    *failureStage = 9;
    const bool result = ext4_fs_put_inode_ref(&ref) == EOK;
    *failureStage = result ? 0 : 9;
    return result;
}

bool recountExtQuotaUsage(struct ext4_fs* fs, int* failureStage) {
    const quint32 features = ext4_get32(&fs->sb, features_read_only);
    const quint32 userQuota = ext4_get32(&fs->sb, usr_quota_inum);
    const quint32 groupQuota = ext4_get32(&fs->sb, grp_quota_inum);
    const quint32 projectQuota = ext4_get32(&fs->sb, project_quota_inum);
    const quint32 orphanFile = ext4_get32(&fs->sb, orphan_file_inum);
    const quint32 firstInode = ext4_get32(&fs->sb, first_inode);
    const quint32 inodeCount = ext4_get32(&fs->sb, inodes_count);
    std::map<quint32, QuotaUsage> users, groups, projects;
    for (quint32 number = 1; number <= inodeCount; ++number) {
        *failureStage = 10;
        struct ext4_inode_ref ref = {};
        if (ext4_fs_get_inode_ref(fs, number, &ref) != EOK) return false;
        if (ext4_inode_get_links_cnt(ref.inode) &&
            (number == 2 || number >= firstInode) &&
            number != userQuota && number != groupQuota &&
            number != projectQuota && number != orphanFile) {
            const quint64 blocks = ext4_inode_get_blocks_count(&fs->sb,
                                                                ref.inode);
            if (blocks > UINT64_MAX / 512) {
                ext4_fs_put_inode_ref(&ref);
                return false;
            }
            const quint64 bytes = blocks * 512;
            const quint32 uid = ext4_inode_get_uid(ref.inode);
            const quint32 gid = ext4_inode_get_gid(ref.inode);
            if (!addQuotaUsage(&users, uid, bytes) ||
                !addQuotaUsage(&groups, gid, bytes)) {
                *failureStage = 13;
                ext4_fs_put_inode_ref(&ref);
                return false;
            }
            if (features & EXT4_FRO_COM_PROJECT) {
                if (ext4_get16(ref.inode, extra_isize) < 32) {
                    *failureStage = 11;
                    ext4_fs_put_inode_ref(&ref);
                    return false;
                }
                const uchar* projectField = reinterpret_cast<const uchar*>(
                    ref.inode) + offsetof(struct ext4_inode, version_hi) + 4;
                const quint32 projectId = qFromLittleEndian<quint32>(projectField);
                if (!addQuotaUsage(&projects, projectId, bytes)) {
                    *failureStage = 13;
                    ext4_fs_put_inode_ref(&ref);
                    return false;
                }
            }
        }
        if (ext4_fs_put_inode_ref(&ref) != EOK) {
            *failureStage = 12;
            return false;
        }
    }
    int innerStage = 0;
    if (features & EXT4_FRO_COM_QUOTA) {
        if (!userQuota && !groupQuota) {
            *failureStage = 20;
            return false;
        }
        if (userQuota && !updateQuotaFile(fs, userQuota, kExtUserQuotaMagic,
                                          users, &innerStage)) {
            *failureStage = 20 + innerStage;
            return false;
        }
        if (groupQuota && !updateQuotaFile(fs, groupQuota, kExtGroupQuotaMagic,
                                           groups, &innerStage)) {
            *failureStage = 40 + innerStage;
            return false;
        }
    }
    if ((features & EXT4_FRO_COM_PROJECT) &&
        !updateQuotaFile(fs, projectQuota, kExtProjectQuotaMagic,
                         projects, &innerStage)) {
        *failureStage = 60 + innerStage;
        return false;
    }
    *failureStage = 0;
    return true;
}

struct SharedOwner {
    quint32 inode = 0;
    ext4_lblk_t logical = 0;
    bool unwritten = false;
};

struct ExtentFileMap {
    quint32 inode = 0;
    std::vector<ext4_extent_map_entry> extents;
};

using SharedBlockOwners = std::map<ext4_fsblk_t, std::vector<SharedOwner>>;

bool copyExtBlock(struct ext4_fs* fs, ext4_fsblk_t source,
                  ext4_fsblk_t destination) {
    struct ext4_block from = {}, to = {};
    const quint32 blockSize = ext4_sb_get_block_size(&fs->sb);
    if (ext4_trans_block_get(fs->bdev, &from, source) != EOK ||
        ext4_trans_block_get(fs->bdev, &to, destination) != EOK)
        return false;
    std::memcpy(to.data, from.data, blockSize);
    return ext4_trans_set_block_dirty(to.buf) == EOK;
}

bool restoreSharedBlock(struct ext4_fs* fs, ext4_fsblk_t blockNumber,
                        quint32 ownerInode) {
    struct ext4_inode_ref ref = {};
    if (ext4_fs_get_inode_ref(fs, ownerInode, &ref) != EOK) return false;
    bool wasFree = false;
    int result = ext4_balloc_try_alloc_block(&ref, blockNumber, &wasFree);
    if (result == EOK && wasFree) {
        const quint64 sectors = ext4_sb_get_block_size(&fs->sb) / 512;
        const quint64 blocks = ext4_inode_get_blocks_count(&fs->sb, ref.inode);
        if (blocks < sectors)
            result = EIO;
        else {
            result = ext4_inode_set_blocks_count(&fs->sb, ref.inode,
                                                  blocks - sectors);
            ref.dirty = result == EOK;
        }
    }
    const int putResult = ext4_fs_put_inode_ref(&ref);
    return result == EOK && putResult == EOK;
}

bool unshareExtBlocks(struct ext4_fs* fs) {
    const quint32 inodeCount = ext4_get32(&fs->sb, inodes_count);
    const quint32 firstInode = ext4_get32(&fs->sb, first_inode);
    const quint64 blockCount = ext4_sb_get_blocks_cnt(&fs->sb);
    std::vector<ExtentFileMap> files;
    SharedBlockOwners owners;
    for (quint32 number = 1; number <= inodeCount; ++number) {
        if (number != 2 && number < firstInode) continue;
        struct ext4_inode_ref ref = {};
        if (ext4_fs_get_inode_ref(fs, number, &ref) != EOK) return false;
        if (!ext4_inode_get_links_cnt(ref.inode)) {
            ext4_fs_put_inode_ref(&ref);
            continue;
        }
        const quint32 inodeFlags = ext4_inode_get_flags(ref.inode);
        if (!(inodeFlags & EXT4_INODE_FLAG_EXTENTS)) {
            const quint32 mode = ext4_inode_get_mode(&fs->sb, ref.inode) &
                EXT4_INODE_MODE_TYPE_MASK;
            const quint64 blocks = ext4_inode_get_blocks_count(&fs->sb,
                                                                ref.inode);
            if ((mode == EXT4_INODE_MODE_FILE ||
                 mode == EXT4_INODE_MODE_DIRECTORY ||
                 mode == EXT4_INODE_MODE_SOFTLINK) && blocks &&
                !(inodeFlags & EXT4_INODE_FLAG_INLINE_DATA)) {
                ext4_fs_put_inode_ref(&ref);
                return false;
            }
            ext4_fs_put_inode_ref(&ref);
            continue;
        }
        struct ext4_extent_map_entry* entries = nullptr;
        size_t count = 0;
        const int collectResult = ext4_extent_collect(&ref, &entries, &count);
        if (collectResult != EOK) {
            ext4_fs_put_inode_ref(&ref);
            ext4_free(entries);
            return false;
        }
        ExtentFileMap file;
        file.inode = number;
        if (count) file.extents.assign(entries, entries + count);
        ext4_free(entries);
        for (const ext4_extent_map_entry& extent : file.extents) {
            if (!extent.length || extent.logical > UINT32_MAX - extent.length ||
                extent.physical >= blockCount ||
                extent.length > blockCount - extent.physical) {
                ext4_fs_put_inode_ref(&ref);
                return false;
            }
            for (uint32_t offset = 0; offset < extent.length; ++offset) {
                owners[extent.physical + offset].push_back({number,
                    extent.logical + offset, extent.unwritten});
            }
        }
        files.push_back(std::move(file));
        if (ext4_fs_put_inode_ref(&ref) != EOK) return false;
    }

    std::map<quint32, std::map<ext4_lblk_t, ext4_fsblk_t>> replacements;
    std::map<ext4_fsblk_t, quint32> canonicalOwners;
    quint64 cloneCount = 0;
    for (auto& item : owners) {
        std::vector<SharedOwner>& references = item.second;
        if (references.size() < 2) continue;
        std::sort(references.begin(), references.end(),
            [](const SharedOwner& left, const SharedOwner& right) {
                return left.inode < right.inode ||
                    (left.inode == right.inode && left.logical < right.logical);
            });
        canonicalOwners.emplace(item.first, references.front().inode);
        for (size_t index = 1; index < references.size(); ++index) {
            replacements[references[index].inode].emplace(
                references[index].logical, item.first);
            ++cloneCount;
        }
    }
    if (cloneCount > ext4_sb_get_free_blocks_cnt(&fs->sb)) return false;

    for (ExtentFileMap& file : files) {
        auto replacementSet = replacements.find(file.inode);
        if (replacementSet == replacements.end()) continue;
        struct ext4_inode_ref ref = {};
        if (ext4_fs_get_inode_ref(fs, file.inode, &ref) != EOK) return false;
        std::map<ext4_lblk_t, ext4_fsblk_t> cloneBlocks;
        std::set<ext4_fsblk_t> sharedToRestore;
        for (const auto& replacement : replacementSet->second) {
            ext4_fsblk_t oldBlock = replacement.second;
            ext4_fsblk_t newBlock = 0;
            if (ext4_balloc_alloc_block(&ref, oldBlock, &newBlock) != EOK ||
                !newBlock) {
                ext4_fs_put_inode_ref(&ref);
                return false;
            }
            const auto ownerList = owners.find(oldBlock);
            if (ownerList == owners.end() || ownerList->second.empty()) {
                ext4_fs_put_inode_ref(&ref);
                return false;
            }
            const auto occurrence = std::find_if(ownerList->second.begin(),
                ownerList->second.end(), [&](const SharedOwner& owner) {
                    return owner.inode == file.inode &&
                           owner.logical == replacement.first;
                });
            if (occurrence == ownerList->second.end() ||
                (!occurrence->unwritten && !copyExtBlock(fs, oldBlock, newBlock))) {
                ext4_fs_put_inode_ref(&ref);
                return false;
            }
            cloneBlocks.emplace(replacement.first, newBlock);
            sharedToRestore.insert(oldBlock);
        }

        std::vector<ext4_extent_map_entry> rebuilt;
        for (const ext4_extent_map_entry& extent : file.extents) {
            for (uint32_t offset = 0; offset < extent.length; ++offset) {
                const ext4_lblk_t logical = extent.logical + offset;
                const auto clone = cloneBlocks.find(logical);
                const ext4_fsblk_t physical = clone == cloneBlocks.end()
                    ? extent.physical + offset : clone->second;
                if (!rebuilt.empty() &&
                    rebuilt.back().logical + rebuilt.back().length == logical &&
                    rebuilt.back().physical + rebuilt.back().length == physical &&
                    rebuilt.back().unwritten == extent.unwritten &&
                    rebuilt.back().length < UINT32_MAX) {
                    ++rebuilt.back().length;
                } else {
                    rebuilt.push_back({logical, 1, physical, extent.unwritten});
                }
            }
        }
        const int replaceResult = ext4_extent_replace(&ref, rebuilt.data(),
                                                       rebuilt.size());
        const int putResult = ext4_fs_put_inode_ref(&ref);
        if (replaceResult != EOK || putResult != EOK) return false;
        for (ext4_fsblk_t sharedBlock : sharedToRestore) {
            const auto canonical = canonicalOwners.find(sharedBlock);
            if (canonical == canonicalOwners.end() ||
                !restoreSharedBlock(fs, sharedBlock, canonical->second))
                return false;
        }
    }

    const quint32 features = ext4_get32(&fs->sb, features_read_only);
    ext4_set32(&fs->sb, features_read_only,
               features & ~EXT4_FRO_COM_SHARED_BLOCKS);
    return true;
}

}  // namespace

namespace peare {
namespace writer {

struct ExtVolumeUpdate::State {
    struct ext4_blockdev_iface iface = {};
    struct ext4_blockdev device = {};
    struct ext4_blockdev_iface journalIface = {};
    struct ext4_blockdev journalDevice = {};
    struct ext4_bcache journalCache = {};
    ExtVolumeUpdate* owner = nullptr;
    peare_ext_io* io = nullptr;
    peare_ext_io* journalIo = nullptr;
    quint64 journalDataOffset = 0;
    bool journalBlockInitialized = false;
    bool journalCacheInitialized = false;
    std::array<uint8_t, 512> physicalBuffer = {};
    bool mmpActive = false;
    bool mmpHasChecksum = false;
    quint64 mmpOffset = 0;
    quint32 mmpChecksumSeed = 0;
    quint32 mmpCheckInterval = 0;
    qint64 mmpLastWrite = 0;
    QByteArray mmpRecord;
};

ExtVolumeUpdate::ExtVolumeUpdate() = default;

ExtVolumeUpdate::~ExtVolumeUpdate() {
    abort();
}

bool ExtVolumeUpdate::probe(const peare_ext_io& io, bool* ext3, bool* ext4) {
    bool journaled = false;
    bool ext4Features = false;
    bool orphanFile = false;
    if (!hasExtFeatures(io, &journaled, &ext4Features, &orphanFile)) return false;
    ExtVolumeUpdate volume;
    if (!volume.mount(io, true, nullptr)) return false;
    const bool orphanValid = !orphanFile ||
        validateOrphanFile(volume.state_->device.fs);
    if (!volume.unmount(nullptr) || !orphanValid) return false;
    if (ext3) *ext3 = journaled;
    if (ext4) *ext4 = ext4Features;
    return true;
}

bool ExtVolumeUpdate::format(const peare_ext_io& io, bool ext3, bool ext4,
                             QString* error) {
    if (ext3 && ext4) {
        if (error) *error = QStringLiteral("The Ext format selection is invalid.");
        return false;
    }
    if (!io.context || !io.read || !io.write || !io.flush || io.size < 4 * 1024 * 1024 ||
        io.size % kIoBlockSize) {
        if (error) *error = QStringLiteral("The Ext volume geometry is invalid.");
        return false;
    }
    ExtVolumeUpdate volume;
    volume.lock_ = std::unique_lock<std::mutex>(g_extMutex);
    volume.io_ = io;
    volume.state_.reset(new (std::nothrow) State);
    if (!volume.state_) {
        if (error) *error = QStringLiteral("Could not allocate the Ext block device.");
        return false;
    }
    volume.state_->iface.open = blockOpen;
    volume.state_->iface.close = blockClose;
    volume.state_->iface.bread = blockRead;
    volume.state_->iface.bwrite = blockWrite;
    volume.state_->iface.ph_bsize = uint32_t(kIoBlockSize);
    volume.state_->iface.ph_bcnt = uint64_t(io.size / kIoBlockSize);
    volume.state_->iface.ph_bbuf = volume.state_->physicalBuffer.data();
    volume.state_->owner = &volume;
    volume.state_->io = &volume.io_;
    volume.state_->iface.p_user = volume.state_.get();
    volume.state_->device.bdif = &volume.state_->iface;
    volume.state_->device.part_size = uint64_t(io.size);

    struct ext4_fs filesystem = {};
    struct ext4_mkfs_info options = {};
    options.block_size = 4096;
    options.label = "Peare";
    options.journal = ext3 || ext4;
    const int result = ext4_mkfs(&filesystem, &volume.state_->device,
                                 &options, ext4 ? F_SET_EXT4 :
                                     ext3 ? F_SET_EXT3 : F_SET_EXT2);
    volume.state_.reset();
    volume.lock_ = std::unique_lock<std::mutex>();
    if (result != EOK) {
        if (error) *error = QStringLiteral("lwext4 could not format the Ext volume.");
        return false;
    }
    bool formattedExt3 = false;
    bool formattedExt4 = false;
    if (!probe(io, &formattedExt3, &formattedExt4) ||
        formattedExt3 != (ext3 || ext4) ||
        formattedExt4 != ext4) {
        if (error) *error = QStringLiteral("The new Ext volume failed validation.");
        return false;
    }
    return true;
}

bool ExtVolumeUpdate::begin(const peare_ext_io& io, QString* error,
                            const peare_ext_io* journalIo) {
    if (mounted_ || registered_ || failed_)
        return fail(error, QStringLiteral("An Ext update is already active."));
    bool ext3 = false;
    bool orphanFile = false;
    bool externalJournal = false;
    if (!hasExtFeatures(io, &ext3, nullptr, &orphanFile, &externalJournal))
        return fail(error, QStringLiteral("The Ext feature set is not supported for writing."));
    if (externalJournal != (journalIo != nullptr))
        return fail(error, externalJournal
            ? QStringLiteral("The external Ext journal device is required.")
            : QStringLiteral("An external journal was supplied for an internal-journal volume."));
    if (externalJournal) {
        std::array<uchar, EXT4_SUPERBLOCK_SIZE> superblock = {};
        std::array<uchar, EXT4_SUPERBLOCK_SIZE> primarySuperblock = {};
        if (!journalIo->read || !journalIo->write || !journalIo->flush ||
            journalIo->size < 4096 ||
            !journalIo->read(journalIo->context, EXT4_SUPERBLOCK_OFFSET,
                             superblock.data(), superblock.size()) ||
            !io.read(io.context, EXT4_SUPERBLOCK_OFFSET,
                     primarySuperblock.data(), primarySuperblock.size()))
            return fail(error, QStringLiteral("The external journal device could not be read."));
        const quint32 journalBlockSize = quint32(1024) <<
            qFromLittleEndian<quint32>(superblock.data() + 24);
        const quint32 primaryBlockSize = quint32(1024) <<
            qFromLittleEndian<quint32>(primarySuperblock.data() + 24);
        const quint32 journalFeatures = qFromLittleEndian<quint32>(
            superblock.data() + 96);
        const quint64 journalOffset = qMax<quint64>(2048, journalBlockSize);
        const quint64 alignedJournalOffset = (journalOffset + journalBlockSize - 1) /
            journalBlockSize * journalBlockSize;
        if (qFromLittleEndian<quint16>(superblock.data() + 56) != 0xef53 ||
            !(journalFeatures & EXT4_FINCOM_JOURNAL_DEV) ||
            journalBlockSize != primaryBlockSize ||
            alignedJournalOffset >= quint64(journalIo->size) ||
            (quint64(journalIo->size) - alignedJournalOffset) % journalBlockSize ||
            std::memcmp(superblock.data() + offsetof(struct ext4_sblock, uuid),
                        primarySuperblock.data() +
                            offsetof(struct ext4_sblock, journal_uuid), UUID_SIZE))
            return fail(error, QStringLiteral("The external journal device is invalid."));
        journalIo_ = *journalIo;
        hasJournalIo_ = true;
    }
    if (orphanFile) {
        if (!mount(io, true, error)) return false;
        QString orphanFailure;
        const bool orphanValid = validateOrphanFile(state_->device.fs,
                                                     &orphanFailure);
        if (!unmount(error)) return false;
        if (!orphanValid)
            return fail(error, QStringLiteral("The Ext orphan file is invalid: %1")
                .arg(orphanFailure));
    }
    if (!mount(io, false, error, journalIo)) return false;
    if (ext3) {
        const bool recoveryPending = (ext4_get32(
            &state_->device.fs->sb, features_incompatible) &
            EXT4_FINCOM_RECOVER) != 0;
        if (!externalJournal || recoveryPending) {
            const int recoverResult = externalJournal
                ? ext4_recover_on_device(kMountPoint, &state_->journalDevice)
                : ext4_recover(kMountPoint);
            if (recoverResult != EOK)
                return fail(error, QStringLiteral(
                    "The Ext3 journal could not be recovered (%1).")
                    .arg(recoverResult));
        }
        const int journalResult = externalJournal
            ? ext4_journal_start_on_device(kMountPoint,
                                           &state_->journalDevice)
            : ext4_journal_start(kMountPoint);
        if (journalResult != EOK)
            return fail(error, QStringLiteral(
                "The Ext3 journal could not be started (%1).")
                .arg(journalResult));
        journalStarted_ = true;
    }
    if (ext4_sb_feature_ro_com(&state_->device.fs->sb,
                               EXT4_FRO_COM_SHARED_BLOCKS) &&
        !unshareSharedBlocks(error)) {
        failed_ = true;
        return false;
    }
    return true;
}

bool ExtVolumeUpdate::mount(const peare_ext_io& io, bool readOnly,
                            QString* error, const peare_ext_io* journalIo) {
    if (!lock_.owns_lock()) lock_ = std::unique_lock<std::mutex>(g_extMutex);
    io_ = io;
    state_.reset(new (std::nothrow) State);
    if (!state_) return fail(error, QStringLiteral("Could not allocate the Ext block device."));
    state_->iface.open = blockOpen;
    state_->iface.close = blockClose;
    state_->iface.bread = blockRead;
    state_->iface.bwrite = blockWrite;
    state_->iface.ph_bsize = uint32_t(kIoBlockSize);
    state_->iface.ph_bcnt = uint64_t(io.size / kIoBlockSize);
    state_->iface.ph_bbuf = state_->physicalBuffer.data();
    state_->owner = this;
    state_->io = &io_;
    if (journalIo) {
        state_->journalIo = &journalIo_;
        std::array<uchar, EXT4_SUPERBLOCK_SIZE> superblock = {};
        if (!journalIo_.read(journalIo_.context, EXT4_SUPERBLOCK_OFFSET,
                superblock.data(), superblock.size()))
            return fail(error, QStringLiteral(
                "The external journal superblock could not be read."));
        const quint32 blockSize = quint32(1024) <<
            qFromLittleEndian<quint32>(superblock.data() + 24);
        state_->journalDataOffset =
            qMax<quint64>(2048, blockSize);
        state_->journalDataOffset =
            (state_->journalDataOffset + blockSize - 1) / blockSize * blockSize;
        state_->journalIface.open = blockOpen;
        state_->journalIface.close = blockClose;
        state_->journalIface.bread = blockRead;
        state_->journalIface.bwrite = blockWrite;
        state_->journalIface.ph_bsize = uint32_t(kIoBlockSize);
        state_->journalIface.ph_bcnt = uint64_t(
            (journalIo_.size - qint64(state_->journalDataOffset)) /
            kIoBlockSize);
        state_->journalIface.ph_bbuf = state_->physicalBuffer.data();
        state_->journalIface.p_user = state_.get();
        state_->journalDevice.bdif = &state_->journalIface;
        state_->journalDevice.part_size = uint64_t(
            journalIo_.size - qint64(state_->journalDataOffset));
        if (ext4_device_register(&state_->journalDevice,
                                 kJournalDeviceName) != EOK)
            return fail(error, QStringLiteral(
                "Ext could not register the external journal device."));
        journalRegistered_ = true;
    }
    state_->iface.p_user = state_.get();
    if (!readOnly && !acquireMmp(error)) return false;
    state_->device.bdif = &state_->iface;
    state_->device.part_size = uint64_t(io.size);
    if (ext4_device_register(&state_->device, kDeviceName) != EOK) {
        releaseMmp(nullptr);
        return fail(error, QStringLiteral("Ext could not register the staged volume."));
    }
    registered_ = true;
    const int mountResult = ext4_mount(kDeviceName, kMountPoint, readOnly);
    if (mountResult != EOK) {
        ext4_device_unregister(kDeviceName);
        registered_ = false;
        releaseMmp(nullptr);
        return fail(error, QStringLiteral("Ext could not mount the staged volume (%1).")
                    .arg(mountResult));
    }
    mounted_ = true;
    if (journalIo) {
        int result = ext4_block_init(&state_->journalDevice);
        if (result != EOK)
            return fail(error, QStringLiteral(
                "Ext could not initialize the external journal device (%1).")
                .arg(result));
        state_->journalBlockInitialized = true;
        const quint32 blockSize = ext4_sb_get_block_size(
            &state_->device.fs->sb);
        ext4_block_set_lb_size(&state_->journalDevice, blockSize);
        result = ext4_bcache_init_dynamic(&state_->journalCache,
            CONFIG_BLOCK_DEV_CACHE_SIZE, blockSize);
        if (result != EOK)
            return fail(error, QStringLiteral(
                "Ext could not initialize the external journal cache (%1).")
                .arg(result));
        state_->journalCacheInitialized = true;
        result = ext4_block_bind_bcache(&state_->journalDevice,
                                        &state_->journalCache);
        if (result != EOK)
            return fail(error, QStringLiteral(
                "Ext could not bind the external journal cache (%1).")
                .arg(result));
    }
    return true;
}

bool ExtVolumeUpdate::acquireMmp(QString* error) {
    if (!state_ || !io_.read || !io_.write || !io_.flush) return false;
    std::array<uchar, EXT4_SUPERBLOCK_SIZE> superblock = {};
    if (!io_.read(io_.context, EXT4_SUPERBLOCK_OFFSET, superblock.data(),
                  superblock.size()))
        return fail(error, QStringLiteral("Ext could not read the MMP superblock fields."));
    const quint32 incompat = qFromLittleEndian<quint32>(superblock.data() + 96);
    if (!(incompat & EXT4_FINCOM_MMP)) return true;

    const quint64 blockSize = quint64(1024) <<
        qFromLittleEndian<quint32>(superblock.data() + 24);
    const quint64 block = qFromLittleEndian<quint64>(superblock.data() +
        offsetof(struct ext4_sblock, mmp_block));
    quint64 filesystemBlocks = qFromLittleEndian<quint32>(superblock.data() + 4);
    if (incompat & EXT4_FINCOM_64BIT)
        filesystemBlocks |= quint64(qFromLittleEndian<quint32>(
            superblock.data() + 336)) << 32;
    const quint32 firstDataBlock =
        qFromLittleEndian<quint32>(superblock.data() + 20);
    if (blockSize < quint64(kMmpRecordSize) || block <= firstDataBlock ||
        block >= filesystemBlocks || block >= quint64(io_.size) / blockSize ||
        block > (std::numeric_limits<quint64>::max() / blockSize))
        return fail(error, QStringLiteral("The Ext MMP block location is invalid."));
    state_->mmpOffset = block * blockSize;
    if (state_->mmpOffset > quint64(io_.size) - kMmpRecordSize)
        return fail(error, QStringLiteral("The Ext MMP record is outside the volume."));

    const quint32 readOnly = qFromLittleEndian<quint32>(superblock.data() + 100);
    state_->mmpHasChecksum =
        (readOnly & EXT4_FRO_COM_METADATA_CSUM) != 0;
    if (state_->mmpHasChecksum) {
        const quint32 incompatFeatures = qFromLittleEndian<quint32>(
            superblock.data() + 96);
        state_->mmpChecksumSeed = incompatFeatures & EXT4_FINCOM_CSUM_SEED
            ? qFromLittleEndian<quint32>(superblock.data() +
                offsetof(struct ext4_sblock, checksum_seed))
            : ext4_crc32c(0xffffffffU,
                superblock.data() + offsetof(struct ext4_sblock, uuid), UUID_SIZE);
    }

    quint32 interval = qFromLittleEndian<quint16>(superblock.data() +
        offsetof(struct ext4_sblock, mmp_interval));
    if (!interval) interval = kMmpMinCheckInterval;
    state_->mmpRecord.resize(int(kMmpRecordSize));
    auto readRecord = [this]() {
        return io_.read(io_.context, qint64(state_->mmpOffset),
                        state_->mmpRecord.data(), state_->mmpRecord.size());
    };
    auto validRecord = [this]() {
        const uchar* bytes = reinterpret_cast<const uchar*>(
            state_->mmpRecord.constData());
        if (qFromLittleEndian<quint32>(bytes) != kMmpMagic) return false;
        return !state_->mmpHasChecksum ||
            qFromLittleEndian<quint32>(bytes + kMmpChecksumOffset) ==
                mmpChecksum(state_->mmpRecord, state_->mmpChecksumSeed);
    };
    if (!readRecord() || !validRecord())
        return fail(error, QStringLiteral("The Ext MMP record is invalid."));
    quint32 sequence = qFromLittleEndian<quint32>(
        reinterpret_cast<const uchar*>(state_->mmpRecord.constData()) + 4);
    const quint32 recordInterval = qFromLittleEndian<quint16>(
        reinterpret_cast<const uchar*>(state_->mmpRecord.constData()) + 112);
    interval = qMax(interval, recordInterval);
    if (!interval) interval = kMmpMinCheckInterval;
    if (sequence != kMmpSeqClean) {
        if (sequence == kMmpSeqFsck)
            return fail(error, QStringLiteral("The Ext volume is marked active by a filesystem check."));
        if (sequence > kMmpSeqMax)
            return fail(error, QStringLiteral("The Ext MMP sequence is invalid."));
        const quint32 waitSeconds = qMin<quint32>(2 * interval + 1, interval + 60);
        QThread::sleep(waitSeconds);
        if (!readRecord() || !validRecord() ||
            qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(
                state_->mmpRecord.constData()) + 4) != sequence)
            return fail(error, QStringLiteral("The Ext volume is active on another writer."));
    }

    state_->mmpCheckInterval = qMax(interval, kMmpMinCheckInterval);
    quint32 claim = QRandomGenerator::system()->bounded(kMmpSeqMax - 1) + 1;
    uchar* record = reinterpret_cast<uchar*>(state_->mmpRecord.data());
    qToLittleEndian<quint32>(claim, record + 4);
    qToLittleEndian<quint64>(quint64(QDateTime::currentSecsSinceEpoch()), record + 8);
    std::memset(record + 16, 0, 64 + 32);
    const QByteArray host = QSysInfo::machineHostName().toUtf8().left(63);
    std::memcpy(record + 16, host.constData(), size_t(host.size()));
    std::memcpy(record + 80, "Peare", 5);
    qToLittleEndian<quint16>(quint16(state_->mmpCheckInterval), record + 112);
    setMmpChecksum(&state_->mmpRecord, state_->mmpChecksumSeed,
                   state_->mmpHasChecksum);
    if (!io_.write(io_.context, qint64(state_->mmpOffset),
                   state_->mmpRecord.constData(), state_->mmpRecord.size()) ||
        !io_.flush(io_.context))
        return fail(error, QStringLiteral("Ext could not claim the MMP record."));

    QThread::sleep(qMin<quint32>(2 * state_->mmpCheckInterval + 1,
                                 state_->mmpCheckInterval + 60));
    if (!readRecord() || !validRecord() ||
        qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(
            state_->mmpRecord.constData()) + 4) != claim)
        return fail(error, QStringLiteral("The Ext MMP claim changed during verification."));
    record = reinterpret_cast<uchar*>(state_->mmpRecord.data());
    qToLittleEndian<quint32>(kMmpSeqFsck, record + 4);
    qToLittleEndian<quint64>(quint64(QDateTime::currentSecsSinceEpoch()), record + 8);
    setMmpChecksum(&state_->mmpRecord, state_->mmpChecksumSeed,
                   state_->mmpHasChecksum);
    if (!io_.write(io_.context, qint64(state_->mmpOffset),
                   state_->mmpRecord.constData(), state_->mmpRecord.size()) ||
        !io_.flush(io_.context))
        return fail(error, QStringLiteral("Ext could not activate the MMP guard."));
    state_->mmpActive = true;
    state_->mmpLastWrite = QDateTime::currentSecsSinceEpoch();
    return true;
}

bool ExtVolumeUpdate::pulseMmp() {
    if (!state_ || !state_->mmpActive) return true;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (now - state_->mmpLastWrite < qMax<quint32>(1, state_->mmpCheckInterval / 2))
        return true;
    QByteArray current(kMmpRecordSize, Qt::Uninitialized);
    if (!io_.read(io_.context, qint64(state_->mmpOffset), current.data(),
                  current.size())) return false;
    const uchar* currentBytes = reinterpret_cast<const uchar*>(current.constData());
    if (qFromLittleEndian<quint32>(currentBytes) != kMmpMagic ||
        qFromLittleEndian<quint32>(currentBytes + 4) != kMmpSeqFsck ||
        current != state_->mmpRecord ||
        (state_->mmpHasChecksum &&
         qFromLittleEndian<quint32>(currentBytes + kMmpChecksumOffset) !=
             mmpChecksum(current, state_->mmpChecksumSeed))) return false;
    uchar* record = reinterpret_cast<uchar*>(state_->mmpRecord.data());
    qToLittleEndian<quint64>(quint64(now), record + 8);
    setMmpChecksum(&state_->mmpRecord, state_->mmpChecksumSeed,
                   state_->mmpHasChecksum);
    if (!io_.write(io_.context, qint64(state_->mmpOffset),
                   state_->mmpRecord.constData(), state_->mmpRecord.size()) ||
        !io_.flush(io_.context)) return false;
    state_->mmpLastWrite = now;
    return true;
}

bool ExtVolumeUpdate::releaseMmp(QString* error) {
    if (!state_ || !state_->mmpActive) return true;
    QByteArray current(kMmpRecordSize, Qt::Uninitialized);
    if (!io_.read(io_.context, qint64(state_->mmpOffset), current.data(),
                  current.size()))
        return fail(error, QStringLiteral("Ext could not verify the MMP record before release."));
    const uchar* bytes = reinterpret_cast<const uchar*>(current.constData());
    if (qFromLittleEndian<quint32>(bytes) != kMmpMagic ||
        qFromLittleEndian<quint32>(bytes + 4) != kMmpSeqFsck ||
        current != state_->mmpRecord ||
        (state_->mmpHasChecksum &&
         qFromLittleEndian<quint32>(bytes + kMmpChecksumOffset) !=
             mmpChecksum(current, state_->mmpChecksumSeed)))
        return fail(error, QStringLiteral("Ext detected a changed MMP record and left the guard active."));
    uchar* record = reinterpret_cast<uchar*>(state_->mmpRecord.data());
    qToLittleEndian<quint32>(kMmpSeqClean, record + 4);
    qToLittleEndian<quint64>(quint64(QDateTime::currentSecsSinceEpoch()), record + 8);
    setMmpChecksum(&state_->mmpRecord, state_->mmpChecksumSeed,
                   state_->mmpHasChecksum);
    if (!io_.write(io_.context, qint64(state_->mmpOffset),
                   state_->mmpRecord.constData(), state_->mmpRecord.size()) ||
        !io_.flush(io_.context))
        return fail(error, QStringLiteral("Ext could not release the MMP guard."));
    state_->mmpActive = false;
    return true;
}

int ExtVolumeUpdate::blockOpen(struct ext4_blockdev* device) {
    if (!device || !device->bdif || !device->bdif->p_user) return EINVAL;
    const State* state = static_cast<const State*>(device->bdif->p_user);
    const peare_ext_io* io = device == &state->journalDevice
        ? state->journalIo : state->io;
    return io->read && io->size > 0 ? EOK : EINVAL;
}

int ExtVolumeUpdate::blockClose(struct ext4_blockdev* device) {
    if (!device || !device->bdif || !device->bdif->p_user) return EINVAL;
    const State* state = static_cast<const State*>(device->bdif->p_user);
    const peare_ext_io* io = device == &state->journalDevice
        ? state->journalIo : state->io;
    return io->flush && io->flush(io->context) ? EOK : EIO;
}

int ExtVolumeUpdate::blockRead(struct ext4_blockdev* device, void* buffer,
                                uint64_t blockId, uint32_t blockCount) {
    if (!device || !device->bdif || !device->bdif->p_user || !buffer || !blockCount)
        return EINVAL;
    const State* state = static_cast<const State*>(device->bdif->p_user);
    const bool journal = device == &state->journalDevice;
    const peare_ext_io* io = journal ? state->journalIo : state->io;
    const quint64 size = journal
        ? state->journalDevice.part_size : quint64(io->size);
    if (blockId > size / kIoBlockSize ||
        blockCount > size / kIoBlockSize - blockId)
        return EIO;
    const int64_t offset = int64_t(blockId * uint64_t(kIoBlockSize)) +
        (journal ? int64_t(state->journalDataOffset) : 0);
    const int64_t length = int64_t(blockCount) * kIoBlockSize;
    return io->read(io->context, offset, buffer, length) ? EOK : EIO;
}

int ExtVolumeUpdate::blockWrite(struct ext4_blockdev* device, const void* buffer,
                                 uint64_t blockId, uint32_t blockCount) {
    if (!device || !device->bdif || !device->bdif->p_user || !buffer || !blockCount)
        return EINVAL;
    State* state = static_cast<State*>(device->bdif->p_user);
    const bool journal = device == &state->journalDevice;
    peare_ext_io* io = journal ? state->journalIo : state->io;
    const quint64 size = journal
        ? state->journalDevice.part_size : quint64(io->size);
    if (!io->write || blockId > size / kIoBlockSize ||
        blockCount > size / kIoBlockSize - blockId) {
        return EIO;
    }
    const int64_t offset = int64_t(blockId * uint64_t(kIoBlockSize)) +
        (journal ? int64_t(state->journalDataOffset) : 0);
    const int64_t length = int64_t(blockCount) * kIoBlockSize;
    if (!journal && state->owner && !state->owner->pulseMmp()) {
        return EIO;
    }
    if (!io->write(io->context, offset, buffer, length))
        return EIO;
    return EOK;
}

bool ExtVolumeUpdate::unmount(QString* error) {
    if (journalStarted_) {
        const int result = ext4_journal_stop(kMountPoint);
        journalStarted_ = false;
        if (result != EOK) {
            if (error) *error = QStringLiteral("The Ext3 journal could not be stopped cleanly.");
            return false;
        }
    }
    if (mounted_) {
        const int result = ext4_umount(kMountPoint);
        if (result != EOK) {
            if (error) *error = QStringLiteral("The Ext volume could not flush and unmount.");
            return false;
        }
        mounted_ = false;
    }
    if (state_ && state_->journalCacheInitialized) {
        ext4_bcache_cleanup(&state_->journalCache);
        ext4_bcache_fini_dynamic(&state_->journalCache);
        state_->journalCacheInitialized = false;
    }
    if (state_ && state_->journalBlockInitialized) {
        if (ext4_block_fini(&state_->journalDevice) != EOK) {
            if (error) *error = QStringLiteral(
                "The external journal device could not close cleanly.");
            return false;
        }
        state_->journalBlockInitialized = false;
    }
    const bool mmpReleased = releaseMmp(error);
    if (registered_) {
        if (ext4_device_unregister(kDeviceName) != EOK) {
            if (error && mmpReleased)
                *error = QStringLiteral("The Ext volume could not unregister.");
            return false;
        }
        registered_ = false;
    }
    if (journalRegistered_) {
        if (ext4_device_unregister(kJournalDeviceName) != EOK) {
            if (error) *error = QStringLiteral(
                "The external journal device could not unregister.");
            return false;
        }
        journalRegistered_ = false;
    }
    state_.reset();
    lock_ = std::unique_lock<std::mutex>();
    return mmpReleased;
}

bool ExtVolumeUpdate::fail(QString* error, const QString& message) {
    failed_ = true;
    if (error) *error = message;
    return false;
}

bool ExtVolumeUpdate::replaceVerityFile(
        const QString& sourcePath, const QString& path,
        const QByteArray& destination, qint64 sourceSize, QString* error) {
    std::array<unsigned char, kFsverityDescriptorSize> oldDescriptor = {};
    size_t oldDescriptorSize = 0;
    const int descriptorResult = ext4_fget_verity_descriptor(
        destination.constData(), oldDescriptor.data(), oldDescriptor.size(),
        &oldDescriptorSize);
    if (descriptorResult != EOK || oldDescriptor[0] != 1 ||
        oldDescriptorSize < kFsverityDescriptorSize ||
        oldDescriptorSize != kFsverityDescriptorSize +
            qFromLittleEndian<quint32>(oldDescriptor.data() + 4))
        return fail(error, QStringLiteral(
            "Ext cannot replace a protected file with an invalid fs-verity descriptor."));
    if (qFromLittleEndian<quint32>(oldDescriptor.data() + 4) != 0)
        return fail(error, QStringLiteral(
            "Ext cannot replace a signed fs-verity file because Peare cannot create its signature."));

    uint32_t mode = 0;
    uint32_t atime = 0;
    uint32_t mtime = 0;
    if (ext4_mode_get(destination.constData(), &mode) != EOK ||
        ext4_atime_get(destination.constData(), &atime) != EOK ||
        ext4_mtime_get(destination.constData(), &mtime) != EOK)
        return fail(error, QStringLiteral(
            "Ext could not read the protected file metadata."));

    const int separator = path.lastIndexOf(QLatin1Char('/'));
    const QString parent = separator < 0
        ? QString() : path.left(separator + 1);
    QString temporaryPath;
    QString backupPath;
    QByteArray temporaryName;
    QByteArray backupName;
    bool namesAvailable = false;
    for (int attempt = 0; attempt < 16 && !namesAvailable; ++attempt) {
        const QString token = QUuid::createUuid().toString(
            QUuid::WithoutBraces).remove(QLatin1Char('-'));
        temporaryPath = parent + QStringLiteral(".peare-verity-") + token +
            QStringLiteral(".tmp");
        backupPath = parent + QStringLiteral(".peare-verity-") + token +
            QStringLiteral(".bak");
        temporaryName = extPath(temporaryPath).toUtf8();
        backupName = extPath(backupPath).toUtf8();
        namesAvailable =
            ext4_inode_exist(temporaryName.constData(), EXT4_DE_UNKNOWN) == ENOENT &&
            ext4_inode_exist(backupName.constData(), EXT4_DE_UNKNOWN) == ENOENT;
    }
    if (!namesAvailable)
        return fail(error, QStringLiteral(
            "Ext could not reserve temporary names for the protected file."));

    if (!writeFile(sourcePath, temporaryPath.toUtf8(), false, error))
        return false;
    const QByteArray temporaryFullPath = extPath(temporaryPath).toUtf8();
    if (!writeFsverityMetadata(temporaryFullPath, quint64(sourceSize), error)) {
        ext4_fremove(temporaryFullPath.constData());
        failed_ = true;
        return false;
    }
    if (ext4_mode_set(temporaryFullPath.constData(), mode) != EOK ||
        ext4_atime_set(temporaryFullPath.constData(), atime) != EOK ||
        ext4_mtime_set(temporaryFullPath.constData(), mtime) != EOK) {
        ext4_fremove(temporaryFullPath.constData());
        return fail(error, QStringLiteral(
            "Ext could not preserve the protected file metadata."));
    }
    if (ext4_frename(destination.constData(), backupName.constData()) != EOK) {
        ext4_fremove(temporaryFullPath.constData());
        return fail(error, QStringLiteral(
            "Ext could not stage the protected file replacement."));
    }
    if (ext4_frename(temporaryFullPath.constData(), destination.constData()) != EOK) {
        const int rollback = ext4_frename(backupName.constData(),
                                          destination.constData());
        ext4_fremove(temporaryFullPath.constData());
        return fail(error, rollback == EOK
            ? QStringLiteral("Ext could not install the protected file replacement.")
            : QStringLiteral("Ext could not restore the protected file after replacement failed."));
    }
    if (ext4_fremove(backupName.constData()) != EOK)
        return fail(error, QStringLiteral(
            "Ext could not remove the replaced protected file."));
    dirty_ = true;
    quotaDirty_ = true;
    return true;
}

bool ExtVolumeUpdate::enableFsVerity(const QByteArray& entryNameUtf8,
                                    QString* error) {
    if (!mounted_ || failed_)
        return fail(error, QStringLiteral("No active Ext update is available."));
    QString path;
    if (!normalizeExtPath(entryNameUtf8, &path, error)) {
        failed_ = true;
        return false;
    }
    const QByteArray fullPath = extPath(path).toUtf8();
    struct ext4_inode inode = {};
    uint32_t inodeNumber = 0;
    struct ext4_sblock* superblock = nullptr;
    if (ext4_get_sblock(kMountPoint, &superblock) != EOK || !superblock ||
        ext4_raw_inode_fill(fullPath.constData(), &inodeNumber, &inode) != EOK ||
        !ext4_inode_is_type(superblock, &inode, EXT4_INODE_MODE_FILE))
        return fail(error, QStringLiteral(
            "Ext fs-verity can only be enabled for a regular file."));
    if (extEncryptionEnabled(state_->device.fs) && inodeIsEncrypted(&inode))
        return fail(error, QStringLiteral(
            "Ext cannot modify an encrypted inode without its fscrypt key."));
    if (ext4_inode_has_flag(&inode, EXT4_INODE_FLAG_VERITY))
        return fail(error, QStringLiteral(
            "The Ext file already has fs-verity enabled."));
    if (!ext4_inode_has_flag(&inode, EXT4_INODE_FLAG_EXTENTS) ||
        ext4_inode_has_flag(&inode, EXT4_INODE_FLAG_INLINE_DATA))
        return fail(error, QStringLiteral(
            "Ext fs-verity requires a regular file stored with extents."));
    const quint64 visibleSize = ext4_inode_get_size(superblock, &inode);
    if (!writeFsverityMetadata(fullPath, visibleSize, error)) {
        failed_ = true;
        return false;
    }
    dirty_ = true;
    quotaDirty_ = true;
    return true;
}

bool ExtVolumeUpdate::writeFile(const QString& sourcePath,
                                const QByteArray& entryNameUtf8, bool replace,
                                QString* error) {
    if (!mounted_ || failed_) return fail(error, QStringLiteral("No active Ext update is available."));
    QString path;
    if (!normalizeExtPath(entryNameUtf8, &path, error)) {
        failed_ = true;
        return false;
    }
    const QByteArray destination = extPath(path).toUtf8();
    bool existingVerity = false;
    const int existingFile = ext4_inode_exist(destination.constData(), EXT4_DE_REG_FILE);
    const int existingAny = ext4_inode_exist(destination.constData(), EXT4_DE_UNKNOWN);
    if (replace ? existingFile != EOK : existingAny == EOK) {
        return fail(error, replace
            ? QStringLiteral("The Ext file to replace does not exist or is not a regular file.")
            : QStringLiteral("The Ext entry already exists."));
    }
    if (replace) {
        struct ext4_inode inode = {};
        uint32_t inodeNumber = 0;
        if (ext4_raw_inode_fill(destination.constData(), &inodeNumber,
                                &inode) != EOK)
            return fail(error, QStringLiteral(
                "Ext could not inspect the file before replacement."));
        if (extEncryptionEnabled(state_->device.fs) &&
            inodeIsEncrypted(&inode))
            return fail(error, QStringLiteral(
                "Ext cannot modify an encrypted inode without its fscrypt key."));
        if (!checkInodeMutationFlags(&inode, error)) {
            failed_ = true;
            return false;
        }
        existingVerity =
            ext4_inode_has_flag(&inode, EXT4_INODE_FLAG_VERITY);
    }
    if (!replace && existingAny != ENOENT)
        return fail(error, QStringLiteral(
            "Ext could not validate the destination path (%1).").arg(existingAny));
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) return fail(error, source.errorString());
    const qint64 sourceSize = source.size();
    if (sourceSize < 0 || !ensureExtParents(
            state_->device.fs, path, error)) {
        failed_ = true;
        return false;
    }
    if (existingVerity)
        return replaceVerityFile(sourcePath, path, destination, sourceSize, error);
    ext4_file file = {};
    const int openResult = ext4_fopen(&file, destination.constData(), "wb");
    if (openResult != EOK)
        return fail(error, QStringLiteral(
            "Ext could not open the destination file (%1).").arg(openResult));
    if (existingAny == ENOENT) {
        const QByteArray parent = destination.left(destination.lastIndexOf('/'));
        if (!inheritInodeFlags(state_->device.fs, parent, file.inode, false)) {
            ext4_fclose(&file);
            failed_ = true;
            return fail(error, QStringLiteral(
                "Ext could not inherit the parent project ID."));
        }
    }
    std::array<char, kCopyBufferSize> buffer = {};
    qint64 copied = 0;
    while (copied < sourceSize) {
        const qint64 requested = qMin<qint64>(qint64(buffer.size()), sourceSize - copied);
        const qint64 count = source.read(buffer.data(), requested);
        if (count <= 0) {
            ext4_fclose(&file);
            return fail(error, QStringLiteral("Ext could not read the complete source file."));
        }
        size_t offset = 0;
        while (offset < size_t(count)) {
            size_t written = 0;
            const size_t remaining = size_t(count) - offset;
            const int writeResult = ext4_fwrite(
                &file, buffer.data() + offset, remaining, &written);
            if (writeResult != EOK || !written || written > remaining) {
                ext4_fclose(&file);
                return fail(error, QStringLiteral(
                    "Ext could not write the complete source file (%1, %2/%3 bytes).")
                    .arg(writeResult).arg(qulonglong(offset + written))
                    .arg(count));
            }
            offset += written;
        }
        copied += count;
    }
    const int closeResult = ext4_fclose(&file);
    if (closeResult != EOK || source.size() != sourceSize || !source.atEnd())
        return fail(error, QStringLiteral("The Ext source file changed or did not close cleanly."));
    dirty_ = true;
    quotaDirty_ = true;
    return true;
}

bool ExtVolumeUpdate::addFile(const QString& sourcePath,
                               const QByteArray& entryNameUtf8, QString* error) {
    return writeFile(sourcePath, entryNameUtf8, false, error);
}

bool ExtVolumeUpdate::replaceFile(const QString& sourcePath,
                                   const QByteArray& entryNameUtf8, QString* error) {
    return writeFile(sourcePath, entryNameUtf8, true, error);
}

bool ExtVolumeUpdate::createDirectory(const QByteArray& entryNameUtf8,
                                       QString* error) {
    if (!mounted_ || failed_) return fail(error, QStringLiteral("No active Ext update is available."));
    QString path;
    if (!normalizeExtPath(entryNameUtf8, &path, error)) {
        failed_ = true;
        return false;
    }
    if (!ensureExtParents(state_->device.fs, path, error)) {
        failed_ = true;
        return false;
    }
    const QByteArray destination = extPath(path).toUtf8();
    const int existing = ext4_inode_exist(destination.constData(), EXT4_DE_UNKNOWN);
    if (existing == EOK)
        return fail(error, QStringLiteral("The Ext directory already exists."));
    if (existing != ENOENT)
        return fail(error, QStringLiteral("Ext could not validate the directory path."));
    if (ext4_dir_mk(destination.constData()) != EOK)
        return fail(error, QStringLiteral("Ext could not create the directory."));
    struct ext4_inode created = {};
    uint32_t createdNumber = 0;
    const QByteArray parent = destination.left(destination.lastIndexOf('/'));
    if (ext4_raw_inode_fill(destination.constData(), &createdNumber, &created) != EOK ||
        !inheritInodeFlags(state_->device.fs, parent, createdNumber, true)) {
        failed_ = true;
        return fail(error, QStringLiteral(
            "Ext could not inherit the parent project ID."));
    }
    dirty_ = true;
    quotaDirty_ = true;
    return true;
}

bool ExtVolumeUpdate::renameEntry(const QByteArray& entryNameUtf8,
                                   const QByteArray& newEntryNameUtf8,
                                   QString* error) {
    if (!mounted_ || failed_) return fail(error, QStringLiteral("No active Ext update is available."));
    QString oldPath, newPath;
    if (!normalizeExtPath(entryNameUtf8, &oldPath, error) ||
        !normalizeExtPath(newEntryNameUtf8, &newPath, error)) {
        failed_ = true;
        return false;
    }
    if (oldPath == newPath) return true;
    const QByteArray oldName = extPath(oldPath).toUtf8();
    const QByteArray newName = extPath(newPath).toUtf8();
    const bool directory = ext4_inode_exist(oldName.constData(), EXT4_DE_DIR) == EOK;
    const bool file = ext4_inode_exist(oldName.constData(), EXT4_DE_REG_FILE) == EOK;
    const bool symlink = ext4_inode_exist(oldName.constData(), EXT4_DE_SYMLINK) == EOK;
    if (!directory && !file && !symlink)
        return fail(error, QStringLiteral("The Ext source entry does not exist."));
    if (rejectEncryptedInode(state_->device.fs, oldName, error)) return false;
    if (!checkPathMutationFlags(state_->device.fs, oldName, error)) {
        failed_ = true;
        return false;
    }
    if (directory && newPath.startsWith(oldPath + QLatin1Char('/')))
        return fail(error, QStringLiteral("An Ext directory cannot be moved inside itself."));
    if (ext4_inode_exist(newName.constData(), EXT4_DE_UNKNOWN) != ENOENT)
        return fail(error, QStringLiteral("The Ext destination entry already exists or cannot be checked."));
    const quint32 quotaFeatures = ext4_get32(&state_->device.fs->sb,
                                              features_read_only) &
        (EXT4_FRO_COM_QUOTA | EXT4_FRO_COM_PROJECT);
    const int oldParentSlash = oldPath.lastIndexOf(QLatin1Char('/'));
    const int newParentSlash = newPath.lastIndexOf(QLatin1Char('/'));
    const QString oldParent = oldPath.left(oldParentSlash);
    const QString newParent = newPath.left(newParentSlash);
    const bool sameParent = oldParent == newParent;
    quint64 oldParentBlocks = 0;
    bool canSkipQuotaRecount = quotaFeatures && sameParent;
    if (canSkipQuotaRecount) {
        struct ext4_inode parentInode = {};
        uint32_t parentNumber = 0;
        const QByteArray parentPath = extPath(oldParent).toUtf8();
        canSkipQuotaRecount = ext4_raw_inode_fill(parentPath.constData(),
            &parentNumber, &parentInode) == EOK;
        if (canSkipQuotaRecount)
            oldParentBlocks = ext4_inode_get_blocks_count(
                &state_->device.fs->sb, &parentInode);
    }
    if (!ensureExtParents(state_->device.fs, oldPath, error) ||
        !ensureExtParents(state_->device.fs, newPath, error)) {
        failed_ = true;
        return false;
    }
    const int result = directory
        ? ext4_dir_mv(oldName.constData(), newName.constData())
        : ext4_frename(oldName.constData(), newName.constData());
    if (result != EOK) return fail(error, QStringLiteral("Ext could not rename the entry."));
    if (quotaFeatures) {
        struct ext4_inode parentInode = {};
        uint32_t parentNumber = 0;
        const QByteArray parentPath = extPath(newParent).toUtf8();
        const bool parentRead = canSkipQuotaRecount &&
            ext4_raw_inode_fill(parentPath.constData(), &parentNumber,
                                &parentInode) == EOK;
        const quint64 newParentBlocks = parentRead
            ? ext4_inode_get_blocks_count(&state_->device.fs->sb, &parentInode)
            : UINT64_MAX;
        quotaDirty_ = quotaDirty_ || !parentRead ||
                      oldParentBlocks != newParentBlocks;
    }
    dirty_ = true;
    return true;
}

bool ExtVolumeUpdate::deleteEntry(const QByteArray& entryNameUtf8, bool recursive,
                                   QString* error) {
    if (!mounted_ || failed_) return fail(error, QStringLiteral("No active Ext update is available."));
    QString path;
    if (!normalizeExtPath(entryNameUtf8, &path, error)) {
        failed_ = true;
        return false;
    }
    const QByteArray name = extPath(path).toUtf8();
    if (!ensureExtParents(state_->device.fs, path, error)) {
        failed_ = true;
        return false;
    }
    if (ext4_inode_exist(name.constData(), EXT4_DE_DIR) == EOK) {
        if (!recursive && !isEmptyDirectory(name))
            return fail(error, QStringLiteral("The Ext directory is not empty."));
        if (!checkDeleteMetadata(name, true, recursive, error)) return false;
        if (ext4_dir_rm(name.constData()) != EOK)
            return fail(error, QStringLiteral("Ext could not delete the directory."));
    } else if (ext4_inode_exist(name.constData(), EXT4_DE_REG_FILE) == EOK ||
               ext4_inode_exist(name.constData(), EXT4_DE_SYMLINK) == EOK) {
        if (!checkDeleteMetadata(name, false, false, error)) return false;
        if (ext4_fremove(name.constData()) != EOK)
            return fail(error, QStringLiteral("Ext could not delete the file."));
    } else {
        return fail(error, QStringLiteral("The Ext entry does not exist or is not a regular file."));
    }
    dirty_ = true;
    quotaDirty_ = true;
    return true;
}

bool ExtVolumeUpdate::checkDeleteMetadata(const QByteArray& path, bool directory,
                                          bool recursive, QString* error) {
    std::set<uint32_t> visited;
    const int result = state_ && state_->device.fs
        ? checkPathXattrs(state_->device.fs, path, directory, recursive, &visited)
        : -1;
    if (result == 0) return true;
    if (error) {
        *error = result == 2
            ? QStringLiteral("Ext cannot modify an encrypted inode without its fscrypt key.")
            : result == 3
                ? QStringLiteral("Ext cannot delete an inode with protected or unsupported flags.")
                : result > 0
                ? QStringLiteral("Ext cannot delete an entry with external extended attributes.")
                : QStringLiteral("Ext could not validate the entry metadata for deletion.");
    }
    failed_ = true;
    return false;
}

bool ExtVolumeUpdate::finish(QString* error) {
    if (!mounted_ || failed_) {
        abort();
        return fail(error, QStringLiteral("The Ext transaction failed and cannot be committed."));
    }
    if (quotaDirty_ && !updateQuotaAccounting(error)) {
        failed_ = true;
        return false;
    }
    return unmount(error);
}

bool ExtVolumeUpdate::updateQuotaAccounting(QString* error) {
    const quint32 features = ext4_get32(&state_->device.fs->sb,
                                        features_read_only);
    int failureStage = 0;
    if (!(features & (EXT4_FRO_COM_QUOTA | EXT4_FRO_COM_PROJECT)) ||
        recountExtQuotaUsage(state_->device.fs, &failureStage))
        return true;
    return fail(error, QStringLiteral(
        "Ext could not update quota usage for the modified volume (stage %1).")
        .arg(failureStage));
}

bool ExtVolumeUpdate::unshareSharedBlocks(QString* error) {
    if (!unshareExtBlocks(state_->device.fs))
        return fail(error, QStringLiteral(
            "Ext could not unshare all duplicate data blocks."));
    dirty_ = true;
    quotaDirty_ = true;
    return true;
}

void ExtVolumeUpdate::abort() {
    if (mounted_ || registered_ || journalRegistered_) unmount(nullptr);
}

}  // namespace writer
}  // namespace peare
