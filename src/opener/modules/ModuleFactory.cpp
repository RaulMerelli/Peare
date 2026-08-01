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
#include "SzddModule.h"
#include "SiemensImgModule.h"
#include "SiemensFwfModule.h"
#include "IsoModule.h"
#include "WimModule.h"

#include <QFile>
#include <QTemporaryFile>
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
    if (info.format == ModuleFormat::SZDD) return SzddModule::open(filePath);
    if (info.format == ModuleFormat::SIEMENS_IMG) return SiemensImgModule::open(filePath);
    if (info.format == ModuleFormat::SIEMENS_FWF) return SiemensFwfModule::open(filePath);
    if (info.format == ModuleFormat::ISO9660) return IsoModule::open(filePath);
    if (info.format == ModuleFormat::WIM) return WimModule::open(filePath);
    return peare::makeUnique<DetectedModule>(std::move(info));
}

ModulePtr ModuleFactory::open(const QString& physicalPath, const QString& logicalName)
{
    const ModuleFormatInfo detected = ModuleFormatDetector::detectFile(physicalPath);
    if (detected.format != ModuleFormat::SZDD &&
        detected.format != ModuleFormat::XUIZ &&
        detected.format != ModuleFormat::SIEMENS_IMG &&
        detected.format != ModuleFormat::SIEMENS_FWF)
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

    const QByteArray data = file.readAll();
    if (detected.format == ModuleFormat::XUIZ)
        return XuizModule::open(data, logicalName);
    if (detected.format == ModuleFormat::SIEMENS_IMG)
        return SiemensImgModule::open(data, logicalName);
    if (detected.format == ModuleFormat::SIEMENS_FWF)
        return SiemensFwfModule::open(data, logicalName);
    return SzddModule::open(data, logicalName);
}

ModulePtr ModuleFactory::open(const QByteArray& data, const QString& logicalName)
{
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
    if (detected.format == ModuleFormat::SZDD)
        return SzddModule::open(data, logicalName);
    if (detected.format == ModuleFormat::XUIZ)
        return XuizModule::open(data, logicalName);
    if (detected.format == ModuleFormat::SIEMENS_IMG)
        return SiemensImgModule::open(data, logicalName);
    if (detected.format == ModuleFormat::SIEMENS_FWF)
        return SiemensFwfModule::open(data, logicalName);
    return open(path);
}

} // namespace peare
