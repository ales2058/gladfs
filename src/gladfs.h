#pragma once
// ============================================================================
//  Слои 2–3 — файловая система.
//
//  Раскладка образа disk.img (2048 блоков по 512 байт):
//
//    блок 0        суперблок: сигнатура, версия, геометрия, счётчики,
//                  настройки шифрования (соль, контрольный блок)
//    блоки 1..8    FAT — по одной записи uint16_t на каждый блок диска:
//                    FAT_FREE     — блок свободен
//                    FAT_EOF      — последний блок цепочки файла
//                    FAT_RESERVED — служебный блок (суперблок, FAT, каталог)
//                    иначе        — номер СЛЕДУЮЩЕГО блока этого же файла
//    блоки 9..24   корневой каталог: 128 записей DirEntry по 64 байта
//    блоки 25..    данные файлов
//
//  Идея та же, что в односвязном списке из лабы №1: поле FAT[i] — это «next»,
//  только хранится список не в оперативной памяти, а на диске, и элементами
//  его являются блоки, а не узлы. Файл = цепочка: каталог -> первый блок ->
//  FAT[first] -> ... -> FAT[x] == FAT_EOF.
//
//  Дополнительно к базовой версии (формат 2):
//    - шифрование данных файлов: XTEA-CTR, ключ из пароля тома;
//    - пользователи и права доступа (rwx для владельца и остальных);
//    - корзина: rm прячет файл, restore возвращает, purge чистит с затиранием.
// ============================================================================

#include <cstdint>
#include <set>
#include <string>
#include <vector>

#include "blockdevice.h"
#include "constants.h"
#include "crypto.h"
#include "date.h"

namespace gladfs {

#pragma pack(push, 1)  // структуры лежат на диске вплотную, без выравнивания

// --- Суперблок (блок 0): паспорт файловой системы --------------------------
struct SuperBlock {
    char     signature[8];        // "GLADFS1\0"
    uint16_t version;             // версия формата
    uint32_t blockSize;           // размер блока
    uint32_t totalBlocks;         // всего блоков на диске
    uint32_t fatFirstBlock;       // номер первого блока FAT
    uint32_t rootFirstBlock;      // номер первого блока каталога
    uint32_t dataFirstBlock;      // номер первого блока данных
    uint32_t freeBlocks;          // свободных блоков (для быстрого df)
    uint32_t fileCount;           // файлов в каталоге
    Date     created;             // дата создания файловой системы
    char     volumeName[VOLUME_NAME_MAX + 1];  // имя тома, оканчивается '\0'
    uint8_t  cryptoFlags;         // CRYPTO_ENABLED — шифрование включено
    uint8_t  salt[8];             // соль для вывода ключа из пароля тома
    uint8_t  verifier[8];         // контрольный блок правильности пароля
    uint8_t  reserved[BLOCK_SIZE - 8 - 2 - 4 * 7 - 6 - (VOLUME_NAME_MAX + 1) - 1 - 8 - 8];
};
#pragma pack(pop)

static_assert(sizeof(SuperBlock) == BLOCK_SIZE, "суперблок занимает ровно блок");

// --- Запись каталога: метаданные одного файла (64 байта) --------------------
//  Поля «имя», «дата создания» и «количество обращений» — те самые три
//  параметра из постановки задачи варианта №6; размер, атрибуты, первый
//  блок, права и остальные даты добавлены, как это делается в настоящих ФС.
#pragma pack(push, 1)
struct DirEntry {
    char     name[NAME_MAX + 1];  // имя файла, оканчивается '\0'
    uint8_t  attributes;          // ATTR_READONLY | ATTR_HIDDEN | ATTR_SYSTEM | ATTR_TRASHED
    uint8_t  flags;               // ENTRY_USED — запись занята
    uint16_t firstBlock;          // первый блок цепочки; FAT_EOF — файл пуст
    uint32_t size;                // размер файла в байтах
    uint32_t accessCount;         // количество обращений к файлу
    Date     created;             // дата создания
    Date     modified;            // дата последнего изменения
    Date     lastAccess;          // дата последнего обращения
    uint32_t nonce;               // nonce шифрования этого файла (CTR-режим)
    uint8_t  ownerUid;            // владелец (идентификатор пользователя)
    uint8_t  permissions;         // права: старший полубайт — владелец, младший — остальные
    uint32_t checksum;            // CRC32 содержимого (защита от подмены/повреждения)
};
#pragma pack(pop)

static_assert(sizeof(DirEntry) == DIRENTRY_SIZE, "запись каталога — ровно 64 байта");

// --- Запись таблицы пользователей (64 байта в скрытом системном файле) ------
#pragma pack(push, 1)
struct UserRecord {
    char    name[NAME_MAX_USERS + 1];  // имя пользователя, оканчивается '\0'
    uint8_t salt[8];                   // соль хеша пароля
    uint8_t hash[8];                   // хеш пароля (KDF на XTEA)
    uint8_t uid;                       // идентификатор пользователя
    uint8_t flags;                     // USER_USED | USER_ADMIN
    uint8_t used;                      // резервное поле совместимости
    uint8_t reserved[64 - (NAME_MAX_USERS + 1) - 8 - 8 - 1 - 1 - 1];
};
#pragma pack(pop)

static_assert(sizeof(UserRecord) == DIRENTRY_SIZE, "запись пользователя — 64 байта");

// --- Коды результатов операций ----------------------------------------------
enum ErrorCode {
    OK = 0,
    ERR_NOT_MOUNTED,     // ФС не смонтирована
    ERR_NO_DISK,         // образ не найден
    ERR_BAD_FORMAT,      // образ не является диском GladFS (или повреждён)
    ERR_IO,              // ошибка чтения/записи образа
    ERR_DISK_FULL,       // на диске нет места
    ERR_DIR_FULL,        // каталог переполнен (128 файлов)
    ERR_FILE_NOT_FOUND,
    ERR_FILE_EXISTS,
    ERR_NAME_INVALID,    // недопустимые символы или длина имени
    ERR_READ_ONLY,       // файл защищён атрибутом «только чтение»
    ERR_CHAIN_BROKEN,    // цепочка FAT повреждена
    ERR_TOO_BIG,         // файл не помещается на диск
    ERR_HOST_IO,         // ошибка доступа к файлу на «большом» диске
    ERR_BAD_ARGS,        // недопустимые аргументы команды
    ERR_ACCESS_DENIED,   // нет прав на операцию
    ERR_WRONG_PASSWORD,  // неверный пароль
    ERR_USER_EXISTS,     // пользователь с таким именем уже есть
    ERR_USER_NOT_FOUND,  // пользователь не найден
    ERR_DATA_CORRUPT,    // контрольная сумма не совпала: данные подменены/повреждены
    ERR_IS_DIRECTORY,    // операция неприменима к каталогу
    ERR_NOT_DIRECTORY,   // путь указывает не на каталог
    ERR_DIR_NOT_EMPTY    // каталог не пуст (rmdir невозможен)
};

// Русское описание кода ошибки
std::string errorText(ErrorCode code);

// Имя файла из записи каталога (с защитой от повреждённого образа)
std::string entryName(const DirEntry& e);

// Строка прав в виде «rw-r--»
std::string permString(uint8_t permissions);

// Разбор пути на компоненты: «docs/2026/x.txt» → {docs, 2026, x.txt}.
// Пустые фрагменты и «.» пропускаются; «..» сохраняется (обрабатывается
// при обходе — на уровень выше, из корня — игнорируется).
std::vector<std::string> splitPath(const std::string& path);

// --- Сводка состояния диска (для команды df) --------------------------------
struct DiskStats {
    std::string volumeName;
    std::string createdText;   // дата создания тома
    uint32_t    totalBlocks;
    uint32_t    blockSize;
    uint32_t    freeBlocks;
    uint32_t    usedBlocks;
    uint32_t    fileCount;
    uint32_t    dirCount;         // подкаталоги (не считая корня)
    uint32_t    fragmentedFiles;  // файлы, цепочка которых не непрерывна
    bool        encrypted = false;
};

// --- Отчёт fsck --------------------------------------------------------------
struct FsckReport {
    int checkedBlocks = 0;
    int problems = 0;    // найдено нарушений
    int repaired = 0;    // исправлено
    std::vector<std::string> log;  // описание каждой проблемы
};

class GladFS {
public:
    // -- жизненный цикл -------------------------------------------------------
    // format создаёт том; пароль тома включает шифрование (пустой — выключает),
    // rootPassword — пароль администратора для входа
    bool format(const std::string& imagePath, const std::string& volumeName,
                const std::string& volumePassword, const std::string& rootPassword,
                std::string& error);
    bool mount(const std::string& imagePath, const std::string& volumePassword,
               std::string& error);
    void unmount();
    bool mounted() const { return mounted_; }
    const std::string& imagePath() const { return disk_.path(); }

    // -- пользователи и права ---------------------------------------------------
    // login проверяет имя и пароль по таблице пользователей и запоминает,
    // от чьего имени работают файловые операции
    ErrorCode login(const std::string& user, const std::string& password);
    std::string currentUser() const;
    uint8_t    currentUid() const { return currentUid_; }
    bool       currentIsAdmin() const { return admin_; }
    bool       loggedIn() const { return loggedIn_; }
    const std::vector<UserRecord>& users() const { return users_; }
    ErrorCode addUser(const std::string& name, const std::string& password);
    ErrorCode setPassword(const std::string& name, const std::string& newPassword);
    ErrorCode changeOwner(const std::string& file, const std::string& userName);
    ErrorCode setPermissions(const std::string& file, uint8_t perms);

    // -- операции над файлами (имена — пути: «docs/x.txt», «/docs» и т.п.) ----
    ErrorCode createFile(const std::string& path);
    ErrorCode writeFile(const std::string& path, const std::string& data, bool append);
    ErrorCode readFile(const std::string& path, std::string& out);
    ErrorCode renameFile(const std::string& oldPath, const std::string& newPath);
    ErrorCode setAttributes(const std::string& path, uint8_t mask, bool set);

    // -- каталоги ---------------------------------------------------------------
    ErrorCode makeDirectory(const std::string& path);
    ErrorCode removeDirectory(const std::string& path);      // rmdir: только пустой
    ErrorCode testDirectory(const std::string& path);        // проверка «это каталог»
    ErrorCode listDir(const std::string& path, std::vector<DirEntry>& out,
                      bool includeHidden) const;

    // -- корзина ---------------------------------------------------------------
    ErrorCode trashFile(const std::string& path);   // rm без флагов
    ErrorCode removeFile(const std::string& path, bool wipe);  // rm -f / rm -w
    ErrorCode restoreFile(const std::string& path);
    ErrorCode purgeTrash();                          // только администратор

    // -- просмотр -------------------------------------------------------------
    ErrorCode statFile(const std::string& path, DirEntry& out) const;

    // Обход всего дерева: пары (путь от корня, копия записи)
    ErrorCode walkTree(std::vector<std::pair<std::string, DirEntry>>& out) const;
    ErrorCode listTrash(std::vector<std::pair<std::string, DirEntry>>& out) const;

    // -- операции из постановки варианта №6 ------------------------------------
    ErrorCode removeOlderThan(const Date& border, std::vector<std::string>& removed,
                              std::vector<std::string>& skipped);
    ErrorCode findMostAccessed(std::string& outPath, DirEntry& out) const;

    // -- служебные операции -----------------------------------------------------
    ErrorCode chainOf(const std::string& path, std::vector<uint32_t>& blocks) const;
    ErrorCode importFile(const std::string& hostPath, const std::string& path);
    ErrorCode exportFile(const std::string& path, const std::string& hostPath);
    DiskStats  stats() const;
    FsckReport fsck(bool repair);

    // Прямая правка байта образа (для демонстрации восстановления после сбоя)
    ErrorCode rawPatch(uint32_t block, uint32_t offset, uint8_t value);
    // Чтение произвольного блока образа (для hexdump)
    bool rawRead(uint32_t block, void* buf) const;
    // Карта занятости диска: один символ на блок ('S','F','R','#','.')
    std::string diskMap() const;
    bool encryptionEnabled() const { return (super_.cryptoFlags & CRYPTO_ENABLED) != 0; }

private:
    // Загруженный каталог: таблица записей + сведения, как сохранить её назад.
    // Корень хранится в root_ (isRoot=true), подкаталоги — файлы со своим
    // содержимым; dirty помечает изменённые таблицы.
    struct DirHandle {
        std::vector<DirEntry> table;
        bool isRoot = true;
        bool dirty = false;
        DirEntry self{};       // запись этого каталога в родительской таблице
        std::string selfName;  // имя в родителе (для записи self обратно)
    };

    // Узел дерева каталогов (для fsck и обхода всего дерева)
    struct TreeNode {
        bool isRoot = false;
        bool dirty = false;
        bool loaded = true;    // удалось ли прочитать содержимое
        std::string path;      // путь каталога от корня (для сообщений fsck)
        DirEntry self{};
        std::string selfName;
        std::vector<DirEntry> table;
        std::vector<TreeNode> children;
    };

    // Поиск записи в таблице каталога; nullptr — если не найдена.
    // Корзина исключается, если includeTrashed == false.
    static DirEntry* tableFind(std::vector<DirEntry>& table, const std::string& name,
                               bool includeTrashed = false);
    static const DirEntry* tableFind(const std::vector<DirEntry>& table,
                                     const std::string& name, bool includeTrashed = false);
    // Поиск записи в КОРНЕВОМ каталоге (кэш root_)
    DirEntry* findEntry(const std::string& name, bool includeTrashed = false);
    const DirEntry* findEntry(const std::string& name, bool includeTrashed = false) const;

    // Прочитать содержимое каталога (файла с атрибутом DIRECTORY)
    ErrorCode loadDirTable(const DirEntry& dir, std::vector<DirEntry>& out) const;
    // Открыть каталоги по первым count компонентам пути; стек[0] — корень.
    // «..» поднимает уровень, в корне игнорируется. Диск не изменяет.
    ErrorCode openPath(const std::vector<std::string>& comps, size_t count,
                       std::vector<DirHandle>& stack) const;
    // Сохранить изменённые таблицы: снизу вверх, запись каталога обновляется
    // в родительской таблице (цепочка сохранений до корня)
    ErrorCode flushDirStack(std::vector<DirHandle>& stack);
    // Загрузка дерева каталогов (для fsck) и сохранение изменённых таблиц
    void loadDirTree(TreeNode& node, FsckReport& rep, std::set<uint32_t>& visited,
                     int depth);
    ErrorCode saveDirTree(TreeNode& node, bool& changedOut);

    // Проверки прав текущего пользователя
    bool canRead(const DirEntry& e) const;
    bool canWrite(const DirEntry& e) const;
    bool canManage(const DirEntry& e) const;  // владелец или администратор

    // Выделить цепочку из count свободных блоков (first — её начало)
    ErrorCode allocateChain(uint32_t count, uint16_t& first);
    // Продлить существующую цепочку, начинающуюся с tail, на count блоков
    ErrorCode extendChain(uint16_t tail, uint32_t count);
    // Освободить всю цепочку, начиная с first
    void freeChain(uint16_t first);
    // Пройти цепочку, собирая номера блоков; false — если она зациклена
    bool walkChain(uint16_t first, std::vector<uint32_t>& blocks) const;

    // Записать на диск суперблок, FAT и каталог (точка фиксации изменений)
    bool persistMetadata();
    // Пересчитать свободные блоки и число файлов по FAT и каталогу
    void refreshCounters();
    // Прочитать байты файла с диска (расшифровав, если нужно; без прав и счётчиков)
    ErrorCode readEntryData(const DirEntry& entry, std::string& out) const;
    // Ядро записи: размещение, шифрование и запись данных (без проверок прав)
    ErrorCode writeFileCore(DirEntry* e, const std::string& data, bool append);
    // Создание записи каталога (внутренняя операция без проверок имени)
    ErrorCode createEntry(const char* name, uint8_t attrs, uint8_t perms, uint8_t uid);
    // Служебная запись файла (внутренние нужды ФС: таблица пользователей)
    ErrorCode writeSystemFile(const std::string& name, const std::string& data);
    ErrorCode readSystemFile(const std::string& name, std::string& out);
    // Удаление записи по компонентам пути БЕЗ проверок прав (для rmold/purge)
    ErrorCode removeByPath(const std::vector<std::string>& comps, bool wipe);

    // Таблица пользователей: загрузка/сохранение/поиск
    ErrorCode loadUsers();
    ErrorCode saveUsers();
    UserRecord* findUser(const std::string& name);
    ErrorCode createUsersFile(const std::string& rootPassword);

    BlockDevice disk_;
    SuperBlock  super_{};
    std::vector<uint16_t> fat_;    // кэш FAT (2048 записей)
    std::vector<DirEntry> root_;   // кэш каталога (128 записей)
    bool mounted_ = false;

    std::string volumePassword_;   // пароль тома (только в ОЗУ; для перемонтирования)
    XteaKey volumeKey_ = {};       // ключ шифрования тома (после mount)
    std::vector<UserRecord> users_;
    bool loggedIn_ = false;
    uint8_t currentUid_ = 0;
    bool admin_ = false;
};

}  // namespace gladfs
