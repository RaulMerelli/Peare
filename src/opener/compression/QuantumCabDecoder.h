#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace peare {
namespace compression {

class QuantumCabDecoder {
public:
    QuantumCabDecoder();
    ~QuantumCabDecoder();

    QuantumCabDecoder(const QuantumCabDecoder&) = delete;
    QuantumCabDecoder& operator=(const QuantumCabDecoder&) = delete;

    bool configure(unsigned windowBits);
    bool decodeBlock(const std::uint8_t* input, std::size_t inputSize,
                     std::size_t outputSize, bool keepHistory,
                     std::vector<std::uint8_t>* output);

private:
    class Model;
    unsigned windowBits_ = 0;
    std::size_t windowSize_ = 0;
    std::size_t windowPosition_ = 0;
    std::size_t historySize_ = 0;
    bool ready_ = false;
    std::vector<std::uint8_t> window_;
    std::unique_ptr<Model> selector_;
    std::unique_ptr<Model[]> literals_;
    std::unique_ptr<Model[]> positions_;
    std::unique_ptr<Model> lengths_;

    void resetModels();
};

}  // namespace compression
}  // namespace peare
