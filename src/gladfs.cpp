#include "gladfs.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <functional>

namespace gladfs {

// ============================================================================
//  Вспомогательные функции
// ============================================================================

Date today() {
    std::time_t t = std::time(nullptr);
    std::tm* loc = std::localtime(&t);
    Date d;
    d.day   = loc != nullptr ? loc->tm_mday : 1;
    d.month = loc != nullptr ? loc->tm_mon + 1 : 1;
    d.year  = loc != nullptr ? loc->tm_year + 1900 : 1970;
    return d;
}

std::string dateToString(const Date& d) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%02u.%02u.%04u",
                  static_cast<unsigned>(d.day),
                  static_cast<unsigned>(d.month),
                  static_cast<unsigned>(d.year));
    return buf;
}

bool parseDate(const std::string& text, Date& out) {
    unsigned day = 0, month = 0, year = 0;
    std::string s = text;
    std::replace(s.begin(), s.end(), '-', '.');  // поддерживаем и дд-мм-гггг
    if (std::sscanf(s.c_str(), "%u.%u.%u", &day, &month, &year) != 3) return false;
    if (month < 1 || month > 12 || day < 1 || day > 31) return false;
    if (year < 1970 || year > 2100) return false;
    out.day   = static_cast<uint16_t>(day);
    out.month = static_cast<uint16_t>(month);
    out.year  = static_cast<uint16_t>(year);
    return true;
}

std::string errorText(ErrorCode code) {
    switch (code) {
        case OK:                return "операция выполнена";
        case ERR_NOT_MOUNTED:   return "файловая система не смонтирована (сначала format или повторите запуск)";
        case ERR_NO_DISK:       return "образ диска не найден";
        case ERR_BAD_FORMAT:    return "образ повреждён или не является диском GladFS";
        case ERR_IO:            return "ошибка чтения/записи образа диска";
        case ERR_DISK_FULL:     return "на диске нет свободного места";
        case ERR_DIR_FULL:      return "таблица переполнена (не более 128 файлов и 32 пользователей)";
        case ERR_FILE_NOT_FOUND:return "файл не найден";
        case ERR_FILE_EXISTS:   return "файл с таким именем уже существует";
        case ERR_NAME_INVALID:  return "недопустимое имя (буквы, цифры, . _ - + ~, до 23 символов)";
        case ERR_READ_ONLY:     return "файл защищён атрибутом «только чтение»";
        case ERR_CHAIN_BROKEN:  return "цепочка блоков файла повреждена (поможет fsck)";
        case ERR_TOO_BIG:       return "файл не помещается на диск";
        case ERR_HOST_IO:       return "ошибка доступа к файлу на «большом» диске";
        case ERR_BAD_ARGS:      return "недопустимые аргументы команды";
        case ERR_ACCESS_DENIED: return "нет прав на операцию";
        case ERR_WRONG_PASSWORD:return "неверный пароль";
        case ERR_USER_EXISTS:   return "пользователь с таким именем уже существует";
        case ERR_USER_NOT_FOUND:return "пользователь не найден";
        case ERR_DATA_CORRUPT:  return "контрольная сумма не совпала: данные подменены или повреждены";
        case ERR_IS_DIRECTORY:  return "это каталог";
        case ERR_NOT_DIRECTORY: return "путь указывает не на каталог";
        case ERR_DIR_NOT_EMPTY: return "каталог не пуст (сначала удалите или перенесите файлы)";
    }
    return "неизвестная ошибка";
}

// Имя из записи каталога (с защитой от отсутствия '\0' в повреждённом образе)
std::string entryName(const DirEntry& e) {
    return std::string(e.name, strnlen(e.name, NAME_MAX + 1));
}

// Строка прав в виде «rw-r--»
std::string permString(uint8_t permissions) {
    std::string s = "------";
    uint8_t owner = (permissions >> PERM_OWNER_SHIFT) & 0x0F;
    uint8_t other = permissions & 0x0F;
    s[0] = (owner & PERM_READ)  ? 'r' : '-';
    s[1] = (owner & PERM_WRITE) ? 'w' : '-';
    s[2] = (owner & PERM_EXEC)  ? 'x' : '-';
    s[3] = (other & PERM_READ)  ? 'r' : '-';
    s[4] = (other & PERM_WRITE) ? 'w' : '-';
    s[5] = (other & PERM_EXEC)  ? 'x' : '-';
    return s;
}

// Проверка допустимости имени (компонента пути)
static bool validName(const std::string& name) {
    if (name.empty() || name.size() > NAME_MAX) return false;
    if (name == "." || name == "..") return false;
    for (char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-' || c == '+' || c == '~';
        if (!ok) return false;
    }
    return true;
}

// Разбор пути: пустые фрагменты и «.» пропускаются, «..» сохраняется
// (при обходе поднимает на уровень выше, в корне игнорируется)
std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (cur == "..") { out.push_back(cur); cur.clear(); }
            else if (!cur.empty() && cur != ".") { out.push_back(cur); cur.clear(); }
            else cur.clear();
        } else {
            cur += c;
        }
    }
    if (cur == "..") out.push_back(cur);
    else if (!cur.empty() && cur != ".") out.push_back(cur);
    return out;
}

// Число блоков, нужное для файла длиной size байт
static uint32_t blocksFor(uint32_t size) {
    return (size + BLOCK_SIZE - 1) / BLOCK_SIZE;
}

// Сериализация таблицы каталога в байты (содержимое файла-каталога)
static std::string serializeTable(const std::vector<DirEntry>& table) {
    std::string bytes;
    bytes.reserve(table.size() * DIRENTRY_SIZE);
    for (const auto& e : table)
        bytes.append(reinterpret_cast<const char*>(&e), DIRENTRY_SIZE);
    return bytes;
}

// Является ли префиксом (для запретов переноса каталогов внутрь себя)
static bool isPathPrefix(const std::vector<std::string>& prefix,
                         const std::vector<std::string>& path) {
    if (prefix.size() > path.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i)
        if (prefix[i] != path[i]) return false;
    return true;
}

// ============================================================================
//  Жизненный цикл: format / mount / unmount
// ============================================================================

bool GladFS::format(const std::string& imagePath, const std::string& volumeName,
                    const std::string& volumePassword,
                    const std::string& rootPassword, std::string& error) {
    unmount();
    if (!disk_.open(imagePath, /*create=*/true, error)) return false;

    // Суперблок
    std::memset(&super_, 0, sizeof(super_));
    std::memcpy(super_.signature, SIGNATURE, sizeof(SIGNATURE));
    super_.version        = FORMAT_VERSION;
    super_.blockSize      = BLOCK_SIZE;
    super_.totalBlocks    = TOTAL_BLOCKS;
    super_.fatFirstBlock  = FAT_FIRST;
    super_.rootFirstBlock = ROOT_FIRST;
    super_.dataFirstBlock = DATA_FIRST;
    super_.freeBlocks     = DATA_BLOCKS;
    super_.fileCount      = 0;
    super_.created        = today();
    std::strncpy(super_.volumeName, volumeName.c_str(), VOLUME_NAME_MAX);

    // Шифрование: пустой пароль тома — данные не шифруются.
    // Ключ выводится из пароля и соли; контрольный блок (verifier) позволяет
    // при монтировании проверить пароль, не храня его.
    super_.cryptoFlags = volumePassword.empty() ? 0 : CRYPTO_ENABLED;
    for (int i = 0; i < 8; ++i) super_.salt[i] = static_cast<uint8_t>(random32());
    if (encryptionEnabled()) {
        volumePassword_ = volumePassword;
        volumeKey_ = deriveKey(volumePassword, super_.salt, KDF_ITERATIONS_VOLUME);
        computeVerifier(volumeKey_, super_.verifier);
    }

    // FAT: служебные блоки помечаем RESERVED, данные свободны
    fat_.assign(TOTAL_BLOCKS, FAT_FREE);
    for (uint32_t i = 0; i < DATA_FIRST; ++i) fat_[i] = FAT_RESERVED;

    // Каталог: все записи пусты
    root_.assign(ROOT_ENTRIES, DirEntry{});

    if (!persistMetadata()) {
        error = "ошибка записи служебных структур";
        unmount();
        return false;
    }
    mounted_ = true;

    // Таблица пользователей (скрытый системный файл в корне) с записью root
    if (createUsersFile(rootPassword) != OK) {
        error = "не удалось создать таблицу пользователей";
        unmount();
        return false;
    }
    loadUsers();   // заполняет кэш пользователей для показа имён владельцев

    // формат выполняет администратор
    loggedIn_ = true;
    currentUid_ = 0;
    admin_ = true;
    return true;
}

bool GladFS::mount(const std::string& imagePath, const std::string& volumePassword,
                   std::string& error) {
    unmount();
    if (!disk_.open(imagePath, /*create=*/false, error)) return false;

    if (disk_.sizeBytes() != static_cast<uint64_t>(TOTAL_BLOCKS) * BLOCK_SIZE) {
        error = "размер образа не соответствует формату GladFS (нужен 1 МиБ)";
        unmount();
        return false;
    }

    // Суперблок
    std::string buf(BLOCK_SIZE, '\0');
    if (!disk_.readBlock(SUPERBLOCK_NO, &buf[0])) {
        error = "не удалось прочитать суперблок";
        unmount();
        return false;
    }
    SuperBlock onDisk;
    std::memcpy(&onDisk, buf.data(), sizeof(onDisk));
    if (std::memcmp(onDisk.signature, SIGNATURE, sizeof(SIGNATURE)) != 0) {
        error = "сигнатура не найдена: это не диск GladFS ";
        unmount();
        return false;
    }
    if (onDisk.version != FORMAT_VERSION || onDisk.blockSize != BLOCK_SIZE ||
        onDisk.totalBlocks != TOTAL_BLOCKS || onDisk.fatFirstBlock != FAT_FIRST ||
        onDisk.rootFirstBlock != ROOT_FIRST || onDisk.dataFirstBlock != DATA_FIRST) {
        error = "версия или геометрия образа не поддерживаются (нужен формат 3)";
        unmount();
        return false;
    }
    super_ = onDisk;

    // Проверка пароля тома: ключ из пароля должен давать тот же контрольный блок
    if (encryptionEnabled()) {
        XteaKey candidate = deriveKey(volumePassword, super_.salt, KDF_ITERATIONS_VOLUME);
        uint8_t check[8];
        computeVerifier(candidate, check);
        if (std::memcmp(check, super_.verifier, 8) != 0) {
            error = "неверный пароль тома";
            unmount();
            return false;
        }
        volumePassword_ = volumePassword;
        volumeKey_ = candidate;
    }

    // FAT
    std::vector<uint8_t> raw(FAT_BLOCKS * BLOCK_SIZE, 0);
    for (uint32_t b = 0; b < FAT_BLOCKS; ++b) {
        if (!disk_.readBlock(FAT_FIRST + b, raw.data() + b * BLOCK_SIZE)) {
            error = "не удалось прочитать FAT";
            unmount();
            return false;
        }
    }
    fat_.resize(TOTAL_BLOCKS);
    std::memcpy(fat_.data(), raw.data(), TOTAL_BLOCKS * sizeof(uint16_t));

    // Корневой каталог
    raw.assign(ROOT_BLOCKS * BLOCK_SIZE, 0);
    for (uint32_t b = 0; b < ROOT_BLOCKS; ++b) {
        if (!disk_.readBlock(ROOT_FIRST + b, raw.data() + b * BLOCK_SIZE)) {
            error = "не удалось прочитать каталог";
            unmount();
            return false;
        }
    }
    root_.resize(ROOT_ENTRIES);
    std::memcpy(root_.data(), raw.data(), ROOT_ENTRIES * DIRENTRY_SIZE);

    mounted_ = true;
    loggedIn_ = false;
    currentUid_ = 0xFF;   // до входа в систему пользователь не определён
    admin_ = false;

    // Таблица пользователей
    if (loadUsers() != OK) {
        error = "не удалось прочитать таблицу пользователей";
        unmount();
        return false;
    }
    return true;
}

void GladFS::unmount() {
    disk_.close();
    mounted_ = false;
    loggedIn_ = false;
    currentUid_ = 0xFF;
    admin_ = false;
    volumePassword_.clear();       // не держим секрет дольше необходимого
    volumeKey_ = XteaKey{};
}

// ============================================================================
//  Внутренние помощники: поиск, цепочки, кэш
// ============================================================================

DirEntry* GladFS::tableFind(std::vector<DirEntry>& table, const std::string& name,
                            bool includeTrashed) {
    for (auto& e : table) {
        if (!(e.flags & ENTRY_USED)) continue;
        if (!includeTrashed && (e.attributes & ATTR_TRASHED)) continue;
        if (entryName(e) == name) return &e;
    }
    return nullptr;
}

const DirEntry* GladFS::tableFind(const std::vector<DirEntry>& table,
                                  const std::string& name, bool includeTrashed) {
    for (const auto& e : table) {
        if (!(e.flags & ENTRY_USED)) continue;
        if (!includeTrashed && (e.attributes & ATTR_TRASHED)) continue;
        if (entryName(e) == name) return &e;
    }
    return nullptr;
}

DirEntry* GladFS::findEntry(const std::string& name, bool includeTrashed) {
    return tableFind(root_, name, includeTrashed);
}

const DirEntry* GladFS::findEntry(const std::string& name, bool includeTrashed) const {
    return tableFind(root_, name, includeTrashed);
}

bool GladFS::walkChain(uint16_t first, std::vector<uint32_t>& blocks) const {
    blocks.clear();
    uint16_t cur = first;
    uint32_t steps = 0;
    while (cur != FAT_EOF) {
        if (cur < DATA_FIRST || cur >= TOTAL_BLOCKS) return false;  // битая ссылка
        if (++steps > TOTAL_BLOCKS) return false;                   // зацикливание
        blocks.push_back(cur);
        cur = fat_[cur];
    }
    return true;
}

ErrorCode GladFS::allocateChain(uint32_t count, uint16_t& first) {
    first = FAT_EOF;
    if (count == 0) return OK;

    // Первый подходящий (first-fit) набор свободных блоков
    std::vector<uint32_t> picked;
    for (uint32_t b = DATA_FIRST; b < TOTAL_BLOCKS && picked.size() < count; ++b)
        if (fat_[b] == FAT_FREE) picked.push_back(b);
    if (picked.size() < count) return ERR_DISK_FULL;

    // Связываем блоки в односвязный список: fat[b] = next
    first = static_cast<uint16_t>(picked.front());
    for (size_t i = 0; i + 1 < picked.size(); ++i)
        fat_[picked[i]] = static_cast<uint16_t>(picked[i + 1]);
    fat_[picked.back()] = FAT_EOF;
    return OK;
}

ErrorCode GladFS::extendChain(uint16_t tail, uint32_t count) {
    uint16_t extraFirst = FAT_EOF;
    ErrorCode r = allocateChain(count, extraFirst);
    if (r != OK) return r;
    if (extraFirst == FAT_EOF) return OK;  // добавлять нечего
    fat_[tail] = extraFirst;               // прицепляем новую цепочку к хвосту
    return OK;
}

void GladFS::freeChain(uint16_t first) {
    uint32_t guard = 0;
    uint16_t cur = first;
    while (cur != FAT_EOF && cur >= DATA_FIRST && cur < TOTAL_BLOCKS &&
           guard++ < TOTAL_BLOCKS) {
        uint16_t next = fat_[cur];
        fat_[cur] = FAT_FREE;
        cur = next;
    }
}

void GladFS::refreshCounters() {
    uint32_t freeCnt = 0;
    for (uint32_t b = DATA_FIRST; b < TOTAL_BLOCKS; ++b)
        if (fat_[b] == FAT_FREE) ++freeCnt;
    super_.freeBlocks = freeCnt;

    uint32_t files = 0;
    for (const auto& e : root_)
        if ((e.flags & ENTRY_USED) && !(e.attributes & ATTR_TRASHED)) ++files;
    super_.fileCount = files;
}

bool GladFS::persistMetadata() {
    if (!disk_.isOpen()) return false;
    refreshCounters();

    if (!disk_.writeBlock(SUPERBLOCK_NO, &super_)) return false;

    std::vector<uint8_t> raw(FAT_BLOCKS * BLOCK_SIZE, 0);
    std::memcpy(raw.data(), fat_.data(), fat_.size() * sizeof(uint16_t));
    for (uint32_t b = 0; b < FAT_BLOCKS; ++b)
        if (!disk_.writeBlock(FAT_FIRST + b, raw.data() + b * BLOCK_SIZE)) return false;

    raw.assign(ROOT_BLOCKS * BLOCK_SIZE, 0);
    std::memcpy(raw.data(), root_.data(), root_.size() * DIRENTRY_SIZE);
    for (uint32_t b = 0; b < ROOT_BLOCKS; ++b)
        if (!disk_.writeBlock(ROOT_FIRST + b, raw.data() + b * BLOCK_SIZE)) return false;
    return true;
}

// Чтение данных файла: блоки читаются, расшифровываются (если нужно) и
// сверяются с контрольной суммой из записи каталога. Несовпадение означает
// подмену или повреждение данных.
ErrorCode GladFS::readEntryData(const DirEntry& entry, std::string& out) const {
    out.clear();
    if (entry.size == 0) {
        if (entry.checksum == 0) return OK;
        return ERR_DATA_CORRUPT;   // пустой файл обязан иметь нулевую сумму
    }

    std::vector<uint32_t> chain;
    if (entry.firstBlock == FAT_EOF || !walkChain(entry.firstBlock, chain))
        return ERR_CHAIN_BROKEN;
    if (entry.size > chain.size() * BLOCK_SIZE) return ERR_CHAIN_BROKEN;

    out.resize(entry.size);
    std::string buf(BLOCK_SIZE, '\0');
    // Читаем только блоки, в которых реально лежат данные файла:
    // цепочка может быть длиннее (лишние блоки, «склейка» цепочек)
    uint32_t blocksToRead = (entry.size + BLOCK_SIZE - 1) / BLOCK_SIZE;
    for (size_t i = 0; i < chain.size() && i < blocksToRead; ++i) {
        if (!disk_.readBlock(chain[i], &buf[0])) return ERR_IO;
        uint32_t offset = static_cast<uint32_t>(i) * BLOCK_SIZE;
        uint32_t n = std::min<uint32_t>(BLOCK_SIZE, entry.size - offset);
        if (encryptionEnabled())
            ctrXor(volumeKey_, entry.nonce, static_cast<uint32_t>(i),
                   reinterpret_cast<uint8_t*>(&buf[0]), n);
        std::memcpy(&out[offset], buf.data(), n);
    }

    if (crc32(out.data(), out.size()) != entry.checksum) return ERR_DATA_CORRUPT;
    return OK;
}

// Ядро записи: размещение, шифрование и запись данных существующего файла
ErrorCode GladFS::writeFileCore(DirEntry* e, const std::string& data, bool append) {
    // Читаем текущее содержимое только для дозаписи в конец
    std::string content;
    if (append && e->size > 0) {
        ErrorCode r = readEntryData(*e, content);
        if (r != OK) return r;
    }
    if (append) content += data; else content = data;
    if (content.size() > static_cast<size_t>(DATA_BLOCKS) * BLOCK_SIZE)
        return ERR_TOO_BIG;

    // Текущая цепочка и потребность в блоках
    std::vector<uint32_t> chain;
    if (e->firstBlock != FAT_EOF && !walkChain(e->firstBlock, chain))
        return ERR_CHAIN_BROKEN;
    uint32_t need = blocksFor(static_cast<uint32_t>(content.size()));
    uint32_t len = static_cast<uint32_t>(chain.size());

    // Сначала проверяем, хватает ли места, и только потом меняем что-либо
    if (need > len) {
        uint32_t extra = need - len;
        uint32_t avail = 0;
        for (uint32_t b = DATA_FIRST; b < TOTAL_BLOCKS && avail < extra; ++b)
            if (fat_[b] == FAT_FREE) ++avail;
        if (avail < extra) return ERR_DISK_FULL;   // ничего не изменили
    }

    if (len > need) {
        // Цепочка длиннее, чем нужно: освобождаем излишек
        if (need == 0) {
            freeChain(e->firstBlock);
            e->firstBlock = FAT_EOF;
        } else {
            for (uint32_t j = need; j < len; ++j) fat_[chain[j]] = FAT_FREE;
            fat_[chain[need - 1]] = FAT_EOF;
        }
    } else if (need > len) {
        if (len == 0) {
            uint16_t first = FAT_EOF;
            ErrorCode r = allocateChain(need, first);
            if (r != OK) return r;
            e->firstBlock = first;
        } else {
            ErrorCode r = extendChain(static_cast<uint16_t>(chain.back()), need - len);
            if (r != OK) return r;
        }
    }

    // Пишем данные по блокам. Блок заполняется нулями и, если включено
    // шифрование, накладывается гамма CTR — на диске лежит шифротекст.
    if (!walkChain(e->firstBlock, chain)) return ERR_CHAIN_BROKEN;
    std::string buf(BLOCK_SIZE, '\0');
    for (uint32_t i = 0; i < need && i < chain.size(); ++i) {
        std::fill(buf.begin(), buf.end(), '\0');
        uint32_t offset = i * BLOCK_SIZE;
        uint32_t n = std::min<uint32_t>(BLOCK_SIZE,
                                        static_cast<uint32_t>(content.size()) - offset);
        std::memcpy(&buf[0], content.data() + offset, n);
        if (encryptionEnabled())
            ctrXor(volumeKey_, e->nonce, i, reinterpret_cast<uint8_t*>(&buf[0]), BLOCK_SIZE);
        if (!disk_.writeBlock(chain[i], buf.data())) return ERR_IO;
    }

    e->size = static_cast<uint32_t>(content.size());
    // Контрольная сумма считается по ОТКРЫТОМУ тексту: она поймает подмену
    // и прямых данных, и шифротекста (расшифрованный мусор даст другую сумму)
    e->checksum = crc32(content.data(), content.size());
    e->modified = today();
    if (!persistMetadata()) return ERR_IO;
    return OK;
}

// Создание записи в КОРНЕВОМ каталоге (внутренняя операция для _users)
ErrorCode GladFS::createEntry(const char* name, uint8_t attrs, uint8_t perms,
                              uint8_t uid) {
    for (auto& e : root_) {
        if (e.flags & ENTRY_USED) continue;
        e = DirEntry{};                                // обнуляем запись
        std::memcpy(e.name, name, std::strlen(name));  // длина уже проверена
        e.attributes = attrs;
        e.flags      = ENTRY_USED;
        e.firstBlock = FAT_EOF;                        // файл пока пуст
        e.nonce      = encryptionEnabled() ? random32() : 0;
        e.ownerUid   = uid;
        e.permissions = perms;
        Date now = today();
        e.created = now;
        e.modified = now;
        e.lastAccess = now;
        if (!persistMetadata()) return ERR_IO;
        return OK;
    }
    return ERR_DIR_FULL;
}

// ============================================================================
//  Каталоги: загрузка содержимого, обход путей, сохранение
// ============================================================================

// Каталог — файл с атрибутом DIRECTORY; его содержимое — массив записей
ErrorCode GladFS::loadDirTable(const DirEntry& dir, std::vector<DirEntry>& out) const {
    if (!(dir.attributes & ATTR_DIRECTORY)) return ERR_NOT_DIRECTORY;
    std::string bytes;
    ErrorCode r = readEntryData(dir, bytes);   // с проверкой контрольной суммы
    if (r != OK) return r;
    if (bytes.size() % DIRENTRY_SIZE != 0) return ERR_CHAIN_BROKEN;
    size_t n = bytes.size() / DIRENTRY_SIZE;
    out.assign(n, DirEntry{});
    if (n > 0) std::memcpy(out.data(), bytes.data(), n * DIRENTRY_SIZE);
    return OK;
}

// Открыть каталоги по первым count компонентам пути (стек, [0] — корень)
ErrorCode GladFS::openPath(const std::vector<std::string>& comps, size_t count,
                           std::vector<DirHandle>& stack) const {
    if (count > comps.size()) return ERR_BAD_ARGS;
    stack.clear();
    DirHandle root;
    root.table = root_;
    root.isRoot = true;
    stack.push_back(std::move(root));

    for (size_t i = 0; i < count; ++i) {
        const std::string& c = comps[i];
        if (c == "..") {
            if (stack.size() > 1) stack.pop_back();   // выше корня подняться нельзя
            continue;
        }
        DirEntry* e = tableFind(stack.back().table, c);
        if (e == nullptr) return ERR_FILE_NOT_FOUND;
        if (!(e->attributes & ATTR_DIRECTORY)) return ERR_NOT_DIRECTORY;
        if (stack.size() > MAX_DIR_DEPTH) return ERR_BAD_ARGS;

        DirHandle next;
        next.isRoot = false;
        next.self = *e;          // копия записи каталога (обновится при сохранении)
        next.selfName = c;
        ErrorCode r = loadDirTable(*e, next.table);
        if (r != OK) return r;
        stack.push_back(std::move(next));
    }
    return OK;
}

// Сохранение изменённых таблиц: снизу вверх. Содержимое каталога пишется как
// файл (writeFileCore обновляет его запись self), после чего self заменяется
// в родительской таблице — и так до корня. Промежуточные состояния на диске
// могут быть неконсистентны при сбое — их исправляет fsck.
ErrorCode GladFS::flushDirStack(std::vector<DirHandle>& stack) {
    for (size_t i = stack.size(); i-- > 1; ) {
        if (!stack[i].dirty) continue;
        ErrorCode r = writeFileCore(&stack[i].self, serializeTable(stack[i].table),
                                    /*append=*/false);
        if (r != OK) return r;
        bool updated = false;
        for (auto& p : stack[i - 1].table) {
            if ((p.flags & ENTRY_USED) && entryName(p) == stack[i].selfName) {
                p = stack[i].self;   // запись каталога могла изменить размер/блоки
                updated = true;
                break;
            }
        }
        if (!updated) return ERR_CHAIN_BROKEN;
        stack[i - 1].dirty = true;
    }
    if (!stack.empty() && stack[0].dirty) root_ = stack[0].table;
    return OK;
}

// Добавить запись в таблицу каталога. Корень ограничен 128 записями,
// подкаталоги растут как файлы (запись дописывается в конец).
static ErrorCode addEntryToTable(std::vector<DirEntry>& table, bool isRoot,
                                 const DirEntry& ne) {
    for (auto& e : table) {
        if (e.flags & ENTRY_USED) continue;
        e = ne;
        return OK;
    }
    if (isRoot) return ERR_DIR_FULL;   // корень не растёт
    table.push_back(ne);
    return OK;
}

// Заполнение новой записи каталога
static void fillEntry(DirEntry& e, const std::string& name, uint8_t attrs,
                      uint8_t perms, uint8_t uid, bool encrypted) {
    e = DirEntry{};
    std::memcpy(e.name, name.c_str(), name.size());
    e.attributes = attrs;
    e.flags = ENTRY_USED;
    e.firstBlock = FAT_EOF;
    e.nonce = encrypted ? random32() : 0;
    e.ownerUid = uid;
    e.permissions = perms;
    Date now = today();
    e.created = now;
    e.modified = now;
    e.lastAccess = now;
}

// ============================================================================
//  Пользователи и права
// ============================================================================

bool GladFS::canRead(const DirEntry& e) const {
    if (admin_) return true;
    uint8_t nib = (e.ownerUid == currentUid_) ? (e.permissions >> PERM_OWNER_SHIFT)
                                              : (e.permissions & 0x0F);
    return (nib & PERM_READ) != 0;
}

bool GladFS::canWrite(const DirEntry& e) const {
    if (admin_) return true;
    uint8_t nib = (e.ownerUid == currentUid_) ? (e.permissions >> PERM_OWNER_SHIFT)
                                              : (e.permissions & 0x0F);
    return (nib & PERM_WRITE) != 0;
}

bool GladFS::canManage(const DirEntry& e) const {
    return admin_ || e.ownerUid == currentUid_;
}

UserRecord* GladFS::findUser(const std::string& name) {
    for (auto& u : users_)
        if ((u.flags & USER_USED) &&
            std::string(u.name, strnlen(u.name, NAME_MAX_USERS + 1)) == name)
            return &u;
    return nullptr;
}

ErrorCode GladFS::createUsersFile(const std::string& rootPassword) {
    // Создаём скрытый системный файл таблицы пользователей в корне
    ErrorCode r = createEntry(USERS_FILE_NAME, ATTR_SYSTEM | ATTR_HIDDEN, 0x00, 0);
    if (r != OK) return r;

    // Первая запись — администратор root
    UserRecord rec{};
    std::memcpy(rec.name, "root", 4);
    rec.uid = 0;
    rec.flags = USER_USED | USER_ADMIN;
    for (int i = 0; i < 8; ++i) rec.salt[i] = static_cast<uint8_t>(random32());
    XteaKey k = deriveKey(rootPassword, rec.salt, KDF_ITERATIONS_USER);
    std::memcpy(rec.hash, k.w, 8);

    std::string data(reinterpret_cast<const char*>(&rec), sizeof(rec));
    return writeSystemFile(USERS_FILE_NAME, data);
}

ErrorCode GladFS::loadUsers() {
    users_.clear();
    std::string data;
    ErrorCode r = readSystemFile(USERS_FILE_NAME, data);
    if (r == ERR_FILE_NOT_FOUND || (r == OK && data.empty())) {
        // Таблица потеряна (например, после тяжёлого сбоя диска) — пересоздаём
        // с root без пароля; иначе том стал бы совсем недоступен.
        return createUsersFile("");
    }
    if (r != OK) return r;

    size_t n = data.size() / sizeof(UserRecord);
    users_.resize(n);
    std::memcpy(users_.data(), data.data(), n * sizeof(UserRecord));
    return OK;
}

ErrorCode GladFS::saveUsers() {
    std::string data;
    data.reserve(users_.size() * sizeof(UserRecord));
    for (const auto& u : users_)
        data.append(reinterpret_cast<const char*>(&u), sizeof(u));
    return writeSystemFile(USERS_FILE_NAME, data);
}

ErrorCode GladFS::login(const std::string& user, const std::string& password) {
    if (!mounted_) return ERR_NOT_MOUNTED;
    UserRecord* u = findUser(user);
    if (u == nullptr) return ERR_USER_NOT_FOUND;

    XteaKey k = deriveKey(password, u->salt, KDF_ITERATIONS_USER);
    if (std::memcmp(k.w, u->hash, 8) != 0) return ERR_WRONG_PASSWORD;

    loggedIn_ = true;
    currentUid_ = u->uid;
    admin_ = (u->flags & USER_ADMIN) != 0;
    return OK;
}

std::string GladFS::currentUser() const {
    for (const auto& u : users_)
        if ((u.flags & USER_USED) && u.uid == currentUid_)
            return std::string(u.name, strnlen(u.name, NAME_MAX_USERS + 1));
    return "(не выполнен вход)";
}

ErrorCode GladFS::addUser(const std::string& name, const std::string& password) {
    if (!mounted_) return ERR_NOT_MOUNTED;
    if (!admin_) return ERR_ACCESS_DENIED;
    if (name.empty() || name.size() > NAME_MAX_USERS) return ERR_NAME_INVALID;
    if (findUser(name) != nullptr) return ERR_USER_EXISTS;
    if (users_.size() >= MAX_USERS) return ERR_DIR_FULL;

    UserRecord rec{};
    std::memcpy(rec.name, name.c_str(), name.size());
    uint8_t nextUid = 0;
    for (const auto& u : users_) nextUid = std::max(nextUid, u.uid);
    rec.uid = static_cast<uint8_t>(nextUid + 1);
    rec.flags = USER_USED;
    for (int i = 0; i < 8; ++i) rec.salt[i] = static_cast<uint8_t>(random32());
    XteaKey k = deriveKey(password, rec.salt, KDF_ITERATIONS_USER);
    std::memcpy(rec.hash, k.w, 8);

    users_.push_back(rec);
    return saveUsers();
}

ErrorCode GladFS::setPassword(const std::string& name, const std::string& newPassword) {
    if (!mounted_) return ERR_NOT_MOUNTED;
    UserRecord* u = findUser(name);
    if (u == nullptr) return ERR_USER_NOT_FOUND;
    // менять пароль можно только себе или администратору
    if (!admin_ && u->uid != currentUid_) return ERR_ACCESS_DENIED;

    for (int i = 0; i < 8; ++i) u->salt[i] = static_cast<uint8_t>(random32());
    XteaKey k = deriveKey(newPassword, u->salt, KDF_ITERATIONS_USER);
    std::memcpy(u->hash, k.w, 8);
    return saveUsers();
}

ErrorCode GladFS::changeOwner(const std::string& path, const std::string& userName) {
    if (!mounted_ || !loggedIn_) return ERR_NOT_MOUNTED;
    if (!admin_) return ERR_ACCESS_DENIED;   // смена владельца — только админ
    std::vector<std::string> comps = splitPath(path);
    if (comps.empty()) return ERR_NAME_INVALID;
    std::vector<DirHandle> stack;
    ErrorCode r = openPath(comps, comps.size() - 1, stack);
    if (r != OK) return r;
    DirEntry* e = tableFind(stack.back().table, comps.back());
    if (e == nullptr) return ERR_FILE_NOT_FOUND;
    if (e->attributes & ATTR_SYSTEM) return ERR_ACCESS_DENIED;
    UserRecord* u = findUser(userName);
    if (u == nullptr) return ERR_USER_NOT_FOUND;
    e->ownerUid = u->uid;
    stack.back().dirty = true;
    r = flushDirStack(stack);
    if (r != OK) return r;
    if (!persistMetadata()) return ERR_IO;
    return OK;
}

ErrorCode GladFS::setPermissions(const std::string& path, uint8_t perms) {
    if (!mounted_ || !loggedIn_) return ERR_NOT_MOUNTED;
    std::vector<std::string> comps = splitPath(path);
    if (comps.empty()) return ERR_NAME_INVALID;
    std::vector<DirHandle> stack;
    ErrorCode r = openPath(comps, comps.size() - 1, stack);
    if (r != OK) return r;
    DirEntry* e = tableFind(stack.back().table, comps.back());
    if (e == nullptr) return ERR_FILE_NOT_FOUND;
    if (e->attributes & ATTR_SYSTEM) return ERR_ACCESS_DENIED;
    if (!canManage(*e)) return ERR_ACCESS_DENIED;
    e->permissions = perms;
    stack.back().dirty = true;
    r = flushDirStack(stack);
    if (r != OK) return r;
    if (!persistMetadata()) return ERR_IO;
    return OK;
}

// Служебная запись файла (для таблицы пользователей): без проверок прав
ErrorCode GladFS::writeSystemFile(const std::string& name, const std::string& data) {
    if (!mounted_) return ERR_NOT_MOUNTED;
    DirEntry* e = findEntry(name);
    if (e == nullptr) return ERR_FILE_NOT_FOUND;
    return writeFileCore(e, data, /*append=*/false);
}

ErrorCode GladFS::readSystemFile(const std::string& name, std::string& out) {
    if (!mounted_) return ERR_NOT_MOUNTED;
    const DirEntry* e = findEntry(name);
    if (e == nullptr) return ERR_FILE_NOT_FOUND;
    return readEntryData(*e, out);
}

// ============================================================================
//  Операции над файлами (имена — пути)
// ============================================================================

ErrorCode GladFS::createFile(const std::string& path) {
    if (!mounted_) return ERR_NOT_MOUNTED;
    if (!loggedIn_) return ERR_ACCESS_DENIED;
    std::vector<std::string> comps = splitPath(path);
    if (comps.empty() || comps.back() == "..") return ERR_NAME_INVALID;
    const std::string& name = comps.back();
    if (!validName(name)) return ERR_NAME_INVALID;
    if (name == USERS_FILE_NAME && comps.size() == 1) return ERR_NAME_INVALID;

    std::vector<DirHandle> stack;
    ErrorCode r = openPath(comps, comps.size() - 1, stack);
    if (r != OK) return r;
    // создание контролируется правами на каталог (корень общий для всех)
    if (!stack.back().isRoot && !canWrite(stack.back().self)) return ERR_ACCESS_DENIED;
    if (tableFind(stack.back().table, name) != nullptr) return ERR_FILE_EXISTS;

    DirEntry ne;
    fillEntry(ne, name, 0, DEFAULT_PERMS, currentUid_, encryptionEnabled());
    r = addEntryToTable(stack.back().table, stack.back().isRoot, ne);
    if (r != OK) return r;
    stack.back().dirty = true;
    r = flushDirStack(stack);
    if (r != OK) return r;
    if (!persistMetadata()) return ERR_IO;
    return OK;
}

ErrorCode GladFS::writeFile(const std::string& path, const std::string& data, bool append) {
    if (!mounted_) return ERR_NOT_MOUNTED;
    if (!loggedIn_) return ERR_ACCESS_DENIED;
    std::vector<std::string> comps = splitPath(path);
    if (comps.empty() || comps.back() == "..") return ERR_NAME_INVALID;

    std::vector<DirHandle> stack;
    ErrorCode r = openPath(comps, comps.size() - 1, stack);
    if (r != OK) return r;
    DirEntry* e = tableFind(stack.back().table, comps.back());
    if (e == nullptr) return ERR_FILE_NOT_FOUND;
    if (e->attributes & ATTR_SYSTEM) return ERR_ACCESS_DENIED;
    if (e->attributes & ATTR_DIRECTORY) return ERR_IS_DIRECTORY;
    if (e->attributes & ATTR_READONLY) return ERR_READ_ONLY;
    if (!canWrite(*e)) return ERR_ACCESS_DENIED;

    r = writeFileCore(e, data, append);
    if (r != OK) return r;
    stack.back().dirty = true;
    r = flushDirStack(stack);
    if (r != OK) return r;
    if (!persistMetadata()) return ERR_IO;
    return OK;
}

ErrorCode GladFS::readFile(const std::string& path, std::string& out) {
    if (!mounted_) return ERR_NOT_MOUNTED;
    if (!loggedIn_) return ERR_ACCESS_DENIED;
    std::vector<std::string> comps = splitPath(path);
    if (comps.empty() || comps.back() == "..") return ERR_NAME_INVALID;

    std::vector<DirHandle> stack;
    ErrorCode r = openPath(comps, comps.size() - 1, stack);
    if (r != OK) return r;
    DirEntry* e = tableFind(stack.back().table, comps.back());
    if (e == nullptr) return ERR_FILE_NOT_FOUND;
    if (e->attributes & ATTR_SYSTEM) return ERR_ACCESS_DENIED;
    if (e->attributes & ATTR_DIRECTORY) return ERR_IS_DIRECTORY;
    if (!canRead(*e)) return ERR_ACCESS_DENIED;

    r = readEntryData(*e, out);
    if (r != OK) return r;

    // Обращение к файлу: увеличиваем счётчик (параметр из варианта №6)
    ++e->accessCount;
    e->lastAccess = today();
    stack.back().dirty = true;
    r = flushDirStack(stack);
    if (r != OK) return r;
    if (!persistMetadata()) return ERR_IO;
    return OK;
}

// ============================================================================
//  Каталоги: mkdir / rmdir / проверки / просмотр
// ============================================================================

ErrorCode GladFS::makeDirectory(const std::string& path) {
    if (!mounted_) return ERR_NOT_MOUNTED;
    if (!loggedIn_) return ERR_ACCESS_DENIED;
    std::vector<std::string> comps = splitPath(path);
    if (comps.empty() || comps.back() == "..") return ERR_NAME_INVALID;
    const std::string& name = comps.back();
    if (!validName(name)) return ERR_NAME_INVALID;

    std::vector<DirHandle> stack;
    ErrorCode r = openPath(comps, comps.size() - 1, stack);
    if (r != OK) return r;
    if (!stack.back().isRoot && !canWrite(stack.back().self)) return ERR_ACCESS_DENIED;
    if (tableFind(stack.back().table, name) != nullptr) return ERR_FILE_EXISTS;

    DirEntry ne;
    fillEntry(ne, name, ATTR_DIRECTORY, DEFAULT_PERMS, currentUid_, encryptionEnabled());
    r = addEntryToTable(stack.back().table, stack.back().isRoot, ne);
    if (r != OK) return r;
    stack.back().dirty = true;
    r = flushDirStack(stack);
    if (r != OK) return r;
    if (!persistMetadata()) return ERR_IO;
    return OK;
}

ErrorCode GladFS::removeDirectory(const std::string& path) {
    if (!mounted_) return ERR_NOT_MOUNTED;
    if (!loggedIn_) return ERR_ACCESS_DENIED;
    std::vector<std::string> comps = splitPath(path);
    if (comps.empty() || comps.back() == "..") return ERR_NAME_INVALID;
    const std::string& name = comps.back();

    std::vector<DirHandle> stack;
    ErrorCode r = openPath(comps, comps.size() - 1, stack);
    if (r != OK) return r;
    DirEntry* e = tableFind(stack.back().table, name);
    if (e == nullptr) return ERR_FILE_NOT_FOUND;
    if (!(e->attributes & ATTR_DIRECTORY)) return ERR_NOT_DIRECTORY;
    if (e->attributes & ATTR_SYSTEM) return ERR_ACCESS_DENIED;
    // удаление контролируется правами на каталог-родитель
    if (!stack.back().isRoot && !canWrite(stack.back().self)) return ERR_ACCESS_DENIED;

    // каталог должен быть пуст (в том числе без записей в корзине)
    std::vector<DirEntry> inner;
    r = loadDirTable(*e, inner);
    if (r != OK) return r;
    for (const auto& t : inner)
        if (t.flags & ENTRY_USED) return ERR_DIR_NOT_EMPTY;

    freeChain(e->firstBlock);
    *e = DirEntry{};
    stack.back().dirty = true;
    r = flushDirStack(stack);
    if (r != OK) return r;
    if (!persistMetadata()) return ERR_IO;
    return OK;
}

ErrorCode GladFS::testDirectory(const std::string& path) {
    if (!mounted_) return ERR_NOT_MOUNTED;
    std::vector<std::string> comps = splitPath(path);
    std::vector<DirHandle> stack;
    return openPath(comps, comps.size(), stack);   // откроет только каталоги
}

ErrorCode GladFS::listDir(const std::string& path, std::vector<DirEntry>& out,
                          bool includeHidden) const {
    if (!mounted_) return ERR_NOT_MOUNTED;
    out.clear();
    std::vector<std::string> comps = splitPath(path);
    std::vector<DirHandle> stack;
    ErrorCode r = openPath(comps, comps.size(), stack);
    if (r != OK) return r;
    for (const auto& e : stack.back().table) {
        if (!(e.flags & ENTRY_USED)) continue;
        if (e.attributes & ATTR_TRASHED) continue;                  // корзина — не здесь
        if (!includeHidden && (e.attributes & ATTR_HIDDEN)) continue;
        out.push_back(e);
    }
    return OK;
}

// Обход всего дерева каталогов: путь от корня + копия записи
ErrorCode GladFS::walkTree(std::vector<std::pair<std::string, DirEntry>>& out) const {
    out.clear();
    std::set<uint32_t> visited;   // защита от циклов каталогов
    std::function<void(const std::vector<DirEntry>&, const std::string&, int)> rec =
        [&](const std::vector<DirEntry>& table, const std::string& prefix, int depth) {
        for (const auto& e : table) {
            if (!(e.flags & ENTRY_USED)) continue;
            std::string p = prefix + entryName(e);
            out.push_back({p, e});
            if (!(e.attributes & ATTR_DIRECTORY) || e.firstBlock == FAT_EOF) continue;
            if (depth >= static_cast<int>(MAX_DIR_DEPTH)) continue;
            if (visited.count(e.firstBlock)) continue;   // уже были в этом каталоге
            visited.insert(e.firstBlock);
            std::vector<DirEntry> sub;
            if (loadDirTable(e, sub) == OK) rec(sub, p + "/", depth + 1);
        }
    };
    rec(root_, "", 0);
    return OK;
}

// ============================================================================
//  Корзина
// ============================================================================

ErrorCode GladFS::trashFile(const std::string& path) {
    if (!mounted_) return ERR_NOT_MOUNTED;
    if (!loggedIn_) return ERR_ACCESS_DENIED;
    std::vector<std::string> comps = splitPath(path);
    if (comps.empty() || comps.back() == "..") return ERR_NAME_INVALID;

    std::vector<DirHandle> stack;
    ErrorCode r = openPath(comps, comps.size() - 1, stack);
    if (r != OK) return r;
    DirEntry* e = tableFind(stack.back().table, comps.back());
    if (e == nullptr) return ERR_FILE_NOT_FOUND;
    if (e->attributes & ATTR_SYSTEM) return ERR_ACCESS_DENIED;
    if (e->attributes & ATTR_DIRECTORY) return ERR_IS_DIRECTORY;
    if (e->attributes & ATTR_READONLY) return ERR_READ_ONLY;
    if (!canWrite(*e)) return ERR_ACCESS_DENIED;

    // Файл лишь помечается: цепочка FAT и данные не трогаются,
    // поэтому восстановление мгновенно и без потерь.
    e->attributes |= ATTR_TRASHED;
    stack.back().dirty = true;
    r = flushDirStack(stack);
    if (r != OK) return r;
    if (!persistMetadata()) return ERR_IO;
    return OK;
}

// Удаление записи по компонентам пути БЕЗ проверок прав (для rmold/purge)
ErrorCode GladFS::removeByPath(const std::vector<std::string>& comps, bool wipe) {
    if (comps.empty()) return ERR_NAME_INVALID;
    std::vector<DirHandle> stack;
    ErrorCode r = openPath(comps, comps.size() - 1, stack);
    if (r != OK) return r;
    // в корзине ищем и удалённые записи
    DirEntry* e = tableFind(stack.back().table, comps.back(), /*includeTrashed=*/true);
    if (e == nullptr) return ERR_FILE_NOT_FOUND;

    std::vector<uint32_t> chain;
    if (e->firstBlock != FAT_EOF && !walkChain(e->firstBlock, chain))
        return ERR_CHAIN_BROKEN;
    if (wipe) {
        std::string zeros(BLOCK_SIZE, '\0');
        for (uint32_t b : chain)
            if (!disk_.writeBlock(b, zeros.data())) return ERR_IO;
    }
    freeChain(e->firstBlock);
    *e = DirEntry{};   // запись каталога свободна
    stack.back().dirty = true;
    r = flushDirStack(stack);
    if (r != OK) return r;
    if (!persistMetadata()) return ERR_IO;
    return OK;
}

ErrorCode GladFS::removeFile(const std::string& path, bool wipe) {
    if (!mounted_) return ERR_NOT_MOUNTED;
    if (!loggedIn_) return ERR_ACCESS_DENIED;
    std::vector<std::string> comps = splitPath(path);
    if (comps.empty() || comps.back() == "..") return ERR_NAME_INVALID;

    std::vector<DirHandle> stack;
    ErrorCode r = openPath(comps, comps.size() - 1, stack);
    if (r != OK) return r;
    DirEntry* e = tableFind(stack.back().table, comps.back());
    if (e == nullptr) return ERR_FILE_NOT_FOUND;
    if (e->attributes & ATTR_SYSTEM) return ERR_ACCESS_DENIED;
    if (e->attributes & ATTR_DIRECTORY) return ERR_IS_DIRECTORY;
    if (e->attributes & ATTR_READONLY) return ERR_READ_ONLY;
    if (!canWrite(*e)) return ERR_ACCESS_DENIED;
    return removeByPath(comps, wipe);
}

ErrorCode GladFS::restoreFile(const std::string& path) {
    if (!mounted_) return ERR_NOT_MOUNTED;
    if (!loggedIn_) return ERR_ACCESS_DENIED;
    std::vector<std::string> comps = splitPath(path);
    if (comps.empty() || comps.back() == "..") return ERR_NAME_INVALID;

    std::vector<DirHandle> stack;
    ErrorCode r = openPath(comps, comps.size() - 1, stack);
    if (r != OK) return r;
    DirEntry* e = tableFind(stack.back().table, comps.back(), /*includeTrashed=*/true);
    if (e == nullptr || !(e->attributes & ATTR_TRASHED)) return ERR_FILE_NOT_FOUND;
    if (tableFind(stack.back().table, comps.back()) != nullptr)
        return ERR_FILE_EXISTS;                     // имя занято живым файлом
    if (!canManage(*e)) return ERR_ACCESS_DENIED;

    e->attributes &= static_cast<uint8_t>(~ATTR_TRASHED);
    stack.back().dirty = true;
    r = flushDirStack(stack);
    if (r != OK) return r;
    if (!persistMetadata()) return ERR_IO;
    return OK;
}

ErrorCode GladFS::purgeTrash() {
    if (!mounted_) return ERR_NOT_MOUNTED;
    if (!admin_) return ERR_ACCESS_DENIED;   // необратимая операция — только админ

    std::vector<std::pair<std::string, DirEntry>> all;
    walkTree(all);
    for (const auto& item : all) {
        const DirEntry& e = item.second;
        if (!(e.attributes & ATTR_TRASHED)) continue;
        removeByPath(splitPath(item.first), /*wipe=*/true);
    }
    return OK;
}

ErrorCode GladFS::listTrash(std::vector<std::pair<std::string, DirEntry>>& out) const {
    if (!mounted_) return ERR_NOT_MOUNTED;
    out.clear();
    std::vector<std::pair<std::string, DirEntry>> all;
    walkTree(all);
    for (const auto& item : all)
        if (item.second.attributes & ATTR_TRASHED) out.push_back(item);
    return OK;
}

// ============================================================================
//  Переименование и перенос (mv)
// ============================================================================

ErrorCode GladFS::renameFile(const std::string& oldPath, const std::string& newPath) {
    if (!mounted_) return ERR_NOT_MOUNTED;
    if (!loggedIn_) return ERR_NOT_MOUNTED;
    std::vector<std::string> compsO = splitPath(oldPath);
    std::vector<std::string> compsN = splitPath(newPath);
    if (compsO.empty() || compsN.empty() || compsO.back() == ".." ||
        compsN.back() == "..")
        return ERR_NAME_INVALID;
    if (!validName(compsN.back())) return ERR_NAME_INVALID;
    if (compsN.back() == USERS_FILE_NAME && compsN.size() == 1)
        return ERR_NAME_INVALID;   // имя зарезервировано

    // Проверки по старому пути
    std::vector<DirHandle> stackO;
    ErrorCode r = openPath(compsO, compsO.size() - 1, stackO);
    if (r != OK) return r;
    DirEntry* e = tableFind(stackO.back().table, compsO.back());
    if (e == nullptr) return ERR_FILE_NOT_FOUND;
    if (e->attributes & ATTR_SYSTEM) return ERR_ACCESS_DENIED;
    if (!canManage(*e)) return ERR_ACCESS_DENIED;

    // Каталог нельзя переносить внутрь себя
    if (e->attributes & ATTR_DIRECTORY && isPathPrefix(compsO, compsN))
        return ERR_BAD_ARGS;

    // Родители совпадаются? (без учёта «..»: сравниваем префиксы-компоненты)
    bool sameParent =
        compsO.size() == compsN.size() &&
        std::equal(compsO.begin(), compsO.end() - 1, compsN.begin());
    // «..» в путях при переносе не поддерживаем — упрощение
    for (const auto& c : compsO) if (c == "..") return ERR_BAD_ARGS;
    for (const auto& c : compsN) if (c == "..") return ERR_BAD_ARGS;

    if (sameParent) {
        DirEntry* clash = tableFind(stackO.back().table, compsN.back());
        if (clash != nullptr && clash != e) return ERR_FILE_EXISTS;
        std::memset(e->name, 0, sizeof(e->name));
        std::memcpy(e->name, compsN.back().c_str(), compsN.back().size());
        stackO.back().dirty = true;
        r = flushDirStack(stackO);
        if (r != OK) return r;
        if (!persistMetadata()) return ERR_IO;
        return OK;
    }

    // Перенос между каталогами. Пути родителей не должны быть вложены друг
    // в друга (иначе потребовалось бы объединять два стека открытых таблиц).
    std::vector<std::string> parentO(compsO.begin(), compsO.end() - 1);
    std::vector<std::string> parentN(compsN.begin(), compsN.end() - 1);
    if (isPathPrefix(parentO, parentN) || isPathPrefix(parentN, parentO))
        return ERR_BAD_ARGS;

    // Предварительные проверки нового места (по свежему чтению с диска)
    {
        std::vector<DirHandle> stackN;
        r = openPath(compsN, compsN.size() - 1, stackN);
        if (r != OK) return r;
        if (!stackN.back().isRoot && !canWrite(stackN.back().self))
            return ERR_ACCESS_DENIED;
        if (tableFind(stackN.back().table, compsN.back()) != nullptr)
            return ERR_FILE_EXISTS;
    }

    // Фаза 1: удалить запись из старого родителя и зафиксировать
    DirEntry moved = *e;
    *e = DirEntry{};
    stackO.back().dirty = true;
    r = flushDirStack(stackO);
    if (r != OK) return r;
    if (!persistMetadata()) return ERR_IO;

    // Фаза 2: добавить запись в нового родителя (свежее чтение с диска).
    // Сбой между фазами оставит блоки «потерянными» — их вернёт fsck.
    std::vector<DirHandle> stackN;
    r = openPath(compsN, compsN.size() - 1, stackN);
    if (r != OK) return r;
    std::memset(moved.name, 0, sizeof(moved.name));
    std::memcpy(moved.name, compsN.back().c_str(), compsN.back().size());
    r = addEntryToTable(stackN.back().table, stackN.back().isRoot, moved);
    if (r != OK) return r;
    stackN.back().dirty = true;
    r = flushDirStack(stackN);
    if (r != OK) return r;
    if (!persistMetadata()) return ERR_IO;
    return OK;
}

ErrorCode GladFS::setAttributes(const std::string& path, uint8_t mask, bool set) {
    if (!mounted_) return ERR_NOT_MOUNTED;
    if (!loggedIn_) return ERR_ACCESS_DENIED;
    std::vector<std::string> comps = splitPath(path);
    if (comps.empty() || comps.back() == "..") return ERR_NAME_INVALID;

    std::vector<DirHandle> stack;
    ErrorCode r = openPath(comps, comps.size() - 1, stack);
    if (r != OK) return r;
    DirEntry* e = tableFind(stack.back().table, comps.back());
    if (e == nullptr) return ERR_FILE_NOT_FOUND;
    if (e->attributes & ATTR_SYSTEM) return ERR_ACCESS_DENIED;
    if (!canManage(*e)) return ERR_ACCESS_DENIED;
    if (set) e->attributes |= mask; else e->attributes &= static_cast<uint8_t>(~mask);
    stack.back().dirty = true;
    r = flushDirStack(stack);
    if (r != OK) return r;
    if (!persistMetadata()) return ERR_IO;
    return OK;
}

// ============================================================================
//  Просмотр
// ============================================================================

ErrorCode GladFS::statFile(const std::string& path, DirEntry& out) const {
    if (!mounted_) return ERR_NOT_MOUNTED;
    std::vector<std::string> comps = splitPath(path);
    if (comps.empty() || comps.back() == "..") return ERR_NAME_INVALID;
    std::vector<DirHandle> stack;
    ErrorCode r = openPath(comps, comps.size() - 1, stack);
    if (r != OK) return r;
    const DirEntry* e = tableFind(stack.back().table, comps.back());
    if (e == nullptr) return ERR_FILE_NOT_FOUND;
    out = *e;
    return OK;
}

// ============================================================================
//  Операции из постановки варианта №6
// ============================================================================

ErrorCode GladFS::removeOlderThan(const Date& border,
                                  std::vector<std::string>& removed,
                                  std::vector<std::string>& skipped) {
    if (!mounted_) return ERR_NOT_MOUNTED;
    if (!loggedIn_) return ERR_ACCESS_DENIED;
    removed.clear();
    skipped.clear();

    std::vector<std::pair<std::string, DirEntry>> all;
    walkTree(all);
    for (const auto& item : all) {
        const DirEntry& e = item.second;
        if (e.attributes & (ATTR_DIRECTORY | ATTR_TRASHED | ATTR_SYSTEM)) continue;
        if (cmpDate(e.created, border) >= 0) continue;   // новее границы
        // защищённые файлы не трогаем: только чтение либо нет права записи
        if ((e.attributes & ATTR_READONLY) || !canWrite(e)) {
            skipped.push_back(item.first);
            continue;
        }
        if (removeByPath(splitPath(item.first), /*wipe=*/false) == OK)
            removed.push_back(item.first);
    }
    return OK;
}

ErrorCode GladFS::findMostAccessed(std::string& outPath, DirEntry& out) const {
    if (!mounted_) return ERR_NOT_MOUNTED;
    std::vector<std::pair<std::string, DirEntry>> all;
    walkTree(all);
    const std::pair<std::string, DirEntry>* best = nullptr;
    for (const auto& item : all) {
        const DirEntry& e = item.second;
        if (e.attributes & (ATTR_DIRECTORY | ATTR_TRASHED | ATTR_SYSTEM)) continue;
        if (best == nullptr || e.accessCount > best->second.accessCount)
            best = &item;
    }
    if (best == nullptr) return ERR_FILE_NOT_FOUND;
    outPath = best->first;
    out = best->second;
    return OK;
}

// ============================================================================
//  Служебные операции
// ============================================================================

ErrorCode GladFS::chainOf(const std::string& path, std::vector<uint32_t>& blocks) const {
    if (!mounted_) return ERR_NOT_MOUNTED;
    std::vector<std::string> comps = splitPath(path);
    if (comps.empty() || comps.back() == "..") return ERR_NAME_INVALID;
    std::vector<DirHandle> stack;
    ErrorCode r = openPath(comps, comps.size() - 1, stack);
    if (r != OK) return r;
    const DirEntry* e = tableFind(stack.back().table, comps.back());
    if (e == nullptr) return ERR_FILE_NOT_FOUND;
    if (e->firstBlock == FAT_EOF) {
        blocks.clear();
        return OK;
    }
    if (!walkChain(e->firstBlock, blocks)) return ERR_CHAIN_BROKEN;
    return OK;
}

ErrorCode GladFS::importFile(const std::string& hostPath, const std::string& path) {
    if (!mounted_) return ERR_NOT_MOUNTED;
    if (!loggedIn_) return ERR_ACCESS_DENIED;
    std::vector<std::string> comps = splitPath(path);
    if (comps.empty() || comps.back() == "..") return ERR_NAME_INVALID;

    std::FILE* f = openHostFile(hostPath, "rb");
    if (f == nullptr) return ERR_HOST_IO;
    std::string data;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    bool bad = std::ferror(f) != 0;
    std::fclose(f);
    if (bad) return ERR_HOST_IO;

    DirEntry probe;
    if (statFile(path, probe) != OK) {
        ErrorCode r = createFile(path);
        if (r != OK) return r;
    }
    return writeFile(path, data, /*append=*/false);
}

ErrorCode GladFS::exportFile(const std::string& path, const std::string& hostPath) {
    if (!mounted_) return ERR_NOT_MOUNTED;
    std::string data;
    ErrorCode r = readFile(path, data);   // чтение — тоже обращение к файлу
    if (r != OK) return r;

    std::FILE* f = openHostFile(hostPath, "wb");
    if (f == nullptr) return ERR_HOST_IO;
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = (std::fclose(f) == 0) && ok;
    return ok ? OK : ERR_HOST_IO;
}

DiskStats GladFS::stats() const {
    DiskStats s;
    s.volumeName = super_.volumeName;
    s.createdText = dateToString(super_.created);
    s.totalBlocks = TOTAL_BLOCKS;
    s.blockSize = BLOCK_SIZE;
    s.encrypted = encryptionEnabled();
    s.freeBlocks = 0;
    for (uint32_t b = DATA_FIRST; b < TOTAL_BLOCKS; ++b)
        if (fat_[b] == FAT_FREE) ++s.freeBlocks;
    s.usedBlocks = DATA_BLOCKS - s.freeBlocks;
    s.fileCount = 0;
    s.dirCount = 0;
    s.fragmentedFiles = 0;

    std::vector<std::pair<std::string, DirEntry>> all;
    walkTree(all);
    for (const auto& item : all) {
        const DirEntry& e = item.second;
        if (e.attributes & ATTR_TRASHED) continue;
        if (e.attributes & ATTR_SYSTEM) continue;   // служебные файлы не считаем
        if (e.attributes & ATTR_DIRECTORY) ++s.dirCount;
        else ++s.fileCount;
        std::vector<uint32_t> chain;
        if (e.firstBlock == FAT_EOF || !walkChain(e.firstBlock, chain)) continue;
        for (size_t i = 1; i < chain.size(); ++i)
            if (chain[i] != chain[i - 1] + 1) {   // блоки не соседние — фрагмент
                ++s.fragmentedFiles;
                break;
            }
    }
    return s;
}

// ============================================================================
//  Дерево каталогов для fsck
// ============================================================================

void GladFS::loadDirTree(TreeNode& node, FsckReport& rep, std::set<uint32_t>& visited,
                         int depth) {
    if (!node.isRoot) {
        if (depth > static_cast<int>(MAX_DIR_DEPTH)) {
            ++rep.problems;
            rep.log.push_back("каталог " + node.path +
                              ": превышена глубина дерева (возможен цикл)");
            return;
        }
        if (node.self.firstBlock == FAT_EOF) return;   // пустой каталог
        if (visited.count(node.self.firstBlock)) {
            ++rep.problems;
            rep.log.push_back("каталог " + node.path +
                              ": цикл в дереве каталогов — содержимое пропущено");
            return;
        }
        visited.insert(node.self.firstBlock);
        ErrorCode r = loadDirTable(node.self, node.table);
        if (r != OK) {
            node.loaded = false;
            ++rep.problems;
            rep.log.push_back("каталог " + node.path + ": содержимое нечитается (" +
                              errorText(r) + ") — проверка внутри пропущена");
            return;
        }
    }
    // Открываем подкаталоги (их записи участвуют в проверках своего узла)
    for (const auto& e : node.table) {
        if (!(e.flags & ENTRY_USED)) continue;
        if (!(e.attributes & ATTR_DIRECTORY)) continue;
        TreeNode child;
        child.self = e;
        child.selfName = entryName(e);
        child.path = node.path.empty() ? child.selfName : node.path + "/" + child.selfName;
        node.children.push_back(std::move(child));
    }
    for (auto& c : node.children)
        loadDirTree(c, rep, visited, depth + 1);
}

ErrorCode GladFS::saveDirTree(TreeNode& node, bool& changedOut) {
    for (auto& c : node.children) {
        bool ch = false;
        ErrorCode r = saveDirTree(c, ch);
        if (r != OK) return r;
        if (ch) {
            // содержимое каталога изменилось — обновляем его запись у родителя
            for (auto& p : node.table) {
                if ((p.flags & ENTRY_USED) && entryName(p) == c.selfName) {
                    p = c.self;
                    node.dirty = true;
                    break;
                }
            }
            changedOut = true;
        }
    }
    if (node.isRoot) {
        root_ = node.table;
        return OK;
    }
    if (node.dirty) {
        ErrorCode r = writeFileCore(&node.self, serializeTable(node.table),
                                    /*append=*/false);
        if (r != OK) return r;
        node.dirty = false;
        changedOut = true;
    }
    return OK;
}

FsckReport GladFS::fsck(bool repair) {
    FsckReport rep;
    rep.checkedBlocks = TOTAL_BLOCKS;
    bool changed = false;

    // 1. Служебные блоки должны быть помечены как RESERVED
    for (uint32_t i = 0; i < DATA_FIRST; ++i) {
        if (fat_[i] != FAT_RESERVED) {
            ++rep.problems;
            rep.log.push_back("блок " + std::to_string(i) +
                              ": служебный блок не помечен как RESERVED");
            if (repair) { fat_[i] = FAT_RESERVED; ++rep.repaired; changed = true; }
        }
    }

    // 2. Загружаем дерево каталогов (корень + все подкаталоги)
    TreeNode root;
    root.isRoot = true;
    root.table = root_;
    std::set<uint32_t> visited;
    loadDirTree(root, rep, visited, 0);

    // 3. Обход всех записей всего дерева. Сначала только собираем информацию:
    // refCount считает, скольким файлам нужен каждый блок; кандидаты на
    // освобождение запоминаются и освобождаются ПОСЛЕ обхода всех файлов —
    // иначе можно освободить блок, который нужен другому файлу.
    std::vector<uint16_t> refCount(TOTAL_BLOCKS, 0);
    std::vector<uint32_t> excessCandidates;

    std::function<void(TreeNode&)> checkNode = [&](TreeNode& node) {
        std::string prefix = node.path.empty() ? "" : node.path + "/";
        for (auto& e : node.table) {
            if (!(e.flags & ENTRY_USED) || e.firstBlock == FAT_EOF) continue;
            std::string name = prefix + entryName(e);
            if (e.attributes & ATTR_DIRECTORY) name += "/";
            if (e.attributes & ATTR_TRASHED) name += " (корзина)";

            // 3а. Первый блок не должен быть свободным или служебным
            if (e.firstBlock < DATA_FIRST || e.firstBlock >= TOTAL_BLOCKS ||
                fat_[e.firstBlock] == FAT_FREE) {
                ++rep.problems;
                rep.log.push_back("файл " + name + ": первый блок цепочки повреждён — файл обнулён");
                if (repair) {
                    e.firstBlock = FAT_EOF;
                    e.size = 0;
                    ++rep.repaired;
                    changed = true;
                    node.dirty = true;
                }
                continue;
            }

            // 3б. Толерантный обход: собираем блоки до первого повреждения
            // (цикл, ссылка за пределы диска или «свободный» блок внутри цепочки)
            std::vector<uint32_t> chain;
            bool damaged = false;
            {
                std::vector<bool> seen(TOTAL_BLOCKS, false);
                uint16_t cur = e.firstBlock;
                while (true) {
                    if (seen[cur]) { damaged = true; break; }          // цикл
                    seen[cur] = true;
                    chain.push_back(cur);
                    uint16_t next = fat_[cur];
                    if (next == FAT_EOF) break;                        // нормальный конец
                    if (next < DATA_FIRST || next >= TOTAL_BLOCKS) {   // ссылка в никуда
                        damaged = true;
                        break;
                    }
                    if (fat_[next] == FAT_FREE) {                      // потеряна ссылка вперёд
                        chain.push_back(next);                         // данные целы — забираем блок
                        damaged = true;
                        break;
                    }
                    cur = next;
                }
            }
            if (damaged) {
                ++rep.problems;
                rep.log.push_back("файл " + name + ": цепочка повреждена — обрезана до " +
                                  std::to_string(chain.size()) + " бл.");
                if (repair) {
                    fat_[chain.back()] = FAT_EOF;
                    ++rep.repaired;
                    changed = true;
                    node.dirty = true;
                }
            }

            // 3в. Блок не может принадлежать двум файлам
            size_t keep = chain.size();
            for (size_t i = 0; i < chain.size(); ++i) {
                if (refCount[chain[i]] == 0) continue;
                ++rep.problems;
                rep.log.push_back("файл " + name + ": блок " + std::to_string(chain[i]) +
                                  " принадлежит другому файлу — цепочка обрезана");
                keep = i;
                if (repair) {
                    if (i == 0) e.firstBlock = FAT_EOF;
                    else        fat_[chain[i - 1]] = FAT_EOF;
                    ++rep.repaired;
                    changed = true;
                    node.dirty = true;
                }
                break;
            }
            chain.resize(keep);
            if (chain.empty()) {
                if (repair && keep == 0 && e.firstBlock != FAT_EOF) {
                    e.size = 0;
                    node.dirty = true;
                }
                continue;
            }

            // 3г. Соответствие размера файла и длины цепочки
            uint32_t cap = static_cast<uint32_t>(chain.size()) * BLOCK_SIZE;
            if (e.size > cap) {
                ++rep.problems;
                rep.log.push_back("файл " + name + ": размер " + std::to_string(e.size) +
                                  " больше ёмкости цепочки — усечён до " + std::to_string(cap));
                if (repair) {
                    e.size = cap;
                    ++rep.repaired;
                    changed = true;
                    node.dirty = true;
                }
            }

            // Излишек цепочки: указатель обрезаем сразу, но блоки освобождаем
            // только после обхода всех файлов (вдруг они нужны другому файлу)
            uint32_t need = blocksFor(e.size);
            if (chain.size() > need) {
                uint32_t excess = static_cast<uint32_t>(chain.size()) - need;
                ++rep.problems;
                rep.log.push_back("файл " + name + ": цепочка длиннее файла на " +
                                  std::to_string(excess) + " бл.");
                if (repair) {
                    if (need == 0) e.firstBlock = FAT_EOF;
                    else           fat_[chain[need - 1]] = FAT_EOF;
                    for (uint32_t j = need; j < chain.size(); ++j)
                        excessCandidates.push_back(chain[j]);
                    ++rep.repaired;
                    changed = true;
                    node.dirty = true;
                }
                chain.resize(need);
            }

            // 3д. Контрольная сумма содержимого: ловит подмену и повреждение.
            // Восстановить содержимое по сумме нельзя — она обновится, только
            // когда в файл запишут новые данные. (Для каталогов содержимое
            // уже прочитано при загрузке дерева; здесь проверяем и его.)
            {
                std::string data;
                ErrorCode rd = readEntryData(e, data);
                if (rd == ERR_DATA_CORRUPT) {
                    ++rep.problems;
                    rep.log.push_back("файл " + name +
                                      ": контрольная сумма не совпадает — данные подменены"
                                      " или повреждены");
                }
            }

            // 3е. Отмечаем блоки, которые этому файлу нужны
            for (uint32_t b : chain) ++refCount[b];
        }
        for (auto& c : node.children) checkNode(c);
    };
    checkNode(root);

    // 4. Освобождение лишних блоков (после обхода всех файлов)
    if (!excessCandidates.empty()) {
        uint32_t freed = 0;
        for (uint32_t b : excessCandidates) {
            if (refCount[b] != 0) continue;   // блок нужен другому файлу — не трогаем
            if (repair) { fat_[b] = FAT_FREE; changed = true; }
            ++freed;
        }
        if (freed > 0) {
            ++rep.problems;
            rep.log.push_back("лишних блоков освобождено: " + std::to_string(freed));
            if (repair) ++rep.repaired;
        }
    }

    // 5. Потерянные блоки: заняты по FAT, но не нужны ни одному файлу
    {
        uint32_t orphans = 0;
        for (uint32_t b = DATA_FIRST; b < TOTAL_BLOCKS; ++b)
            if (fat_[b] != FAT_FREE && refCount[b] == 0) ++orphans;
        if (orphans > 0) {
            ++rep.problems;
            rep.log.push_back("потерянных блоков (заняты, но без файла): " +
                              std::to_string(orphans));
            if (repair) {
                for (uint32_t b = DATA_FIRST; b < TOTAL_BLOCKS; ++b)
                    if (fat_[b] != FAT_FREE && refCount[b] == 0) fat_[b] = FAT_FREE;
                ++rep.repaired;
                changed = true;
            }
        }
    }

    // 6. Счётчики суперблока
    {
        uint32_t realFree = 0;
        for (uint32_t b = DATA_FIRST; b < TOTAL_BLOCKS; ++b)
            if (fat_[b] == FAT_FREE) ++realFree;
        uint32_t realFiles = 0;
        for (const auto& e : root_)
            if ((e.flags & ENTRY_USED) && !(e.attributes & ATTR_TRASHED)) ++realFiles;
        if (super_.freeBlocks != realFree || super_.fileCount != realFiles) {
            ++rep.problems;
            rep.log.push_back("счётчики суперблока не соответствуют действительности");
            if (repair) {
                super_.freeBlocks = realFree;
                super_.fileCount = realFiles;
                ++rep.repaired;
                changed = true;
            }
        }
    }

    // Сохраняем изменённые таблицы каталогов (снизу вверх) и метаданные
    if (repair && changed) {
        bool treeChanged = false;
        saveDirTree(root, treeChanged);
        persistMetadata();
    }
    return rep;
}

ErrorCode GladFS::rawPatch(uint32_t block, uint32_t offset, uint8_t value) {
    if (!mounted_) return ERR_NOT_MOUNTED;
    if (block >= TOTAL_BLOCKS || offset >= BLOCK_SIZE) return ERR_BAD_ARGS;

    std::string buf(BLOCK_SIZE, '\0');
    if (!disk_.readBlock(block, &buf[0])) return ERR_IO;
    buf[offset] = static_cast<char>(value);
    if (!disk_.writeBlock(block, buf.data())) return ERR_IO;

    // Правка сделана «в обход» ФС: перечитываем структуры с диска.
    // Ключ шифрования не меняется: он выводится из пароля, а не лежит на диске.
    std::string path = disk_.path();
    std::string password = volumePassword_;   // unmount() сотрёт состояние
    bool wasLoggedIn = loggedIn_;
    uint8_t uid = currentUid_;
    bool wasAdmin = admin_;
    unmount();
    std::string error;
    if (!mount(path, password, error)) return ERR_BAD_FORMAT;
    // восстанавливаем сеанс пользователя (пароль тома не равен паролю входа)
    loggedIn_ = wasLoggedIn;
    currentUid_ = uid;
    admin_ = wasAdmin;
    return OK;
}

bool GladFS::rawRead(uint32_t block, void* buf) const {
    return disk_.readBlock(block, buf);
}

std::string GladFS::diskMap() const {
    std::string map(TOTAL_BLOCKS, '.');
    for (uint32_t b = 0; b < TOTAL_BLOCKS; ++b) {
        char c;
        if (b == SUPERBLOCK_NO)          c = 'S';
        else if (b >= FAT_FIRST && b < FAT_FIRST + FAT_BLOCKS)         c = 'F';
        else if (b >= ROOT_FIRST && b < ROOT_FIRST + ROOT_BLOCKS)      c = 'R';
        else if (fat_[b] != FAT_FREE)    c = '#';
        else                             c = '.';
        map[b] = c;
    }
    return map;
}

}  // namespace gladfs
