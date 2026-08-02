#pragma once

// Read-only file-system interface for the DiscUtils-compatible stack, ported
// from the read surface of DiscUtils.Core (IFileSystem + the Vfs reader model).
//
// A concrete reader (Iso9660Reader, WimReader, ...) parses a positioned byte
// store (an IByteStore over the image) and exposes:
//   * per-directory enumeration (== IFileSystem.GetDirectories / GetFiles), so
//     a consumer can expand the tree lazily, and
//   * OpenFile(path) returning the file content as a positioned IByteStore,
//     which reads on demand from the underlying image (== IFileSystem.OpenFile
//     returning a stream that lazily reads the disk).
//
// The content store returned by openFile is itself an IByteStore, so a file
// inside one image can be handed to another reader/opener — the recursion that
// makes the layers stack (image -> file -> another container).

#include <string>
#include <vector>

#include "DiscStore.h"

namespace peare {
namespace fs {

struct DiscEntry {
    std::string name;        // leaf name (no path separators)
    bool isDirectory = false;
    std::int64_t length = 0; // content length in bytes (0 for directories)
};

class IDiscFileSystem {
public:
    virtual ~IDiscFileSystem() {}

    // Human-readable name of the detected file system (e.g. "ISO 9660 (Joliet)").
    virtual std::string friendlyName() const = 0;

    // True when the image parsed cleanly. When false, error() explains why and
    // the other methods return empty results.
    virtual bool valid() const = 0;
    virtual std::string error() const = 0;

    // Immediate children of the directory at dirPath ("" or "/" is the root),
    // '/'-separated. == IFileSystem.GetDirectories + GetFiles for one directory.
    virtual std::vector<DiscEntry> list(const std::string& dirPath) const = 0;

    // Open the content of the file at path as a positioned store that reads from
    // the image on demand (no eager materialisation). Returns null if absent.
    virtual ByteStorePtr openFile(const std::string& path) const = 0;
};

typedef std::shared_ptr<IDiscFileSystem> DiscFileSystemPtr;

}  // namespace fs
}  // namespace peare
