#pragma once

#include <QString>

namespace peare {

enum class ModuleFormat {
    Unknown,
    DosMZ,
    PE,
    NE,
    LE,
    LX,
    XEX,
    XBE,
    XUIZ,
    LIVE_PIRS,
    CON,
    OS2_PACK,
    SZDD,
    SIEMENS_IMG,
    SIEMENS_FWF,
    SIEMENS_FSF,
    ISO9660,
    WIM,
    FAT,
    UDF,
    EXFAT,
    VMDK,
    VHD,
    VDI,
    VHDX,
    SDI,
    XVA,
    SWAP,
    LVM,
    EXT,
    NTFS,
    XFS,
    JFS,
    HPFS,
    SQUASHFS,
    HFSPLUS,
    DMG,
    BTRFS,
    REGISTRY,
    BOOTCONFIG,
    CAB,
    RAW_DISK,
    ZIP,
    TAR,
    LINUX_RAID,
    DYNAMIC_DISK,
    WINCE_ROM,
    FFU,
    OS2_EA,
    RESX,
    CUE_BIN
};

struct ModuleFormatInfo {
    ModuleFormat format = ModuleFormat::Unknown;
    quint32 headerOffset = 0;
    QString description;
    QString error;

    ModuleFormatInfo() = default;
    ModuleFormatInfo(ModuleFormat f, quint32 offset, const QString& desc, const QString& err)
        : format(f), headerOffset(offset), description(desc), error(err) {}

    bool isValid() const noexcept { return error.isEmpty() && format != ModuleFormat::Unknown; }
};

class ModuleFormatDetector final {
public:
    static ModuleFormatInfo detectFile(const QString &filePath);
    // Header-based detection over an in-memory buffer (a resource's first bytes).
    // Used for the cheap is-container peek; recognises formats identifiable from
    // a header (MZ/PE/NE/LE/LX, XBE/XEX/XUIZ, STFS, OS/2 PACK, SZDD, ISO).
    static ModuleFormatInfo detectBuffer(const QByteArray &data);
    // Strong-signature-only detection for arbitrary resource payloads. Unlike
    // detectBuffer(), this never performs structural scans and is therefore
    // safe to run over every executable resource using only a small prefix.
    static ModuleFormatInfo detectNestedBuffer(const QByteArray &data);
    static QString formatName(ModuleFormat format);
};

} // namespace peare
