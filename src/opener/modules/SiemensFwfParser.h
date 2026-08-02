#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <map>
#include "EmbeddedZlibInflate.h"
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

namespace {

struct Error : std::runtime_error {
  using std::runtime_error::runtime_error;
};

std::uint32_t read_u32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8U) |
         (static_cast<std::uint32_t>(p[2]) << 16U) |
         (static_cast<std::uint32_t>(p[3]) << 24U);
}

std::string hex32(std::uint32_t value) {
  std::ostringstream os;
  os << "0x" << std::uppercase << std::hex << std::setw(8)
     << std::setfill('0') << value;
  return os.str();
}

bool has_zlib_header(const std::vector<std::uint8_t>& data,
                     std::size_t offset = 0) {
  if (offset + 2 > data.size()) return false;
  const unsigned cmf = data[offset];
  const unsigned flg = data[offset + 1];
  return (cmf & 0x0FU) == 8U && (cmf >> 4U) <= 7U &&
         ((cmf << 8U) + flg) % 31U == 0U && (flg & 0x20U) == 0U;
}

std::vector<std::uint8_t> inflate_zlib(
    const std::vector<std::uint8_t>& input,
    prosave_optional<std::size_t> expected_size = {}) {
  if (!has_zlib_header(input)) throw Error("invalid zlib header");
  try {
    const std::size_t limit = expected_size ? *expected_size
                                            : std::numeric_limits<std::size_t>::max();
    std::vector<std::uint8_t> output = prosave_embedded::inflateZlib(input, limit);
    if (expected_size && output.size() != *expected_size)
      throw Error("zlib output does not match descriptor size");
    return output;
  } catch (const prosave_embedded::InflateError&) {
    throw Error("zlib stream is truncated or invalid");
  }
}

enum class FwfPayloadKind { Empty, Zlib, Fsf, Raw, Oms, FlashImage };

struct FwfBlob {
  std::size_t index{};
  std::size_t attr_offset{};     // offset of the A3 blob attribute record
  std::size_t payload_offset{};  // offset of the blob bytes in the stream
  std::size_t payload_size{};
  std::size_t aid{};             // OMS attribute id of the blob field
  std::string name;              // lookback name (nk.nb0.N / ImagePart.N / ...)
  std::string class_name;        // enclosing object name (aid 233)
  FwfPayloadKind kind{FwfPayloadKind::Raw};
  std::vector<std::uint8_t> payload;
  std::vector<std::uint8_t> inflated;  // only for zlib
  std::size_t nk_output_offset{static_cast<std::size_t>(-1)};
};

struct FwfDecoded {
  std::vector<std::uint8_t> nk_image;
  std::vector<FwfBlob> blobs;
  std::size_t nk_part_count{};
  bool has_flash{false};
  bool has_bootloader{false};
  bool has_flash_image{false};
  std::size_t unclassified_bytes{};
};

std::vector<std::size_t> find_bytes(const std::vector<std::uint8_t>& data,
                                    const std::uint8_t* needle,
                                    std::size_t needle_size) {
  std::vector<std::size_t> positions;
  if (needle_size == 0 || needle_size > data.size()) {
    return positions;
  }
  auto it = data.begin();
  while (it != data.end()) {
    it = std::search(it, data.end(), needle, needle + needle_size);
    if (it == data.end()) {
      break;
    }
    positions.push_back(static_cast<std::size_t>(it - data.begin()));
    ++it;
  }
  return positions;
}

// OMS Serializer variable-length unsigned integer: successive bytes contribute
// 7 value bits (big-endian accumulation); high bit set means more bytes follow.
std::pair<std::uint32_t, std::size_t> decode_oms_uint(
    const std::vector<std::uint8_t>& data, std::size_t off) {
  std::uint32_t value = 0;
  std::size_t start = off;
  while (off < data.size()) {
    const std::uint8_t b = data[off++];
    value = (value << 7) | (b & 0x7FU);
    if ((b & 0x80U) == 0) {
      return {value, off};
    }
    if (off - start > 5) {
      throw Error("OMS uint exceeds maximum encoded length");
    }
  }
  throw Error("truncated OMS uint");
}

std::string fwf_lookback_name(const std::vector<std::uint8_t>& fwf,
                              std::size_t marker_offset) {
  // Class instance names (nk.nb0.N / ImagePart.N) are serialized immediately
  // before the InPlaceBlobStreamed marker. Pick the match closest to the
  // marker so a previous nk.nb0 name does not shadow ImagePart.2.
  const std::size_t begin =
      marker_offset > 200 ? marker_offset - 200 : 0;
  const std::string window(
      reinterpret_cast<const char*>(fwf.data() + begin),
      marker_offset - begin);

  const char* patterns[] = {"nk.nb0.", "ImagePart.", "Kernel-"};
  std::size_t best_pos = std::string::npos;
  std::string best_name;
  for (const char* pattern : patterns) {
    std::size_t search = 0;
    while (search < window.size()) {
      const auto pos = window.find(pattern, search);
      if (pos == std::string::npos) {
        break;
      }
      std::size_t end = pos;
      while (end < window.size()) {
        const unsigned char c = static_cast<unsigned char>(window[end]);
        if (!(std::isalnum(c) || c == '.' || c == '-' || c == '_')) {
          break;
        }
        ++end;
      }
      if (best_pos == std::string::npos || pos >= best_pos) {
        best_pos = pos;
        best_name = window.substr(pos, end - pos);
      }
      search = pos + 1;
    }
  }
  return best_name;
}

const char* fwf_kind_name(FwfPayloadKind kind) {
  switch (kind) {
    case FwfPayloadKind::Empty:
      return "empty";
    case FwfPayloadKind::Zlib:
      return "zlib";
    case FwfPayloadKind::Fsf:
      return "fsf";
    case FwfPayloadKind::Oms:
      return "oms";
    case FwfPayloadKind::FlashImage:
      return "flash_image";
    case FwfPayloadKind::Raw:
    default:
      return "raw";
  }
}

bool is_nk_part_name(const std::string& name) {
  return name.rfind("nk.nb0.", 0) == 0;
}

bool is_image_part_name(const std::string& name) {
  return name.rfind("ImagePart.", 0) == 0;
}

// --- OMS+ stream primitives (grammar recovered from OsOms.dll; see
// docs/FWF_OMS_FORMAT.md). All ids and lengths use the OMS 7-bit big-endian
// varint; scalar values are big-endian. ---

// Non-throwing OMS varint (up to 8 groups, 64-bit accumulator).
bool read_oms_uint(const std::vector<std::uint8_t>& d, std::size_t off,
                   std::uint64_t& value, std::size_t& next) {
  std::uint64_t v = 0;
  std::size_t n = 0;
  while (off + n < d.size()) {
    const std::uint8_t b = d[off + n++];
    v = (v << 7) | (b & 0x7FU);
    if ((b & 0x80U) == 0) {
      value = v;
      next = off + n;
      return true;
    }
    if (n > 8) {
      return false;
    }
  }
  return false;
}

// A position is a record boundary if it is EOF or the start of a new OMS token
// (object/attribute/link/container 0xA1-0xA9, level markers 0xB0-0xBF, or the
// 0x03 stream header). Used to size streamed blobs deterministically.
bool oms_record_boundary(const std::vector<std::uint8_t>& d, std::size_t pos) {
  if (pos == d.size()) {
    return true;
  }
  if (pos > d.size()) {
    return false;
  }
  const std::uint8_t b = d[pos];
  return b == 0x03 || (b >= 0xA1 && b <= 0xA9) || (b >= 0xB0 && b <= 0xBF);
}

// Object-open header: A1 <rid/class bytes> 20 00, with no record tag in the
// header body. Returns the header length, or nullopt if this is not one.
prosave_optional<std::size_t> oms_object_header_len(
    const std::vector<std::uint8_t>& d, std::size_t p) {
  if (d[p] != 0xA1) {
    return {};
  }
  const std::size_t lim = std::min(d.size() - 1, p + 26);
  for (std::size_t q = p + 2; q < lim; ++q) {
    if (d[q] == 0x20 && d[q + 1] == 0x00) {
      for (std::size_t k = p + 1; k < q; ++k) {
        const std::uint8_t b = d[k];
        if (b == 0xA1 || b == 0xA3 || b == 0xA4) {
          return {};
        }
      }
      return q + 2 - p;
    }
  }
  return {};
}

// Width of a fixed-size scalar value type (from get_value_from_blob_classic);
// 0 means variable-length (string 0x15 / blob 0x14 / struct 0x17).
std::size_t oms_scalar_size(std::uint8_t t) {
  switch (t) {
    case 2: case 6: case 10: return 1;
    case 3: case 7: case 11: return 2;
    case 4: case 8: case 12: case 14: case 18: case 19: return 4;
    case 5: case 9: case 13: case 15: case 16: case 17: return 8;
    default: return 0;
  }
}

// Decode a blob value at `type_pos` (which points at the 0x14 type byte). The
// three on-disk forms are: plain `14 <len> <data>`, empty `14 00`, and
// streamed `14 00 <len> <data>` (the extra 0x00 flag of type 0x40000014).
bool oms_blob_span(const std::vector<std::uint8_t>& d, std::size_t type_pos,
                   std::size_t& data_off, std::uint64_t& len) {
  std::uint64_t l1 = 0;
  std::size_t nx = 0;
  if (!read_oms_uint(d, type_pos + 1, l1, nx)) {
    return false;
  }
  if (l1 != 0) {
    len = l1;
    data_off = nx;  // plain blob
    return true;
  }
  // first byte after 0x14 is 0x00: either empty or streamed.
  std::uint64_t L = 0;
  std::size_t doff = 0;
  if (read_oms_uint(d, nx, L, doff)) {
    const std::uint64_t end = static_cast<std::uint64_t>(doff) + L;
    if (L > 0 && end <= d.size() &&
        oms_record_boundary(d, static_cast<std::size_t>(end))) {
      len = L;
      data_off = doff;  // streamed / sized blob
      return true;
    }
  }
  len = 0;
  data_off = nx;  // genuinely empty blob
  return true;
}

void classify_fwf_blob(FwfBlob& blob) {
  const auto& p = blob.payload;
  if (p.empty()) {
    blob.kind = FwfPayloadKind::Empty;
    return;
  }
  if (has_zlib_header(p)) {
    try {
      blob.inflated = inflate_zlib(p);
      blob.kind = FwfPayloadKind::Zlib;
      return;
    } catch (const std::exception&) {
      // not a clean zlib stream after all; fall through
    }
  }
  if (p.size() >= 4 && p[0] == 0x31 && p[1] == 0x18 && p[2] == 0x10 &&
      p[3] == 0x06) {
    blob.kind = FwfPayloadKind::FlashImage;  // block-container flash image (V15+)
    return;
  }
  if (p.size() >= 2 && p[0] == 0x03 && p[1] == 0xA1) {
    blob.kind = FwfPayloadKind::Oms;  // nested OMS sub-stream
    return;
  }
  for (std::size_t i = 0; i + 3 <= p.size() && i < 8; ++i) {
    if (p[i] == 'F' && p[i + 1] == 'S' && p[i + 2] == 'F') {
      blob.kind = FwfPayloadKind::Fsf;
      return;
    }
  }
  blob.kind = FwfPayloadKind::Raw;
}

// Walk the OMS+ object stream, harvesting every blob value with its exact
// stream-declared length. This handles every .fwf variant (streamed kernel
// slices, FSF flash volumes, the V15 raw flash image, nested sub-streams)
// because it is driven by the token grammar, not by class-name markers.
FwfDecoded decode_fwf(const std::vector<std::uint8_t>& fwf) {
  FwfDecoded decoded;
  const std::size_t n = fwf.size();
  std::size_t p = 0;
  std::string ctx_name;
  if (p < n && fwf[p] == 0x03) {
    ++p;
  }
  while (p < n) {
    const std::uint8_t b = fwf[p];
    if (b == 0xA3) {  // attribute: A3 <aid> 00 <type> <value>
      std::uint64_t aid = 0;
      std::size_t q = 0;
      if (!read_oms_uint(fwf, p + 1, aid, q) || q >= n || fwf[q] != 0x00) {
        ++p; ++decoded.unclassified_bytes; continue;
      }
      const std::uint8_t t = fwf[q + 1];
      const std::size_t r = q + 2;
      if (t == 0x15) {  // string
        std::uint64_t ln = 0;
        std::size_t sr = 0;
        if (!read_oms_uint(fwf, r, ln, sr) ||
            static_cast<std::uint64_t>(sr) + ln > n) {
          ++p; ++decoded.unclassified_bytes; continue;
        }
        std::string s(reinterpret_cast<const char*>(fwf.data() + sr),
                      static_cast<std::size_t>(ln));
        if (aid == 233) {
          ctx_name = s;
        }
        p = sr + static_cast<std::size_t>(ln);
        continue;
      }
      if (t == 0x14) {  // blob
        std::size_t doff = 0;
        std::uint64_t ln = 0;
        if (!oms_blob_span(fwf, q + 1, doff, ln) ||
            static_cast<std::uint64_t>(doff) + ln > n) {
          ++p; ++decoded.unclassified_bytes; continue;
        }
        FwfBlob blob;
        blob.index = decoded.blobs.size();
        blob.attr_offset = p;
        blob.payload_offset = doff;
        blob.payload_size = static_cast<std::size_t>(ln);
        blob.aid = static_cast<std::size_t>(aid);
        blob.class_name = ctx_name;
        blob.name = fwf_lookback_name(fwf, p);
        blob.payload.assign(
            fwf.begin() + static_cast<std::ptrdiff_t>(doff),
            fwf.begin() + static_cast<std::ptrdiff_t>(doff + ln));
        classify_fwf_blob(blob);
        decoded.blobs.push_back(std::move(blob));
        p = doff + static_cast<std::size_t>(ln);
        continue;
      }
      if (t == 0x04) {  // uint varint
        std::uint64_t v = 0;
        std::size_t nx = 0;
        if (read_oms_uint(fwf, r, v, nx)) { p = nx; continue; }
        ++p; ++decoded.unclassified_bytes; continue;
      }
      if (t == 0x01) { p = r + 1; continue; }  // bool
      const std::size_t sz = oms_scalar_size(t);
      if (sz) { p = r + sz; continue; }
      ++p; ++decoded.unclassified_bytes; continue;
    }
    if (b == 0xA4) {  // link: A4 <aid> 10 00 00 <b>
      std::uint64_t aid = 0;
      std::size_t q = 0;
      if (read_oms_uint(fwf, p + 1, aid, q) && q + 3 <= n && fwf[q] == 0x10 &&
          fwf[q + 1] == 0x00 && fwf[q + 2] == 0x00) {
        p = q + 4; continue;
      }
      ++p; ++decoded.unclassified_bytes; continue;
    }
    if (b == 0xA1) {  // object open
      if (const auto hl = oms_object_header_len(fwf, p)) { p += *hl; continue; }
      ++p; ++decoded.unclassified_bytes; continue;
    }
    if (b == 0xA2 || b == 0xA5 || b == 0xA6 || b == 0xA7 || b == 0xA9 ||
        (b >= 0xB0 && b <= 0xBF) || b == 0x20) {
      ++p; continue;  // container / level / structural tokens
    }
    ++p; ++decoded.unclassified_bytes;
  }

  if (decoded.blobs.empty()) {
    throw Error("no OMS blobs found in FWF (not a recognised OMS stream)");
  }

  // NK image = the nk.nb0.* zlib slices, concatenated in stream order. Absent
  // for the V15+ raw-flash layout (which carries a single flash-image blob).
  for (auto& blob : decoded.blobs) {
    if (blob.kind == FwfPayloadKind::Zlib && is_nk_part_name(blob.name)) {
      blob.nk_output_offset = decoded.nk_image.size();
      decoded.nk_image.insert(decoded.nk_image.end(), blob.inflated.begin(),
                              blob.inflated.end());
      ++decoded.nk_part_count;
    }
  }
  for (const auto& blob : decoded.blobs) {
    if (blob.kind == FwfPayloadKind::Fsf) {
      decoded.has_flash = true;
    }
    if (blob.kind == FwfPayloadKind::FlashImage) {
      decoded.has_flash_image = true;
    }
    if (blob.kind == FwfPayloadKind::Raw && blob.payload_size > 0 &&
        is_image_part_name(blob.name)) {
      decoded.has_bootloader = true;
    }
  }
  return decoded;
}

std::string json_escape(const std::string& s) {
  std::ostringstream os;
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') {
      os << '\\' << static_cast<char>(c);
    } else if (c < 0x20) {
      os << "\\u" << std::hex << std::setw(4) << std::setfill('0')
         << static_cast<int>(c) << std::dec;
    } else {
      os << static_cast<char>(c);
    }
  }
  return os.str();
}

std::string build_fwf_manifest(const fs::path& input,
                               const FwfDecoded& decoded) {
  std::ostringstream os;
  os << "{\n"
     << "  \"source\": \"" << json_escape(input.filename().string()) << "\",\n"
     << "  \"format\": \"FWF\",\n"
     << "  \"layout\": \"oms-object-tree\",\n"
     << "  \"blob_count\": " << decoded.blobs.size() << ",\n"
     << "  \"nk_part_count\": " << decoded.nk_part_count << ",\n"
     << "  \"nk_size\": " << decoded.nk_image.size() << ",\n"
     << "  \"has_flash_fsf\": " << (decoded.has_flash ? "true" : "false")
     << ",\n"
     << "  \"has_flash_image\": "
     << (decoded.has_flash_image ? "true" : "false") << ",\n"
     << "  \"has_bootloader\": "
     << (decoded.has_bootloader ? "true" : "false") << ",\n"
     << "  \"unclassified_bytes\": " << decoded.unclassified_bytes << ",\n"
     << "  \"blobs\": [\n";
  for (std::size_t i = 0; i < decoded.blobs.size(); ++i) {
    const auto& blob = decoded.blobs[i];
    os << "    {\"index\": " << blob.index
       << ", \"name\": \"" << json_escape(blob.name) << "\""
       << ", \"class\": \"" << json_escape(blob.class_name) << "\""
       << ", \"aid\": " << blob.aid
       << ", \"kind\": \"" << fwf_kind_name(blob.kind) << "\""
       << ", \"attr_offset\": " << blob.attr_offset
       << ", \"payload_offset\": " << blob.payload_offset
       << ", \"payload_size\": " << blob.payload_size;
    if (blob.kind == FwfPayloadKind::Zlib) {
      os << ", \"inflated_size\": " << blob.inflated.size();
    }
    if (blob.nk_output_offset != static_cast<std::size_t>(-1)) {
      os << ", \"nk_output_offset\": " << blob.nk_output_offset;
    }
    os << "}" << (i + 1 == decoded.blobs.size() ? "\n" : ",\n");
  }
  os << "  ]\n}\n";
  return os.str();
}

std::string sanitize_fwf_filename(const std::string& name, std::size_t index) {
  if (name.empty()) {
    return "blob_" + std::to_string(index);
  }
  std::string out;
  out.reserve(name.size());
  for (unsigned char c : name) {
    if (std::isalnum(c) || c == '.' || c == '-' || c == '_') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('_');
    }
  }
  return out;
}


} // namespace

} // namespace
