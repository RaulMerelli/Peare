#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include "DeflateDecoder.h"
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs {
class path {
public:
  path() {}
  path(const char* value) : value_(value ? value : "") {}
  path(const std::string& value) : value_(value) {}
  std::string string() const { return value_; }
  path filename() const {
    const std::size_t pos = value_.find_last_of("/\\");
    return path(pos == std::string::npos ? value_ : value_.substr(pos + 1));
  }
  path parent_path() const {
    const std::size_t pos = value_.find_last_of("/\\");
    return path(pos == std::string::npos ? std::string() : value_.substr(0, pos));
  }
  path stem() const {
    const std::string name = filename().string();
    const std::size_t pos = name.find_last_of('.');
    return path(pos == std::string::npos ? name : name.substr(0, pos));
  }
private:
  std::string value_;
};
inline path operator/(const path& left, const path& right) {
  const std::string a = left.string();
  const std::string b = right.string();
  if (a.empty()) return right;
  if (b.empty()) return left;
  return path(a + "/" + b);
}
inline path operator/(const path& left, const char* right) { return left / path(right); }
inline void create_directories(const path&) {}
inline path absolute(const path& value) { return value; }
}

template <typename T>
class prosave_optional {
public:
  prosave_optional() : has_(false), value_() {}
  prosave_optional(const T& value) : has_(true), value_(value) {}
  prosave_optional& operator=(const T& value) { value_ = value; has_ = true; return *this; }
  explicit operator bool() const { return has_; }
  const T& operator*() const { return value_; }
  T& operator*() { return value_; }
  const T* operator->() const { return &value_; }
  T* operator->() { return &value_; }
  void reset() { has_ = false; value_ = T(); }
  bool operator==(const T& value) const { return has_ && value_ == value; }
private:
  bool has_;
  T value_;
};

namespace {

constexpr std::uint32_t kFooterMagic = 50534808U; // 0x03031998
constexpr std::size_t kFooterSize = 64;
constexpr std::size_t kSectionRecordSize = 42;
constexpr std::size_t kChunkHeaderSize = 8;
constexpr std::size_t kMaxChunkOutput = 0x4000;
constexpr std::uint16_t kCrcPolynomial = 0x8408;

struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

std::uint16_t read_u16(const std::uint8_t* p) {
  return static_cast<std::uint16_t>(p[0]) |
         (static_cast<std::uint16_t>(p[1]) << 8U);
}

std::uint32_t read_u32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8U) |
         (static_cast<std::uint32_t>(p[2]) << 16U) |
         (static_cast<std::uint32_t>(p[3]) << 24U);
}

void write_u16(std::vector<std::uint8_t>& out, std::size_t off,
               std::uint16_t value) {
  out.at(off) = static_cast<std::uint8_t>(value);
  out.at(off + 1) = static_cast<std::uint8_t>(value >> 8U);
}

void write_u32(std::vector<std::uint8_t>& out, std::size_t off,
               std::uint32_t value) {
  out.at(off) = static_cast<std::uint8_t>(value);
  out.at(off + 1) = static_cast<std::uint8_t>(value >> 8U);
  out.at(off + 2) = static_cast<std::uint8_t>(value >> 16U);
  out.at(off + 3) = static_cast<std::uint8_t>(value >> 24U);
}

std::string hex32(std::uint32_t value) {
  std::ostringstream os;
  os << "0x" << std::uppercase << std::hex << std::setw(8)
     << std::setfill('0') << value;
  return os.str();
}

std::vector<std::uint8_t> read_file(const fs::path& path) {
  std::ifstream in(path.string().c_str(), std::ios::binary);
  if (!in) {
    throw Error("cannot open input: " + path.string());
  }
  in.seekg(0, std::ios::end);
  const auto end = in.tellg();
  if (end < 0) {
    throw Error("cannot determine input size");
  }
  const auto size = static_cast<std::uint64_t>(end);
  if (size > std::numeric_limits<std::size_t>::max()) {
    throw Error("input is too large for this build");
  }
  std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
  in.seekg(0, std::ios::beg);
  if (!data.empty() &&
      !in.read(reinterpret_cast<char*>(data.data()),
               static_cast<std::streamsize>(data.size()))) {
    throw Error("cannot read complete input");
  }
  return data;
}

void write_file(const fs::path& path, const std::vector<std::uint8_t>& data) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path.string().c_str(), std::ios::binary | std::ios::trunc);
  if (!out) {
    throw Error("cannot create output: " + path.string());
  }
  if (!data.empty()) {
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
  }
  if (!out) {
    throw Error("cannot write output: " + path.string());
  }
}

void write_text(const fs::path& path, const std::string& text) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path.string().c_str(), std::ios::binary | std::ios::trunc);
  if (!out || !(out << text)) {
    throw Error("cannot write output: " + path.string());
  }
}

struct Footer {
  std::uint16_t section_count{};
  std::uint32_t metadata_offset{};
  std::uint32_t auxiliary_offset{};
  std::array<std::uint8_t, kFooterSize> raw{};
};

Footer parse_footer(const std::vector<std::uint8_t>& image) {
  if (image.size() < kFooterSize) {
    throw Error("input is shorter than the 64-byte ProSave footer");
  }
  const std::size_t off = image.size() - kFooterSize;
  if (read_u32(image.data() + image.size() - 4) != kFooterMagic) {
    throw Error("invalid footer magic (expected 0x03031998)");
  }

  Footer footer;
  std::copy_n(image.data() + off, kFooterSize, footer.raw.data());
  footer.section_count = read_u16(footer.raw.data());
  footer.metadata_offset = read_u32(footer.raw.data() + 2);
  footer.auxiliary_offset = read_u32(footer.raw.data() + 42);

  const std::uint64_t metadata_end =
      static_cast<std::uint64_t>(footer.metadata_offset) +
      static_cast<std::uint64_t>(footer.section_count) * kSectionRecordSize;
  if (footer.metadata_offset > off || metadata_end > off) {
    throw Error("section table points outside the pre-footer area");
  }
  if (footer.auxiliary_offset != 0 && footer.auxiliary_offset > off) {
    throw Error("auxiliary metadata offset points outside the pre-footer area");
  }
  return footer;
}

struct Section {
  std::size_t index{};
  std::uint32_t bitmask{};
  std::uint32_t file_offset{};
  std::uint32_t decompressed_size{};
  std::uint32_t start_offset{};
  std::uint16_t data_crc{};
  std::uint32_t stored_size{};
  std::uint16_t stream_crc{};
  std::array<std::uint8_t, kSectionRecordSize> raw{};
};

std::vector<Section> parse_sections(const std::vector<std::uint8_t>& image,
                                    const Footer& footer) {
  std::vector<Section> sections;
  sections.reserve(footer.section_count);
  for (std::size_t i = 0; i < footer.section_count; ++i) {
    const std::size_t off = footer.metadata_offset + i * kSectionRecordSize;
    Section section;
    section.index = i;
    std::copy_n(image.data() + off, kSectionRecordSize, section.raw.data());
    section.bitmask = read_u32(section.raw.data());
    section.file_offset = read_u32(section.raw.data() + 4);
    section.decompressed_size = read_u32(section.raw.data() + 8);
    section.start_offset = read_u32(section.raw.data() + 12);
    section.data_crc = read_u16(section.raw.data() + 16);
    section.stored_size = read_u32(section.raw.data() + 18);
    section.stream_crc = read_u16(section.raw.data() + 22);

    const std::uint64_t end = static_cast<std::uint64_t>(section.file_offset) +
                              section.stored_size;
    if (section.file_offset > image.size() || end > image.size()) {
      throw Error("section " + std::to_string(i) +
                  " points outside the input file");
    }
    sections.push_back(section);
  }
  return sections;
}

std::array<std::uint16_t, 256> make_crc_table(std::uint16_t polynomial) {
  std::array<std::uint16_t, 256> table{};
  for (std::uint32_t i = 0; i < table.size(); ++i) {
    std::uint16_t value = static_cast<std::uint16_t>(i);
    for (int bit = 0; bit < 8; ++bit) {
      value = static_cast<std::uint16_t>(
          (value >> 1U) ^ ((value & 1U) ? polynomial : 0U));
    }
    table[i] = value;
  }
  return table;
}

std::uint16_t crc_update(const std::uint8_t* data, std::size_t size,
                         std::uint16_t crc,
                         const std::array<std::uint16_t, 256>& table) {
  for (std::size_t i = 0; i < size; ++i) {
    crc = static_cast<std::uint16_t>(
        table[static_cast<std::uint8_t>(crc ^ data[i])] ^ (crc >> 8U));
  }
  return crc;
}

struct ChunkProbe {
  ChunkProbe() : payload(nullptr), payload_size(0), expected_crc(0) {}
  ChunkProbe(const std::uint8_t* p, std::size_t size, std::uint16_t crc)
      : payload(p), payload_size(size), expected_crc(crc) {}
  const std::uint8_t* payload;
  std::size_t payload_size;
  std::uint16_t expected_crc;
};

std::vector<std::uint16_t> infer_crc_polynomials(const ChunkProbe& probe) {
  std::vector<std::uint16_t> matches;
  // sub_10005C50 exposes the table update form, but the decompilation does
  // not include the table initializer. Do not impose polynomial bit-shape
  // assumptions that are not present in the decompiled code.
  for (std::uint32_t candidate = 1; candidate <= 0xFFFF; ++candidate) {
    const auto table = make_crc_table(static_cast<std::uint16_t>(candidate));
    if (crc_update(probe.payload, probe.payload_size, 0xFFFF, table) ==
        probe.expected_crc) {
      matches.push_back(static_cast<std::uint16_t>(candidate));
    }
  }
  return matches;
}

std::vector<std::uint8_t> lzss_decompress(const std::uint8_t* input,
                                          std::size_t input_size) {
  std::vector<std::uint8_t> output;
  output.reserve(kMaxChunkOutput);
  std::size_t pos = 0;
  int bits_left = 1;
  std::uint8_t flags = 0;

  while (pos < input_size) {
    if (--bits_left == 0) {
      flags = input[pos++];
      bits_left = 8;
    }

    while ((flags & 0x80U) != 0) {
      flags = static_cast<std::uint8_t>(flags << 1U);
      if (pos + 2 > input_size) {
        throw Error("truncated LZSS back-reference");
      }
      const std::uint16_t word = read_u16(input + pos);
      pos += 2;
      const std::size_t distance = word & 0x0FFFU;
      std::size_t length = (word >> 12U) + 2U;
      if (length == 17U) {
        if (pos >= input_size) {
          throw Error("truncated LZSS extended length");
        }
        // Exact sub_10017CD0 behavior: replacement, not 17 + extension.
        length = input[pos++];
      }
      if (distance == 0 || distance > output.size()) {
        throw Error("invalid LZSS back-reference distance");
      }
      if (output.size() + length > kMaxChunkOutput) {
        throw Error("LZSS chunk exceeds ProSave's 0x4000-byte buffer");
      }
      for (std::size_t i = 0; i < length; ++i) {
        output.push_back(output[output.size() - distance]);
      }
      if (pos >= input_size) {
        return output;
      }
      if (--bits_left == 0) {
        flags = input[pos++];
        bits_left = 8;
      }
    }

    flags = static_cast<std::uint8_t>(flags << 1U);
    if (pos >= input_size) {
      throw Error("truncated LZSS literal");
    }
    if (output.size() == kMaxChunkOutput) {
      throw Error("LZSS chunk exceeds ProSave's 0x4000-byte buffer");
    }
    output.push_back(input[pos++]);
  }
  return output;
}

struct ExtractContext {
  bool verify_crc{true};
  prosave_optional<std::uint16_t> polynomial{kCrcPolynomial};
  std::vector<std::uint16_t> candidates;
  bool candidates_initialized{false};
  bool allow_polynomial_fallback{true};
};

struct DecodedLayers {
  std::vector<std::uint8_t> final_data;
  std::vector<std::vector<std::uint8_t>> nested;
};

void constrain_crc(ExtractContext& context, const std::uint8_t* data,
                   std::size_t size, std::uint16_t expected_crc) {
  if (!context.verify_crc) {
    return;
  }
  if (context.polynomial) {
    const auto table = make_crc_table(*context.polynomial);
    if (crc_update(data, size, 0xFFFF, table) != expected_crc) {
      if (!context.allow_polynomial_fallback) {
        throw Error("CRC mismatch");
      }
      context.polynomial.reset();
      context.candidates.clear();
      context.candidates_initialized = false;
    } else {
      context.allow_polynomial_fallback = false;
      return;
    }
  }

  if (!context.candidates_initialized) {
    context.candidates =
        infer_crc_polynomials(ChunkProbe{data, size, expected_crc});
    context.candidates_initialized = true;
  } else {
    context.candidates.erase(
        std::remove_if(
            context.candidates.begin(), context.candidates.end(),
            [&](std::uint16_t candidate) {
              const auto table = make_crc_table(candidate);
              return crc_update(data, size, 0xFFFF, table) != expected_crc;
            }),
        context.candidates.end());
  }
  if (context.candidates.empty()) {
    throw Error("CRC table could not be reconstructed from the IMG checksums");
  }
  if (context.candidates.size() == 1) {
    context.polynomial = context.candidates.front();
    context.allow_polynomial_fallback = false;
  }
}

prosave_optional<std::vector<std::uint8_t>> try_decode_chunk_stream(
    const std::vector<std::uint8_t>& input, ExtractContext& context) {
  if (input.size() < kChunkHeaderSize) {
    return {};
  }

  ExtractContext trial = context;
  std::vector<std::uint8_t> output;
  output.reserve(input.size());
  std::size_t pos = 0;
  std::size_t chunk_count = 0;

  try {
    while (pos < input.size()) {
      if (input.size() - pos < kChunkHeaderSize) {
        return {};
      }
      const std::uint8_t* header = input.data() + pos;
      const std::uint16_t packed_size = read_u16(header);
      const std::uint16_t unpacked_size = read_u16(header + 2);
      const std::uint16_t packed_crc = read_u16(header + 4);
      const std::uint16_t unpacked_crc = read_u16(header + 6);
      if (packed_size == 0 || unpacked_size == 0 ||
          packed_size > kMaxChunkOutput ||
          unpacked_size > kMaxChunkOutput ||
          packed_size > input.size() - pos - kChunkHeaderSize) {
        return {};
      }

      const std::uint8_t* payload = header + kChunkHeaderSize;
      constrain_crc(trial, payload, packed_size, packed_crc);

      std::vector<std::uint8_t> chunk;
      if (packed_size == unpacked_size) {
        chunk.assign(payload, payload + packed_size);
      } else {
        chunk = lzss_decompress(payload, packed_size);
      }
      if (chunk.size() != unpacked_size) {
        return {};
      }
      constrain_crc(trial, chunk.data(), chunk.size(), unpacked_crc);
      output.insert(output.end(), chunk.begin(), chunk.end());
      pos += kChunkHeaderSize + packed_size;
      ++chunk_count;
    }
  } catch (const Error&) {
    return {};
  }

  if (chunk_count == 0 || pos != input.size()) {
    return {};
  }
  context = std::move(trial);
  return output;
}

DecodedLayers decode_nested_layers(std::vector<std::uint8_t> data,
                                   ExtractContext& context) {
  DecodedLayers result;
  for (std::size_t depth = 0; depth < 32; ++depth) {
    auto decoded = try_decode_chunk_stream(data, context);
    if (!decoded) {
      result.final_data = std::move(data);
      return result;
    }
    data = std::move(*decoded);
    result.nested.push_back(data);
  }
  throw Error("nested ProSave chunk depth exceeds 32 levels");
}

std::vector<std::uint8_t> extract_section(
    const std::vector<std::uint8_t>& image, const Section& section,
    ExtractContext& context) {
  std::vector<std::uint8_t> output;
  output.reserve(section.decompressed_size);

  std::size_t pos = section.file_offset;
  std::uint32_t remaining = section.stored_size;
  while (remaining != 0) {
    if (remaining < kChunkHeaderSize || pos + kChunkHeaderSize > image.size()) {
      throw Error("truncated chunk header in section " +
                  std::to_string(section.index));
    }
    const std::uint8_t* header = image.data() + pos;
    const std::uint16_t packed_size = read_u16(header);
    const std::uint16_t unpacked_hint = read_u16(header + 2);
    const std::uint16_t packed_crc = read_u16(header + 4);
    const std::uint16_t unpacked_crc = read_u16(header + 6);
    const std::uint32_t record_size = kChunkHeaderSize + packed_size;
    if (record_size > remaining || pos + record_size > image.size()) {
      throw Error("truncated chunk payload in section " +
                  std::to_string(section.index));
    }
    const std::uint8_t* payload = header + kChunkHeaderSize;

    try {
      constrain_crc(context, payload, packed_size, packed_crc);
    } catch (const Error&) {
      throw Error("compressed CRC mismatch in section " +
                  std::to_string(section.index));
    }

    std::vector<std::uint8_t> chunk;
    if (packed_size == unpacked_hint) {
      chunk.assign(payload, payload + packed_size);
    } else {
      chunk = lzss_decompress(payload, packed_size);
    }

    try {
      constrain_crc(context, chunk.data(), chunk.size(), unpacked_crc);
    } catch (const Error&) {
      throw Error("decompressed CRC mismatch in section " +
                  std::to_string(section.index));
    }

    output.insert(output.end(), chunk.begin(), chunk.end());
    pos += record_size;
    remaining -= record_size;
  }

  if (output.size() != section.decompressed_size) {
    throw Error("decompressed size mismatch in section " +
                std::to_string(section.index) + ": expected " +
                std::to_string(section.decompressed_size) + ", got " +
                std::to_string(output.size()));
  }
  if (context.verify_crc) {
    try {
      constrain_crc(context, output.data(), output.size(), section.data_crc);
      constrain_crc(context, image.data() + section.file_offset,
                    section.stored_size, section.stream_crc);
    } catch (const Error&) {
      throw Error("section-level CRC mismatch in section " +
                  std::to_string(section.index));
    }
  }
  return output;
}

std::string section_filename(const Section& section) {
  std::ostringstream os;
  os << "section_" << std::setw(3) << std::setfill('0') << section.index
     << "_mask_" << std::uppercase << std::hex << std::setw(8)
     << section.bitmask << ".bin";
  return os.str();
}

void write_named_payloads(const fs::path& root, const Section& section,
                          const std::vector<std::uint8_t>& data) {
  std::ostringstream mapped_name;
  mapped_name << "address_" << std::uppercase << std::hex << std::setw(8)
              << std::setfill('0') << section.start_offset << "_mask_"
              << std::setw(8) << section.bitmask << ".bin";
  write_file(root / "prosave" / "flash_map" / mapped_name.str(), data);

  switch (section.bitmask) {
    case 0x00001000U:
      write_file(root / "prosave" / "ftp" / "FTPRoot" / "Upload" /
                     "partition_info.nb0",
                 data);
      write_file(root / "prosave" / "lowlevel" / "MBR", data);
      break;
    case 0x00000001U:
      write_file(root / "prosave" / "ftp" / "FTPRoot" / "Upload" /
                     "nk.nb0",
                 data);
      write_file(root / "prosave" / "lowlevel" / "FlashBoot" / "nk.nb0",
                 data);
      break;
    case 0x20000000U:
      write_file(root / "prosave" / "ftp" / "FlashBoot" / "nk.nb0.desc",
                 data);
      write_file(root / "prosave" / "lowlevel" / "FlashBoot" /
                     "nk.nb0.desc",
                 data);
      break;
    default:
      break;
  }
}

prosave_optional<std::size_t> find_section_by_mask(
    const std::vector<Section>& sections, std::uint32_t mask) {
  for (std::size_t i = 0; i < sections.size(); ++i) {
    if (sections[i].bitmask == mask) {
      return i;
    }
  }
  return {};
}

struct FlashRegion {
  FlashRegion() : base(0), payload_index(0) {}
  FlashRegion(std::uint32_t value, std::size_t index)
      : base(value), payload_index(index) {}
  std::uint32_t base;
  std::size_t payload_index;
};

bool has_zlib_header(const std::vector<std::uint8_t>& data,
                     std::size_t offset = 0) {
  if (offset + 2 > data.size()) {
    return false;
  }
  const unsigned cmf = data[offset];
  const unsigned flg = data[offset + 1];
  return (cmf & 0x0FU) == 8U && (cmf >> 4U) <= 7U &&
         ((cmf << 8U) + flg) % 31U == 0U && (flg & 0x20U) == 0U;
}

std::vector<std::uint8_t> inflate_zlib(
    const std::vector<std::uint8_t>& input,
    prosave_optional<std::size_t> expected_size = {}) {
  if (!has_zlib_header(input)) {
    throw Error("invalid zlib header");
  }
  const std::size_t limit = expected_size ? *expected_size
                                          : std::numeric_limits<std::size_t>::max();
  std::vector<std::uint8_t> output;
  std::string inflate_error;
  if (!peare::compression::inflateZlib(input, limit, &output, &inflate_error))
    throw Error("zlib stream is truncated or invalid");
  if (expected_size && output.size() != *expected_size) {
      throw Error("zlib output does not match descriptor size");
    }
  return output;
}

void write_reconstructed_images(
    const fs::path& root, const std::vector<Section>& sections,
    const std::vector<std::vector<std::uint8_t>>& payloads) {
  // The ProSave mask mapping table is consumed by sub_10018ED0,
  // sub_10018FC0 and sub_100191A0. The aggregate-mask branches in the
  // update path expose all five data/descriptor combinations.
  const std::array<std::pair<std::uint32_t, std::uint32_t>, 5> pairs{{
      {0x00000001U, 0x20000000U},
      {0x00000002U, 0x40000000U},
      {0x00000040U, 0x00000080U},
      {0x00000200U, 0x00000400U},
      {0x00000800U, 0x10000000U},
  }};
  std::vector<FlashRegion> regions;
  // Overrides used when a payload was zlib-inflated to match descriptor size.
  std::map<std::size_t, std::vector<std::uint8_t>> inflated_overrides;
  auto resolve_payload =
      [&](std::size_t idx) -> const std::vector<std::uint8_t>& {
    auto it = inflated_overrides.find(idx);
    return it != inflated_overrides.end() ? it->second : payloads.at(idx);
  };

  for (const auto& mask_pair : pairs) {
    const std::uint32_t data_mask = mask_pair.first;
    const std::uint32_t descriptor_mask = mask_pair.second;
    const auto data_index = find_section_by_mask(sections, data_mask);
    const auto descriptor_index =
        find_section_by_mask(sections, descriptor_mask);
    if (!data_index && !descriptor_index) {
      continue;
    }
    if (!data_index || !descriptor_index) {
      throw Error("incomplete ProSave data/descriptor section pair");
    }
    const auto& descriptor = payloads.at(*descriptor_index);
    if (descriptor.size() != 64 || read_u32(descriptor.data()) != 0x19101998U) {
      throw Error("invalid 64-byte ProSave image descriptor");
    }
    const std::uint32_t base = read_u32(descriptor.data() + 6);
    const std::uint32_t size = read_u32(descriptor.data() + 14);
    const auto& payload_ref = payloads.at(*data_index);
    if (size != payload_ref.size()) {
      // Some V1 ce00 / V12 277F-W2 / TP177B 4" images store the NK section as
      // a single zlib (deflate) stream whose inflated length matches the
      // descriptor. ProSave's transferred-payload path (sub_1001E9D0) feeds
      // such streams through the standard inflate before writing to flash.
      if (has_zlib_header(payload_ref)) {
        inflated_overrides.emplace(
            *data_index, inflate_zlib(payload_ref, size));
      } else {
        throw Error("descriptor size does not match fully decoded payload");
      }
    }
    regions.push_back(FlashRegion{base, *data_index});
  }

  if (regions.empty()) {
    return;
  }
  std::sort(regions.begin(), regions.end(),
            [](const FlashRegion& a, const FlashRegion& b) {
              return a.base < b.base;
            });
  const std::uint64_t image_base = regions.front().base;
  std::uint64_t image_end = image_base;
  for (const auto& region : regions) {
    image_end = std::max(
        image_end, static_cast<std::uint64_t>(region.base) +
                       resolve_payload(region.payload_index).size());
  }
  if (image_end - image_base > std::numeric_limits<std::size_t>::max()) {
    throw Error("reconstructed flash image is too large");
  }

  std::vector<std::uint8_t> flash(
      static_cast<std::size_t>(image_end - image_base), 0xFF);
  std::uint64_t previous_end = image_base;
  for (const auto& region : regions) {
    const auto& payload = resolve_payload(region.payload_index);
    if (region.base < previous_end) {
      throw Error("overlapping ProSave flash regions");
    }
    const std::size_t offset =
        static_cast<std::size_t>(region.base - image_base);
    std::copy(payload.begin(), payload.end(), flash.begin() + offset);
    previous_end = static_cast<std::uint64_t>(region.base) + payload.size();
  }

  std::ostringstream name;
  name << "flash_base_" << std::uppercase << std::hex << std::setw(8)
       << std::setfill('0') << image_base << ".nb0";
  write_file(root / "reconstructed" / name.str(), flash);

  // Several Siemens panels leave the ROMHDR file-offset field at ECEC+8 as
  // zero because the on-device loader uses only the virtual pointer at
  // ECEC+4. Walkers that resolve the ROMHDR via file offsets (notably
  // every off-device WinCE ROM scanner) need it populated. We compute the
  // file offset from the virtual pointer minus the image base and write it
  // in. This does not modify any code bytes; only an unused header field.
  for (std::size_t off = 0; off + 12 <= flash.size(); ++off) {
    if (read_u32(flash.data() + off) != 0x43454345U ||
        read_u32(flash.data() + off + 8) != 0) {
      continue;
    }
    const std::uint32_t pointer = read_u32(flash.data() + off + 4);
    if (pointer < image_base || pointer >= image_end) {
      continue;
    }
    write_u32(flash, off + 8,
              static_cast<std::uint32_t>(pointer - image_base));
  }
  write_file(root / "reconstructed" / "NK.bin", flash);
}

std::string build_manifest(const fs::path& input, const Footer& footer,
                           const std::vector<Section>& sections,
                           const ExtractContext& context,
                           const std::vector<std::size_t>& final_sizes,
                           const std::vector<std::size_t>& nested_layers) {
  std::ostringstream os;
  os << "{\n"
     << "  \"source\": \"" << input.filename().string() << "\",\n"
     << "  \"footer_magic\": \"0x03031998\",\n"
     << "  \"section_count\": " << footer.section_count << ",\n"
     << "  \"metadata_offset\": " << footer.metadata_offset << ",\n"
     << "  \"auxiliary_offset\": " << footer.auxiliary_offset << ",\n"
     << "  \"crc_verification\": "
     << (context.verify_crc ? "true" : "false") << ",\n";
  if (context.polynomial) {
    os << "  \"crc_polynomial\": \"0x" << std::uppercase << std::hex
       << std::setw(4) << std::setfill('0') << *context.polynomial
       << std::dec << "\",\n";
  }
  os << "  \"sections\": [\n";
  for (std::size_t i = 0; i < sections.size(); ++i) {
    const auto& s = sections[i];
    os << "    {\"index\": " << s.index << ", \"bitmask\": \""
       << hex32(s.bitmask) << "\", \"file_offset\": " << s.file_offset
       << ", \"decompressed_size\": " << s.decompressed_size
       << ", \"final_size\": " << final_sizes.at(i)
       << ", \"nested_chunk_layers\": " << nested_layers.at(i)
       << ", \"start_offset\": " << s.start_offset
       << ", \"stored_size\": " << s.stored_size << "}";
    os << (i + 1 == sections.size() ? "\n" : ",\n");
  }
  os << "  ]\n}\n";
  return os.str();
}

std::size_t common_file_payload_end(const std::vector<std::uint8_t>& image,
                                    const Footer& footer) {
  const std::size_t footer_begin = image.size() - kFooterSize;
  const std::size_t payload_end =
      footer.auxiliary_offset != 0 ? footer.auxiliary_offset : footer_begin;
  if (payload_end == 0 || payload_end > footer_begin) {
    throw Error("invalid ProSave common-file payload boundary");
  }
  return payload_end;
}

void extract_smart_common_file(const fs::path& input, const fs::path& output,
                               const std::vector<std::uint8_t>& image,
                               const Footer& footer) {
  const std::size_t payload_end = common_file_payload_end(image, footer);

  // ps_osupdate_smart passes zero-section IMG files to ImageTransfer as the
  // CommonFile itself. Saving the equivalent transfer payload therefore only
  // removes the ProSave-owned auxiliary data and footer.
  std::vector<std::uint8_t> payload(image.begin(), image.begin() + payload_end);
  write_file(output / "reconstructed" / "NK.bin", payload);

  std::ostringstream manifest;
  manifest << "{\n"
           << "  \"source\": \"" << input.filename().string() << "\",\n"
           << "  \"footer_magic\": \"0x03031998\",\n"
           << "  \"layout\": \"smart-common-file\",\n"
           << "  \"section_count\": 0,\n"
           << "  \"auxiliary_offset\": " << footer.auxiliary_offset << ",\n"
           << "  \"output_size\": " << payload.size() << "\n"
           << "}\n";
  write_text(output / "manifest.json", manifest.str());
  std::cout << "common-file payload -> " << payload.size() << " bytes\n";
}

void extract_image(const fs::path& input, const fs::path& output,
                   bool verify_crc) {
  const auto image = read_file(input);
  const Footer footer = parse_footer(image);
  const auto sections = parse_sections(image, footer);
  ExtractContext context;
  context.verify_crc = verify_crc;
  std::vector<std::size_t> final_sizes;
  std::vector<std::size_t> nested_layer_counts;
  std::vector<std::vector<std::uint8_t>> payloads;
  final_sizes.reserve(sections.size());
  nested_layer_counts.reserve(sections.size());
  payloads.reserve(sections.size());

  fs::create_directories(output);
  write_file(output / "metadata" / "footer.bin",
             std::vector<std::uint8_t>(footer.raw.begin(), footer.raw.end()));

  const std::size_t table_begin = footer.metadata_offset;
  const std::size_t table_end =
      table_begin + sections.size() * kSectionRecordSize;
  write_file(output / "metadata" / "section_table.bin",
             std::vector<std::uint8_t>(image.begin() + table_begin,
                                       image.begin() + table_end));

  const std::size_t footer_begin = image.size() - kFooterSize;
  if (footer.auxiliary_offset != 0 && footer.auxiliary_offset < footer_begin) {
    write_file(output / "metadata" / "auxiliary.bin",
               std::vector<std::uint8_t>(image.begin() + footer.auxiliary_offset,
                                         image.begin() + footer_begin));
  }

  if (sections.empty()) {
    extract_smart_common_file(input, output, image, footer);
    return;
  }

  for (const auto& section : sections) {
    auto data = extract_section(image, section, context);
    write_file(output / "layers" /
                   (section_filename(section) + ".layer_1.bin"),
               data);
    auto decoded = decode_nested_layers(std::move(data), context);
    for (std::size_t i = 0; i < decoded.nested.size(); ++i) {
      write_file(output / "layers" /
                     (section_filename(section) + ".layer_" +
                      std::to_string(i + 2) + ".bin"),
                 decoded.nested[i]);
    }
    final_sizes.push_back(decoded.final_data.size());
    nested_layer_counts.push_back(decoded.nested.size());
    write_file(output / "sections" / section_filename(section),
               decoded.final_data);
    write_named_payloads(output, section, decoded.final_data);
    payloads.push_back(decoded.final_data);
    std::cout << "section " << section.index << " mask="
              << hex32(section.bitmask) << " -> "
              << decoded.final_data.size() << " bytes ("
              << decoded.nested.size() << " nested layer(s))\n";
  }

  write_reconstructed_images(output, sections, payloads);

  write_text(output / "manifest.json",
             build_manifest(input, footer, sections, context, final_sizes,
                            nested_layer_counts));
  if (context.polynomial) {
    std::cout << "CRC polynomial: 0x" << std::uppercase << std::hex
              << std::setw(4) << std::setfill('0') << *context.polynomial
              << std::dec << "\n";
  }

}

} // namespace
