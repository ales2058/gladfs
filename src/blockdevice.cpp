#include "blockdevice.h"

#include "constants.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace gladfs {

BlockDevice::~BlockDevice() {
    close();
}

bool BlockDevice::open(const std::string& path, bool create, std::string& error) {
    close();
    path_ = path;

    // Сначала пробуем открыть существующий образ...
    handle_ = openHostFile(path, "r+b");
    if (handle_ == nullptr && create) {
        // ...если его нет и разрешено создание — создаём пустой.
        handle_ = openHostFile(path, "w+b");
        if (handle_ != nullptr) {
            // Сразу отводим образу полный размер, записывая последний блок.
            std::string zeros(BLOCK_SIZE, '\0');
            if (fseek(handle_, static_cast<long>(TOTAL_BLOCKS - 1) * BLOCK_SIZE, SEEK_SET) != 0 ||
                fwrite(zeros.data(), 1, BLOCK_SIZE, handle_) != BLOCK_SIZE) {
                error = "не удалось отвести место под образ диска";
                close();
                return false;
            }
            fflush(handle_);
        }
    }
    if (handle_ == nullptr) {
        error = "образ диска не найден (воспользуйтесь командой format)";
        return false;
    }
    return true;
}

void BlockDevice::close() {
    if (handle_ != nullptr) {
        fflush(handle_);
        fclose(handle_);
        handle_ = nullptr;
    }
}

bool BlockDevice::readBlock(uint32_t index, void* buf) const {
    if (handle_ == nullptr || index >= TOTAL_BLOCKS) return false;
    if (fseek(handle_, static_cast<long>(index) * BLOCK_SIZE, SEEK_SET) != 0) return false;
    return fread(buf, 1, BLOCK_SIZE, handle_) == BLOCK_SIZE;
}

bool BlockDevice::writeBlock(uint32_t index, const void* buf) {
    if (handle_ == nullptr || index >= TOTAL_BLOCKS) return false;
    if (fseek(handle_, static_cast<long>(index) * BLOCK_SIZE, SEEK_SET) != 0) return false;
    if (fwrite(buf, 1, BLOCK_SIZE, handle_) != BLOCK_SIZE) return false;
    fflush(handle_);  // сразу выталкиваем на диск (важно для демонстрации fsck)
    return true;
}

uint64_t BlockDevice::sizeBytes() const {
    if (handle_ == nullptr) return 0;
    long old = ftell(handle_);
    fseek(handle_, 0, SEEK_END);
    long end = ftell(handle_);
    if (old >= 0) fseek(handle_, old, SEEK_SET);
    return end < 0 ? 0 : static_cast<uint64_t>(end);
}

std::FILE* openHostFile(const std::string& path, const char* mode) {
#ifdef _WIN32
    //UTF-8 -> wide char -> _wfopen.
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return nullptr;
    std::wstring wide(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wide[0], wlen);

    std::wstring wideMode;
    for (const char* p = mode; *p != '\0'; ++p) wideMode.push_back(static_cast<wchar_t>(*p));

    return _wfopen(wide.c_str(), wideMode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

}  // namespace gladfs
