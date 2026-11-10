#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <vector>

#include "../compression/third_party/lzma/Ppmd7.h"

namespace peare {
namespace rar4 {

struct LzState {
    LzState();
    ~LzState();
    LzState(const LzState&) = delete;
    LzState& operator=(const LzState&) = delete;
    void reset();
    void resetPpmd();
    void pushHistory(std::uint8_t value);
    bool historyAtDistance(std::uint32_t distance, std::size_t window,
                           std::uint8_t* value) const;

    std::array<std::uint8_t, 404> priorLengths{};
    bool hasPriorLengths = false;
    std::array<std::uint32_t, 4> recentDistances{{0, 0, 0, 0}};
    std::uint32_t previousLowDistance = 0;
    bool hasPreviousLowDistance = false;
    unsigned repeatedLowDistance = 0;
    std::uint32_t lastDistance = 0;
    std::uint32_t lastLength = 0;
    CPpmd7 ppmdModel{};
    bool ppmdValid = false;
    std::uint32_t ppmdMemorySize = 0;
    std::uint8_t ppmdEscape = 2;

private:
    std::vector<std::uint8_t> history_;
    std::size_t historyWrite_ = 0;
    std::size_t historyCount_ = 0;
};

bool decodeLz(const std::uint8_t* input, std::size_t inputSize,
              std::size_t outputSize, bool solid,
              std::size_t historyWindow, LzState* state,
              std::vector<std::uint8_t>* output);

} // namespace rar4
} // namespace peare
