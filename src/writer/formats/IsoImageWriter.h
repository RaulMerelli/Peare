#ifndef PEARE_WRITER_ISO_IMAGE_WRITER_H
#define PEARE_WRITER_ISO_IMAGE_WRITER_H

#include <QString>

#include <vector>

namespace peare {
namespace writer {

struct IsoImageEntry {
    QString path;
    QString sourcePath;
    bool directory = false;
    quint64 sourceOffset = 0;
    quint32 sourceSize = 0;
    bool sourceIsRange = false;
};

class IsoImageWriter final {
public:
    static bool write(const QString& outputPath,
                      const std::vector<IsoImageEntry>& entries,
                      QString* error);
    static bool readEntries(const QString& imagePath,
                            std::vector<IsoImageEntry>* entries,
                            QString* error);
};

}  // namespace writer
}  // namespace peare

#endif
