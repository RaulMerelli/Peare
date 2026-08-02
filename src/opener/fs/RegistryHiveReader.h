#pragma once

#include "DiscFileSystem.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace peare {
namespace fs {

class RegistryHiveReader final : public IDiscFileSystem {
public:
    explicit RegistryHiveReader(ByteStorePtr store);

    std::string friendlyName() const override { return friendly_; }
    bool valid() const override { return valid_; }
    std::string error() const override { return error_; }
    std::vector<DiscEntry> list(const std::string& dirPath) const override;
    ByteStorePtr openFile(const std::string& path) const override;

private:
    struct KeyCell {
        std::int32_t index = -1;
        std::uint16_t flags = 0;
        std::int32_t subKeyCount = 0;
        std::int32_t subKeysIndex = -1;
        std::int32_t valueCount = 0;
        std::int32_t valueListIndex = -1;
        std::string name;
    };

    struct ValueCell {
        std::int32_t index = -1;
        std::int32_t dataLength = 0;
        std::int32_t dataIndex = -1;
        std::uint32_t type = 0;
        std::uint16_t flags = 0;
        std::string name;
    };

    struct RawCell {
        bool valid = false;
        std::int32_t size = 0;
        std::vector<std::uint8_t> data;
    };

    void parse();
    RawCell rawCell(std::int32_t index) const;
    bool parseKey(std::int32_t index, KeyCell* key) const;
    bool parseValue(std::int32_t index, ValueCell* value) const;
    bool collectSubKeyIndexes(std::int32_t listIndex, std::vector<std::int32_t>* out,
                              int depth = 0) const;
    bool readValueData(const ValueCell& value, std::vector<std::uint8_t>* out) const;
    std::vector<std::int32_t> valueIndexes(const KeyCell& key) const;
    bool keyForPath(const std::string& path, KeyCell* key) const;
    std::string valuePayload(const ValueCell& value, const std::vector<std::uint8_t>& data) const;

    ByteStorePtr store_;
    bool valid_ = false;
    std::string error_;
    std::string friendly_ = "Windows Registry hive";
    std::int32_t rootCell_ = -1;
    std::int32_t hiveLength_ = 0;
};

}  // namespace fs
}  // namespace peare

