#pragma once
// ============================================================================
//  Слой 1 — «носитель». Класс BlockDevice прячет файл-образ disk.img
//  и предоставляет интерфейс из двух операций: прочитать блок и записать
//  блок. Никаких файлов, имён и каталогов на этом слое нет — только
//  НОМЕРА БЛОКОВ, то есть прямое обращение к определённому месту на диске.
// ============================================================================

#include <cstdint>
#include <cstdio>
#include <string>

namespace gladfs {

class BlockDevice {
public:
    BlockDevice() = default;
    ~BlockDevice();

    BlockDevice(const BlockDevice&) = delete;
    BlockDevice& operator=(const BlockDevice&) = delete;

    // Открыть образ: режим "r+b", либо создание нового файла (create=true).
    bool open(const std::string& path, bool create, std::string& error);
    void close();

    bool isOpen() const { return handle_ != nullptr; }
    const std::string& path() const { return path_; }

    // Прочитать блок с номером index в буфер buf (не меньше BLOCK_SIZE байт).
    bool readBlock(uint32_t index, void* buf) const;

    // Записать блок с номером index из буфера buf (BLOCK_SIZE байт).
    bool writeBlock(uint32_t index, const void* buf);

    // Физический размер образа в байтах (для проверки при монтировании)
    uint64_t sizeBytes() const;

private:
    std::FILE* handle_ = nullptr;
    std::string path_;
};

// Открытие файла на «большом» диске (для import/export). На Windows путь
// из консоли приходит в UTF-8, поэтому конвертируется в wide-char.
std::FILE* openHostFile(const std::string& path, const char* mode);

}  // namespace gladfs
