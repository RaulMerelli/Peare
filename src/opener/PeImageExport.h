#pragma once

#include <QByteArray>
#include <QString>

namespace peare {

enum class PeImageLayoutKind {
    Invalid,
    File,
    LoadedImage,
    Hybrid
};

struct PeImageLayoutResult {
    PeImageLayoutKind kind = PeImageLayoutKind::Invalid;
    QString error;

    PeImageLayoutResult() = default;
    PeImageLayoutResult(PeImageLayoutKind k, const QString& err) : kind(k), error(err) {}

    bool isValid() const noexcept { return error.isEmpty() && kind != PeImageLayoutKind::Invalid; }
};

class PeImageExport {
public:
    static PeImageLayoutResult classify(const QByteArray& data);
    static QByteArray rebuildFileImage(const QByteArray& loadedImage, QString* error = nullptr);
};

} // namespace peare
