#pragma once

#include <QByteArray>
#include <QString>
#include <QtGlobal>

namespace peare {

bool decompressOs2Pack2(const QByteArray& data, qsizetype start, qsizetype end,
                        quint32 expectedSize, QByteArray* decoded, QString* error);

} // namespace peare
