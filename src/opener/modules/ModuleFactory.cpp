#include "ModuleFactory.h"
#include "Compat.h"

#include "PeFileModule.h"
#include "PeImageModule.h"
#include "NeModule.h"
#include "LxModule.h"
#include "LeModule.h"
#include "XexModule.h"
#include "XbeModule.h"
#include "XuizModule.h"
#include "LivePirsModule.h"
#include "ConModule.h"
#include "Os2PackModule.h"
#include "Os2EaModule.h"
#include "SzddModule.h"
#include "ResxModule.h"
#include "CueBinModule.h"
#include "CabModule.h"
#include "RawDiskModule.h"
#include "FloppyImageModule.h"
#include "FfuModule.h"
#include "ZipModule.h"
#include "TarModule.h"
#include "Ps3PupModule.h"
#include "WadModule.h"
#include "MtfModule.h"
#include "MdfMdsModule.h"
#include "ParallelsModule.h"
#include "Ps2RomdirModule.h"
#include "MsiModule.h"
#include "MsgModule.h"
#include "PlayReadyXapModule.h"
#include "EAppxModule.h"
#include "SiemensImgModule.h"
#include "SiemensFwfModule.h"
#include "SiemensFsfModule.h"
#include "wince/WinceRomModule.h"
#include "IsoModule.h"
#include "WimModule.h"
#include "FatModule.h"
#include "UdfModule.h"
#include "ExFatModule.h"
#include "VmdkModule.h"
#include "VhdModule.h"
#include "VdiModule.h"
#include "QcowModule.h"
#include "VhdxModule.h"
#include "SdiModule.h"
#include "XvaModule.h"
#include "SwapModule.h"
#include "LvmModule.h"
#include "LinuxRaidModule.h"
#include "DynamicDiskModule.h"
#include "ExtModule.h"
#include "NtfsModule.h"
#include "XfsModule.h"
#include "JfsModule.h"
#include "HpfsModule.h"
#include "SquashFsModule.h"
#include "HfsPlusModule.h"
#include "DmgModule.h"
#include "BtrfsModule.h"
#include "RegistryModule.h"
#include "BootConfigModule.h"

#include "../fs/LinuxRaid.h"
#include "../fs/DynamicDisk.h"
#include "../fs/FloppyImage.h"

#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <limits>
#include <cstring>
#include <utility>

namespace peare {
namespace {

class DetectedModule final : public IModule {
public:
    explicit DetectedModule(ModuleInfo info)
        : info_(std::move(info))
    {
    }

    const ModuleInfo& info() const noexcept override { return info_; }

private:
    ModuleInfo info_;
};

ModuleInfo detect(const QString& filePath)
{
    const ModuleFormatInfo detected = ModuleFormatDetector::detectFile(filePath);
    ModuleInfo info;
    info.filePath = filePath;
    info.format = detected.format;
    info.description = detected.description;
    info.headerOffset = detected.headerOffset;
    info.error = detected.error;
    return info;
}

bool isEncryptedAppFamily(ModuleFormat format)
{
    return format == ModuleFormat::EAPPX || format == ModuleFormat::EMSIX ||
           format == ModuleFormat::EAPPXBUNDLE || format == ModuleFormat::EMSIXBUNDLE;
}

bool isZipFamily(ModuleFormat format)
{
    return format == ModuleFormat::ZIP || format == ModuleFormat::APPX ||
           format == ModuleFormat::MSIX || format == ModuleFormat::APPXBUNDLE ||
           format == ModuleFormat::XAP;
}

bool isPlayReadyXapFile(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) &&
           PlayReadyXapModule::isHeader(file.read(0x20));
}

} // namespace

ModulePtr ModuleFactory::open(const QString& filePath)
{
    ModuleInfo info = detect(filePath);
    if (!info.isValid()) return peare::makeUnique<DetectedModule>(std::move(info));
    if (info.format == ModuleFormat::PE) {
        auto fileModule = PeFileModule::open(filePath);
        if (fileModule->info().isValid()) return fileModule;

        // Some XEX-extracted DLLs are memory images rather than normal PE files.
        // Retry with direct RVA addressing before reporting the static-layout error.
        auto imageModule = PeImageModule::openFile(filePath);
        if (imageModule->info().isValid()) return imageModule;
        return fileModule;
    }
    if (info.format == ModuleFormat::NE) return NeModule::open(filePath);
    if (info.format == ModuleFormat::LE) return LeModule::open(filePath);
    if (info.format == ModuleFormat::LX) return LxModule::open(filePath);
    if (info.format == ModuleFormat::XEX) return XexModule::open(filePath);
    if (info.format == ModuleFormat::XBE) return XbeModule::open(filePath);
    if (info.format == ModuleFormat::XUIZ) return XuizModule::open(filePath);
    if (info.format == ModuleFormat::LIVE_PIRS) return LivePirsModule::open(filePath);
    if (info.format == ModuleFormat::CON) return ConModule::open(filePath);
    if (info.format == ModuleFormat::OS2_PACK) return Os2PackModule::open(filePath);
    if (info.format == ModuleFormat::OS2_EA) return Os2EaModule::open(filePath);
    if (info.format == ModuleFormat::SZDD) return SzddModule::open(filePath);
    if (info.format == ModuleFormat::RESX) return ResxModule::open(filePath);
    if (info.format == ModuleFormat::CUE_BIN) return CueBinModule::open(filePath);
    if (info.format == ModuleFormat::CAB) return CabModule::open(filePath);
    if (isEncryptedAppFamily(info.format)) return EAppxModule::open(filePath);
    if (info.format == ModuleFormat::XAP && isPlayReadyXapFile(filePath))
        return PlayReadyXapModule::open(filePath);
    if (isZipFamily(info.format)) return ZipModule::open(filePath);
    if (info.format == ModuleFormat::TAR) return TarModule::open(filePath);
    if (info.format == ModuleFormat::PS3_PUP) return Ps3PupModule::open(filePath);
    if (info.format == ModuleFormat::WAD) return WadModule::open(filePath);
    if (info.format == ModuleFormat::MTF) return MtfModule::open(filePath);
    if (info.format == ModuleFormat::MDF_MDS) return MdfMdsModule::open(filePath);
    if (info.format == ModuleFormat::PARALLELS_HDD) return ParallelsModule::open(filePath);
    if (info.format == ModuleFormat::PS2_ROMDIR) return Ps2RomdirModule::open(filePath);
    if (info.format == ModuleFormat::MSI) return MsiModule::open(filePath);
    if (info.format == ModuleFormat::MSG) return MsgModule::open(filePath);
    if (info.format == ModuleFormat::WINCE_ROM) return WinceRomModule::open(filePath);
    if (info.format == ModuleFormat::FFU) return FfuModule::open(filePath);
    if (info.format == ModuleFormat::RAW_DISK) return RawDiskModule::open(filePath);
    if (info.format == ModuleFormat::FLOPPY_IMAGE) return FloppyImageModule::open(filePath);
    if (info.format == ModuleFormat::SIEMENS_IMG) return SiemensImgModule::open(filePath);
    if (info.format == ModuleFormat::SIEMENS_FWF) return SiemensFwfModule::open(filePath);
    if (info.format == ModuleFormat::SIEMENS_FSF) return SiemensFsfModule::open(filePath);
    if (info.format == ModuleFormat::ISO9660) return IsoModule::open(filePath);
    if (info.format == ModuleFormat::WIM) return WimModule::open(filePath);
    if (info.format == ModuleFormat::FAT) return FatModule::open(filePath);
    if (info.format == ModuleFormat::UDF) return UdfModule::open(filePath);
    if (info.format == ModuleFormat::EXFAT) return ExFatModule::open(filePath);
    if (info.format == ModuleFormat::VMDK) return VmdkModule::open(filePath);
    if (info.format == ModuleFormat::VHD) return VhdModule::open(filePath);
    if (info.format == ModuleFormat::VDI) return VdiModule::open(filePath);
    if (info.format == ModuleFormat::QCOW || info.format == ModuleFormat::QCOW2)
        return QcowModule::open(filePath);
    if (info.format == ModuleFormat::VHDX) return VhdxModule::open(filePath);
    if (info.format == ModuleFormat::SDI) return SdiModule::open(filePath);
    if (info.format == ModuleFormat::XVA) return XvaModule::open(filePath);
    if (info.format == ModuleFormat::SWAP) return SwapModule::open(filePath);
    if (info.format == ModuleFormat::LVM) return LvmModule::open(filePath);
    if (info.format == ModuleFormat::LINUX_RAID) return LinuxRaidModule::open(filePath);
    if (info.format == ModuleFormat::DYNAMIC_DISK) return DynamicDiskModule::open(filePath);
    if (info.format == ModuleFormat::EXT) return ExtModule::open(filePath);
    if (info.format == ModuleFormat::NTFS) return NtfsModule::open(filePath);
    if (info.format == ModuleFormat::XFS) return XfsModule::open(filePath);
    if (info.format == ModuleFormat::JFS) return JfsModule::open(filePath);
    if (info.format == ModuleFormat::HPFS) return HpfsModule::open(filePath);
    if (info.format == ModuleFormat::SQUASHFS) return SquashFsModule::open(filePath);
    if (info.format == ModuleFormat::HFSPLUS) return HfsPlusModule::open(filePath);
    if (info.format == ModuleFormat::DMG) return DmgModule::open(filePath);
    if (info.format == ModuleFormat::BTRFS) return BtrfsModule::open(filePath);
    if (info.format == ModuleFormat::REGISTRY) {
        ModulePtr bcd = BootConfigModule::open(filePath);
        if (bcd->info().isValid()) return bcd;
        return RegistryModule::open(filePath);
    }
    return peare::makeUnique<DetectedModule>(std::move(info));
}

ModulePtr ModuleFactory::open(const QString& physicalPath, const QString& logicalName)
{
    const ModuleFormatInfo detected = ModuleFormatDetector::detectFile(physicalPath);
    if (FloppyImageModule::hasSupportedExtension(logicalName) &&
        fs::floppyGeometryForCapacity(QFileInfo(physicalPath).size()).valid() &&
        (detected.format == ModuleFormat::Unknown || detected.format == ModuleFormat::FAT))
        return FloppyImageModule::open(physicalPath, logicalName);

    if (detected.format == ModuleFormat::PS3_PUP)
        return Ps3PupModule::open(physicalPath);
    if (detected.format == ModuleFormat::XAP && isPlayReadyXapFile(physicalPath))
        return PlayReadyXapModule::open(physicalPath);

    if (detected.format != ModuleFormat::SZDD &&
        detected.format != ModuleFormat::CAB &&
        !isZipFamily(detected.format) &&
        !isEncryptedAppFamily(detected.format) &&
        detected.format != ModuleFormat::TAR &&
        detected.format != ModuleFormat::WINCE_ROM &&
        detected.format != ModuleFormat::XUIZ &&
        detected.format != ModuleFormat::SIEMENS_IMG &&
        detected.format != ModuleFormat::SIEMENS_FWF &&
        detected.format != ModuleFormat::SIEMENS_FSF &&
        detected.format != ModuleFormat::OS2_EA &&
        detected.format != ModuleFormat::RESX)
        return open(physicalPath);

    QFile file(physicalPath);
    if (!file.open(QIODevice::ReadOnly)) {
        ModuleInfo info;
        info.filePath = logicalName;
        info.format = detected.format;
        info.description = detected.description;
        info.headerOffset = detected.headerOffset;
        info.error = file.errorString();
        return peare::makeUnique<DetectedModule>(std::move(info));
    }

    // ZIP already has a positioned reader. Keep it lazy instead of copying the
    // complete archive merely because it came from a temporary embedded source.
    if (isEncryptedAppFamily(detected.format)) return EAppxModule::open(physicalPath);
    if (isZipFamily(detected.format))
        return ZipModule::open(physicalPath, logicalName);

    const QByteArray data = file.readAll();
    if (detected.format == ModuleFormat::XUIZ)
        return XuizModule::open(data, logicalName);
    if (detected.format == ModuleFormat::SIEMENS_IMG)
        return SiemensImgModule::open(data, logicalName);
    if (detected.format == ModuleFormat::SIEMENS_FWF)
        return SiemensFwfModule::open(data, logicalName);
    if (detected.format == ModuleFormat::SIEMENS_FSF)
        return SiemensFsfModule::open(data, logicalName);
    if (detected.format == ModuleFormat::OS2_EA)
        return Os2EaModule::open(data, logicalName);
    if (detected.format == ModuleFormat::RESX)
        return ResxModule::open(data, logicalName);
    if (detected.format == ModuleFormat::CAB)
        return CabModule::open(data, logicalName);
    if (detected.format == ModuleFormat::TAR)
        return TarModule::open(data, logicalName);
    if (detected.format == ModuleFormat::PS3_PUP)
        return Ps3PupModule::open(data, logicalName);
    if (detected.format == ModuleFormat::WINCE_ROM)
        return WinceRomModule::open(data, logicalName);
    if (detected.format == ModuleFormat::FFU)
        return FfuModule::open(std::make_shared<fs::MemoryStore>(
            reinterpret_cast<const std::uint8_t*>(data.constData()), std::size_t(data.size())),
            logicalName);
    return SzddModule::open(data, logicalName);
}

ModulePtr ModuleFactory::open(const fs::ByteStorePtr& disc, const QString& sourceName,
                              const QString& subPath)
{
    if (!disc) {
        ModuleInfo info;
        info.filePath = sourceName;
        info.error = QStringLiteral("Null byte source");
        return peare::makeUnique<DetectedModule>(std::move(info));
    }
    // Cheap header peek (covers ISO's descriptor at 0x8001, and the UDF volume
    // recognition sequence which can extend past 0x9000 on bridge discs).
    const std::vector<std::uint8_t> head = disc->readRange(0, 0x14000);
    const QByteArray header(reinterpret_cast<const char*>(head.data()),
                            static_cast<int>(head.size()));
    const ModuleFormatInfo detected = ModuleFormatDetector::detectBuffer(header);

    // ProSave common-file IMG images often begin with B000FF. Inspect the
    // constant-size footer before dispatching the payload signature, otherwise
    // nested opening skips the Siemens wrapper and goes straight to WinCE.
    // Avoid this tail probe on expensive sequential/compressed stores.
    const bool imgCandidate = sourceName.endsWith(QStringLiteral(".img"),
                                                   Qt::CaseInsensitive) ||
                              detected.format == ModuleFormat::WINCE_ROM;
    if (imgCandidate && disc->capacity() >= 4 && disc->cheapRandomAccess()) {
        const std::vector<std::uint8_t> imgTail = disc->readRange(disc->capacity() - 4, 4);
        if (imgTail.size() == 4) {
            const std::uint32_t magic = std::uint32_t(imgTail[0]) |
                (std::uint32_t(imgTail[1]) << 8) | (std::uint32_t(imgTail[2]) << 16) |
                (std::uint32_t(imgTail[3]) << 24);
            if (magic == 0x03031998U) {
                if (disc->capacity() > std::numeric_limits<int>::max()) {
                    ModuleInfo info;
                    info.filePath = sourceName;
                    info.format = ModuleFormat::SIEMENS_IMG;
                    info.description = QStringLiteral("Siemens ProSave IMG firmware archive");
                    info.error = QStringLiteral("Embedded Siemens IMG is too large to materialise");
                    return peare::makeUnique<DetectedModule>(std::move(info));
                }
                const std::vector<std::uint8_t> all = disc->readAll();
                return SiemensImgModule::open(
                    QByteArray(reinterpret_cast<const char*>(all.data()), int(all.size())),
                    sourceName);
            }
        }
    }

    if (FloppyImageModule::hasSupportedExtension(sourceName) &&
        fs::floppyGeometryForCapacity(disc->capacity()).valid() &&
        (detected.format == ModuleFormat::Unknown || detected.format == ModuleFormat::FAT))
        return FloppyImageModule::open(disc, sourceName, subPath);

    if (detected.format == ModuleFormat::WINCE_ROM)
        return WinceRomModule::open(disc, sourceName, subPath);
    if (detected.format == ModuleFormat::FFU)
        return FfuModule::open(disc, sourceName);
    if (detected.format == ModuleFormat::XAP && PlayReadyXapModule::isHeader(header.left(0x20)))
        return PlayReadyXapModule::open(disc, sourceName);
    if (isZipFamily(detected.format))
        return ZipModule::open(disc, sourceName);
    if (detected.format == ModuleFormat::SIEMENS_FSF)
        return SiemensFsfModule::open(disc, sourceName);
    if (detected.format == ModuleFormat::SIEMENS_FWF) {
        if (disc->capacity() > std::numeric_limits<int>::max()) {
            ModuleInfo info;
            info.filePath = sourceName;
            info.format = ModuleFormat::SIEMENS_FWF;
            info.description = QStringLiteral("Siemens FWF OMS firmware archive");
            info.error = QStringLiteral("Embedded Siemens FWF is too large to materialise");
            return peare::makeUnique<DetectedModule>(std::move(info));
        }
        const std::vector<std::uint8_t> all = disc->readAll();
        return SiemensFwfModule::open(
            QByteArray(reinterpret_cast<const char*>(all.data()), int(all.size())), sourceName);
    }
    if (disc->capacity() >= 512 && disc->cheapRandomAccess()) {
        const std::vector<std::uint8_t> tail = disc->readRange(disc->capacity() - 512, 512);
        if (tail.size() >= 4 && std::memcmp(tail.data(), "koly", 4) == 0)
            return DmgModule::open(disc, sourceName);
    }
    if (fs::hasDynamicDiskMetadata(disc)) return DynamicDiskModule::open(disc, sourceName);
    if (fs::hasLinuxRaidSuperblock(disc)) return LinuxRaidModule::open(disc, sourceName);
    if (detected.format == ModuleFormat::ISO9660) return IsoModule::open(disc, sourceName, subPath);
    if (detected.format == ModuleFormat::WIM) return WimModule::open(disc, sourceName, subPath);
    if (detected.format == ModuleFormat::FAT) return FatModule::open(disc, sourceName, subPath);
    if (detected.format == ModuleFormat::UDF) return UdfModule::open(disc, sourceName, subPath);
    if (detected.format == ModuleFormat::EXFAT) return ExFatModule::open(disc, sourceName, subPath);
    if (detected.format == ModuleFormat::VMDK) return VmdkModule::open(disc, sourceName);
    if (isEncryptedAppFamily(detected.format)) return EAppxModule::open(disc, sourceName);
    if (detected.format == ModuleFormat::TAR) return TarModule::open(disc, sourceName);
    if (detected.format == ModuleFormat::PS3_PUP) return Ps3PupModule::open(disc, sourceName);
    if (detected.format == ModuleFormat::WAD) return WadModule::open(disc, sourceName);
    if (detected.format == ModuleFormat::MTF) return MtfModule::open(disc, sourceName);
    if (detected.format == ModuleFormat::PARALLELS_HDD) return ParallelsModule::open(disc, sourceName);
    if (detected.format == ModuleFormat::PS2_ROMDIR) return Ps2RomdirModule::open(disc, sourceName);
    if (detected.format == ModuleFormat::MSG ||
        (MsiModule::hasCompoundMagic(header.left(8)) && MsgModule::isOutlookMessage(disc)))
        return MsgModule::open(disc, sourceName);
    if (detected.format == ModuleFormat::MSI ||
        (detected.format == ModuleFormat::Unknown && MsiModule::hasInstallerExtension(sourceName) &&
         MsiModule::hasCompoundMagic(header.left(8))))
        return MsiModule::open(disc, sourceName);
    if (detected.format == ModuleFormat::RAW_DISK) return RawDiskModule::open(disc, sourceName);
    if (detected.format == ModuleFormat::FLOPPY_IMAGE)
        return FloppyImageModule::open(disc, sourceName, subPath);
    if (detected.format == ModuleFormat::VHD) return VhdModule::open(disc, sourceName);
    if (detected.format == ModuleFormat::VDI) return VdiModule::open(disc, sourceName);
    if (detected.format == ModuleFormat::QCOW || detected.format == ModuleFormat::QCOW2)
        return QcowModule::open(disc, sourceName);
    if (detected.format == ModuleFormat::VHDX) return VhdxModule::open(disc, sourceName);
    if (detected.format == ModuleFormat::SDI) return SdiModule::open(disc, sourceName);
    if (detected.format == ModuleFormat::XVA) return XvaModule::open(disc, sourceName);
    if (detected.format == ModuleFormat::SWAP) return SwapModule::open(disc, sourceName);
    if (detected.format == ModuleFormat::LVM) return LvmModule::open(disc, sourceName);
    if (detected.format == ModuleFormat::LINUX_RAID) return LinuxRaidModule::open(disc, sourceName);
    if (detected.format == ModuleFormat::DYNAMIC_DISK) return DynamicDiskModule::open(disc, sourceName);
    if (detected.format == ModuleFormat::EXT) return ExtModule::open(disc, sourceName, subPath);
    if (detected.format == ModuleFormat::NTFS) return NtfsModule::open(disc, sourceName, subPath);
    if (detected.format == ModuleFormat::XFS) return XfsModule::open(disc, sourceName, subPath);
    if (detected.format == ModuleFormat::JFS) return JfsModule::open(disc, sourceName, subPath);
    if (detected.format == ModuleFormat::HPFS) return HpfsModule::open(disc, sourceName, subPath);
    if (detected.format == ModuleFormat::SQUASHFS) return SquashFsModule::open(disc, sourceName, subPath);
    if (detected.format == ModuleFormat::HFSPLUS) return HfsPlusModule::open(disc, sourceName, subPath);
    if (detected.format == ModuleFormat::DMG) return DmgModule::open(disc, sourceName);
    if (detected.format == ModuleFormat::BTRFS) return BtrfsModule::open(disc, sourceName, subPath);
    if (detected.format == ModuleFormat::REGISTRY) {
        ModulePtr bcd = BootConfigModule::open(disc, sourceName, subPath);
        if (bcd->info().isValid()) return bcd;
        return RegistryModule::open(disc, sourceName, subPath);
    }
    // Non-filesystem formats need the whole content: materialise once.
    const std::vector<std::uint8_t> all = disc->readAll();
    return open(QByteArray(reinterpret_cast<const char*>(all.data()),
                           static_cast<int>(all.size())), sourceName);
}

ModulePtr ModuleFactory::open(const QByteArray& data, const QString& logicalName)
{
    // A Siemens common-file IMG may start with B000FF; its footer is the outer
    // format discriminator and therefore takes precedence. QByteArray access is
    // O(1), so this adds no scan or extra materialisation.
    if (data.size() >= 64) {
        const auto* p = reinterpret_cast<const unsigned char*>(
            data.constData() + data.size() - 4);
        const quint32 magic = quint32(p[0]) | (quint32(p[1]) << 8) |
                              (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
        if (magic == 0x03031998U)
            return SiemensImgModule::open(data, logicalName);
    }

    if (FloppyImageModule::hasSupportedExtension(logicalName) &&
        fs::floppyGeometryForCapacity(data.size()).valid()) {
        const ModuleFormatInfo strong = ModuleFormatDetector::detectNestedBuffer(data.left(0x14000));
        if (strong.format == ModuleFormat::Unknown || strong.format == ModuleFormat::FAT) {
            const fs::ByteStorePtr image = std::make_shared<fs::MemoryStore>(
                reinterpret_cast<const std::uint8_t*>(data.constData()), std::size_t(data.size()));
            return FloppyImageModule::open(image, logicalName);
        }
    }

    // Strong signatures can be dispatched directly from memory. This is the
    // common path for an archive stored in a PE resource and avoids a temporary
    // file plus a second complete read.
    const ModuleFormatInfo direct = ModuleFormatDetector::detectNestedBuffer(data.left(0x14000));
    if (direct.format == ModuleFormat::WINCE_ROM)
        return WinceRomModule::open(data, logicalName);
    if (direct.format == ModuleFormat::FFU)
        return FfuModule::open(std::make_shared<fs::MemoryStore>(
            reinterpret_cast<const std::uint8_t*>(data.constData()), std::size_t(data.size())),
            logicalName);
    if (direct.format == ModuleFormat::XAP && PlayReadyXapModule::isHeader(data.left(0x20)))
        return PlayReadyXapModule::open(data, logicalName);
    if (isEncryptedAppFamily(direct.format)) return EAppxModule::open(data, logicalName);
    if (isZipFamily(direct.format))
        return ZipModule::open(data, logicalName);
    if (direct.format == ModuleFormat::CAB)
        return CabModule::open(data, logicalName);
    if (direct.format == ModuleFormat::TAR)
        return TarModule::open(data, logicalName);
    if (direct.format == ModuleFormat::PS3_PUP)
        return Ps3PupModule::open(data, logicalName);
    if (direct.format == ModuleFormat::PS2_ROMDIR)
        return Ps2RomdirModule::open(data, logicalName);
    if (direct.format == ModuleFormat::MSG)
        return MsgModule::open(data, logicalName);
    if (direct.format == ModuleFormat::Unknown && MsiModule::hasCompoundMagic(data.left(8))) {
        const fs::ByteStorePtr compoundStore = std::make_shared<fs::MemoryStore>(
            reinterpret_cast<const std::uint8_t*>(data.constData()), std::size_t(data.size()));
        if (MsgModule::isOutlookMessage(compoundStore))
            return MsgModule::open(compoundStore, logicalName);
    }
    if (direct.format == ModuleFormat::MSI ||
        (direct.format == ModuleFormat::Unknown && MsiModule::hasInstallerExtension(logicalName) &&
         MsiModule::hasCompoundMagic(data.left(8))))
        return MsiModule::open(data, logicalName);
    if (direct.format == ModuleFormat::SZDD)
        return SzddModule::open(data, logicalName);
    if (direct.format == ModuleFormat::XUIZ)
        return XuizModule::open(data, logicalName);
    if (direct.format == ModuleFormat::SIEMENS_FWF)
        return SiemensFwfModule::open(data, logicalName);
    if (direct.format == ModuleFormat::SIEMENS_FSF)
        return SiemensFsfModule::open(data, logicalName);
    if (direct.format == ModuleFormat::OS2_EA)
        return Os2EaModule::open(data, logicalName);
    if (direct.format == ModuleFormat::RESX)
        return ResxModule::open(data, logicalName);

    QTemporaryFile temporary;
    if (!temporary.open() || temporary.write(data) != data.size() || !temporary.flush()) {
        ModuleInfo info;
        info.filePath = logicalName;
        info.error = temporary.errorString().isEmpty()
            ? QStringLiteral("Cannot create temporary file for embedded payload")
            : temporary.errorString();
        return peare::makeUnique<DetectedModule>(std::move(info));
    }

    const QString path = temporary.fileName();
    temporary.close();
    const ModuleFormatInfo detected = ModuleFormatDetector::detectFile(path);
    if (detected.format == ModuleFormat::WINCE_ROM)
        return WinceRomModule::open(data, logicalName);
    if (detected.format == ModuleFormat::FFU)
        return FfuModule::open(std::make_shared<fs::MemoryStore>(
            reinterpret_cast<const std::uint8_t*>(data.constData()), std::size_t(data.size())),
            logicalName);
    if (detected.format == ModuleFormat::SZDD)
        return SzddModule::open(data, logicalName);
    if (detected.format == ModuleFormat::CAB)
        return CabModule::open(data, logicalName);
    if (detected.format == ModuleFormat::XAP && PlayReadyXapModule::isHeader(data.left(0x20)))
        return PlayReadyXapModule::open(data, logicalName);
    if (isZipFamily(detected.format))
        return ZipModule::open(data, logicalName);
    if (detected.format == ModuleFormat::TAR)
        return TarModule::open(data, logicalName);
    if (detected.format == ModuleFormat::PS3_PUP)
        return Ps3PupModule::open(data, logicalName);
    if (detected.format == ModuleFormat::XUIZ)
        return XuizModule::open(data, logicalName);
    if (detected.format == ModuleFormat::SIEMENS_IMG)
        return SiemensImgModule::open(data, logicalName);
    if (detected.format == ModuleFormat::SIEMENS_FWF)
        return SiemensFwfModule::open(data, logicalName);
    if (detected.format == ModuleFormat::SIEMENS_FSF)
        return SiemensFsfModule::open(data, logicalName);
    if (detected.format == ModuleFormat::OS2_EA)
        return Os2EaModule::open(data, logicalName);
    if (detected.format == ModuleFormat::RESX)
        return ResxModule::open(data, logicalName);
    if (detected.format == ModuleFormat::MSG)
        return MsgModule::open(data, logicalName);
    return open(path);
}

} // namespace peare
