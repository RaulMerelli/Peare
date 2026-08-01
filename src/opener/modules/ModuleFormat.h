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
    ISO9660,
    WIM
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
    static QString formatName(ModuleFormat format);
};

} // namespace peare
