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
};

class IsoImageWriter final {
public:
    static bool write(const QString& outputPath,
                      const std::vector<IsoImageEntry>& entries,
                      QString* error);
};

}  // namespace writer
}  // namespace peare

#endif
