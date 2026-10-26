#include "crypto.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <random>

#include "constants.h"

namespace gladfs {

namespace {

// Константа XTEA — дробная часть золотого сечения
constexpr uint32_t kDelta = 0x9E3779B9u;
constexpr int      kRounds = 32;

// Слова для «перемешивания» при развёртке ключа
constexpr uint32_t kMixA = 0x9E3779B9u;
constexpr uint32_t kMixB = 0x61C88647u;

// 32-битные слова читаются/пишутся big-endian — так принято в тест-векторах XTEA
uint32_t readBE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

void writeBE32(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

}  // namespace

void xteaEncryptBlock(const XteaKey& key, const uint8_t in[8], uint8_t out[8]) {
    uint32_t v0 = readBE32(in);
    uint32_t v1 = readBE32(in + 4);
    uint32_t sum = 0;
    for (int i = 0; i < kRounds; ++i) {
        v0 += (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + key.w[sum & 3]);
        sum += kDelta;
        v1 += (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + key.w[(sum >> 11) & 3]);
    }
    writeBE32(out, v0);
    writeBE32(out + 4, v1);
}

void xteaDecryptBlock(const XteaKey& key, const uint8_t in[8], uint8_t out[8]) {
    uint32_t v0 = readBE32(in);
    uint32_t v1 = readBE32(in + 4);
    uint32_t sum = kDelta * static_cast<uint32_t>(kRounds);
    for (int i = 0; i < kRounds; ++i) {
        v1 -= (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + key.w[(sum >> 11) & 3]);
        sum -= kDelta;
        v0 -= (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + key.w[sum & 3]);
    }
    writeBE32(out, v0);
    writeBE32(out + 4, v1);
}

void ctrXor(const XteaKey& key, uint32_t nonce, uint32_t blockIndex,
            uint8_t* data, uint32_t len) {
    constexpr uint32_t kSubblocks = BLOCK_SIZE / 8;  // 64-битных блоков в блоке файла
    uint8_t counterBlock[8];
    uint8_t keystream[8];
    for (uint32_t off = 0; off < len; off += 8) {
        writeBE32(counterBlock, nonce);
        writeBE32(counterBlock + 4, blockIndex * kSubblocks + off / 8);
        xteaEncryptBlock(key, counterBlock, keystream);
        uint32_t n = std::min<uint32_t>(8, len - off);
        for (uint32_t i = 0; i < n; ++i) data[off + i] ^= keystream[i];
    }
}

XteaKey deriveKey(const std::string& password, const uint8_t salt[8],
                  uint32_t iterations) {
    // Данные для «прокачки» через цепочку: пароль, дополненный до кратности 8,
    // блок с длиной пароля и соль (чтобы одинаковые пароли с разными солями
    // давали разные ключи).
    std::vector<uint8_t> data(password.begin(), password.end());
    while (data.size() % 8 != 0) data.push_back(0);
    uint8_t lenBlock[8] = {};
    lenBlock[0] = static_cast<uint8_t>(password.size());
    data.insert(data.end(), lenBlock, lenBlock + 8);
    data.insert(data.end(), salt, salt + 8);

    // Состояние цепочки стартует с соли; каждый блок данных шифруется ключом,
    // равным предыдущему состоянию, и XOR-ится с ним
    uint32_t s0 = readBE32(salt);
    uint32_t s1 = readBE32(salt + 4);
    for (uint32_t iter = 0; iter < iterations; ++iter) {
        for (size_t off = 0; off < data.size(); off += 8) {
            XteaKey step = {{s0, s1, s0 ^ kMixA, s1 ^ kMixB}};
            uint8_t blk[8];
            writeBE32(blk, s0 ^ readBE32(data.data() + off));
            writeBE32(blk + 4, s1 ^ readBE32(data.data() + off + 4));
            xteaEncryptBlock(step, blk, blk);
            s0 = readBE32(blk);
            s1 = readBE32(blk + 4);
        }
    }

    // Разворачиваем 64-битное состояние в 128-битный ключ
    XteaKey key = {{s0, s1, s0 ^ kMixA, s1 ^ kMixB}};
    xteaEncryptBlock(key, reinterpret_cast<const uint8_t*>(&key.w[0]),
                     reinterpret_cast<uint8_t*>(&key.w[0]));
    xteaEncryptBlock(key, reinterpret_cast<const uint8_t*>(&key.w[2]),
                     reinterpret_cast<uint8_t*>(&key.w[2]));
    return key;
}

void computeVerifier(const XteaKey& key, uint8_t out[8]) {
    static const uint8_t magic[8] = {'G', 'l', 'a', 'd', 'F', 'S', '!', '\x01'};
    xteaEncryptBlock(key, magic, out);
}

uint32_t crc32(const void* data, size_t len) {
    // Таблица строится один раз при первом вызове
    static uint32_t table[256];
    static bool tableReady = false;
    if (!tableReady) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        tableReady = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < len; ++i)
        crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

uint32_t random32() {
    static std::mt19937 engine(static_cast<uint32_t>(std::time(nullptr)) ^
                               static_cast<uint32_t>(std::clock()) ^ 0x5EED5EEDu);
    return engine();
}

bool xteaSelfTest(std::string& report) {
    // Официальные тест-векторы XTEA из набора mbed TLS (PolarSSL).
    // Ключ и открытый текст загружаются big-endian.
    struct Vector {
        uint8_t key[16];
        uint8_t plain[8];
        uint8_t cipher[8];
    };
    const Vector vectors[] = {
        {{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F},
         {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'},
         {0x49, 0x7D, 0xF3, 0xD0, 0x72, 0x61, 0x2C, 0xB5}},
        {{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F},
         {0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41},
         {0xE7, 0x8F, 0x2D, 0x13, 0x74, 0x43, 0x41, 0xD8}},
        {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
         {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'},
         {0xA0, 0x39, 0x05, 0x89, 0xF8, 0xB8, 0xEF, 0xA5}},
        {{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
         {0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41, 0x41},
         {0xED, 0x23, 0x37, 0x5A, 0x82, 0x1A, 0x8C, 0x2D}},
    };

    report.clear();
    bool ok = true;
    for (size_t i = 0; i < sizeof(vectors) / sizeof(vectors[0]); ++i) {
        const Vector& v = vectors[i];
        // слова ключа загружаются big-endian (как в тест-векторах mbed TLS)
        XteaKey key = {{readBE32(v.key), readBE32(v.key + 4),
                        readBE32(v.key + 8), readBE32(v.key + 12)}};

        uint8_t out[8], back[8];
        xteaEncryptBlock(key, v.plain, out);
        xteaDecryptBlock(key, out, back);
        bool encOk = std::memcmp(out, v.cipher, 8) == 0;
        bool decOk = std::memcmp(back, v.plain, 8) == 0;

        char line[128];
        std::snprintf(line, sizeof(line), "вектор %d: шифрование %s, расшифровка %s",
                      static_cast<int>(i + 1), encOk ? "верно" : "ОШИБКА",
                      decOk ? "верно" : "ОШИБКА");
        report += line;
        report += "\n";
        ok = ok && encOk && decOk;
    }

    // Проверка CRC32 по известному вектору (значение для "123456789")
    static const char kCrcInput[] = "123456789";
    bool crcOk = crc32(kCrcInput, 9) == 0xCBF43926u;
    report += std::string("CRC32 вектор: ") + (crcOk ? "верно" : "ОШИБКА") + "\n";
    return ok && crcOk;
}

}  // namespace gladfs
