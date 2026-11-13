#include <kapala/tls.h>

#include <kapala/sys.h>

extern "C" {
void* memcpy(void* dest, void const* src, unsigned long n);
void* memmove(void* dest, void const* src, unsigned long n);
}

namespace Kapala {

namespace C = Crypto;
using u8 = unsigned char;
using u64 = unsigned long;

namespace {

constexpr u64 MAX_RECORD = 16384 + 256; // ciphertext limit
constexpr u64 IN_CAPACITY = 5 + MAX_RECORD + 64;
constexpr u64 HANDSHAKE_CAPACITY = 64 * 1024;

constexpr u8 CHANGE_CIPHER_SPEC = 20, ALERT = 21, HANDSHAKE = 22, APPLICATION_DATA = 23;
constexpr u8 CLIENT_HELLO = 1, SERVER_HELLO = 2, NEW_SESSION_TICKET = 4, ENCRYPTED_EXTENSIONS = 8,
             CERTIFICATE = 11, CERTIFICATE_REQUEST = 13, CERTIFICATE_VERIFY = 15, FINISHED = 20, KEY_UPDATE = 24;
constexpr unsigned short AES_128_GCM = 0x1301, CHACHA20_POLY1305 = 0x1303;

// Hello Retry Request is a ServerHello with this random.
constexpr u8 RETRY_RANDOM[32] = {0xcf, 0x21, 0xad, 0x74, 0xe5, 0x9a, 0x61, 0x11, 0xbe, 0x1d, 0x8c,
                                  0x02, 0x1e, 0x65, 0xb8, 0x91, 0xc2, 0xa2, 0x11, 0x16, 0x7a, 0xbb,
                                  0x8c, 0x5e, 0x07, 0x9e, 0x09, 0xe2, 0xc8, 0xa8, 0x33, 0x9c};

unsigned short get16(u8 const* p) { return static_cast<unsigned short>(p[0] << 8 | p[1]); }
unsigned get24(u8 const* p) { return unsigned{p[0]} << 16 | unsigned{p[1]} << 8 | p[2]; }

// A message being built, with room for length prefixes filled in later.
struct Writer {
    u8* data;
    u64 length = 0;

    void u8_(u8 v) { data[length++] = v; }
    void u16(unsigned v) {
        u8_(static_cast<u8>(v >> 8));
        u8_(static_cast<u8>(v));
    }
    void bytes(void const* p, u64 n) {
        memcpy(data + length, p, n);
        length += n;
    }
    u64 begin(int size) { // a length prefix of `size` bytes, filled by end()
        u64 at = length;
        length += static_cast<u64>(size);
        return at;
    }
    void end(u64 at, int size) {
        u64 n = length - at - static_cast<u64>(size);
        for (int i = size - 1; i >= 0; i--) {
            data[at + static_cast<u64>(i)] = static_cast<u8>(n);
            n >>= 8;
        }
    }
};

// HKDF-Expand-Label (RFC 8446 7.1).
void expandLabel(u8 const secret[32], char const* label, u8 const* context, u64 contextLength, u8* out, u64 length) {
    u8 info[2 + 1 + 6 + 32 + 1 + 64];
    u64 n = 0;
    info[n++] = static_cast<u8>(length >> 8);
    info[n++] = static_cast<u8>(length);
    u64 labelLength = 0;
    while (label[labelLength])
        labelLength++;
    info[n++] = static_cast<u8>(6 + labelLength);
    memcpy(info + n, "tls13 ", 6);
    n += 6;
    memcpy(info + n, label, labelLength);
    n += labelLength;
    info[n++] = static_cast<u8>(contextLength);
    memcpy(info + n, context, contextLength);
    n += contextLength;
    C::hkdfExpand(secret, info, n, out, length);
}

void deriveSecret(u8 const secret[32], char const* label, u8 const hash[32], u8 out[32]) {
    expandLabel(secret, label, hash, 32, out, 32);
}

char const* alertText(u8 code) {
    switch (code) {
    case 40: return "the server refused the handshake (handshake_failure)";
    case 42: case 43: case 44: case 45: case 46: return "the server rejected a certificate";
    case 47: return "the server found an illegal parameter";
    case 70: return "the server doesn't speak TLS 1.3 (protocol_version)";
    case 71: return "the server wants stronger security (insufficient_security)";
    case 80: return "the server had an internal error";
    case 112: return "the server doesn't know this site name (unrecognized_name)";
    default: return "the server sent an alert";
    }
}

} // namespace

Tls::~Tls() {
    C::wipe(&_read, sizeof(_read));
    C::wipe(&_write, sizeof(_write));
    C::wipe(_readSecret, sizeof(_readSecret));
    delete[] _in;
    delete[] _plain;
    delete[] _handshake;
    delete[] _chainData;
}

long Tls::fail(char const* why, long code) {
    _error = why;
    return code;
}

// MARK: Records

long Tls::sendRecord(u8 type, u8 const* data, u64 length) {
    static u8 record[5 + 16384 + 1 + 16];
    while (length) {
        u64 n = length < 16384 ? length : 16384;
        u8* header = record;
        header[0] = _encrypted ? APPLICATION_DATA : type;
        header[1] = 3;
        header[2] = 3;
        u64 total = n;
        if (_encrypted) {
            u8 inner[16384 + 1];
            memcpy(inner, data, n);
            inner[n] = type;
            total = n + 1 + 16;
            header[3] = static_cast<u8>(total >> 8);
            header[4] = static_cast<u8>(total);
            u8 nonce[12];
            memcpy(nonce, _write.iv, 12);
            for (int i = 0; i < 8; i++)
                nonce[4 + i] ^= static_cast<u8>(_write.sequence >> (56 - 8 * i));
            _write.sequence++;
            if (_aes)
                C::aesGcmSeal(_write.key, nonce, header, 5, inner, n + 1, record + 5);
            else
                C::chachaPolySeal(_write.key, nonce, header, 5, inner, n + 1, record + 5);
        } else {
            header[3] = static_cast<u8>(n >> 8);
            header[4] = static_cast<u8>(n);
            memcpy(record + 5, data, n);
        }
        long r = _net->send(_socket, record, 5 + total);
        if (r < 0)
            return r;
        data += n;
        length -= n;
    }
    return Sys::OK;
}

// Reads one whole record from the connection (data points into _in, valid
// until the next call).
long Tls::readRecord(u8& type, u8*& data, u64& length, unsigned timeoutMs) {
    if (_inConsumed) { // drop the previous record
        memmove(_in, _in + _inConsumed, _inLength - _inConsumed);
        _inLength -= _inConsumed;
        _inConsumed = 0;
    }
    for (;;) {
        if (_inLength >= 5) {
            u64 recordLength = get16(_in + 3);
            if (recordLength > MAX_RECORD)
                return fail("the server sent a record that's too big", Sys::INVALID_ARGUMENT);
            if (_inLength >= 5 + recordLength) {
                type = _in[0];
                data = _in + 5;
                length = recordLength;
                _inConsumed = 5 + recordLength;
                return Sys::OK;
            }
        }
        long n = _net->receive(_socket, _in + _inLength, IN_CAPACITY - _inLength, timeoutMs);
        if (n < 0)
            return fail("the connection failed", n);
        if (n == 0)
            return fail("the server closed the connection", Sys::PEER_CLOSED);
        _inLength += static_cast<u64>(n);
    }
}

void Tls::setKeys(Direction& d, u8 const secret[32]) {
    expandLabel(secret, "key", nullptr, 0, d.key, _aes ? 16 : 32);
    expandLabel(secret, "iv", nullptr, 0, d.iv, 12);
    d.sequence = 0;
}

// Decrypts a record in place; `type` becomes the real content type.
bool Tls::decrypt(Direction& d, u8 const* header, u8* data, u64& length, u8& type) {
    if (length < 17)
        return false;
    u8 nonce[12];
    memcpy(nonce, d.iv, 12);
    for (int i = 0; i < 8; i++)
        nonce[4 + i] ^= static_cast<u8>(d.sequence >> (56 - 8 * i));
    d.sequence++;
    u64 plainLength = length - 16;
    bool ok = _aes ? C::aesGcmOpen(d.key, nonce, header, 5, data, plainLength, data)
                   : C::chachaPolyOpen(d.key, nonce, header, 5, data, plainLength, data);
    if (not ok)
        return false;
    // The content type is the last non-zero byte (zeros after it are padding).
    while (plainLength and data[plainLength - 1] == 0)
        plainLength--;
    if (plainLength == 0)
        return false;
    type = data[--plainLength];
    length = plainLength;
    return true;
}

// The next handshake message (collecting it across records if needed).
long Tls::nextHandshake(u8& type, u8*& body, u64& length) {
    if (_handshakeConsumed) {
        memmove(_handshake, _handshake + _handshakeConsumed, _handshakeLength - _handshakeConsumed);
        _handshakeLength -= _handshakeConsumed;
        _handshakeConsumed = 0;
    }
    for (;;) {
        if (_handshakeLength >= 4) {
            u64 n = get24(_handshake + 1);
            if (n + 4 > HANDSHAKE_CAPACITY)
                return fail("a handshake message is too big", Sys::INVALID_ARGUMENT);
            if (_handshakeLength >= 4 + n) {
                type = _handshake[0];
                body = _handshake;
                length = 4 + n; // the whole message, header included
                _handshakeConsumed = 4 + n;
                return Sys::OK;
            }
        }
        u8 recordType;
        u8* data;
        u64 recordLength;
        long r = readRecord(recordType, data, recordLength, 15000);
        if (r < 0)
            return r;
        if (recordType == CHANGE_CIPHER_SPEC)
            continue; // sent only for old middleboxes
        if (_encrypted and recordType == APPLICATION_DATA) {
            if (not decrypt(_read, data - 5, data, recordLength, recordType))
                return fail("a record from the server didn't decrypt", Sys::INVALID_ARGUMENT);
        }
        if (recordType == ALERT)
            return fail(recordLength >= 2 ? alertText(data[1]) : "alert", Sys::CONNECTION_RESET);
        if (recordType != HANDSHAKE)
            return fail("the server sent something unexpected", Sys::INVALID_ARGUMENT);
        if (_handshakeLength + recordLength > HANDSHAKE_CAPACITY)
            return fail("the handshake is too big", Sys::INVALID_ARGUMENT);
        memcpy(_handshake + _handshakeLength, data, recordLength);
        _handshakeLength += recordLength;
    }
}

// MARK: Handshake

long Tls::connect(Net& net, long socket, char const* host) {
    _net = &net;
    _socket = socket;
    _in = new u8[IN_CAPACITY];
    _plain = new u8[MAX_RECORD];
    _handshake = new u8[HANDSHAKE_CAPACITY];
    if (not _in or not _plain or not _handshake)
        return fail("out of memory", Sys::OUT_OF_MEMORY);

    u8 secretKey[32], publicKey[32], random[32], sessionId[32];
    Sys::random(secretKey, 32);
    Sys::random(random, 32);
    Sys::random(sessionId, 32);
    C::x25519Base(publicKey, secretKey);

    // ClientHello
    u8 hello[512];
    Writer w{hello};
    w.u8_(CLIENT_HELLO);
    u64 helloLength = w.begin(3);
    w.u16(0x0303); // "TLS 1.2" here; the real version is in supported_versions
    w.bytes(random, 32);
    w.u8_(32);
    w.bytes(sessionId, 32);
    w.u16(4);
    w.u16(CHACHA20_POLY1305);
    w.u16(AES_128_GCM);
    w.u8_(1);
    w.u8_(0); // no compression
    u64 extensions = w.begin(2);
    { // server_name
        u64 hostLength = 0;
        while (host[hostLength])
            hostLength++;
        w.u16(0);
        u64 e = w.begin(2);
        u64 list = w.begin(2);
        w.u8_(0);
        w.u16(static_cast<unsigned>(hostLength));
        w.bytes(host, hostLength);
        w.end(list, 2);
        w.end(e, 2);
    }
    w.u16(43); // supported_versions: TLS 1.3
    w.u16(3);
    w.u8_(2);
    w.u16(0x0304);
    w.u16(10); // supported_groups: x25519
    w.u16(4);
    w.u16(2);
    w.u16(0x001d);
    { // signature_algorithms (what certificates may be signed with)
        static constexpr unsigned short SCHEMES[] = {0x0403, 0x0804, 0x0401, 0x0503, 0x0805,
                                                     0x0501, 0x0806, 0x0601, 0x0807};
        w.u16(13);
        u64 e = w.begin(2);
        u64 list = w.begin(2);
        for (unsigned short s : SCHEMES)
            w.u16(s);
        w.end(list, 2);
        w.end(e, 2);
    }
    w.u16(51); // key_share: our X25519 public key
    w.u16(2 + 2 + 2 + 32);
    w.u16(2 + 2 + 32);
    w.u16(0x001d);
    w.u16(32);
    w.bytes(publicKey, 32);
    w.u16(16); // ALPN: http/1.1
    w.u16(2 + 1 + 8);
    w.u16(1 + 8);
    w.u8_(8);
    w.bytes("http/1.1", 8);
    w.u16(45); // psk_key_exchange_modes (expected by some servers)
    w.u16(2);
    w.u8_(1);
    w.u8_(1);
    w.end(extensions, 2);
    w.end(helloLength, 3);

    C::Sha256 transcript;
    transcript.update(hello, w.length);
    long r = sendRecord(HANDSHAKE, hello, w.length);
    if (r < 0)
        return fail("couldn't send the hello", r);

    // ServerHello
    u8 type;
    u8* message;
    u64 length;
    r = nextHandshake(type, message, length);
    if (r < 0)
        return r;
    if (type != SERVER_HELLO or length < 4 + 2 + 32 + 1)
        return fail("the server didn't answer with a ServerHello", Sys::INVALID_ARGUMENT);
    u8 const* p = message + 4;
    u8 const* end = message + length;
    if (C::equal(p + 2, RETRY_RANDOM, 32))
        return fail("the server wants a different key exchange (HelloRetryRequest)", Sys::INVALID_ARGUMENT);
    p += 2 + 32;
    p += 1 + p[0]; // session id
    if (p + 5 > end)
        return fail("the ServerHello is cut short", Sys::INVALID_ARGUMENT);
    unsigned short suite = get16(p);
    p += 3; // suite and compression
    u8 serverKey[32];
    bool haveKey = false, tls13 = false;
    u64 extLength = get16(p);
    p += 2;
    u8 const* extEnd = p + extLength <= end ? p + extLength : end;
    while (p + 4 <= extEnd) {
        unsigned short ext = get16(p), n = get16(p + 2);
        u8 const* v = p + 4;
        if (v + n > extEnd)
            break;
        if (ext == 43 and n == 2 and get16(v) == 0x0304)
            tls13 = true;
        if (ext == 51 and n == 36 and get16(v) == 0x001d and get16(v + 2) == 32) {
            memcpy(serverKey, v + 4, 32);
            haveKey = true;
        }
        p = v + n;
    }
    if (not tls13)
        return fail("the server only speaks an older TLS (1.2 or below)", Sys::INVALID_ARGUMENT);
    if (not haveKey)
        return fail("the server didn't send an X25519 key", Sys::INVALID_ARGUMENT);
    if (suite != AES_128_GCM and suite != CHACHA20_POLY1305)
        return fail("the server chose a cipher we don't have", Sys::INVALID_ARGUMENT);
    _aes = suite == AES_128_GCM;
    _cipher = _aes ? "TLS 1.3, AES-128-GCM" : "TLS 1.3, ChaCha20-Poly1305";
    transcript.update(message, length);

    // Handshake secrets.
    u8 shared[32];
    C::x25519(shared, secretKey, serverKey);
    C::wipe(secretKey, 32);
    u8 zeros[32] = {};
    if (C::equal(shared, zeros, 32))
        return fail("the server's key is invalid", Sys::INVALID_ARGUMENT);
    u8 emptyHash[32], early[32], derived[32], handshakeSecret[32], hash[32];
    C::Sha256::hash(nullptr, 0, emptyHash);
    C::hkdfExtract(nullptr, 0, zeros, 32, early);
    deriveSecret(early, "derived", emptyHash, derived);
    C::hkdfExtract(derived, 32, shared, 32, handshakeSecret);
    C::wipe(shared, 32);
    transcript.peek(hash);
    u8 clientHs[32], serverHs[32];
    deriveSecret(handshakeSecret, "c hs traffic", hash, clientHs);
    deriveSecret(handshakeSecret, "s hs traffic", hash, serverHs);
    setKeys(_read, serverHs);
    _encrypted = true; // from now on the server's records are encrypted

    // EncryptedExtensions, Certificate, CertificateVerify, Finished.
    for (;;) {
        r = nextHandshake(type, message, length);
        if (r < 0)
            return r;
        if (type == CERTIFICATE) {
            if (long bad = checkCertificates(message, length, host))
                return bad;
        } else if (type == CERTIFICATE_VERIFY) {
            // Signed: 64 spaces, a label, 0, and the transcript hash so far.
            transcript.peek(hash);
            static constexpr char LABEL[] = "TLS 1.3, server CertificateVerify";
            u8 content[64 + sizeof(LABEL) + 32];
            for (int i = 0; i < 64; i++)
                content[i] = ' ';
            for (unsigned long i = 0; i < sizeof(LABEL); i++)
                content[64 + i] = static_cast<u8>(LABEL[i]); // the final 0 included
            for (int i = 0; i < 32; i++)
                content[64 + sizeof(LABEL) + i] = hash[i];
            if (_chain.count == 0 or length < 8 or 8 + get16(message + 6) != length)
                return fail("the server's CertificateVerify is malformed", Sys::INVALID_ARGUMENT);
            if (char const* why = X509::verifyHandshakeSignature(_chain.certs[0], _chain.lengths[0], get16(message + 4),
                                                                 message + 8, get16(message + 6), content,
                                                                 sizeof(content)))
                return fail(why, Sys::ACCESS_DENIED);
            _verified = true;
        }
        if (type == FINISHED) {
            if (not _verified)
                return fail("the server didn't prove who it is (no certificate)", Sys::ACCESS_DENIED);
            u8 finishedKey[32], expected[32];
            expandLabel(serverHs, "finished", nullptr, 0, finishedKey, 32);
            transcript.peek(hash);
            C::hmacSha256(finishedKey, 32, hash, 32, expected);
            if (length != 4 + 32 or not C::equal(expected, message + 4, 32))
                return fail("the server's Finished message doesn't match: the handshake was tampered with",
                            Sys::INVALID_ARGUMENT);
            transcript.update(message, length);
            break;
        }
        if (type == CERTIFICATE_REQUEST)
            return fail("the server wants a client certificate", Sys::INVALID_ARGUMENT);
        if (type != ENCRYPTED_EXTENSIONS and type != CERTIFICATE and type != CERTIFICATE_VERIFY)
            return fail("the server sent an unexpected handshake message", Sys::INVALID_ARGUMENT);
        transcript.update(message, length);
    }

    // Application secrets (from the transcript up to the server's Finished).
    u8 master[32], clientAp[32];
    transcript.peek(hash);
    deriveSecret(handshakeSecret, "derived", emptyHash, derived);
    C::hkdfExtract(derived, 32, zeros, 32, master);
    deriveSecret(master, "c ap traffic", hash, clientAp);
    deriveSecret(master, "s ap traffic", hash, _readSecret);

    // Our Finished, under our handshake key.
    u8 ccs = 1;
    _encrypted = false;
    sendRecord(CHANGE_CIPHER_SPEC, &ccs, 1);
    _encrypted = true;
    setKeys(_write, clientHs);
    u8 finished[4 + 32] = {FINISHED, 0, 0, 32};
    u8 finishedKey[32];
    expandLabel(clientHs, "finished", nullptr, 0, finishedKey, 32);
    C::hmacSha256(finishedKey, 32, hash, 32, finished + 4);
    r = sendRecord(HANDSHAKE, finished, sizeof(finished));
    if (r < 0)
        return fail("couldn't send Finished", r);

    setKeys(_write, clientAp);
    setKeys(_read, _readSecret);
    C::wipe(handshakeSecret, 32);
    C::wipe(master, 32);
    C::wipe(clientAp, 32);
    C::wipe(clientHs, 32);
    C::wipe(serverHs, 32);
    return Sys::OK;
}

// Copies the server's certificates out of its Certificate message (the
// handshake buffer is reused) and checks them against `host`.
long Tls::checkCertificates(u8 const* message, u64 length, char const* host) {
    delete[] _chainData;
    _chainData = new u8[length];
    if (not _chainData)
        return fail("out of memory", Sys::OUT_OF_MEMORY);
    for (u64 i = 0; i < length; i++)
        _chainData[i] = message[i];
    u8 const* p = _chainData + 4;
    u8 const* end = _chainData + length;
    if (p >= end or p + 1 + p[0] + 3 > end)
        return fail("the server's Certificate message is malformed", Sys::INVALID_ARGUMENT);
    p += 1 + p[0]; // request context
    u8 const* listEnd = p + 3 + get24(p);
    p += 3;
    if (listEnd != end)
        return fail("the server's Certificate message is malformed", Sys::INVALID_ARGUMENT);
    _chain.count = 0;
    while (p < listEnd) {
        if (p + 3 > listEnd)
            return fail("the server's Certificate message is malformed", Sys::INVALID_ARGUMENT);
        u64 n = get24(p);
        p += 3;
        if (p + n + 2 > listEnd)
            return fail("the server's Certificate message is malformed", Sys::INVALID_ARGUMENT);
        if (_chain.count < X509::MAX_CHAIN) {
            _chain.certs[_chain.count] = p;
            _chain.lengths[_chain.count++] = n;
        }
        p += n;
        p += 2 + get16(p); // extensions
    }
    Sys::SystemInfo info{};
    Sys::systemInfo(info);
    static auto const* roots = static_cast<u8 const*>(Sys::openFile("roots"));
    if (not roots)
        return fail("this system has no trusted roots (roots.bin)", Sys::NOT_FOUND);
    if (char const* why = X509::verifyChain(_chain, host, info.unixTime, roots))
        return fail(why, Sys::ACCESS_DENIED);
    return Sys::OK;
}

// MARK: Application data

long Tls::send(void const* data, u64 length) {
    if (not _encrypted or _closed)
        return Sys::PEER_CLOSED;
    long r = sendRecord(APPLICATION_DATA, static_cast<u8 const*>(data), length);
    return r < 0 ? r : static_cast<long>(length);
}

long Tls::receive(void* data, u64 capacity, unsigned timeoutMs) {
    while (_plainLength == 0) {
        if (_closed)
            return 0;
        u8 type;
        u8* record;
        u64 length;
        long r = readRecord(type, record, length, timeoutMs);
        if (r == Sys::PEER_CLOSED) {
            _closed = true; // (a server closing without close_notify)
            return 0;
        }
        if (r < 0)
            return r;
        if (type == CHANGE_CIPHER_SPEC)
            continue;
        if (type != APPLICATION_DATA or not decrypt(_read, record - 5, record, length, type))
            return fail("a record from the server didn't decrypt", Sys::INVALID_ARGUMENT);
        if (type == ALERT) {
            _closed = true;
            if (length >= 2 and record[1] == 0) // close_notify
                return 0;
            return fail(length >= 2 ? alertText(record[1]) : "alert", Sys::CONNECTION_RESET);
        }
        if (type == HANDSHAKE) {
            // After the handshake: session tickets (ignored) and key updates.
            u64 at = 0;
            while (at + 4 <= length) {
                u64 n = get24(record + at + 1);
                if (record[at] == KEY_UPDATE) {
                    u8 next[32];
                    expandLabel(_readSecret, "traffic upd", nullptr, 0, next, 32);
                    memcpy(_readSecret, next, 32);
                    setKeys(_read, _readSecret);
                }
                (void)NEW_SESSION_TICKET;
                at += 4 + n;
            }
            continue;
        }
        if (type != APPLICATION_DATA)
            continue;
        memcpy(_plain, record, length);
        _plainStart = 0;
        _plainLength = length;
    }
    u64 n = _plainLength < capacity ? _plainLength : capacity;
    memcpy(data, _plain + _plainStart, n);
    _plainStart += n;
    _plainLength -= n;
    return static_cast<long>(n);
}

void Tls::close() {
    if (_net and _socket >= 0) {
        if (_encrypted and not _closed) {
            u8 notify[2] = {1, 0}; // warning, close_notify
            sendRecord(ALERT, notify, 2);
        }
        _net->close(_socket);
    }
    _socket = -1;
    _closed = true;
}

} // namespace Kapala
