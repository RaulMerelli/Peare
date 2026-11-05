#pragma once

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QVector>
#include <functional>

namespace peare { namespace resources {

using WadLumpResolver = std::function<QByteArray(const QString&, const QString&)>;

class WadImageDecoder final {
public:
    static QVector<QImage> decode(const QByteArray& payload, const QString& type,
                                  const WadLumpResolver& resolver, QString* error = nullptr);
};

} }
