#pragma once

#include "DiscStore.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace peare {
namespace fs {

struct AndroidLogicalPartition {
    std::string name;
    std::uint32_t attributes = 0;
    ByteStorePtr content;
};

class AndroidSuperStore final {
public:
    static bool detect(const ByteStorePtr& source);
    static bool detect(const std::uint8_t* data, std::size_t size);
    static std::shared_ptr<AndroidSuperStore> open(const ByteStorePtr& source,
                                                   std::string* error);

    const std::vector<AndroidLogicalPartition>& partitions() const noexcept {
        return partitions_;
    }

private:
    bool parse(std::string* error);

    ByteStorePtr source_;
    std::vector<AndroidLogicalPartition> partitions_;
};

}  // namespace fs
}  // namespace peare
