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
    ISO9660
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
    static QString formatName(ModuleFormat format);
};

} // namespace peare
