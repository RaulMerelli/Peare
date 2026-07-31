#include "PeImageExport.h"

#include <QtGlobal>
#include <climits>

namespace peare {
namespace {
quint16 u16(const QByteArray& d, qsizetype o)
{
    if (o < 0 || o + 2 > d.size()) return 0;
    const auto* p = reinterpret_cast<const uchar*>(d.constData() + o);
    return quint16(p[0]) | (quint16(p[1]) << 8);
}
quint32 u32(const QByteArray& d, qsizetype o)
{
    if (o < 0 || o + 4 > d.size()) return 0;
    const auto* p = reinterpret_cast<const uchar*>(d.constData() + o);
    return quint32(p[0]) | (quint32(p[1]) << 8) | (quint32(p[2]) << 16) | (quint32(p[3]) << 24);
}
void put32(QByteArray& d, qsizetype o, quint32 v)
{
    if (o < 0 || o + 4 > d.size()) return;
    auto* p = reinterpret_cast<uchar*>(d.data() + o);
    p[0] = uchar(v); p[1] = uchar(v >> 8); p[2] = uchar(v >> 16); p[3] = uchar(v >> 24);
}
quint32 alignUp(quint32 value, quint32 alignment)
{
    if (!alignment) return value;
    const quint64 result = (quint64(value) + alignment - 1) / alignment * alignment;
    return result <= 0xffffffffu ? quint32(result) : 0;
}
struct Headers {
    quint32 pe = 0;
    quint16 sections = 0;
    quint16 optionalSize = 0;
    quint32 fileAlignment = 0;
    quint32 sizeOfHeaders = 0;
    qsizetype sectionTable = 0;
};
bool parse(const QByteArray& d, Headers* h, QString* error)
{
    if (d.size() < 0x40 || u16(d, 0) != 0x5a4d) { if (error) *error = QStringLiteral("Invalid MZ header"); return false; }
    const quint32 pe = u32(d, 0x3c);
    if (quint64(pe) + 24 > quint64(d.size()) || u32(d, pe) != 0x00004550) { if (error) *error = QStringLiteral("Invalid PE header"); return false; }
    const quint16 optionalSize = u16(d, pe + 20);
    const qsizetype opt = qsizetype(pe) + 24;
    const quint16 magic = u16(d, opt);
    if (magic != 0x10b && magic != 0x20b) { if (error) *error = QStringLiteral("Unsupported PE optional header"); return false; }
    Headers out;
    out.pe = pe;
    out.sections = u16(d, pe + 6);
    out.optionalSize = optionalSize;
    out.fileAlignment = u32(d, opt + 36);
    out.sizeOfHeaders = u32(d, opt + 60);
    out.sectionTable = opt + optionalSize;
    if (!out.fileAlignment || out.sectionTable + qsizetype(out.sections) * 40 > d.size()) { if (error) *error = QStringLiteral("Invalid PE section table"); return false; }
    if (h) *h = out;
    return true;
}
}

PeImageLayoutResult PeImageExport::classify(const QByteArray& data)
{
    Headers h; QString error;
    if (!parse(data, &h, &error)) return {PeImageLayoutKind::Invalid, error};
    bool filePossible = true;
    bool imagePossible = true;
    for (quint16 i = 0; i < h.sections; ++i) {
        const qsizetype o = h.sectionTable + qsizetype(i) * 40;
        const quint32 virtualSize = u32(data, o + 8);
        const quint32 virtualAddress = u32(data, o + 12);
        const quint32 rawSize = u32(data, o + 16);
        const quint32 rawPointer = u32(data, o + 20);
        if (rawSize && quint64(rawPointer) + rawSize > quint64(data.size())) filePossible = false;
        const quint32 imageSpan = qMax(virtualSize, rawSize);
        if (imageSpan && quint64(virtualAddress) + imageSpan > quint64(data.size())) imagePossible = false;
    }
    if (filePossible && imagePossible) return {PeImageLayoutKind::Hybrid, {}};
    if (filePossible) return {PeImageLayoutKind::File, {}};
    if (imagePossible) return {PeImageLayoutKind::LoadedImage, {}};
    return {PeImageLayoutKind::Invalid, QStringLiteral("Neither PE file layout nor loaded-image layout is complete")};
}

QByteArray PeImageExport::rebuildFileImage(const QByteArray& loadedImage, QString* error)
{
    Headers h; QString localError;
    if (!parse(loadedImage, &h, &localError)) { if (error) *error = localError; return {}; }
    quint32 nextRaw = alignUp(qMax(h.sizeOfHeaders, quint32(h.sectionTable + qsizetype(h.sections) * 40)), h.fileAlignment);
    if (!nextRaw) { if (error) *error = QStringLiteral("Header alignment overflow"); return {}; }
    QByteArray out(int(nextRaw), '\0');
    const qsizetype optionalHeader = qsizetype(h.pe) + 24;
    const qsizetype headerCopy = qMin<qsizetype>(out.size(), loadedImage.size());
    if (headerCopy > 0) {
        // Named int values avoid Qt 5 treating a literal zero as a null pointer
        // candidate for QByteArray::replace(const char*, ...).
        const int replaceOffset = 0;
        const int replaceLength = int(headerCopy);
        out.replace(replaceOffset, replaceLength, loadedImage.constData(), replaceLength);
    }
    put32(out, optionalHeader + 60, nextRaw);

    for (quint16 i = 0; i < h.sections; ++i) {
        const qsizetype o = h.sectionTable + qsizetype(i) * 40;
        const quint32 virtualSize = u32(loadedImage, o + 8);
        const quint32 virtualAddress = u32(loadedImage, o + 12);
        quint32 rawSize = alignUp(virtualSize, h.fileAlignment);
        if (!rawSize && virtualSize) { if (error) *error = QStringLiteral("Section size alignment overflow"); return {}; }
        if (quint64(virtualAddress) + virtualSize > quint64(loadedImage.size())) {
            if (error) *error = QStringLiteral("Loaded image does not contain complete section %1").arg(i);
            return {};
        }
        put32(out, o + 16, rawSize);
        put32(out, o + 20, rawSize ? nextRaw : 0);
        if (rawSize) {
            const quint64 wanted = quint64(nextRaw) + rawSize;
            if (wanted > quint64(INT_MAX)) { if (error) *error = QStringLiteral("Rebuilt PE exceeds QByteArray limits"); return {}; }
            out.resize(int(wanted));
            if (virtualSize) out.replace(int(nextRaw), int(virtualSize), loadedImage.constData() + virtualAddress, int(virtualSize));
            nextRaw += rawSize;
        }
    }
    if (error) error->clear();
    return out;
}

} // namespace peare
