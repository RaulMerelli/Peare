#include "FMIM.h"
#include <QtEndian>

namespace peare {
namespace resources {
namespace {
quint32 be32(const QByteArray& d, int o) {
    return o >= 0 && o + 4 <= d.size()
        ? qFromBigEndian<quint32>(reinterpret_cast<const uchar*>(d.constData() + o)) : 0;
}
QString cleanField(const QByteArray& bytes) {
    QByteArray clean = bytes;
    clean.replace('\0', QByteArray());
    return QString::fromLatin1(clean).trimmed();
}
}
ResourcePreview FMIM::preview(const ResourceEntry& entry) {
    ResourcePreview result;
    const QByteArray& data = entry.data;
    static const uchar pattern[8] = {0,0,0,1,0,1,0,1};
    const int payloadOffset = 4 + 8 + 6 * 0x200 + 4 + 0xF8;
    if (data.size() < payloadOffset || data.left(4) != QByteArrayLiteral("FMIM")) {
        result.error = QStringLiteral("Invalid or truncated FMIM file"); return result;
    }
    for (int i = 0; i < 8; ++i) if (uchar(data.at(4 + i)) != pattern[i]) {
        result.error = QStringLiteral("Invalid FMIM header"); return result;
    }
    static const char* labels[] = {"Song","Album","Artist 1","Artist 2","Genre 1","Genre 2"};
    QString text = QStringLiteral("Format: FMIM\n");
    int offset = 12;
    for (const char* label : labels) {
        text += QString::fromLatin1(label) + QStringLiteral(": ") + cleanField(data.mid(offset, 0x200)) + QLatin1Char('\n');
        offset += 0x200;
    }
    text += QStringLiteral("Declared payload size: %1\n").arg(be32(data, offset));
    EmbeddedExport embedded;
    embedded.fileName = QStringLiteral("audio.wma");
    embedded.extension = QStringLiteral(".wma");
    embedded.bytes = data.mid(payloadOffset);
    result.embeddedExports.push_back(embedded);
    result.text = text;
    return result;
}
} // namespace resources
} // namespace peare
