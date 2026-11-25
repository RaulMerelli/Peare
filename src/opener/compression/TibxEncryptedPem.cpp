#include "TibxEncryptedPem.h"
#include "../../common/compression/AesInitialization.h"
#include "third_party/lzma/Aes.h"

#include <QCryptographicHash>
#include <QMessageAuthenticationCode>
#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace peare {
namespace fs {
namespace {
typedef std::vector<std::uint8_t> Bytes;

void wipe(void *p, std::size_t n) {
    volatile unsigned char *q = static_cast<volatile unsigned char *>(p);
    while (n--)
        *q++ = 0;
}
void clear(Bytes *v) {
    if (!v->empty())
        wipe(v->data(), v->size());
    v->clear();
}
struct Guard {
    Bytes &v;
    explicit Guard(Bytes &b) : v(b) {
    }
    ~Guard() {
        clear(&v);
    }
};
struct QtGuard {
    QByteArray &v;
    explicit QtGuard(QByteArray &b) : v(b) {
    }
    ~QtGuard() {
        if (!v.isEmpty())
            wipe(v.data(), std::size_t(v.size()));
    }
};
struct AesGuard {
    UInt32 *p;
    explicit AesGuard(UInt32 *v) : p(v) {
    }
    ~AesGuard() {
        wipe(p, AES_NUM_IVMRK_WORDS * sizeof(UInt32));
    }
};

struct View {
    const std::uint8_t *p;
    std::size_t n;
};
struct Reader {
    View v;
    bool get(unsigned tag, View *out) {
        if (v.n < 2 || v.p[0] != tag)
            return false;
        std::size_t at = 2, len = v.p[1];
        if (len & 128) {
            const std::size_t count = len & 127;
            if (!count || count > sizeof(std::size_t) || count > v.n - 2 || v.p[2] == 0)
                return false;
            len = 0;
            for (std::size_t i = 0; i < count; ++i) {
                if (len > (std::numeric_limits<std::size_t>::max() >> 8))
                    return false;
                len = (len << 8) | v.p[at++];
            }
            if (len < 128)
                return false;
        }
        if (len > v.n - at)
            return false;
        out->p = v.p + at;
        out->n = len;
        v.p += at + len;
        v.n -= at + len;
        return true;
    }
    bool empty() const {
        return v.n == 0;
    }
};
bool eq(View v, const unsigned char *b, std::size_t n) {
    return v.n == n && std::memcmp(v.p, b, n) == 0;
}
bool integer(View v, std::uint64_t *n) {
    if (!v.n || (v.p[0] & 128) || (v.n > 1 && v.p[0] == 0 && !(v.p[1] & 128)))
        return false;
    if (v.p[0] == 0) {
        ++v.p;
        --v.n;
    }
    if (v.n > 8)
        return false;
    *n = 0;
    for (std::size_t i = 0; i < v.n; ++i)
        *n = (*n << 8) | v.p[i];
    return true;
}
bool space(unsigned char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
int b64(unsigned char c) {
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}
bool decodePem(const Bytes &pem, Bytes *out) {
    const char begin[] = "-----BEGIN ENCRYPTED PRIVATE KEY-----";
    const char end[] = "-----END ENCRYPTED PRIVATE KEY-----";
    Bytes::const_iterator first =
        std::search(pem.begin(), pem.end(), begin, begin + sizeof(begin) - 1);
    if (first == pem.end())
        return false;
    if (first != pem.begin() && first[-1] != '\n' && first[-1] != '\r')
        return false;
    first += sizeof(begin) - 1;
    Bytes::const_iterator last = std::search(first, pem.end(), end, end + sizeof(end) - 1);
    if (last == pem.end())
        return false;
    for (Bytes::const_iterator tail = last + sizeof(end) - 1; tail != pem.end(); ++tail)
        if (!space(*tail))
            return false;
    Bytes chars;
    Guard g(chars);
    for (; first != last; ++first) {
        if (!space(*first))
            chars.push_back(*first);
    }
    if (chars.empty() || chars.size() % 4)
        return false;
    for (std::size_t i = 0; i < chars.size(); i += 4) {
        int a = b64(chars[i]), b = b64(chars[i + 1]);
        bool p2 = chars[i + 2] == '=', p3 = chars[i + 3] == '=';
        int c = p2 ? 0 : b64(chars[i + 2]), d = p3 ? 0 : b64(chars[i + 3]);
        if (a < 0 || b < 0 || c < 0 || d < 0 || (p2 && !p3) ||
            ((p2 || p3) && i + 4 != chars.size()) || (p2 && (b & 15)) || (p3 && !p2 && (c & 3)))
            return false;
        out->push_back(std::uint8_t((a << 2) | (b >> 4)));
        if (!p2)
            out->push_back(std::uint8_t((b << 4) | (c >> 2)));
        if (!p3)
            out->push_back(std::uint8_t((c << 6) | d));
    }
    return true;
}

bool privateEnvelope(const Bytes &data) {
    if (data.empty())
        return false;
    Reader all{{data.data(), data.size()}};
    View seq, version, alg, oid, key, extra;
    if (!all.get(0x30, &seq) || !all.empty())
        return false;
    Reader r{seq};
    std::uint64_t v;
    if (!r.get(2, &version) || !integer(version, &v) || v > 1 || !r.get(0x30, &alg))
        return false;
    Reader a{alg};
    if (!a.get(6, &oid) || !oid.n)
        return false;
    if (!a.empty()) {
        if ((a.v.p[0] & 31) == 31 || !a.get(a.v.p[0], &extra) || !a.empty())
            return false;
    }
    if (!r.get(4, &key) || !key.n)
        return false;
    if (!r.empty() && r.v.p[0] == 0xa0 && !r.get(0xa0, &extra))
        return false;
    if (!r.empty() && r.v.p[0] == 0x81) {
        if (v != 1 || !r.get(0x81, &extra) || extra.n < 2 || extra.p[0] > 7 ||
            (extra.p[0] && (extra.p[extra.n - 1] & ((1U << extra.p[0]) - 1))))
            return false;
    }
    return r.empty();
}

TibxEncryptedPemResult decrypt(const Bytes &pem, const Bytes &pass, Bytes *output) {
    static const unsigned char pbes2[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 1, 5, 13};
    static const unsigned char pbkdf2[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 1, 5, 12};
    static const unsigned char hmac[] = {0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 2};
    static const unsigned char aes[] = {0x60, 0x86, 0x48, 1, 0x65, 3, 4, 1};
    Bytes encoded;
    Guard encGuard(encoded);
    if (!decodePem(pem, &encoded) || encoded.empty())
        return TibxEncryptedPemResult::InvalidEncoding;
    Reader root{{encoded.data(), encoded.size()}};
    View outer, aid, oid, params, ciphertext, kdf, scheme, kparams, salt, num, iv;
    if (!root.get(0x30, &outer) || !root.empty())
        return TibxEncryptedPemResult::InvalidEncoding;
    Reader r{outer};
    if (!r.get(0x30, &aid) || !r.get(4, &ciphertext) || !r.empty())
        return TibxEncryptedPemResult::InvalidEncoding;
    Reader a{aid};
    if (!a.get(6, &oid))
        return TibxEncryptedPemResult::InvalidEncoding;
    if (!eq(oid, pbes2, sizeof(pbes2)))
        return TibxEncryptedPemResult::Unsupported;
    if (!a.get(0x30, &params) || !a.empty())
        return TibxEncryptedPemResult::InvalidEncoding;
    Reader p{params};
    if (!p.get(0x30, &kdf) || !p.get(0x30, &scheme) || !p.empty())
        return TibxEncryptedPemResult::InvalidEncoding;
    Reader k{kdf};
    if (!k.get(6, &oid))
        return TibxEncryptedPemResult::InvalidEncoding;
    if (!eq(oid, pbkdf2, sizeof(pbkdf2)))
        return TibxEncryptedPemResult::Unsupported;
    if (!k.get(0x30, &kparams) || !k.empty())
        return TibxEncryptedPemResult::InvalidEncoding;
    Reader kp{kparams};
    if (kp.empty())
        return TibxEncryptedPemResult::InvalidEncoding;
    if (kp.v.p[0] != 4)
        return TibxEncryptedPemResult::Unsupported;
    if (!kp.get(4, &salt) || !kp.get(2, &num))
        return TibxEncryptedPemResult::InvalidEncoding;
    std::uint64_t iterations, specified = 0;
    if (!integer(num, &iterations) || !iterations)
        return TibxEncryptedPemResult::InvalidEncoding;
    if (!kp.empty() && kp.v.p[0] == 2) {
        if (!kp.get(2, &num) || !integer(num, &specified) || !specified)
            return TibxEncryptedPemResult::InvalidEncoding;
    }
    QCryptographicHash::Algorithm prf = QCryptographicHash::Sha1;
    if (!kp.empty()) {
        View prfSeq, nullParam;
        if (!kp.get(0x30, &prfSeq) || !kp.empty())
            return TibxEncryptedPemResult::InvalidEncoding;
        Reader pr{prfSeq};
        if (!pr.get(6, &oid))
            return TibxEncryptedPemResult::InvalidEncoding;
        if (oid.n != sizeof(hmac) + 1 || std::memcmp(oid.p, hmac, sizeof(hmac)))
            return TibxEncryptedPemResult::Unsupported;
        switch (oid.p[sizeof(hmac)]) {
        case 7:
            prf = QCryptographicHash::Sha1;
            break;
        case 8:
            prf = QCryptographicHash::Sha224;
            break;
        case 9:
            prf = QCryptographicHash::Sha256;
            break;
        case 10:
            prf = QCryptographicHash::Sha384;
            break;
        case 11:
            prf = QCryptographicHash::Sha512;
            break;
        default:
            return TibxEncryptedPemResult::Unsupported;
        }
        if (!pr.empty() && (!pr.get(5, &nullParam) || nullParam.n || !pr.empty()))
            return TibxEncryptedPemResult::InvalidEncoding;
    }
    Reader s{scheme};
    if (!s.get(6, &oid))
        return TibxEncryptedPemResult::InvalidEncoding;
    if (oid.n != sizeof(aes) + 1 || std::memcmp(oid.p, aes, sizeof(aes)))
        return TibxEncryptedPemResult::Unsupported;
    std::size_t keySize = oid.p[sizeof(aes)] == 2    ? 16
                          : oid.p[sizeof(aes)] == 22 ? 24
                          : oid.p[sizeof(aes)] == 42 ? 32
                                                     : 0;
    if (!keySize)
        return TibxEncryptedPemResult::Unsupported;
    if (!s.get(4, &iv) || !s.empty() || iv.n != 16 || !ciphertext.n || ciphertext.n % 16 ||
        (specified && specified != keySize))
        return TibxEncryptedPemResult::InvalidEncoding;
    if (pass.size() > std::size_t(std::numeric_limits<int>::max()) ||
        salt.n > std::size_t(std::numeric_limits<int>::max()) - 4)
        return TibxEncryptedPemResult::Unsupported;
    const std::size_t digestSize = static_cast<std::size_t>(QCryptographicHash::hashLength(prf));
    const std::size_t blocks = (keySize + digestSize - 1) / digestSize;
    const std::uint64_t maxHmacEvaluations = 1000000;
    if (iterations > maxHmacEvaluations / blocks)
        return TibxEncryptedPemResult::WorkLimitExceeded;
    QByteArray password(reinterpret_cast<const char *>(pass.data()), int(pass.size()));
    QtGuard pg(password);
    QByteArray initial(reinterpret_cast<const char *>(salt.p), int(salt.n));
    QtGuard ig(initial);
    Bytes derived;
    Guard dg(derived);
    derived.reserve(keySize);
    for (unsigned block = 1; derived.size() < keySize; ++block) {
        initial.append(char(block >> 24));
        initial.append(char(block >> 16));
        initial.append(char(block >> 8));
        initial.append(char(block));
        QByteArray u = QMessageAuthenticationCode::hash(initial, password, prf);
        QtGuard ug(u);
        QByteArray t = u;
        t.detach();
        QtGuard tg(t);
        initial.chop(4);
        for (std::uint64_t j = 1; j < iterations; ++j) {
            QByteArray next = QMessageAuthenticationCode::hash(u, password, prf);
            if (!u.isEmpty())
                wipe(u.data(), std::size_t(u.size()));
            u = next;
            next.clear();
            for (int i = 0; i < t.size(); ++i)
                t[i] = char(static_cast<unsigned char>(t[i]) ^ static_cast<unsigned char>(u[i]));
        }
        const std::size_t count = std::min(keySize - derived.size(), std::size_t(t.size()));
        derived.insert(derived.end(), reinterpret_cast<const std::uint8_t *>(t.constData()),
                       reinterpret_cast<const std::uint8_t *>(t.constData()) + count);
    }
    compression::initializeAesTables();
    alignas(16) UInt32 state[AES_NUM_IVMRK_WORDS] = {};
    AesGuard ag(state);
    Aes_SetKey_Dec(state + 4, derived.data(), unsigned(keySize));
    AesCbc_Init(state, iv.p);
    Bytes plain(ciphertext.p, ciphertext.p + ciphertext.n);
    Guard plainGuard(plain);
    alignas(16) Byte block[16] = {};
    for (std::size_t pos = 0; pos < plain.size(); pos += sizeof(block)) {
        std::memcpy(block, plain.data() + pos, sizeof(block));
        AesCbc_Decode(state, block, 1);
        std::memcpy(plain.data() + pos, block, sizeof(block));
    }
    wipe(block, sizeof(block));
    const unsigned pad = plain.back();
    unsigned bad = (pad == 0 || pad > 16);
    for (unsigned i = 1; i <= 16; ++i)
        if (i <= pad)
            bad |= plain[plain.size() - i] ^ pad;
    if (bad)
        return TibxEncryptedPemResult::IncorrectPassphraseOrCorrupt;
    plain.resize(plain.size() - pad);
    if (!privateEnvelope(plain))
        return TibxEncryptedPemResult::IncorrectPassphraseOrCorrupt;
    output->swap(plain);
    return TibxEncryptedPemResult::Success;
}
} // namespace

TibxEncryptedPemResult tibxDecryptEncryptedPem(const Bytes &pem, bool present,
                                               const Bytes &passphrase, Bytes *der) {
    if (!der)
        return TibxEncryptedPemResult::InvalidArgument;
    if (der == &pem || der == &passphrase) {
        clear(der);
        return TibxEncryptedPemResult::InvalidArgument;
    }
    clear(der);
    if (!present)
        return TibxEncryptedPemResult::PassphraseRequired;
    try {
        return decrypt(pem, passphrase, der);
    } catch (const std::bad_alloc &) {
        clear(der);
        return TibxEncryptedPemResult::InvalidArgument;
    } catch (...) {
        clear(der);
        return TibxEncryptedPemResult::InvalidEncoding;
    }
}
} // namespace fs
} // namespace peare
