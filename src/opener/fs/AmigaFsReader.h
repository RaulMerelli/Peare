#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "DiscFileSystem.h"

namespace peare {
namespace fs {

class AmigaFsReader final : public IDiscFileSystem {
public:
    explicit AmigaFsReader(ByteStorePtr image);

    std::string friendlyName() const override { return friendly_; }
    bool valid() const override { return valid_; }
    std::string error() const override { return error_; }
    std::vector<DiscEntry> list(const std::string& dirPath) const override;
    ByteStorePtr openFile(const std::string& path) const override;

private:
    struct Object {
        std::uint32_t block = 0;
        std::uint32_t type = 0;
        std::string name;
        std::uint32_t size = 0;
    };

    void parse();
    bool readBlock(std::uint32_t block, std::vector<std::uint8_t>* bytes) const;
    bool findDirectory(const std::string& path, std::uint32_t* block) const;
    bool readObject(std::uint32_t block, Object* object) const;
    std::vector<std::uint32_t> dataBlocks(const Object& object) const;
    std::vector<Object> directory(std::uint32_t block) const;

    ByteStorePtr image_;
    bool valid_ = false;
    bool fastFileSystem_ = false;
    std::uint32_t rootBlock_ = 0;
    std::uint32_t blockCount_ = 0;
    std::string error_;
    std::string friendly_;
};

}  // namespace fs
}  // namespace peare
