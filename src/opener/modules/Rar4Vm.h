#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace peare {
namespace rar4 {

enum class VmOpcode : std::uint8_t {
    Mov, Cmp, Add, Sub, Jz, Jnz, Inc, Dec, Jmp, Xor,
    And, Or, Test, Js, Jns, Jb, Jbe, Ja, Jae, Push,
    Pop, Call, Ret, Not, Shl, Shr, Sar, Neg, Pusha, Popa,
    Pushf, Popf, Movzx, Movsx, Xchg, Mul, Div, Adc, Sbb, Print
};

enum VmFlag : std::uint32_t {
    VmCarry = 0x00000001U,
    VmZero = 0x00000002U,
    VmSign = 0x80000000U
};

struct VmState {
    std::array<std::uint32_t, 8> registers{};
    std::uint32_t flags = 0;
};

struct VmOptions {
    const std::uint8_t* globalData = nullptr;
    std::size_t globalDataSize = 0;
};

struct VmResult {
    bool success = false;
    std::string error;
    std::size_t operations = 0;
    std::uint32_t outputPosition = 0;
    std::uint32_t outputSize = 0;
};

// Executes checksum-protected VM code. The caller supplies R0-R6 and optional
// global data; R7 and flags are initialized by the VM. Memory needs four guard
// bytes beyond the 0x40000-byte VM address space for bounded dword accesses.
VmResult executeVm(const std::uint8_t* bytecode, std::size_t bytecodeSize,
                   std::uint8_t* memory, std::size_t memorySize,
                   VmState* state, const VmOptions& options = VmOptions());

VmResult executeVm(const std::vector<std::uint8_t>& bytecode,
                   std::vector<std::uint8_t>* memory, VmState* state,
                   const VmOptions& options = VmOptions());

} // namespace rar4
} // namespace peare
