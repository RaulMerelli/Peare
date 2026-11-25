#include "TibxRsa.h"
#include "TibxSecureRandom.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <new>
#include <stdexcept>

namespace peare {
namespace fs {
namespace {

typedef std::vector<std::uint8_t> Bytes;

void wipe(void* data, std::size_t size) {
    volatile unsigned char* p = static_cast<volatile unsigned char*>(data);
    while (size--) *p++ = 0;
}

template <typename T> class WipeGuard {
public:
    explicit WipeGuard(T& value) : value_(value) {}
    ~WipeGuard() { if (!value_.empty()) wipe(&value_[0], value_.size() * sizeof(typename T::value_type)); }
    WipeGuard(const WipeGuard&) = delete;
    WipeGuard& operator=(const WipeGuard&) = delete;
private:
    T& value_;
};

template <typename T> class WipeAllocator : public std::allocator<T> {
public:
    template <typename U> struct rebind { typedef WipeAllocator<U> other; };
    WipeAllocator() noexcept {}
    template <typename U> WipeAllocator(const WipeAllocator<U>&) noexcept {}
    void deallocate(T* p, std::size_t count) noexcept {
        if (p) wipe(p, count * sizeof(T));
        std::allocator<T>::deallocate(p, count);
    }
};
typedef std::vector<std::uint32_t, WipeAllocator<std::uint32_t> > Words;

struct Key {
    Bytes n;
    Bytes e;
    Bytes d;
    Bytes p, q, dp, dq, qi;
    ~Key() {
        Bytes* parts[] = {&d, &p, &q, &dp, &dq, &qi};
        for (std::size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i)
            if (!parts[i]->empty()) wipe(&(*parts[i])[0], parts[i]->size());
    }
};

bool validateKey(const Key& key);

bool space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

int base64(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

TibxRsaResult pemDecode(const std::string& pem, Bytes* out, bool* pkcs8) {
    if (pem.size() > 32768) return TibxRsaResult::InvalidKey;
    const std::string rsa = "-----BEGIN RSA PRIVATE KEY-----";
    const std::string plain = "-----BEGIN PRIVATE KEY-----";
    const std::string encrypted = "-----BEGIN ENCRYPTED PRIVATE KEY-----";
    std::size_t start = 0;
    for (;;) {
        while (start < pem.size() && (pem[start] == ' ' || pem[start] == '\t')) ++start;
        if (pem.compare(start, rsa.size(), rsa) == 0 ||
            pem.compare(start, plain.size(), plain) == 0 ||
            pem.compare(start, encrypted.size(), encrypted) == 0) break;
        const std::size_t next = pem.find_first_of("\r\n", start);
        if (next == std::string::npos) return TibxRsaResult::InvalidKey;
        start = next + 1;
        if (pem[next] == '\r' && start < pem.size() && pem[start] == '\n') ++start;
    }
    if (pem.compare(start, encrypted.size(), encrypted) == 0) return TibxRsaResult::Unsupported;
    std::string end;
    if (pem.compare(start, rsa.size(), rsa) == 0) {
        start += rsa.size(); end = "-----END RSA PRIVATE KEY-----"; *pkcs8 = false;
    } else if (pem.compare(start, plain.size(), plain) == 0) {
        start += plain.size(); end = "-----END PRIVATE KEY-----"; *pkcs8 = true;
    } else return TibxRsaResult::InvalidKey;
    const std::size_t finish = pem.find(end, start);
    if (finish == std::string::npos) return TibxRsaResult::InvalidKey;
    std::string encoded;
    WipeGuard<std::string> encodedGuard(encoded);
    encoded.reserve(finish - start);
    for (std::size_t i = start; i < finish; ++i) {
        if (space(pem[i])) continue;
        if (base64(pem[i]) < 0 && pem[i] != '=') return TibxRsaResult::InvalidKey;
        encoded.push_back(pem[i]);
    }
    if (encoded.empty() || encoded.size() % 4) return TibxRsaResult::InvalidKey;
    Bytes decoded;
    WipeGuard<Bytes> decodedGuard(decoded);
    decoded.reserve((encoded.size() / 4) * 3);
    for (std::size_t i = 0; i < encoded.size(); i += 4) {
        const int a = base64(encoded[i]), b = base64(encoded[i + 1]);
        const bool p2 = encoded[i + 2] == '=', p3 = encoded[i + 3] == '=';
        const int c = p2 ? 0 : base64(encoded[i + 2]);
        const int d = p3 ? 0 : base64(encoded[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0 || (p2 && !p3) ||
            ((p2 || p3) && i + 4 != encoded.size()) ||
            (p2 && (b & 15)) || (p3 && !p2 && (c & 3))) return TibxRsaResult::InvalidKey;
        decoded.push_back(static_cast<std::uint8_t>((a << 2) | (b >> 4)));
        if (!p2) decoded.push_back(static_cast<std::uint8_t>((b << 4) | (c >> 2)));
        if (!p3) decoded.push_back(static_cast<std::uint8_t>((c << 6) | d));
    }
    out->swap(decoded);
    return TibxRsaResult::Success;
}

class Der {
public:
    Der(const std::uint8_t* data, std::size_t size) : data_(data), size_(size), pos_(0) {}
    bool read(std::uint8_t tag, Der* value) {
        if (size_ - pos_ < 2 || data_[pos_++] != tag) return false;
        std::size_t len = data_[pos_++];
        if (len & 0x80U) {
            const std::size_t count = len & 0x7fU;
            if (!count || count > sizeof(std::size_t) || count > size_ - pos_ || !data_[pos_]) return false;
            len = 0;
            for (std::size_t i = 0; i < count; ++i) {
                if (len > (std::numeric_limits<std::size_t>::max() >> 8)) return false;
                len = (len << 8) | data_[pos_++];
            }
            if (len < 128) return false;
        }
        if (len > size_ - pos_) return false;
        *value = Der(data_ + pos_, len);
        pos_ += len;
        return true;
    }
    bool empty() const { return pos_ == size_; }
    bool hasTag(std::uint8_t tag) const { return !empty() && data_[pos_] == tag; }
    bool any() {
        if (size_ - pos_ < 2) return false;
        std::size_t start = pos_++;
        if ((data_[start] & 31U) == 31U) {
            if (pos_ == size_ || (data_[pos_] & 127U) == 0) return false;
            while (pos_ < size_ && (data_[pos_++] & 128U)) {}
            if (pos_ == size_ && (data_[pos_ - 1] & 128U)) return false;
        }
        if (pos_ == size_) return false;
        std::size_t len = data_[pos_++];
        if (len & 128U) {
            const std::size_t count = len & 127U;
            if (!count || count > sizeof(std::size_t) || count > size_ - pos_ || !data_[pos_]) return false;
            len = 0;
            for (std::size_t i = 0; i < count; ++i) {
                if (len > (std::numeric_limits<std::size_t>::max() >> 8)) return false;
                len = (len << 8) | data_[pos_++];
            }
            if (len < 128) return false;
        }
        if (len > size_ - pos_) return false;
        pos_ += len;
        return true;
    }
    const std::uint8_t* data() const { return data_; }
    std::size_t size() const { return size_; }
private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_;
};

bool integer(Der* parent, Bytes* out, bool allowZero) {
    Der item(nullptr, 0);
    if (!parent->read(0x02, &item) || !item.size() || (item.data()[0] & 0x80U)) return false;
    std::size_t skip = 0;
    if (item.size() > 1 && !item.data()[0]) {
        if (!(item.data()[1] & 0x80U)) return false;
        skip = 1;
    }
    if (!allowZero && item.size() == 1 && !item.data()[0]) return false;
    out->assign(item.data() + skip, item.data() + item.size());
    return true;
}

bool lessBytes(const Bytes& a, const Bytes& b) {
    return a.size() != b.size() ? a.size() < b.size() :
        std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end());
}

TibxRsaResult parseKey(const std::string& pem, Key* key) {
    Bytes der;
    WipeGuard<Bytes> derGuard(der);
    bool pkcs8 = false;
    TibxRsaResult status = pemDecode(pem, &der, &pkcs8);
    if (status != TibxRsaResult::Success) return status;
    Der input(der.data(), der.size()), outer(nullptr, 0);
    if (!input.read(0x30, &outer) || !input.empty()) return TibxRsaResult::InvalidKey;
    Bytes publicN, publicE;
    if (pkcs8) {
        Bytes version;
        Der algorithm(nullptr, 0), oid(nullptr, 0), param(nullptr, 0), privateKey(nullptr, 0);
        static const std::uint8_t rsaOid[] = {0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01};
        if (!integer(&outer, &version, true) || version.size() != 1 || version[0] > 1 ||
            !outer.read(0x30, &algorithm) || !algorithm.read(0x06, &oid) ||
            oid.size() != sizeof(rsaOid) || !std::equal(oid.data(), oid.data() + oid.size(), rsaOid))
            return TibxRsaResult::InvalidKey;
        if (!algorithm.empty() && (!algorithm.read(0x05, &param) || param.size() || !algorithm.empty()))
            return TibxRsaResult::InvalidKey;
        if (!outer.read(0x04, &privateKey)) return TibxRsaResult::InvalidKey;
        if (outer.hasTag(0xa0)) {
            Der attrs(nullptr, 0);
            if (!outer.read(0xa0, &attrs)) return TibxRsaResult::InvalidKey;
            while (!attrs.empty()) {
                Der attr(nullptr, 0), attrOid(nullptr, 0), values(nullptr, 0);
                if (!attrs.read(0x30, &attr) || !attr.read(0x06, &attrOid) || !attrOid.size() ||
                    !attr.read(0x31, &values) || !attr.empty() || values.empty()) return TibxRsaResult::InvalidKey;
                bool atStart = true;
                for (std::size_t i = 0; i < attrOid.size(); ++i) {
                    if (atStart && attrOid.data()[i] == 0x80U) return TibxRsaResult::InvalidKey;
                    atStart = !(attrOid.data()[i] & 0x80U);
                }
                if (!atStart) return TibxRsaResult::InvalidKey;
                while (!values.empty()) if (!values.any()) return TibxRsaResult::InvalidKey;
            }
        }
        const bool hasPublic = outer.hasTag(0x81);
        if (hasPublic) {
            Der publicKey(nullptr, 0), sequence(nullptr, 0);
            if (!outer.read(0x81, &publicKey) || publicKey.size() < 2 || publicKey.data()[0] != 0)
                return TibxRsaResult::InvalidKey;
            Der encodedPublic(publicKey.data() + 1, publicKey.size() - 1);
            if (!encodedPublic.read(0x30, &sequence) || !encodedPublic.empty() ||
                !integer(&sequence, &publicN, false) || !integer(&sequence, &publicE, false) || !sequence.empty())
                return TibxRsaResult::InvalidKey;
        }
        if (!outer.empty() || (version[0] == 1) != hasPublic) return TibxRsaResult::InvalidKey;
        Der wrapped(privateKey.data(), privateKey.size()), sequence(nullptr, 0);
        if (!wrapped.read(0x30, &sequence) || !wrapped.empty()) return TibxRsaResult::InvalidKey;
        outer = sequence;
    }
    Bytes version, n, e, d, p, q, dp, dq, qi;
    WipeGuard<Bytes> dGuard(d), pGuard(p), qGuard(q), dpGuard(dp), dqGuard(dq), qiGuard(qi);
    if (!integer(&outer, &version, true) || version.size() != 1 || version[0] != 0 ||
        !integer(&outer, &n, false) || !integer(&outer, &e, false) || !integer(&outer, &d, false))
        return TibxRsaResult::InvalidKey;
    if (!integer(&outer, &p, false) || !integer(&outer, &q, false) ||
        !integer(&outer, &dp, false) || !integer(&outer, &dq, false) ||
        !integer(&outer, &qi, false)) return TibxRsaResult::InvalidKey;
    if (!outer.empty()) return TibxRsaResult::Unsupported;
    if (!publicN.empty() && (publicN != n || publicE != e)) return TibxRsaResult::InvalidKey;
    if (n.size() < 64 || n.size() > 512) return TibxRsaResult::Unsupported;
    if (!(n.back() & 1U) || !(e.back() & 1U) || !(d.back() & 1U) ||
        (e.size() == 1 && e[0] < 3) || !lessBytes(e, n) || !lessBytes(d, n))
        return TibxRsaResult::InvalidKey;
    key->n.swap(n); key->e.swap(e); key->d.swap(d);
    key->p.swap(p); key->q.swap(q); key->dp.swap(dp); key->dq.swap(dq); key->qi.swap(qi);
    if (!validateKey(*key)) return TibxRsaResult::InvalidKey;
    return TibxRsaResult::Success;
}

void appendLength(Bytes* out, std::size_t length) {
    if (length < 128) { out->push_back(static_cast<std::uint8_t>(length)); return; }
    Bytes bytes;
    while (length) { bytes.push_back(static_cast<std::uint8_t>(length)); length >>= 8; }
    out->push_back(static_cast<std::uint8_t>(0x80U | bytes.size()));
    out->insert(out->end(), bytes.rbegin(), bytes.rend());
}

void appendDer(Bytes* out, std::uint8_t tag, const Bytes& value) {
    out->push_back(tag); appendLength(out, value.size());
    out->insert(out->end(), value.begin(), value.end());
}

void appendInteger(Bytes* out, const Bytes& value) {
    Bytes body;
    if (value[0] & 0x80U) body.push_back(0);
    body.insert(body.end(), value.begin(), value.end());
    appendDer(out, 0x02, body);
}

Bytes spki(const Key& key) {
    static const std::uint8_t alg[] = {0x30,0x0d,0x06,0x09,0x2a,0x86,0x48,0x86,0xf7,0x0d,0x01,0x01,0x01,0x05,0x00};
    Bytes publicValues, publicKey, bitString(1, 0), body(alg, alg + sizeof(alg)), result;
    appendInteger(&publicValues, key.n); appendInteger(&publicValues, key.e);
    appendDer(&publicKey, 0x30, publicValues);
    bitString.insert(bitString.end(), publicKey.begin(), publicKey.end());
    appendDer(&body, 0x03, bitString); appendDer(&result, 0x30, body);
    return result;
}

Words words(const Bytes& input, std::size_t count) {
    Words result(count, 0);
    for (std::size_t i = 0; i < input.size(); ++i)
        result[i / 4] |= std::uint32_t(input[input.size() - 1 - i]) << ((i % 4) * 8);
    return result;
}

int compare(const Words& a, const Words& b) {
    for (std::size_t i = a.size(); i-- > 0; ) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

void addMod(Words* a, const Words& b, const Words& n) {
    std::uint64_t carry = 0;
    for (std::size_t i = 0; i < a->size(); ++i) {
        const std::uint64_t sum = std::uint64_t((*a)[i]) + b[i] + carry;
        (*a)[i] = static_cast<std::uint32_t>(sum); carry = sum >> 32;
    }
    if (compare(*a, n) < 0) return;
    std::uint64_t borrow = 0;
    for (std::size_t i = 0; i < a->size(); ++i) {
        const std::uint64_t sub = std::uint64_t(n[i]) + borrow;
        const std::uint64_t value = (*a)[i];
        (*a)[i] = static_cast<std::uint32_t>(value - sub);
        borrow = value < sub;
    }
}

Words multiplyMod(const Words& a, const Words& b, const Words& n) {
    Words result(n.size(), 0), addend(a);
    for (std::size_t i = 0; i < b.size(); ++i) {
        std::uint32_t bits = b[i];
        for (unsigned j = 0; j < 32; ++j) {
            if (bits & 1U) addMod(&result, addend, n);
            bits >>= 1;
            addMod(&addend, addend, n);
        }
    }
    return result;
}

bool isZero(const Words& value) {
    for (std::size_t i = 0; i < value.size(); ++i)
        if (value[i]) return false;
    return true;
}

void subtract(Words* a, const Words& b) {
    std::uint64_t borrow = 0;
    for (std::size_t i = 0; i < a->size(); ++i) {
        const std::uint64_t v = std::uint64_t((*a)[i]) - b[i] - borrow;
        (*a)[i] = static_cast<std::uint32_t>(v);
        borrow = v >> 63;
    }
}

void shiftRight(Words* value) {
    for (std::size_t i = 0; i + 1 < value->size(); ++i)
        (*value)[i] = ((*value)[i] >> 1) | ((*value)[i + 1] << 31);
    value->back() >>= 1;
}

void halfMod(Words* value, const Words& n) {
    if ((*value)[0] & 1U) {
        std::uint64_t carry = 0;
        for (std::size_t i = 0; i < value->size(); ++i) {
            const std::uint64_t sum = std::uint64_t((*value)[i]) + n[i] + carry;
            (*value)[i] = static_cast<std::uint32_t>(sum);
            carry = sum >> 32;
        }
    }
    shiftRight(value);
}

void subtractMod(Words* a, const Words& b, const Words& n) {
    if (compare(*a, b) < 0) {
        std::uint64_t carry = 0;
        for (std::size_t i = 0; i < a->size(); ++i) {
            const std::uint64_t sum = std::uint64_t((*a)[i]) + n[i] + carry;
            (*a)[i] = static_cast<std::uint32_t>(sum);
            carry = sum >> 32;
        }
    }
    subtract(a, b);
}

bool inverseMod(const Words& input, const Words& n, Words* inverse) {
    Words u(input), v(n), x(n.size(), 0), y(n.size(), 0), one(n.size(), 0);
    x[0] = 1; one[0] = 1;
    while (!isZero(u) && !isZero(v) && compare(u, one) != 0 && compare(v, one) != 0) {
        while (!isZero(u) && !(u[0] & 1U)) { shiftRight(&u); halfMod(&x, n); }
        while (!isZero(v) && !(v[0] & 1U)) { shiftRight(&v); halfMod(&y, n); }
        if (compare(u, v) >= 0) { subtract(&u, v); subtractMod(&x, y, n); }
        else { subtract(&v, u); subtractMod(&y, x, n); }
    }
    if (compare(u, one) == 0) { inverse->swap(x); return true; }
    if (compare(v, one) == 0) { inverse->swap(y); return true; }
    return false;
}

class Montgomery {
public:
    explicit Montgomery(const Words& modulus) : n_(modulus), k_(modulus.size() - 1), inverse_(1) {
        for (int i = 0; i < 5; ++i) inverse_ *= 2U - n_[0] * inverse_;
        inverse_ = 0U - inverse_;
        one_.assign(n_.size(), 0); one_[0] = 1;
        for (std::size_t i = 0; i < k_ * 32; ++i) addMod(&one_, one_, n_);
    }
    Words mul(const Words& a, const Words& b) const {
        Words t(2 * k_ + 2, 0);
        for (std::size_t i = 0; i < k_; ++i) {
            std::uint64_t carry = 0;
            for (std::size_t j = 0; j < k_; ++j) {
                const std::uint64_t v = std::uint64_t(a[i]) * b[j] + t[i + j] + carry;
                t[i + j] = static_cast<std::uint32_t>(v); carry = v >> 32;
            }
            for (std::size_t at = i + k_; at < t.size(); ++at) {
                const std::uint64_t v = std::uint64_t(t[at]) + carry;
                t[at] = static_cast<std::uint32_t>(v); carry = v >> 32;
            }
        }
        for (std::size_t i = 0; i < k_; ++i) {
            const std::uint32_t m = t[i] * inverse_;
            std::uint64_t carry = 0;
            for (std::size_t j = 0; j < k_; ++j) {
                const std::uint64_t v = std::uint64_t(m) * n_[j] + t[i + j] + carry;
                t[i + j] = static_cast<std::uint32_t>(v); carry = v >> 32;
            }
            for (std::size_t at = i + k_; at < t.size(); ++at) {
                const std::uint64_t v = std::uint64_t(t[at]) + carry;
                t[at] = static_cast<std::uint32_t>(v); carry = v >> 32;
            }
        }
        Words result(n_.size(), 0);
        for (std::size_t i = 0; i <= k_; ++i) result[i] = t[k_ + i];
        Words difference(n_.size(), 0);
        std::uint64_t borrow = 0;
        for (std::size_t i = 0; i < result.size(); ++i) {
            const std::uint64_t v = std::uint64_t(result[i]) - n_[i] - borrow;
            difference[i] = static_cast<std::uint32_t>(v);
            borrow = v >> 63;
        }
        const std::uint32_t mask = 0U - static_cast<std::uint32_t>(borrow ^ 1U);
        for (std::size_t i = 0; i < result.size(); ++i)
            result[i] = (difference[i] & mask) | (result[i] & ~mask);
        return result;
    }
    Words power(const Words& base, const Bytes& exponent) const {
        Words factor(base), result(one_);
        for (std::size_t i = 0; i < k_ * 32; ++i) addMod(&factor, factor, n_);
        for (std::size_t i = 0; i < exponent.size(); ++i)
            for (int bit = 7; bit >= 0; --bit) {
                result = mul(result, result);
                if (exponent[i] & (1U << bit)) result = mul(result, factor);
            }
        Words one(n_.size(), 0); one[0] = 1;
        return mul(result, one);
    }
    Words multiplyNormal(const Words& a, const Words& b) const {
        Words r2(one_);
        for (std::size_t i = 0; i < k_ * 32; ++i) addMod(&r2, r2, n_);
        return mul(mul(a, r2), b);
    }
    Words privatePower(const Words& base, const Bytes& exponent, std::size_t width) const {
        Bytes padded(width, 0);
        WipeGuard<Bytes> paddedGuard(padded);
        std::copy(exponent.begin(), exponent.end(), padded.end() - exponent.size());
        Words factor(base), result(one_);
        for (std::size_t i = 0; i < k_ * 32; ++i) addMod(&factor, factor, n_);
        for (std::size_t i = 0; i < padded.size(); ++i) {
            for (int bit = 7; bit >= 0; --bit) {
                Words square = mul(result, result);
                Words product = mul(square, factor);
                volatile std::uint32_t opaqueMask = 0U - std::uint32_t((padded[i] >> bit) & 1U);
                const std::uint32_t mask = opaqueMask;
                for (std::size_t j = 0; j < result.size(); ++j)
                    result[j] = (square[j] & ~mask) | (product[j] & mask);
            }
        }
        Words one(n_.size(), 0); one[0] = 1;
        return mul(result, one);
    }
private:
    Words n_, one_;
    std::size_t k_;
    std::uint32_t inverse_;
};

TibxRsaResult makeBaseBlind(const Bytes& modulus, const Words& n, Words* factor, Words* inverse) {
    Bytes sample(modulus.size(), 0);
    WipeGuard<Bytes> sampleGuard(sample);
    std::uint8_t mask = 0xffU;
    while (!(modulus[0] & (mask ^ (mask >> 1)))) mask >>= 1;
    Words one(n.size(), 0); one[0] = 1;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        if (!tibxSecureRandom(sample.data(), sample.size())) return TibxRsaResult::ResourceError;
        sample[0] &= mask;
        Words candidate = words(sample, n.size());
        if (compare(candidate, one) <= 0 || compare(candidate, n) >= 0) continue;
        Words inv(n.size(), 0);
        if (!inverseMod(candidate, n, &inv)) continue;
        if (compare(Montgomery(n).multiplyNormal(candidate, inv), one) != 0) return TibxRsaResult::ResourceError;
        factor->swap(candidate); inverse->swap(inv);
        return TibxRsaResult::Success;
    }
    return TibxRsaResult::ResourceError;
}

Words powerMod(const Words& base, const Bytes& exponent, const Words& n) {
    return Montgomery(n).power(base, exponent);
}

Bytes toBytes(const Words& value, std::size_t size) {
    Bytes result(size);
    for (std::size_t i = 0; i < size; ++i)
        result[size - 1 - i] = static_cast<std::uint8_t>(value[i / 4] >> ((i % 4) * 8));
    return result;
}

Words multiplyTruncated(const Words& a, const Words& b) {
    Words product(a.size(), 0);
    for (std::size_t i = 0; i < a.size(); ++i) {
        std::uint64_t carry = 0;
        for (std::size_t j = 0; j < a.size() - i; ++j) {
            const std::uint64_t value = std::uint64_t(a[i]) * b[j] + product[i + j] + carry;
            product[i + j] = static_cast<std::uint32_t>(value);
            carry = value >> 32;
        }
    }
    return product;
}

Bytes blindedExponent(const Key& key, const Bytes& random) {
    const std::size_t width = key.n.size() + random.size() + 1;
    const std::size_t count = (width + 3) / 4;
    Words p = words(key.p, count), q = words(key.q, count);
    --p[0]; --q[0];
    Words factor = words(random, count);
    Words exponent = multiplyTruncated(multiplyTruncated(p, q), factor);
    const Words d = words(key.d, count);
    std::uint64_t carry = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t value = std::uint64_t(exponent[i]) + d[i] + carry;
        exponent[i] = static_cast<std::uint32_t>(value);
        carry = value >> 32;
    }
    return toBytes(exponent, width);
}

Words reduce(const Bytes& input, const Words& modulus) {
    Words result(modulus.size(), 0), one(modulus.size(), 0); one[0] = 1;
    for (std::size_t i = 0; i < input.size(); ++i)
        for (int bit = 7; bit >= 0; --bit) {
            addMod(&result, result, modulus);
            if (input[i] & (1U << bit)) addMod(&result, one, modulus);
        }
    return result;
}

bool probablePrime(const Bytes& value) {
    static const std::uint32_t bases[] = {2,3,5,7,11,13,17,19,23,29,31,37,41,43,47,53};
    const std::size_t count = (value.size() + 3) / 4 + 1;
    Words n = words(value, count), minus(n), one(count, 0); one[0] = 1;
    --minus[0];
    Words odd(minus);
    unsigned shifts = 0;
    while (!(odd[0] & 1U)) {
        for (std::size_t i = 0; i + 1 < odd.size(); ++i) odd[i] = (odd[i] >> 1) | (odd[i + 1] << 31);
        odd.back() >>= 1; ++shifts;
    }
    Bytes exponent = toBytes(odd, value.size());
    WipeGuard<Bytes> exponentGuard(exponent);
    const Montgomery mont(n);
    for (std::size_t i = 0; i < sizeof(bases) / sizeof(bases[0]); ++i) {
        Words base(count, 0); base[0] = bases[i];
        if (compare(base, n) >= 0) continue;
        Words x = mont.power(base, exponent);
        if (compare(x, one) == 0 || compare(x, minus) == 0) continue;
        bool passed = false;
        for (unsigned j = 1; j < shifts; ++j) {
            x = multiplyMod(x, x, n);
            if (compare(x, minus) == 0) { passed = true; break; }
            if (compare(x, one) == 0) return false;
        }
        if (!passed) return false;
    }
    return true;
}

bool validateKey(const Key& key) {
    if (key.p.size() > key.n.size() || key.q.size() > key.n.size() ||
        key.p.size() + key.q.size() > key.n.size() + 1 ||
        key.dp.size() > key.p.size() || key.dq.size() > key.q.size() || key.qi.size() > key.p.size() ||
        !(key.p.back() & 1U) || !(key.q.back() & 1U) ||
        (key.p.size() == 1 && key.p[0] < 3) || (key.q.size() == 1 && key.q[0] < 3) || key.p == key.q)
        return false;
    const std::size_t count = (key.n.size() + 3) / 4 + 1;
    const Words n = words(key.n, count), p = words(key.p, count), q = words(key.q, count);
    if (compare(p, n) >= 0 || compare(q, n) >= 0 || compare(words(key.qi, count), p) >= 0) return false;
    Words product(count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        std::uint64_t carry = 0;
        for (std::size_t j = 0; j + i < count; ++j) {
            const std::uint64_t v = std::uint64_t(p[i]) * q[j] + product[i + j] + carry;
            product[i + j] = static_cast<std::uint32_t>(v); carry = v >> 32;
        }
        if (carry) return false;
    }
    if (compare(product, n) != 0) return false;
    Words pm(p), qm(q), one(count, 0); --pm[0]; --qm[0]; one[0] = 1;
    const Words dP = reduce(key.d, pm), dQ = reduce(key.d, qm);
    if (compare(dP, words(key.dp, count)) || compare(dQ, words(key.dq, count)) ||
        compare(multiplyMod(reduce(key.e, pm), dP, pm), one) ||
        compare(multiplyMod(reduce(key.e, qm), dQ, qm), one) ||
        compare(multiplyMod(reduce(key.q, p), words(key.qi, count), p), one)) return false;
    return probablePrime(key.p) && probablePrime(key.q);
}

} // namespace

struct TibxRsaPrivateKey::Impl {
    Key key;
};

TibxRsaPrivateKey::TibxRsaPrivateKey() = default;
TibxRsaPrivateKey::~TibxRsaPrivateKey() = default;

TibxRsaResult TibxRsaPrivateKey::loadPem(const std::string& privateKeyPem) {
    impl_.reset();
    try {
        std::unique_ptr<Impl> candidate(new Impl);
        const TibxRsaResult status = parseKey(privateKeyPem, &candidate->key);
        if (status != TibxRsaResult::Success) return status;
        impl_.swap(candidate);
        return TibxRsaResult::Success;
    } catch (const std::bad_alloc&) { return TibxRsaResult::ResourceError; }
      catch (const std::length_error&) { return TibxRsaResult::ResourceError; }
}

TibxRsaResult TibxRsaPrivateKey::publicSpkiDer(Bytes* spkiDer) const {
    if (!spkiDer) return TibxRsaResult::InvalidArgument;
    spkiDer->clear();
    if (!impl_) return TibxRsaResult::InvalidKey;
    try {
        Bytes result = spki(impl_->key); spkiDer->swap(result);
        return TibxRsaResult::Success;
    } catch (const std::bad_alloc&) { return TibxRsaResult::ResourceError; }
      catch (const std::length_error&) { return TibxRsaResult::ResourceError; }
}

TibxRsaResult TibxRsaPrivateKey::decryptPkcs1v15(const Bytes& ciphertext, Bytes* plaintext) const {
    if (!plaintext) return TibxRsaResult::InvalidArgument;
    if (&ciphertext == plaintext) { plaintext->clear(); return TibxRsaResult::InvalidArgument; }
    plaintext->clear();
    if (!impl_) return TibxRsaResult::InvalidKey;
    try {
        if (!ciphertext.empty() && std::all_of(ciphertext.begin(), ciphertext.end() - 1,
            [](std::uint8_t c) { return c == 0; }) && ciphertext.back() <= 1)
            return TibxRsaResult::DecryptionFailed;
        const Key& key = impl_->key;
        if (ciphertext.size() != key.n.size()) return TibxRsaResult::DecryptionFailed;
        const std::size_t count = (key.n.size() + 3) / 4 + 1;
        const Words n = words(key.n, count), c = words(ciphertext, count);
        if (compare(c, n) >= 0) return TibxRsaResult::DecryptionFailed;
        Words factor(count, 0), inverse(count, 0);
        const TibxRsaResult blindStatus = makeBaseBlind(key.n, n, &factor, &inverse);
        if (blindStatus != TibxRsaResult::Success) return blindStatus;
        const Words blinded = Montgomery(n).multiplyNormal(c, powerMod(factor, key.e, n));
        Bytes random(16, 0);
        WipeGuard<Bytes> randomGuard(random);
        if (!tibxSecureRandom(random.data(), random.size())) return TibxRsaResult::ResourceError;
        random[0] |= 0x80U;
        Bytes exponent = blindedExponent(key, random);
        WipeGuard<Bytes> exponentGuard(exponent);
        const Words blindedMessage = Montgomery(n).privatePower(blinded, exponent, exponent.size());
        const Words message = Montgomery(n).multiplyNormal(blindedMessage, inverse);
        if (compare(powerMod(message, key.e, n), c) != 0) return TibxRsaResult::DecryptionFailed;
        Bytes encoded = toBytes(message, key.n.size());
        WipeGuard<Bytes> encodedGuard(encoded);
        if (encoded.size() < 11 || encoded[0] != 0 || encoded[1] != 2)
            return TibxRsaResult::DecryptionFailed;
        std::size_t separator = 2;
        while (separator < encoded.size() && encoded[separator]) ++separator;
        if (separator < 10 || separator == encoded.size()) return TibxRsaResult::DecryptionFailed;
        Bytes result(encoded.begin() + separator + 1, encoded.end());
        plaintext->swap(result);
        return TibxRsaResult::Success;
    } catch (const std::bad_alloc&) { return TibxRsaResult::ResourceError; }
      catch (const std::length_error&) { return TibxRsaResult::ResourceError; }
}

TibxRsaResult tibxRsaPublicSpkiDer(const std::string& privateKeyPem, Bytes* spkiDer) {
    if (!spkiDer) return TibxRsaResult::InvalidArgument;
    spkiDer->clear();
    TibxRsaPrivateKey key;
    const TibxRsaResult status = key.loadPem(privateKeyPem);
    return status == TibxRsaResult::Success ? key.publicSpkiDer(spkiDer) : status;
}

TibxRsaResult tibxRsaDecryptPkcs1v15(const std::string& privateKeyPem,
                                     const Bytes& ciphertext, Bytes* plaintext) {
    if (!plaintext) return TibxRsaResult::InvalidArgument;
    if (&ciphertext == plaintext) { plaintext->clear(); return TibxRsaResult::InvalidArgument; }
    plaintext->clear();
    if (!ciphertext.empty() && std::all_of(ciphertext.begin(), ciphertext.end() - 1,
        [](std::uint8_t c) { return c == 0; }) && ciphertext.back() <= 1)
        return TibxRsaResult::DecryptionFailed;
    TibxRsaPrivateKey key;
    const TibxRsaResult status = key.loadPem(privateKeyPem);
    return status == TibxRsaResult::Success ? key.decryptPkcs1v15(ciphertext, plaintext) : status;
}

} // namespace fs
} // namespace peare
