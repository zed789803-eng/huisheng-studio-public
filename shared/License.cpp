#include "License.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sys/stat.h>

namespace hs::license {

namespace {

// ---- SHA-256 --------------------------------------------------------------
inline uint32_t rotr(uint32_t value, int bits)
{
    return (value >> bits) | (value << (32 - bits));
}

const uint32_t kRound[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u,
    0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u,
    0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
    0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au,
    0x5b9cca4fu, 0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

struct Sha256
{
    uint32_t state[8] = { 0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                          0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u };
    uint8_t  buffer[64] = {};
    size_t   buffered = 0;
    uint64_t total = 0;

    void transform(const uint8_t* block)
    {
        uint32_t w[64];

        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16)
                 | (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);

        for (int i = 16; i < 64; ++i)
        {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

        for (int i = 0; i < 64; ++i)
        {
            const uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = h + s1 + ch + kRound[i] + w[i];
            const uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = s0 + maj;

            h = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }

        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
        state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }

    void update(const uint8_t* data, size_t size)
    {
        total += size;

        while (size > 0)
        {
            const size_t take = std::min(size, size_t(64) - buffered);
            std::memcpy(buffer + buffered, data, take);
            buffered += take;
            data += take;
            size -= take;

            if (buffered == 64)
            {
                transform(buffer);
                buffered = 0;
            }
        }
    }

    void finish(uint8_t out[32])
    {
        const uint64_t bits = total * 8;
        const uint8_t one = 0x80;
        const uint8_t zero = 0x00;

        update(&one, 1);

        while (buffered != 56)
            update(&zero, 1);

        uint8_t length[8];

        for (int i = 0; i < 8; ++i)
            length[i] = uint8_t((bits >> (56 - i * 8)) & 0xff);

        update(length, 8);

        for (int i = 0; i < 8; ++i)
        {
            out[i * 4]     = uint8_t(state[i] >> 24);
            out[i * 4 + 1] = uint8_t(state[i] >> 16);
            out[i * 4 + 2] = uint8_t(state[i] >> 8);
            out[i * 4 + 3] = uint8_t(state[i]);
        }
    }
};

void sha256(const uint8_t* data, size_t size, uint8_t out[32])
{
    Sha256 hash;
    hash.update(data, size);
    hash.finish(out);
}

void hmacSha256(const uint8_t key[32], const uint8_t* data, size_t size, uint8_t out[32])
{
    uint8_t inner[64];
    uint8_t outer[64];

    // Both pads are the 32 byte key zero-extended to the 64 byte block size and
    // then XORed with the constant; the tail must be 0x36 / 0x5c, not 0x36 for
    // both, or the outer hash runs over the wrong pad.
    for (size_t i = 0; i < 64; ++i)
    {
        const uint8_t k = i < 32 ? key[i] : uint8_t(0);
        inner[i] = uint8_t(k ^ 0x36);
        outer[i] = uint8_t(k ^ 0x5c);
    }

    uint8_t innerHash[32];
    Sha256 first;
    first.update(inner, sizeof(inner));
    first.update(data, size);
    first.finish(innerHash);

    Sha256 second;
    second.update(outer, sizeof(outer));
    second.update(innerHash, sizeof(innerHash));
    second.finish(out);
}

// ---- Crockford Base32 ------------------------------------------------------
const char* kAlphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

std::string base32Encode(const uint8_t* data, size_t size)
{
    std::string out;
    out.reserve((size * 8 + 4) / 5);

    uint32_t value = 0;
    int bits = 0;

    for (size_t i = 0; i < size; ++i)
    {
        value = (value << 8) | data[i];
        bits += 8;

        while (bits >= 5)
        {
            bits -= 5;
            out.push_back(kAlphabet[(value >> bits) & 31]);
        }
    }

    if (bits > 0)
        out.push_back(kAlphabet[(value << (5 - bits)) & 31]);

    return out;
}

int alphabetIndex(char c)
{
    // Crockford folds the four confusable letters into digits on decode.
    if (c == 'I' || c == 'L') c = '1';
    if (c == 'O') c = '0';

    for (int i = 0; i < 32; ++i)
        if (kAlphabet[i] == c)
            return i;

    return -1;
}

bool base32Decode(const std::string& text, std::string& out)
{
    out.clear();

    uint32_t value = 0;
    int bits = 0;

    for (char raw : text)
    {
        const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(raw)));

        if (c == '-' || c == ' ')
            continue;

        const int index = alphabetIndex(c);

        if (index < 0)
            return false;

        value = (value << 5) | uint32_t(index);
        bits += 5;

        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(char((value >> bits) & 0xff));
        }
    }

    return true;
}

std::string trimmed(const std::string& text)
{
    size_t begin = 0;
    size_t end = text.size();

    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
        ++begin;

    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;

    return text.substr(begin, end - begin);
}

constexpr int kMacBytes = 6;

// payload = 0x01 | 名字长度 | 名字(UTF-8)
std::string buildPayload(const std::string& serial, bool& ok)
{
    ok = serial.size() >= 1 && serial.size() <= 24;

    if (! ok)
        return {};

    std::string payload;
    payload.push_back(char(0x01));
    payload.push_back(char(uint8_t(serial.size())));
    payload += serial;
    return payload;
}

} // namespace

std::string makeCode(const uint8_t key[32], const std::string& serial)
{
    bool ok = false;
    const std::string payload = buildPayload(serial, ok);

    if (! ok)
        return {};

    uint8_t mac[32];
    hmacSha256(key, reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), mac);

    std::string blob = payload;
    blob.append(reinterpret_cast<const char*>(mac), kMacBytes);

    const std::string body = base32Encode(reinterpret_cast<const uint8_t*>(blob.data()), blob.size());

    std::string grouped = "HS1-";

    for (size_t i = 0; i < body.size(); ++i)
    {
        if (i > 0 && i % 4 == 0)
            grouped.push_back('-');
        grouped.push_back(body[i]);
    }

    return grouped;
}

Result verifyCode(const std::string& input, const uint8_t key[32])
{
    Result result;

    std::string text = trimmed(input);

    if (text.empty())
        return result;

    for (char& c : text)
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

    // Accept the code with or without its prefix and with any dash grouping.
    if (text.compare(0, 3, "HS1") == 0)
        text = text.substr(3);

    while (! text.empty() && text.front() == '-')
        text.erase(text.begin());

    std::string blob;

    if (! base32Decode(text, blob))
        return result;

    if (blob.size() < 2 + kMacBytes || blob.size() > 2 + 24 + kMacBytes)
        return result;

    if (static_cast<uint8_t>(blob[0]) != 0x01)
        return result;

    const size_t serialSize = static_cast<uint8_t>(blob[1]);

    if (serialSize < 1 || serialSize > 24 || blob.size() != 2 + serialSize + kMacBytes)
        return result;

    const std::string payload = blob.substr(0, 2 + serialSize);

    uint8_t mac[32];
    hmacSha256(key, reinterpret_cast<const uint8_t*>(payload.data()), payload.size(), mac);

    uint8_t diff = 0;

    for (int i = 0; i < kMacBytes; ++i)
        diff |= uint8_t(mac[i]) ^ uint8_t(blob[2 + serialSize + i]);

    if (diff != 0)
        return result;

    result.valid = true;
    result.serial = payload.substr(2);
    return result;
}

// 产品密钥：不进源码仓库。
//
// 密钥来源只有一个约定的文件 shared/LicenseKey.generated.h，构建前生成：
//   * CI 从仓库密钥（GitHub Secret HS_PRODUCT_KEY，64 位十六进制）生成；
//   * 本地开发自己生成，文件已在 .gitignore 里。
// 生成不了就用 32 个 0 占位：照样编译、界面照样显示"未注册"，但算出来的码
// 跟注册机对不上——这是有意的：源码公开，不等于公开"给任意名字发码"的能力。
#if __has_include("LicenseKey.generated.h")
#include "LicenseKey.generated.h"
#define HS_LICENSE_KEY_INJECTED 1
#else
#define HS_LICENSE_KEY_INJECTED 0
#define HS_LICENSE_KEY_BYTES \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
#endif

namespace {

constexpr uint8_t kKey[32] = { HS_LICENSE_KEY_BYTES };

} // namespace

bool productKeyInjected()
{
    return HS_LICENSE_KEY_INJECTED != 0;
}

const uint8_t* productKey()
{
    return kKey;
}

std::string make(const std::string& serial)
{
    return makeCode(productKey(), serial);
}

Result verify(const std::string& input)
{
    return verifyCode(input, productKey());
}

namespace {

// mkdir -p: on a normal Mac the only missing part is the leaf, but a brand new
// account (or a test that points HOME somewhere fresh) has no parents yet, and
// an activation that silently fails to save would look like a broken code.
bool makeDirs(const std::string& path)
{
    if (path.empty())
        return false;

    for (size_t i = 1; i <= path.size(); ++i)
    {
        if (i != path.size() && path[i] != '/')
            continue;

        ::mkdir(path.substr(0, i).c_str(), 0700);
    }

    struct stat info = {};

    return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

} // namespace

std::string storeDir()
{
    const char* home = std::getenv("HOME");

    if (home == nullptr || *home == '\0')
        return {};

    return std::string(home) + "/Library/Application Support/huishengstudio";
}

std::string storePath()
{
    const std::string dir = storeDir();

    if (dir.empty())
        return {};

    return dir + "/license.txt";
}

std::string storedCode()
{
    const std::string path = storePath();

    if (path.empty())
        return {};

    std::ifstream file(path);

    if (! file)
        return {};

    std::string line;
    std::getline(file, line);
    return trimmed(line);
}

bool storeCode(const std::string& code)
{
    const std::string dir = storeDir();
    const std::string path = storePath();

    if (dir.empty() || path.empty())
        return false;

    if (! makeDirs(dir))
        return false;

    std::ofstream file(path, std::ios::trunc);

    if (! file)
        return false;

    file << trimmed(code) << "\n";
    return file.good();
}

Result current()
{
    return verify(storedCode());
}

Result activate(const std::string& input)
{
    const Result result = verify(input);

    if (result.valid)
        storeCode(trimmed(input));

    return result;
}

} // namespace hs::license
