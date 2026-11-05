#include "AmigaAnimFont.h"

#include <QVector>

#include <cstring>

namespace peare {
namespace font {
namespace {

const int kMaxAnimBytes = 64 * 1024 * 1024;
const int kMaxFrames = 8192;

bool fail(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

quint16 be16(const QByteArray& data, int offset)
{
    return (quint16(quint8(data.at(offset))) << 8) | quint8(data.at(offset + 1));
}

quint32 be32(const QByteArray& data, int offset)
{
    return (quint32(quint8(data.at(offset))) << 24) |
           (quint32(quint8(data.at(offset + 1))) << 16) |
           (quint32(quint8(data.at(offset + 2))) << 8) |
           quint8(data.at(offset + 3));
}

struct FrameChunks {
    QByteArray bmhd;
    QByteArray anhd;
    QByteArray cmap;
    QByteArray body;
    QByteArray delta;
};

bool parseFrame(const QByteArray& data, int start, int end, FrameChunks* chunks)
{
    if (start < 0 || end < start || end > data.size() || end - start < 4 ||
        data.mid(start, 4) != QByteArrayLiteral("ILBM"))
        return false;
    int pos = start + 4;
    while (pos < end) {
        if (end - pos < 8) return false;
        const QByteArray id = data.mid(pos, 4);
        const quint32 size = be32(data, pos + 4);
        const quint64 next = quint64(pos) + 8 + size + (size & 1u);
        if (next > quint64(end)) return false;
        const QByteArray value = data.mid(pos + 8, int(size));
        if (id == QByteArrayLiteral("BMHD")) chunks->bmhd = value;
        else if (id == QByteArrayLiteral("ANHD")) chunks->anhd = value;
        else if (id == QByteArrayLiteral("CMAP")) chunks->cmap = value;
        else if (id == QByteArrayLiteral("BODY")) chunks->body = value;
        else if (id == QByteArrayLiteral("DLTA")) chunks->delta = value;
        pos = int(next);
    }
    return pos == end;
}

bool unpackByteRun1(const QByteArray& source, int* position, int length, QByteArray* output)
{
    if (length < 0 || output->size() > kMaxAnimBytes - length) return false;
    const int target = output->size() + length;
    while (output->size() < target) {
        if (*position >= source.size()) return false;
        const qint8 control = qint8(source.at((*position)++));
        if (control >= 0) {
            const int count = int(control) + 1;
            if (count > target - output->size() || count > source.size() - *position)
                return false;
            output->append(source.constData() + *position, count);
            *position += count;
        } else if (control != -128) {
            const int count = 1 - int(control);
            if (count > target - output->size() || *position >= source.size()) return false;
            output->append(QByteArray(count, source.at((*position)++)));
        }
    }
    return true;
}

bool decodeBody(const QByteArray& body, int width, int height, int depth, int masking,
                bool compressed, QByteArray* planar)
{
    const int rowBytes = ((width + 15) / 16) * 2;
    const int planes = depth + (masking == 1 ? 1 : 0);
    const qint64 expected = qint64(rowBytes) * height * planes;
    if (expected <= 0 || expected > kMaxAnimBytes) return false;
    planar->fill('\0', int(expected));
    int pos = 0;
    for (int y = 0; y < height; ++y) {
        for (int plane = 0; plane < planes; ++plane) {
            const int destination = (plane * height + y) * rowBytes;
            if (compressed) {
                QByteArray row;
                if (!unpackByteRun1(body, &pos, rowBytes, &row) || row.size() != rowBytes)
                    return false;
                std::memcpy(planar->data() + destination, row.constData(), size_t(rowBytes));
            } else {
                if (body.size() - pos < rowBytes) return false;
                std::memcpy(planar->data() + destination, body.constData() + pos,
                            size_t(rowBytes));
                pos += rowBytes;
            }
        }
    }
    while (pos < body.size())
        if (qint8(body.at(pos++)) != -128) return false;
    return true;
}

bool applyOp5(const QByteArray& delta, int width, int height, int planes, bool xorMode,
              QByteArray* planar)
{
    if (delta.size() < 64) return false;
    const int rowBytes = ((width + 15) / 16) * 2;
    for (int plane = 0; plane < planes; ++plane) {
        const quint32 offset = be32(delta, plane * 4);
        if (!offset) continue;
        if (offset < 64 || offset >= quint32(delta.size())) return false;
        int pos = int(offset);
        const int planeBase = plane * rowBytes * height;
        for (int column = 0; column < rowBytes; ++column) {
            if (pos >= delta.size()) return false;
            const int opCount = quint8(delta.at(pos++));
            int row = 0;
            for (int op = 0; op < opCount; ++op) {
                if (pos >= delta.size()) return false;
                const quint8 code = quint8(delta.at(pos++));
                if (code & 0x80) {
                    const int count = code & 0x7f;
                    if (!count || count > height - row || count > delta.size() - pos)
                        return false;
                    for (int i = 0; i < count; ++i, ++row) {
                        const int target = planeBase + row * rowBytes + column;
                        const char value = delta.at(pos++);
                        (*planar)[target] = xorMode ? char((*planar).at(target) ^ value) : value;
                    }
                } else if (code) {
                    if (code > height - row) return false;
                    row += code;
                } else {
                    if (delta.size() - pos < 2) return false;
                    const int count = quint8(delta.at(pos++));
                    const char value = delta.at(pos++);
                    if (!count || count > height - row) return false;
                    for (int i = 0; i < count; ++i, ++row) {
                        const int target = planeBase + row * rowBytes + column;
                        (*planar)[target] = xorMode ? char((*planar).at(target) ^ value) : value;
                    }
                }
            }
        }
    }
    return true;
}

bool appendFrame(const QByteArray& planar, const QVector<quint32>& palette,
                 int width, int height, int depth, int masking, quint16 transparent,
                 BitmapFont* font)
{
    const int rowBytes = ((width + 15) / 16) * 2;
    Glyph glyph;
    glyph.codepoint = quint32(font->glyphs.size());
    glyph.width = width;
    glyph.height = height;
    glyph.bearingY = height;
    glyph.advance = width;
    glyph.palette = palette;
    glyph.pixels.resize(width * height);
    const int maskStride = (width + 7) / 8;
    if (masking) glyph.mask.fill('\0', maskStride * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int color = 0;
            for (int plane = 0; plane < depth; ++plane) {
                const int offset = (plane * height + y) * rowBytes + x / 8;
                if (quint8(planar.at(offset)) & (0x80u >> (x & 7))) color |= 1 << plane;
            }
            glyph.pixels[y * width + x] = char(color);
            bool opaque = true;
            if (masking == 1) {
                const int offset = (depth * height + y) * rowBytes + x / 8;
                opaque = (quint8(planar.at(offset)) & (0x80u >> (x & 7))) != 0;
            } else if (masking == 2) {
                opaque = color != transparent;
            }
            if (opaque && masking) {
                const int offset = y * maskStride + x / 8;
                glyph.mask[offset] = char(quint8(glyph.mask.at(offset)) |
                                          (0x80u >> (x & 7)));
            }
        }
    }
    font->glyphs.push_back(glyph);
    return true;
}

} // namespace

bool hasAmigaAnimFontHeader(const QByteArray& data)
{
    return data.size() >= 12 && data.startsWith(QByteArrayLiteral("FORM")) &&
           data.mid(8, 4) == QByteArrayLiteral("ANIM");
}

bool parseAmigaAnimFont(const QByteArray& data, BitmapFont* out, QString* error)
{
    if (!out || !hasAmigaAnimFontHeader(data) || data.size() > kMaxAnimBytes)
        return fail(error, QStringLiteral("Invalid Amiga AnimFont header or size"));
    const quint32 formSize = be32(data, 4);
    if (formSize < 4 || quint64(formSize) + 8 != quint64(data.size()))
        return fail(error, QStringLiteral("Invalid Amiga AnimFont FORM length"));
    QVector<FrameChunks> frames;
    int pos = 12;
    while (pos < data.size()) {
        if (data.size() - pos < 8 || data.mid(pos, 4) != QByteArrayLiteral("FORM"))
            return fail(error, QStringLiteral("Invalid Amiga AnimFont frame FORM"));
        const quint32 size = be32(data, pos + 4);
        const quint64 next = quint64(pos) + 8 + size + (size & 1u);
        if (size < 4 || next > quint64(data.size()) || frames.size() >= kMaxFrames)
            return fail(error, QStringLiteral("Invalid Amiga AnimFont frame length"));
        FrameChunks frame;
        if (!parseFrame(data, pos + 8, pos + 8 + int(size), &frame))
            return fail(error, QStringLiteral("Malformed Amiga AnimFont frame chunks"));
        frames.push_back(frame);
        pos = int(next);
    }
    if (frames.isEmpty() || frames.first().bmhd.size() != 20 ||
        frames.first().body.isEmpty())
        return fail(error, QStringLiteral("Amiga AnimFont has no complete first ILBM frame"));
    if (!frames.first().anhd.isEmpty() &&
        (frames.first().anhd.size() != 40 || quint8(frames.first().anhd.at(0)) != 0))
        return fail(error, QStringLiteral("Invalid Amiga AnimFont first-frame ANHD"));
    const QByteArray& bmhd = frames.first().bmhd;
    const int width = be16(bmhd, 0);
    const int height = be16(bmhd, 2);
    const int depth = quint8(bmhd.at(8));
    const int masking = quint8(bmhd.at(9));
    const int compression = quint8(bmhd.at(10));
    const quint16 transparent = be16(bmhd, 12);
    if (width <= 0 || height <= 0 || width > 256 || height > 256 ||
        qint64(width) * height > 65536 || depth <= 0 || depth > 8 ||
        (masking != 0 && masking != 1 && masking != 2) ||
        (compression != 0 && compression != 1))
        return fail(error, QStringLiteral("Unsupported Amiga AnimFont bitmap layout"));
    if (masking == 2 && transparent >= (1u << depth))
        return fail(error, QStringLiteral("Invalid Amiga AnimFont transparent color"));
    if (qint64(width) * height * frames.size() > 16 * 1024 * 1024)
        return fail(error, QStringLiteral("Amiga AnimFont decoded image limit exceeded"));
    if (frames.first().cmap.isEmpty() || frames.first().cmap.size() % 3 ||
        frames.first().cmap.size() / 3 > (1 << depth))
        return fail(error, QStringLiteral("Invalid Amiga AnimFont color map"));
    QVector<quint32> palette;
    for (int i = 0; i < frames.first().cmap.size(); i += 3)
        palette.push_back((quint32(quint8(frames.first().cmap.at(i))) << 16) |
                          (quint32(quint8(frames.first().cmap.at(i + 1))) << 8) |
                          quint8(frames.first().cmap.at(i + 2)));
    BitmapFont font;
    font.format = Format::AmigaAnim;
    font.name = QStringLiteral("Amiga AnimFont");
    font.width = width;
    font.height = height;
    font.ascent = height;
    font.fixedWidth = true;
    font.palette = palette;
    QByteArray buffers[2];
    if (!decodeBody(frames.first().body, width, height, depth, masking,
                    compression == 1, &buffers[0]))
        return fail(error, QStringLiteral("Invalid Amiga AnimFont first-frame bitmap"));
    buffers[1] = buffers[0];
    int visibleBuffer = 0;
    if (!appendFrame(buffers[visibleBuffer], palette, width, height, depth, masking,
                     transparent, &font))
        return fail(error, QStringLiteral("Invalid Amiga AnimFont first-frame bitmap"));
    for (int i = 1; i < frames.size(); ++i) {
        const FrameChunks& frame = frames.at(i);
        if (!frame.bmhd.isEmpty())
            return fail(error, QStringLiteral("Amiga AnimFont delta frames must use ANHD"));
        if (frame.cmap.size() % 3 || frame.cmap.size() / 3 > (1 << depth))
            return fail(error, QStringLiteral("Invalid Amiga AnimFont frame color map"));
        if (!frame.cmap.isEmpty()) {
            palette.clear();
            for (int j = 0; j < frame.cmap.size(); j += 3)
                palette.push_back((quint32(quint8(frame.cmap.at(j))) << 16) |
                                  (quint32(quint8(frame.cmap.at(j + 1))) << 8) |
                                  quint8(frame.cmap.at(j + 2)));
            font.palette = palette;
        }
        if (frame.anhd.size() != 40)
            return fail(error, QStringLiteral("Amiga AnimFont frame has no valid ANHD"));
        const int operation = quint8(frame.anhd.at(0));
        const int targetBuffer = 1 - visibleBuffer;
        if (operation == 0) {
            if (frame.body.isEmpty() ||
                !decodeBody(frame.body, width, height, depth, masking,
                            compression == 1, &buffers[targetBuffer]))
                return fail(error, QStringLiteral("Invalid Amiga AnimFont full frame"));
        } else if (operation == 5) {
            const int interleave = quint8(frame.anhd.at(18));
            const quint32 bits = be32(frame.anhd, 20);
            const int deltaPlanes = depth + (masking == 1 ? 1 : 0);
            if ((interleave != 0 && interleave != 1 && interleave != 2) ||
                (bits & ~2u) || deltaPlanes > 8 || frame.delta.isEmpty())
                return fail(error, QStringLiteral("Unsupported or malformed Amiga AnimFont op-5 delta"));
            if (interleave == 1) buffers[targetBuffer] = buffers[visibleBuffer];
            if (!applyOp5(frame.delta, width, height, deltaPlanes, (bits & 2u) != 0,
                          &buffers[targetBuffer]))
                return fail(error, QStringLiteral("Unsupported or malformed Amiga AnimFont op-5 delta"));
        } else {
            return fail(error, QStringLiteral("Unsupported Amiga AnimFont frame operation"));
        }
        if (!appendFrame(buffers[targetBuffer], palette, width, height, depth, masking,
                         transparent, &font))
            return fail(error, QStringLiteral("Invalid Amiga AnimFont frame bitmap"));
        visibleBuffer = targetBuffer;
    }
    *out = font;
    if (error) error->clear();
    return true;
}

} // namespace font
} // namespace peare
