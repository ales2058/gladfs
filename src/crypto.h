#pragma once
// ============================================================================
//  Криптографическая основа GladFS.
//
//  XTEA — учебный блочный шифр (блок 64 бита, ключ 128 бит, 32 раунда),
//  режим CTR: гамма = XTEA(nonce_файла, номер_блока). CTR выбран потому,
//  что позволяет шифровать каждый блок данных независимо — как и требуется
//  файловой системе с блочным доступом.
//
//  Ключ выводится из пароля тома функцией deriveKey (соль + цепочка на XTEA,
//  много итераций — усложняет перебор пароля).
// ============================================================================

#include <cstdint>
#include <string>

namespace gladfs {

// 128-битный ключ XTEA (четыре 32-битных слова)
struct XteaKey {
    uint32_t w[4];
};

// Шифрование/расшифровка одного 64-битного блока
void xteaEncryptBlock(const XteaKey& key, const uint8_t in[8], uint8_t out[8]);
void xteaDecryptBlock(const XteaKey& key, const uint8_t in[8], uint8_t out[8]);

// Наложение гаммы CTR на данные одного блока файла (длина до 512 байт).
// blockIndex — номер 512-байтного блока внутри файла.
void ctrXor(const XteaKey& key, uint32_t nonce, uint32_t blockIndex,
            uint8_t* data, uint32_t len);

// Вывод ключа из пароля и соли (KDF на том же XTEA)
XteaKey deriveKey(const std::string& password, const uint8_t salt[8],
                  uint32_t iterations);

// Контрольный блок для проверки пароля тома при монтировании
void computeVerifier(const XteaKey& key, uint8_t out[8]);

// CRC32 (полином 0xEDB88320, как в ZIP/Ethernet) — контрольная сумма
// содержимого файла для обнаружения подмены или повреждения данных
uint32_t crc32(const void* data, size_t len);

// Случайное 32-битное число (для солей и nonce). ГПСЧ учебный: для настоящей
// системы следовало бы использовать системный криптостойкий источник.
uint32_t random32();

// Самопроверка по официальным тест-векторам XTEA (mbed TLS)
bool xteaSelfTest(std::string& report);

}  // namespace gladfs
