#ifndef PEARE_WRITER_VMDK_IMAGE_ACCESS_H
#define PEARE_WRITER_VMDK_IMAGE_ACCESS_H

#include <QString>

namespace peare {
namespace writer {

class VmdkImageAccess {
public:
    virtual ~VmdkImageAccess() {}
    virtual bool valid() const = 0;
    virtual qint64 capacity() const = 0;
    virtual bool read(qint64 offset, char* data, qint64 length) const = 0;
    virtual bool write(qint64 offset, const char* data, qint64 length) = 0;
    virtual bool finish(QString* error) = 0;
};

}  // namespace writer
}  // namespace peare

#endif
