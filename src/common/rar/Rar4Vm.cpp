#include "Rar4Vm.h"

#include <limits>
#include <stdexcept>

namespace peare {
namespace rar4 {
namespace {

const std::size_t kOperationLimit = 25000000;
const std::size_t kVmMemorySize = 0x40000;
const std::size_t kGlobalBase = 0x3c000;

class VmFailure : public std::runtime_error {
public:
    explicit VmFailure(const char* message) : std::runtime_error(message) {}
};

class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    std::uint32_t read(unsigned count) {
        if (count > 32 || size_ > (std::numeric_limits<std::size_t>::max)() / 8 ||
            bitPosition_ > size_ * 8 || count > size_ * 8 - bitPosition_)
            throw VmFailure("truncated RAR4 VM bytecode");
        std::uint32_t value = 0;
        for (unsigned i = 0; i < count; ++i) {
            const std::size_t byte = bitPosition_ / 8;
            const unsigned bit = 7U - static_cast<unsigned>(bitPosition_ % 8);
            value = (value << 1) | ((data_[byte] >> bit) & 1U);
            ++bitPosition_;
        }
        return value;
    }

    std::uint32_t readData() {
        const unsigned form = read(2);
        if (form == 0) return read(4);
        if (form == 1) {
            const unsigned marker = read(4);
            if (marker == 0) {
                const std::uint32_t value = read(8);
                return (value & 0x80U) ? value | 0xffffff00U : value;
            }
            return (marker << 4) | read(4);
        }
        if (form == 2) return read(16);
        return (read(16) << 16) | read(16);
    }

    std::uint8_t readByte() { return static_cast<std::uint8_t>(read(8)); }
    std::size_t remaining() const { return size_ * 8 - bitPosition_; }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t bitPosition_ = 0;
};

enum class OperandKind { Register, Immediate, Memory };

struct Operand {
    OperandKind kind = OperandKind::Immediate;
    unsigned width = 4;
    unsigned reg = 0;
    bool hasRegister = false;
    unsigned baseRegister = 0;
    std::uint32_t displacement = 0;
    std::uint32_t value = 0;
};

struct Instruction {
    VmOpcode opcode;
    unsigned operandCount = 0;
    unsigned width = 4;
    Operand operands[2];
    bool hasPreparedTarget = false;
    std::int64_t preparedTarget = 0;
    bool implicit = false;
};

struct OpcodeInfo {
    unsigned operandCount;
    bool byteMode;
};

enum class StandardFilter {
    None, E8, E8E9, Itanium, Delta, Rgb, Audio, Upcase
};

struct StandardSignature {
    std::size_t size;
    std::uint32_t crc;
    StandardFilter filter;
};

const StandardSignature kStandardSignatures[] = {
    {53, 0xad576887U, StandardFilter::E8},
    {57, 0x3cd7e57eU, StandardFilter::E8E9},
    {120, 0x3769893fU, StandardFilter::Itanium},
    {29, 0x0e06077dU, StandardFilter::Delta},
    {149, 0x1c2c5dc8U, StandardFilter::Rgb},
    {216, 0xbc85e701U, StandardFilter::Audio},
    {40, 0x46b9c560U, StandardFilter::Upcase}
};

std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
    std::uint32_t crc = 0xffffffffU;
    for (std::size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1U) ? 0xedb88320U : 0U);
    }
    return crc ^ 0xffffffffU;
}

StandardFilter identifyStandardFilter(const std::uint8_t* code, std::size_t size) {
    if (!code || size == 0) return StandardFilter::None;
    std::uint8_t xorChecksum = 0;
    for (std::size_t i = 1; i < size; ++i) xorChecksum ^= code[i];
    if (xorChecksum != code[0]) return StandardFilter::None;
    const std::uint32_t checksum = crc32(code, size);
    for (std::size_t i = 0; i < sizeof(kStandardSignatures) / sizeof(kStandardSignatures[0]); ++i)
        if (kStandardSignatures[i].size == size && kStandardSignatures[i].crc == checksum)
            return kStandardSignatures[i].filter;
    return StandardFilter::None;
}

std::uint32_t getLe32(const std::uint8_t* data) {
    return static_cast<std::uint32_t>(data[0]) |
           (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) |
           (static_cast<std::uint32_t>(data[3]) << 24);
}

void putLe32(std::uint8_t* data, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        data[i] = static_cast<std::uint8_t>(value >> (i * 8U));
}

int signedByte(std::uint8_t value) {
    return value & 0x80U ? static_cast<int>(value) - 256 : value;
}

std::uint32_t itaniumGetBits(const std::uint8_t* data, unsigned bitPosition,
                             unsigned bitCount) {
    const std::size_t bytePosition = bitPosition / 8U;
    const unsigned bitOffset = bitPosition & 7U;
    std::uint32_t field = static_cast<std::uint32_t>(data[bytePosition]) |
        (static_cast<std::uint32_t>(data[bytePosition + 1]) << 8) |
        (static_cast<std::uint32_t>(data[bytePosition + 2]) << 16) |
        (static_cast<std::uint32_t>(data[bytePosition + 3]) << 24);
    field >>= bitOffset;
    return field & ((1U << bitCount) - 1U);
}

void itaniumSetBits(std::uint8_t* data, unsigned bitPosition, unsigned bitCount,
                    std::uint32_t value) {
    for (unsigned i = 0; i < bitCount; ++i) {
        const unsigned position = bitPosition + i;
        const std::uint8_t mask = static_cast<std::uint8_t>(1U << (position & 7U));
        std::uint8_t& byte = data[position / 8U];
        byte = static_cast<std::uint8_t>((byte & ~mask) | (((value >> i) & 1U) ? mask : 0U));
    }
}

bool executeStandardFilter(StandardFilter filter, std::uint8_t* memory,
                           const VmState& state) {
    const std::uint32_t size = state.registers[4];
    const std::uint32_t fileOffset = state.registers[6];
    switch (filter) {
    case StandardFilter::E8: case StandardFilter::E8E9: {
        if (size < 4 || size >= kGlobalBase) return false;
        const std::uint8_t second = filter == StandardFilter::E8E9 ? 0xe9U : 0xe8U;
        for (std::uint32_t position = 0; position < size - 4U;) {
            const std::uint8_t opcode = memory[position++];
            if (opcode != 0xe8U && opcode != second) continue;
            const std::uint32_t offset = position + fileOffset;
            const std::uint32_t address = getLe32(memory + position);
            if (address & VmSign) {
                if (((address + offset) & VmSign) == 0)
                    putLe32(memory + position, address + 0x01000000U);
            } else if (address < 0x01000000U) {
                putLe32(memory + position, address - offset);
            }
            position += 4;
        }
        break;
    }
    case StandardFilter::Itanium: {
        if (size < 21 || size >= kGlobalBase) return false;
        std::uint32_t filePosition = fileOffset >> 4;
        for (std::uint32_t position = 0; position < size - 21U; position += 16, ++filePosition) {
            const int slot = static_cast<int>(memory[position] & 0x1fU) - 0x10;
            if (slot < 0) continue;
            static const std::uint8_t masks[16] = {
                4, 4, 6, 6, 0, 0, 7, 7, 4, 4, 0, 0, 4, 4, 0, 0
            };
            for (unsigned index = 0; index < 3; ++index) {
                if (!(masks[slot] & (1U << index))) continue;
                const unsigned start = index * 41U + 5U;
                if (itaniumGetBits(memory + position, start + 37U, 4) == 5) {
                    const std::uint32_t offset = itaniumGetBits(memory + position, start + 13U, 20);
                    itaniumSetBits(memory + position, start + 13U, 20,
                                   (offset - filePosition) & 0xfffffU);
                }
            }
        }
        break;
    }
    case StandardFilter::Delta: {
        if (size >= kGlobalBase / 2) return false;
        const std::uint32_t channels = state.registers[0];
        if (channels == 0 || channels > 128) return false;
        std::uint32_t source = 0;
        for (std::uint32_t channel = 0; channel < channels && channel < size; ++channel) {
            std::uint8_t previous = 0;
            for (std::uint32_t dest = size + channel; dest < size * 2U; dest += channels)
                memory[dest] = previous = static_cast<std::uint8_t>(previous - memory[source++]);
        }
        putLe32(memory + kGlobalBase + 0x20, size);
        break;
    }
    case StandardFilter::Rgb: {
        const std::uint32_t widthRegister = state.registers[0];
        const std::uint32_t posR = state.registers[1];
        if (size < 3 || size >= kGlobalBase / 2 || widthRegister < 3 || posR > 2) return false;
        const std::uint32_t width = widthRegister - 3;
        std::uint8_t* source = memory;
        std::uint8_t* destination = memory + size;
        for (std::uint32_t channel = 0; channel < 3; ++channel) {
            std::uint32_t previous = 0;
            for (std::uint32_t i = channel; i < size; i += 3) {
                std::uint32_t predicted;
                const std::int64_t upperPosition = static_cast<std::int64_t>(i) - width;
                if (upperPosition >= 3) {
                    const std::uint8_t upper = destination[upperPosition];
                    const std::uint8_t upperLeft = destination[upperPosition - 3];
                    predicted = previous + upper - upperLeft;
                    const int pa = predicted > previous ? static_cast<int>(predicted - previous)
                                                        : static_cast<int>(previous - predicted);
                    const int pb = predicted > upper ? static_cast<int>(predicted - upper)
                                                     : static_cast<int>(upper - predicted);
                    const int pc = predicted > upperLeft ? static_cast<int>(predicted - upperLeft)
                                                         : static_cast<int>(upperLeft - predicted);
                    if (pa <= pb && pa <= pc) predicted = previous;
                    else if (pb <= pc) predicted = upper;
                    else predicted = upperLeft;
                } else {
                    predicted = previous;
                }
                previous = destination[i] = static_cast<std::uint8_t>(predicted - *source++);
            }
        }
        for (std::uint32_t i = posR; i < size - (size >= 2 ? 2U : size); i += 3) {
            const std::uint8_t green = destination[i + 1];
            destination[i] = static_cast<std::uint8_t>(destination[i] + green);
            destination[i + 2] = static_cast<std::uint8_t>(destination[i + 2] + green);
        }
        putLe32(memory + kGlobalBase + 0x20, size);
        break;
    }
    case StandardFilter::Audio: {
        if (size >= kGlobalBase / 2) return false;
        const std::uint32_t channels = state.registers[0];
        if (channels == 0 || channels > 128) return false;
        std::uint8_t* source = memory;
        std::uint8_t* destination = memory + size;
        for (std::uint32_t channel = 0; channel < channels && channel < size; ++channel) {
            std::uint32_t previousByte = 0;
            int previousDelta = 0;
            int d1 = 0, d2 = 0, d3 = 0;
            int k1 = 0, k2 = 0, k3 = 0;
            std::uint32_t differences[7] = {};
            for (std::uint32_t i = channel, byteCount = 0; i < size; i += channels, ++byteCount) {
                d3 = d2;
                d2 = previousDelta - d1;
                d1 = previousDelta;
                const std::uint32_t predicted = (8U * previousByte + k1 * d1 + k2 * d2 + k3 * d3) >> 3 & 0xffU;
                const std::uint32_t current = *source++;
                const std::uint32_t decoded = predicted - current;
                destination[i] = static_cast<std::uint8_t>(decoded);
                previousDelta = signedByte(static_cast<std::uint8_t>(decoded - previousByte));
                previousByte = decoded;
                const int d = signedByte(static_cast<std::uint8_t>(current)) * 8;
                differences[0] += static_cast<std::uint32_t>(d < 0 ? -d : d);
                differences[1] += static_cast<std::uint32_t>((d - d1) < 0 ? -(d - d1) : (d - d1));
                differences[2] += static_cast<std::uint32_t>((d + d1) < 0 ? -(d + d1) : (d + d1));
                differences[3] += static_cast<std::uint32_t>((d - d2) < 0 ? -(d - d2) : (d - d2));
                differences[4] += static_cast<std::uint32_t>((d + d2) < 0 ? -(d + d2) : (d + d2));
                differences[5] += static_cast<std::uint32_t>((d - d3) < 0 ? -(d - d3) : (d - d3));
                differences[6] += static_cast<std::uint32_t>((d + d3) < 0 ? -(d + d3) : (d + d3));
                if ((byteCount & 0x1fU) == 0) {
                    std::uint32_t minimum = differences[0];
                    unsigned selected = 0;
                    for (unsigned j = 0; j < 7; ++j) {
                        if (differences[j] < minimum) {
                            minimum = differences[j];
                            selected = j;
                        }
                        differences[j] = 0;
                    }
                    switch (selected) {
                    case 1: if (k1 >= -16) --k1; break;
                    case 2: if (k1 < 16) ++k1; break;
                    case 3: if (k2 >= -16) --k2; break;
                    case 4: if (k2 < 16) ++k2; break;
                    case 5: if (k3 >= -16) --k3; break;
                    case 6: if (k3 < 16) ++k3; break;
                    }
                }
            }
        }
        putLe32(memory + kGlobalBase + 0x20, size);
        break;
    }
    case StandardFilter::Upcase: {
        if (size >= kGlobalBase / 2) return false;
        std::uint32_t source = 0;
        std::uint32_t destination = size;
        while (source < size) {
            std::uint8_t value = memory[source++];
            if (value == 2) {
                if (source >= size) return false;
                value = memory[source++];
                if (value != 2) value = static_cast<std::uint8_t>(value - 32U);
            }
            memory[destination++] = value;
        }
        putLe32(memory + kGlobalBase + 0x1c, destination - size);
        putLe32(memory + kGlobalBase + 0x20, size);
        break;
    }
    case StandardFilter::None:
        return false;
    }
    return true;
}

OpcodeInfo opcodeInfo(VmOpcode opcode) {
    switch (opcode) {
    case VmOpcode::Mov: case VmOpcode::Cmp: case VmOpcode::Add: case VmOpcode::Sub:
    case VmOpcode::Xor: case VmOpcode::And: case VmOpcode::Or: case VmOpcode::Test:
    case VmOpcode::Shl: case VmOpcode::Shr: case VmOpcode::Sar: case VmOpcode::Xchg:
    case VmOpcode::Mul: case VmOpcode::Div: case VmOpcode::Adc: case VmOpcode::Sbb:
        return {2, true};
    case VmOpcode::Inc: case VmOpcode::Dec: case VmOpcode::Not: case VmOpcode::Neg:
        return {1, true};
    case VmOpcode::Jz: case VmOpcode::Jnz: case VmOpcode::Jmp: case VmOpcode::Js:
    case VmOpcode::Jns: case VmOpcode::Jb: case VmOpcode::Jbe: case VmOpcode::Ja:
    case VmOpcode::Jae: case VmOpcode::Call: case VmOpcode::Push: case VmOpcode::Pop:
        return {1, false};
    case VmOpcode::Movzx: case VmOpcode::Movsx:
        return {2, false};
    case VmOpcode::Ret: case VmOpcode::Pusha: case VmOpcode::Popa: case VmOpcode::Pushf:
    case VmOpcode::Popf: case VmOpcode::Print:
        return {0, false};
    }
    throw VmFailure("unknown RAR4 VM opcode");
}

bool isBranch(VmOpcode opcode) {
    return opcode == VmOpcode::Jz || opcode == VmOpcode::Jnz || opcode == VmOpcode::Jmp ||
           opcode == VmOpcode::Call || opcode == VmOpcode::Js || opcode == VmOpcode::Jns ||
           opcode == VmOpcode::Jb || opcode == VmOpcode::Jbe || opcode == VmOpcode::Ja ||
           opcode == VmOpcode::Jae;
}

Operand readOperand(BitReader* reader, unsigned width) {
    Operand operand;
    operand.width = width;
    if (reader->read(1)) {
        operand.kind = OperandKind::Register;
        operand.reg = reader->read(3);
        return operand;
    }
    if (reader->read(1)) {
        operand.kind = OperandKind::Memory;
        if (!reader->read(1)) {
            operand.hasRegister = true;
            operand.baseRegister = reader->read(3);
        } else {
            if (!reader->read(1)) {
                operand.hasRegister = true;
                operand.baseRegister = reader->read(3);
            }
            operand.displacement = reader->readData();
        }
        return operand;
    }
    operand.kind = OperandKind::Immediate;
    operand.value = width == 1 ? reader->read(8) : reader->readData();
    return operand;
}

std::int64_t jumpTarget(std::uint32_t value, std::size_t instructionIndex) {
    if (value <= 7) return static_cast<std::int64_t>(instructionIndex) + value;
    if (value <= 15) return static_cast<std::int64_t>(instructionIndex) + value - 16;
    if (value <= 135) return static_cast<std::int64_t>(instructionIndex) + value - 8;
    if (value <= 255) return static_cast<std::int64_t>(instructionIndex) + value - 264;
    return static_cast<std::int64_t>(value) - 256;
}

std::vector<Instruction> prepareProgram(const std::uint8_t* code, std::size_t codeSize,
                                        std::vector<std::uint8_t>* staticData) {
    if (!code || codeSize < 2 || codeSize - 1 > (std::numeric_limits<std::size_t>::max)() / 8)
        throw VmFailure("RAR4 VM bytecode is too short or too large");
    std::uint8_t checksum = 0;
    for (std::size_t i = 1; i < codeSize; ++i) checksum ^= code[i];
    if (checksum != code[0]) throw VmFailure("RAR4 VM bytecode checksum mismatch");

    BitReader reader(code + 1, codeSize - 1);
    if (reader.read(1)) {
        const std::uint32_t encodedCount = reader.readData();
        if (encodedCount == (std::numeric_limits<std::uint32_t>::max)())
            throw VmFailure("RAR4 VM static data is too large");
        const std::uint32_t count = encodedCount + 1U;
        if (count > reader.remaining() / 8)
            throw VmFailure("truncated RAR4 VM static data");
        staticData->reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) staticData->push_back(reader.readByte());
    }

    std::vector<Instruction> program;
    while (reader.remaining() >= 4) {
        const bool extended = reader.read(1) != 0;
        const int decoded = extended ? static_cast<int>(reader.read(5)) + 32 - 24
                                     : static_cast<int>(reader.read(3));
        if (decoded < 0 || decoded > static_cast<int>(VmOpcode::Print))
            throw VmFailure("invalid RAR4 VM opcode");
        Instruction instruction;
        instruction.opcode = static_cast<VmOpcode>(decoded);
        const OpcodeInfo info = opcodeInfo(instruction.opcode);
        instruction.operandCount = info.operandCount;
        instruction.width = info.byteMode && reader.read(1) ? 1U : 4U;
        for (unsigned i = 0; i < instruction.operandCount; ++i) {
            instruction.operands[i] = readOperand(&reader, instruction.width);
            if ((instruction.opcode == VmOpcode::Movzx || instruction.opcode == VmOpcode::Movsx) && i == 1)
                instruction.operands[i].width = 1;
        }
        if (isBranch(instruction.opcode) &&
            instruction.operands[0].kind == OperandKind::Immediate) {
            instruction.hasPreparedTarget = true;
            instruction.preparedTarget = jumpTarget(instruction.operands[0].value, program.size());
        }
        program.push_back(instruction);
    }
    Instruction ret;
    ret.opcode = VmOpcode::Ret;
    ret.implicit = true;
    program.push_back(ret);
    return program;
}

std::uint32_t maskFor(unsigned width) { return width == 1 ? 0xffU : 0xffffffffU; }

std::size_t effectiveAddress(const Operand& operand, const VmState& state) {
    std::uint32_t address = operand.displacement;
    if (operand.hasRegister) address += state.registers[operand.baseRegister];
    return static_cast<std::size_t>(address & static_cast<std::uint32_t>(kVmMemorySize - 1));
}

std::uint32_t readOperandValue(const Operand& operand, const VmState& state,
                               const std::uint8_t* memory) {
    if (operand.kind == OperandKind::Register)
        return state.registers[operand.reg] & maskFor(operand.width);
    if (operand.kind == OperandKind::Immediate) return operand.value & maskFor(operand.width);
    const std::size_t address = effectiveAddress(operand, state);
    if (address + operand.width > kVmMemorySize + 4U)
        throw VmFailure("RAR4 VM memory read is out of bounds");
    std::uint32_t value = memory[address];
    for (unsigned i = 1; i < operand.width; ++i)
        value |= static_cast<std::uint32_t>(memory[address + i]) << (i * 8U);
    return value;
}

void writeOperandValue(const Operand& operand, VmState* state, std::uint8_t* memory,
                       std::uint32_t value) {
    value &= maskFor(operand.width);
    if (operand.kind == OperandKind::Register) {
        if (operand.width == 1)
            state->registers[operand.reg] = (state->registers[operand.reg] & 0xffffff00U) | value;
        else
            state->registers[operand.reg] = value;
        return;
    }
    if (operand.kind != OperandKind::Memory) throw VmFailure("RAR4 VM target is not writable");
    const std::size_t address = effectiveAddress(operand, *state);
    if (address + operand.width > kVmMemorySize + 4U)
        throw VmFailure("RAR4 VM memory write is out of bounds");
    for (unsigned i = 0; i < operand.width; ++i)
        memory[address + i] = static_cast<std::uint8_t>(value >> (i * 8U));
}

void requireWritable(const Operand& operand) {
    if (operand.kind == OperandKind::Immediate) throw VmFailure("RAR4 VM target is immediate");
}

std::uint32_t readStack(std::uint8_t* memory, VmState* state) {
    const std::uint32_t sp = state->registers[7] & static_cast<std::uint32_t>(kVmMemorySize - 1);
    std::uint32_t value = 0;
    for (unsigned i = 0; i < 4; ++i)
        value |= static_cast<std::uint32_t>(memory[sp + i]) << (i * 8U);
    state->registers[7] += 4U;
    return value;
}

void writeStack(std::uint8_t* memory, VmState* state, std::uint32_t value) {
    state->registers[7] -= 4U;
    const std::uint32_t next = state->registers[7] & static_cast<std::uint32_t>(kVmMemorySize - 1);
    for (unsigned i = 0; i < 4; ++i)
        memory[next + i] = static_cast<std::uint8_t>(value >> (i * 8U));
}

std::uint32_t load32(const std::uint8_t* memory, std::size_t offset) {
    return static_cast<std::uint32_t>(memory[offset]) |
           (static_cast<std::uint32_t>(memory[offset + 1]) << 8) |
           (static_cast<std::uint32_t>(memory[offset + 2]) << 16) |
           (static_cast<std::uint32_t>(memory[offset + 3]) << 24);
}

void initializeGlobal(std::uint8_t* memory, const VmOptions& options,
                      const std::vector<std::uint8_t>& staticData) {
    const std::size_t globalCapacity = 0x2000;
    if ((options.globalDataSize && !options.globalData) ||
        options.globalDataSize > globalCapacity ||
        staticData.size() > globalCapacity - options.globalDataSize)
        throw VmFailure("RAR4 VM global data exceeds memory");
    for (std::size_t i = 0; i < options.globalDataSize; ++i)
        memory[kGlobalBase + i] = options.globalData[i];
    for (std::size_t i = 0; i < staticData.size(); ++i)
        memory[kGlobalBase + options.globalDataSize + i] = staticData[i];
}

void initializeGlobalHeader(std::uint8_t* memory, const VmState& state) {
    for (unsigned i = 0; i < 7; ++i)
        putLe32(memory + kGlobalBase + i * 4U, state.registers[i]);
    putLe32(memory + kGlobalBase + 0x1c, state.registers[4]);
    putLe32(memory + kGlobalBase + 0x20, 0);
    putLe32(memory + kGlobalBase + 0x24, state.registers[6]);
}

std::uint32_t branchTarget(const Instruction& instruction, const VmState& state,
                           const std::uint8_t* memory, std::size_t instructionCount) {
    const std::int64_t target = instruction.hasPreparedTarget
        ? instruction.preparedTarget
        : static_cast<std::int64_t>(readOperandValue(instruction.operands[0], state, memory));
    const std::uint32_t vmTarget = static_cast<std::uint32_t>(target);
    if (vmTarget >= instructionCount) return static_cast<std::uint32_t>(instructionCount);
    return vmTarget;
}

} // namespace

VmResult executeVm(const std::uint8_t* bytecode, std::size_t bytecodeSize,
                   std::uint8_t* memory, std::size_t memorySize,
                   VmState* state, const VmOptions& options) {
    VmResult result;
    if (!state || !memory || memorySize < kVmMemorySize + 4U || (!bytecode && bytecodeSize)) {
        result.error = "invalid RAR4 VM input buffer or state";
        return result;
    }
    try {
        const StandardFilter standard = identifyStandardFilter(bytecode, bytecodeSize);
        if (standard != StandardFilter::None) {
            initializeGlobal(memory, options, std::vector<std::uint8_t>());
            initializeGlobalHeader(memory, *state);
            state->registers[7] = static_cast<std::uint32_t>(kVmMemorySize);
            state->flags = 0;
            if (!executeStandardFilter(standard, memory, *state))
                throw VmFailure("RAR4 standard filter rejected its input");
            result.outputPosition = load32(memory, kGlobalBase + 0x20) &
                static_cast<std::uint32_t>(kVmMemorySize - 1);
            result.outputSize = load32(memory, kGlobalBase + 0x1c) &
                static_cast<std::uint32_t>(kVmMemorySize - 1);
            if (static_cast<std::uint64_t>(result.outputPosition) + result.outputSize >= kVmMemorySize)
                throw VmFailure("RAR4 standard filter output exceeds VM memory");
            result.success = true;
            return result;
        }
        std::vector<std::uint8_t> staticData;
        const std::vector<Instruction> program = prepareProgram(bytecode, bytecodeSize, &staticData);
        initializeGlobal(memory, options, staticData);
        initializeGlobalHeader(memory, *state);
        state->registers[7] = static_cast<std::uint32_t>(kVmMemorySize);
        state->flags = 0;
        std::size_t pc = 0;
        while (pc < program.size()) {
            if (result.operations >= kOperationLimit)
                throw VmFailure("RAR4 VM instruction limit exceeded");
            ++result.operations;
            const Instruction& instruction = program[pc];
            if (instruction.implicit) break;
            const VmOpcode op = instruction.opcode;
            const Operand* a = instruction.operandCount ? &instruction.operands[0] : nullptr;
            const Operand* b = instruction.operandCount > 1 ? &instruction.operands[1] : nullptr;
            const unsigned width = instruction.width;
            std::size_t nextPc = pc + 1;

            if (isBranch(op)) {
                bool taken = false;
                switch (op) {
                case VmOpcode::Jmp: case VmOpcode::Call: taken = true; break;
                case VmOpcode::Jz: taken = (state->flags & VmZero) != 0; break;
                case VmOpcode::Jnz: taken = (state->flags & VmZero) == 0; break;
                case VmOpcode::Js: taken = (state->flags & VmSign) != 0; break;
                case VmOpcode::Jns: taken = (state->flags & VmSign) == 0; break;
                case VmOpcode::Jb: taken = (state->flags & VmCarry) != 0; break;
                case VmOpcode::Jbe: taken = (state->flags & (VmCarry | VmZero)) != 0; break;
                case VmOpcode::Ja: taken = (state->flags & (VmCarry | VmZero)) == 0; break;
                case VmOpcode::Jae: taken = (state->flags & VmCarry) == 0; break;
                default: break;
                }
                if (taken) {
                    const std::uint32_t target = branchTarget(instruction, *state, memory, program.size());
                    if (op == VmOpcode::Call) writeStack(memory, state, static_cast<std::uint32_t>(nextPc));
                    nextPc = target;
                }
            } else {
                const std::uint32_t left = a ? readOperandValue(*a, *state, memory) : 0;
                const std::uint32_t right = b ? readOperandValue(*b, *state, memory) : 0;
                switch (op) {
                case VmOpcode::Mov:
                    requireWritable(*a);
                    writeOperandValue(*a, state, memory, right);
                    break;
                case VmOpcode::Cmp: case VmOpcode::Test: {
                    const std::uint32_t value = op == VmOpcode::Cmp ? left - right : left & right;
                    state->flags = value == 0 ? VmZero : (value > left && op == VmOpcode::Cmp ? VmCarry : 0U) |
                                                        (value & VmSign);
                    break;
                }
                case VmOpcode::Add: {
                    requireWritable(*a);
                    std::uint32_t value = left + right;
                    if (width == 1) value &= 0xffU;
                    state->flags = (value < left ? VmCarry : 0U) |
                        (value == 0 ? VmZero : (width == 1 ? (value & 0x80U ? VmSign : 0U) : value & VmSign));
                    writeOperandValue(*a, state, memory, value);
                    break;
                }
                case VmOpcode::Sub: {
                    requireWritable(*a);
                    const std::uint32_t value = left - right;
                    state->flags = value == 0 ? VmZero : (value > left ? VmCarry : 0U) | (value & VmSign);
                    writeOperandValue(*a, state, memory, value);
                    break;
                }
                case VmOpcode::Inc: case VmOpcode::Dec: {
                    requireWritable(*a);
                    std::uint32_t value = op == VmOpcode::Inc ? left + 1U : left - 1U;
                    if (op == VmOpcode::Inc && width == 1) value &= 0xffU;
                    state->flags = value == 0 ? VmZero : value & VmSign;
                    writeOperandValue(*a, state, memory, value);
                    break;
                }
                case VmOpcode::Xor: case VmOpcode::And: case VmOpcode::Or: {
                    requireWritable(*a);
                    const std::uint32_t value = op == VmOpcode::Xor ? left ^ right :
                        (op == VmOpcode::And ? left & right : left | right);
                    writeOperandValue(*a, state, memory, value);
                    state->flags = value == 0 ? VmZero : value & VmSign;
                    break;
                }
                case VmOpcode::Not:
                    requireWritable(*a);
                    writeOperandValue(*a, state, memory, ~left);
                    break;
                case VmOpcode::Neg: {
                    requireWritable(*a);
                    const std::uint32_t value = 0U - left;
                    state->flags = value == 0 ? VmZero : VmCarry | (value & VmSign);
                    writeOperandValue(*a, state, memory, value);
                    break;
                }
                case VmOpcode::Shl: case VmOpcode::Shr: case VmOpcode::Sar: {
                    requireWritable(*a);
                    if (right == 0 || right >= 32)
                        throw VmFailure("RAR4 VM shift count is undefined by the reference semantics");
                    const unsigned count = right;
                    std::uint32_t value = 0;
                    std::uint32_t carry = 0;
                    if (op == VmOpcode::Shl) {
                        value = left << count;
                        carry = (left << (count - 1U)) & VmSign;
                    } else if (op == VmOpcode::Shr) {
                        value = left >> count;
                        carry = (left >> (count - 1U)) & VmCarry;
                    } else {
                        value = left >> count;
                        if (left & VmSign) value |= 0xffffffffU << (32U - count);
                        carry = (left >> (count - 1U)) & VmCarry;
                    }
                    state->flags = (value == 0 ? VmZero : value & VmSign) | carry;
                    writeOperandValue(*a, state, memory, value);
                    break;
                }
                case VmOpcode::Push:
                    writeStack(memory, state, readOperandValue(*a, *state, memory));
                    break;
                case VmOpcode::Pop:
                    requireWritable(*a);
                    writeOperandValue(*a, state, memory, readStack(memory, state));
                    break;
                case VmOpcode::Ret:
                    if (state->registers[7] >= kVmMemorySize) {
                        nextPc = program.size();
                        break;
                    }
                    {
                        const std::uint32_t target = readStack(memory, state);
                        nextPc = target >= program.size() ? program.size() : target;
                    }
                    break;
                case VmOpcode::Pusha: {
                    const std::array<std::uint32_t, 8> saved = state->registers;
                    for (unsigned i = 0; i < saved.size(); ++i) writeStack(memory, state, saved[i]);
                    break;
                }
                case VmOpcode::Popa: {
                    const std::uint32_t stackStart = state->registers[7];
                    for (unsigned i = 0; i < 8; ++i) {
                        const std::uint32_t address = (stackStart + i * 4U) &
                            static_cast<std::uint32_t>(kVmMemorySize - 1);
                        state->registers[7U - i] = load32(memory, address);
                    }
                    break;
                }
                case VmOpcode::Pushf:
                    writeStack(memory, state, state->flags);
                    break;
                case VmOpcode::Popf:
                    state->flags = readStack(memory, state);
                    break;
                case VmOpcode::Movzx: case VmOpcode::Movsx: {
                    requireWritable(*a);
                    std::uint32_t value = right & 0xffU;
                    if (op == VmOpcode::Movsx && (value & 0x80U)) value |= 0xffffff00U;
                    writeOperandValue(*a, state, memory, value);
                    break;
                }
                case VmOpcode::Xchg:
                    requireWritable(*a);
                    requireWritable(*b);
                    writeOperandValue(*a, state, memory, right);
                    writeOperandValue(*b, state, memory, left);
                    break;
                case VmOpcode::Mul: {
                    requireWritable(*a);
                    writeOperandValue(*a, state, memory, left * right);
                    break;
                }
                case VmOpcode::Div:
                    requireWritable(*a);
                    if (right) writeOperandValue(*a, state, memory, left / right);
                    break;
                case VmOpcode::Adc: case VmOpcode::Sbb: {
                    requireWritable(*a);
                    const std::uint32_t carry = (state->flags & VmCarry) != 0;
                    std::uint32_t value = op == VmOpcode::Adc
                        ? left + right + carry : left - right - carry;
                    if (width == 1) value &= 0xffU;
                    const bool carryOut = op == VmOpcode::Adc
                        ? (value < left || (value == left && carry != 0))
                        : (value > left || (value == left && carry != 0));
                    state->flags = (carryOut ? VmCarry : 0U) |
                        (value == 0 ? VmZero : value & VmSign);
                    writeOperandValue(*a, state, memory, value);
                    break;
                }
                case VmOpcode::Print:
                    break;
                case VmOpcode::Jz: case VmOpcode::Jnz: case VmOpcode::Jmp:
                case VmOpcode::Js: case VmOpcode::Jns: case VmOpcode::Jb: case VmOpcode::Jbe:
                case VmOpcode::Ja: case VmOpcode::Jae: case VmOpcode::Call:
                    break;
                }
            }
            pc = nextPc;
        }
        result.outputPosition = load32(memory, kGlobalBase + 0x20) &
            static_cast<std::uint32_t>(kVmMemorySize - 1);
        result.outputSize = load32(memory, kGlobalBase + 0x1c) &
            static_cast<std::uint32_t>(kVmMemorySize - 1);
        if (static_cast<std::uint64_t>(result.outputPosition) + result.outputSize >= kVmMemorySize)
            result.outputPosition = result.outputSize = 0;
        result.success = true;
    } catch (const std::exception& error) {
        result.error = error.what();
    } catch (...) {
        result.error = "unexpected RAR4 VM failure";
    }
    return result;
}

VmResult executeVm(const std::vector<std::uint8_t>& bytecode,
                   std::vector<std::uint8_t>* memory, VmState* state,
                   const VmOptions& options) {
    if (!memory) {
        VmResult result;
        result.error = "invalid RAR4 VM memory buffer";
        return result;
    }
    return executeVm(bytecode.empty() ? nullptr : bytecode.data(), bytecode.size(),
                     memory->empty() ? nullptr : memory->data(), memory->size(), state, options);
}

} // namespace rar4
} // namespace peare
