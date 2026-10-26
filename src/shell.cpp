// ============================================================================
//  Слой 4 — диалоговая оболочка (интерактивный шелл fs>).
//  Здесь разбираются команды пользователя, выводятся таблицы и отчёты,
//  контролируются ошибки ввода (как того требует постановка задачи).
//  Поддерживаются пути: «docs/2026/x.txt», «..», «/» — с текущим каталогом.
// ============================================================================

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "constants.h"
#include "gladfs.h"

using gladfs::DirEntry;
using gladfs::ErrorCode;
using gladfs::OK;
using gladfs::ERR_NOT_MOUNTED;
using gladfs::ERR_IO;
using gladfs::ERR_IS_DIRECTORY;

namespace {

using Args = std::vector<std::string>;

// Разбор строки на слова; текст в кавычках считается одним словом
Args tokenize(const std::string& line) {
    Args out;
    std::string cur;
    bool inQuotes = false;
    for (char c : line) {
        if (c == '"') { inQuotes = !inQuotes; continue; }
        if (!inQuotes && (c == ' ' || c == '\t')) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

bool isUnsignedNumber(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

bool parseUnsigned(const std::string& s, uint32_t& v) {
    if (!isUnsignedNumber(s)) return false;
    unsigned long parsed = std::strtoul(s.c_str(), nullptr, 10);
    v = static_cast<uint32_t>(parsed);
    return true;
}

// Вопрос «да/нет»
bool confirm(const std::string& question) {
    std::cout << question << " (y/n): ";
    std::string answer;
    std::getline(std::cin, answer);
    return !answer.empty() && (answer[0] == 'y' || answer[0] == 'Y');
}

// Ввод строки с повтором (для паролей)
std::string promptLine(const std::string& question) {
    std::cout << question;
    std::string s;
    std::getline(std::cin, s);
    return s;
}

std::string attrString(uint8_t attributes) {
    std::string s = "---";
    if (attributes & gladfs::ATTR_READONLY) s[0] = 'R';
    if (attributes & gladfs::ATTR_HIDDEN)   s[1] = 'H';
    if (attributes & gladfs::ATTR_SYSTEM)   s[2] = 'S';
    return s;
}

// Сокращённая запись цепочки блоков: «13» или «13->17->409->... (5 бл.)»
std::string blocksString(const std::vector<uint32_t>& chain) {
    if (chain.empty()) return "-";
    std::string s = std::to_string(chain[0]);
    for (size_t i = 1; i < chain.size() && i <= 2; ++i)
        s += "->" + std::to_string(chain[i]);
    if (chain.size() > 3)
        s += "->... (" + std::to_string(chain.size()) + " бл.)";
    return s;
}

void printError(ErrorCode code) {
    std::cout << "Ошибка: " << gladfs::errorText(code) << ".\n";
}

// Ширина строки «как её видит пользователь»: в UTF-8 кириллица занимает
// два байта, но выводится одним символом, поэтому setw здесь не подходит.
size_t displayWidth(const std::string& s) {
    size_t w = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) ++w;   // считаем не-continuation байты
    return w;
}

std::string padRight(const std::string& s, size_t width) {
    size_t w = displayWidth(s);
    return s + std::string(width > w ? width - w : 0, ' ');
}

std::string padLeft(const std::string& s, size_t width) {
    size_t w = displayWidth(s);
    return std::string(width > w ? width - w : 0, ' ') + s;
}

struct Shell {
    gladfs::GladFS fs;
    std::string imagePath;
    std::vector<std::string> cwd;   // текущий каталог (компоненты пути)
    bool running = true;

    // Превратить аргумент в абсолютный путь с учётом текущего каталога
    std::vector<std::string> resolveArg(const std::string& arg) const;
    static std::string pathFromComps(const std::vector<std::string>& comps);
    std::string argPath(const Args& args, size_t index) const;
    std::string ownerName(uint8_t uid) const;
    void printDirList(const std::vector<DirEntry>& files, bool longForm);

    void run();
    bool mountInteractive();
    bool loginInteractive();
    bool needMount();

    void cmdHelp(const Args&);
    void cmdFormat(const Args&);
    void cmdLs(const Args&);
    void cmdCd(const Args&);
    void cmdPwd(const Args&);
    void cmdMkdir(const Args&);
    void cmdRmdir(const Args&);
    void cmdCreate(const Args&);
    void cmdWrite(const Args&);
    void cmdCat(const Args&);
    void cmdRm(const Args&);
    void cmdMv(const Args&);
    void cmdStat(const Args&);
    void cmdAttr(const Args&);
    void cmdDf(const Args&);
    void cmdFsck(const Args&);
    void cmdHexdump(const Args&);
    void cmdBlocks(const Args&);
    void cmdFmap(const Args&);
    void cmdImport(const Args&);
    void cmdExport(const Args&);
    void cmdTop(const Args&);
    void cmdRmOld(const Args&);
    void cmdDebug(const Args&);
    // пользователи и права
    void cmdWhoAmI(const Args&);
    void cmdUsers(const Args&);
    void cmdAddUser(const Args&);
    void cmdPasswd(const Args&);
    void cmdChown(const Args&);
    void cmdChmod(const Args&);
    // корзина
    void cmdTrash(const Args&);
    void cmdRestore(const Args&);
    void cmdPurge(const Args&);
};

// ----------------------------------------------------------------------------
std::string Shell::pathFromComps(const std::vector<std::string>& comps) {
    if (comps.empty()) return "/";
    std::string s;
    for (const auto& c : comps) { s += "/"; s += c; }
    return s;
}

std::vector<std::string> Shell::resolveArg(const std::string& arg) const {
    std::vector<std::string> out = (arg.empty() || arg[0] == '/') ?
                                   std::vector<std::string>() : cwd;
    for (const std::string& c : gladfs::splitPath(arg)) {
        if (c == "..") { if (!out.empty()) out.pop_back(); }
        else out.push_back(c);
    }
    return out;
}

std::string Shell::argPath(const Args& args, size_t index) const {
    return pathFromComps(resolveArg(args[index]));
}

// Имя пользователя по uid (для колонки ls)
std::string Shell::ownerName(uint8_t uid) const {
    for (const auto& u : fs.users())
        if ((u.flags & gladfs::USER_USED) && u.uid == uid)
            return std::string(u.name, strnlen(u.name, gladfs::NAME_MAX_USERS + 1));
    return "?" + std::to_string(uid);
}

// ----------------------------------------------------------------------------
// Монтирование: если том зашифрован, сначала пробуем пустой пароль, затем
// спрашиваем реальный (не более трёх попыток).
bool Shell::mountInteractive() {
    std::string error;
    if (fs.mount(imagePath, "", error)) return true;
    if (error != "неверный пароль тома") {
        std::cout << "Диск не смонтирован: " << error << ".\n";
        return false;
    }
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::string password = promptLine("Том зашифрован. Пароль тома: ");
        if (fs.mount(imagePath, password, error)) return true;
        std::cout << "Ошибка: " << error << ".\n";
    }
    std::cout << "Доступ к тому запрещён.\n";
    return false;
}

// Вход пользователя (до трёх попыток)
bool Shell::loginInteractive() {
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::string user = promptLine("Пользователь [root]: ");
        if (user.empty()) user = "root";
        std::string password = promptLine("Пароль: ");
        ErrorCode r = fs.login(user, password);
        if (r == OK) {
            std::cout << "Вход выполнен: " << user
                      << (fs.currentIsAdmin() ? " (администратор)" : "") << ".\n";
            return true;
        }
        printError(r);
    }
    std::cout << "Вход не выполнен — доступ к операциям закрыт.\n";
    return false;
}

void Shell::run() {
    std::cout << "GladFS " << gladfs::SIGNATURE << " — учебная файловая система\n";
    std::cout << "Образ диска: " << imagePath << " ("
              << gladfs::TOTAL_BLOCKS << " блоков по "
              << gladfs::BLOCK_SIZE << " байт = 1 МиБ)\n";
    bool mounted = mountInteractive();
    if (mounted) {
        gladfs::DiskStats st = fs.stats();
        std::cout << "Диск смонтирован: том «" << st.volumeName << "»"
                  << (st.encrypted ? ", шифрование включено" : "")
                  << ", файлов: " << st.fileCount
                  << ", свободно блоков: " << st.freeBlocks << ".\n";
        loginInteractive();
    }
    std::cout << "help — список команд.\n\n";

    std::string line;
    while (running) {
        std::cout << "fs:" << pathFromComps(cwd) << "> ";
        if (!std::getline(std::cin, line)) break;   // конец ввода (Ctrl+Z)
        Args args = tokenize(line);
        if (args.empty()) continue;

        const std::string& cmd = args[0];
        Args rest(args.begin() + 1, args.end());
        if      (cmd == "help" || cmd == "?") cmdHelp(rest);
        else if (cmd == "format")   cmdFormat(rest);
        else if (cmd == "ls")       cmdLs(rest);
        else if (cmd == "cd")       cmdCd(rest);
        else if (cmd == "pwd")      cmdPwd(rest);
        else if (cmd == "mkdir")    cmdMkdir(rest);
        else if (cmd == "rmdir")    cmdRmdir(rest);
        else if (cmd == "create")   cmdCreate(rest);
        else if (cmd == "write")    cmdWrite(rest);
        else if (cmd == "cat")      cmdCat(rest);
        else if (cmd == "rm")       cmdRm(rest);
        else if (cmd == "mv")       cmdMv(rest);
        else if (cmd == "stat")     cmdStat(rest);
        else if (cmd == "attr")     cmdAttr(rest);
        else if (cmd == "df")       cmdDf(rest);
        else if (cmd == "fsck")     cmdFsck(rest);
        else if (cmd == "hexdump")  cmdHexdump(rest);
        else if (cmd == "blocks")   cmdBlocks(rest);
        else if (cmd == "fmap")     cmdFmap(rest);
        else if (cmd == "import")   cmdImport(rest);
        else if (cmd == "export")   cmdExport(rest);
        else if (cmd == "top")      cmdTop(rest);
        else if (cmd == "rmold")    cmdRmOld(rest);
        else if (cmd == "debug")    cmdDebug(rest);
        else if (cmd == "whoami")   cmdWhoAmI(rest);
        else if (cmd == "users")    cmdUsers(rest);
        else if (cmd == "adduser")  cmdAddUser(rest);
        else if (cmd == "passwd")   cmdPasswd(rest);
        else if (cmd == "chown")    cmdChown(rest);
        else if (cmd == "chmod")    cmdChmod(rest);
        else if (cmd == "trash")    cmdTrash(rest);
        else if (cmd == "restore")  cmdRestore(rest);
        else if (cmd == "purge")    cmdPurge(rest);
        else if (cmd == "exit" || cmd == "quit") running = false;
        else std::cout << "Неизвестная команда: " << cmd << " (help — список команд).\n";
    }
    fs.unmount();
    std::cout << "Диск отмонтирован. До встречи!\n";
}

bool Shell::needMount() {
    if (fs.mounted()) return true;
    std::cout << "Ошибка: " << gladfs::errorText(ERR_NOT_MOUNTED) << ".\n";
    return false;
}

// ----------------------------------------------------------------------------
void Shell::cmdHelp(const Args&) {
    std::cout <<
        "Команды GladFS (имена файлов — пути: docs/x.txt, .. , /):\n"
        "  format [том]            создать/очистить диск (спросит пароли)\n"
        "  ls [-l] [-a] [--sort=name|date|access] [путь]\n"
        "                          каталог файлов (-l подробно, -a со скрытыми)\n"
        "  cd [путь]               сменить каталог (без аргумента — в корень)\n"
        "  pwd                     показать текущий каталог\n"
        "  mkdir <имя>             создать каталог\n"
        "  rmdir <имя>             удалить пустой каталог\n"
        "  create <имя>            создать пустой файл\n"
        "  write <имя> [-a] [текст] записать файл (-a дописать; без текста —\n"
        "                          многострочный ввод до строки с точкой)\n"
        "  cat <имя>               вывести содержимое файла (+1 к обращениям)\n"
        "  rm <имя> [-f|-w]        удалить: в корзину / -f сразу / -w с затиранием\n"
        "  trash                   содержимое корзины (по всему дереву)\n"
        "  restore <имя>           вернуть файл из корзины\n"
        "  purge                   очистить корзину с затиранием (администратор)\n"
        "  mv <старое> <новое>     переименовать / перенести в другой каталог\n"
        "  attr <имя> <+|-><ro|hid|sys>  установить/снять атрибут\n"
        "  stat <имя>              все метаданные файла\n"
        "  top                     файл с наибольшим количеством обращений\n"
        "  rmold <дд.мм.гггг>      удалить файлы старше даты по всему дереву\n"
        "  whoami                  текущий пользователь\n"
        "  users                   список пользователей (администратор)\n"
        "  adduser <имя>           добавить пользователя (администратор)\n"
        "  passwd [имя]            сменить пароль (свой; любой — администратору)\n"
        "  chown <файл> <юзер>     сменить владельца (администратор)\n"
        "  chmod <файл> <права>    права: три восьмеричные цифры, напр. 640\n"
        "  df                      сводка по диску\n"
        "  blocks <имя>            цепочка блоков файла в FAT\n"
        "  fmap                    карта занятости диска\n"
        "  hexdump <блок|имя>      шестнадцатеричный дамп блока (шифротекст — как есть)\n"
        "  import <путь> [имя]     скопировать файл с большого диска в GladFS\n"
        "  export <имя> <путь>     скопировать файл из GladFS на большой диск\n"
        "  fsck [--repair]         проверка целостности (и восстановление)\n"
        "  debug write <блок> <смещ> <hex>  правка байта на диске (демо сбоев)\n"
        "  debug crypto            самопроверка шифра XTEA по тест-векторам\n"
        "  exit                    выход\n";
}

void Shell::cmdFormat(const Args& args) {
    if (args.size() > 1) {
        std::cout << "Использование: format [имя_тома]\n";
        return;
    }
    std::string volume = args.empty() ? "GLADISK" : args[0];

    // Форматирование уничтожает всё — спрашиваем подтверждение
    std::FILE* probe = gladfs::openHostFile(imagePath, "rb");
    bool exists = probe != nullptr;
    if (probe != nullptr) std::fclose(probe);
    if (exists && !confirm("Форматирование уничтожит все данные на диске. Продолжить?"))
        return;

    std::string volumePassword;
    std::string password2;
    // Пароль тома: включает шифрование всех данных (пустой — без шифрования)
    while (true) {
        volumePassword = promptLine("Пароль тома (Enter — без шифрования): ");
        password2 = promptLine("Повторите пароль: ");
        if (volumePassword == password2) break;
        std::cout << "Ошибка: пароли не совпадают, повторите.\n";
    }
    // Пароль администратора root для входа в систему
    std::string rootPassword;
    while (true) {
        rootPassword = promptLine("Пароль администратора (root): ");
        password2 = promptLine("Повторите пароль: ");
        if (rootPassword == password2) break;
        std::cout << "Ошибка: пароли не совпадают, повторите.\n";
    }

    std::string error;
    if (fs.format(imagePath, volume, volumePassword, rootPassword, error)) {
        gladfs::DiskStats st = fs.stats();
        std::cout << "Диск отформатирован: том «" << volume << "»"
                  << (st.encrypted ? ", шифрование XTEA-CTR включено" : "")
                  << ", свободно блоков: " << st.freeBlocks
                  << " из " << gladfs::DATA_BLOCKS << ".\n"
                  << "Вы вошли как root (администратор).\n";
        cwd.clear();
    } else {
        std::cout << "Ошибка форматирования: " << error << ".\n";
    }
}

// Печать таблицы каталога (общая для ls и stat-подобного вывода)
void Shell::printDirList(const std::vector<DirEntry>& files, bool longForm) {
    std::cout << "--------------------------------------------------------------------------------\n";
    std::cout << padRight("ИМЯ", 24) << padLeft("РАЗМЕР", 8) << "  "
              << padLeft("СОЗДАН", 10) << " "
              << padLeft("ОБРАЩ", 6) << " "
              << padLeft("АТР", 4) << " "
              << padRight("ВЛАДЕЛЕЦ", 10) << " "
              << "ПРАВА\n";
    std::cout << "--------------------------------------------------------------------------------\n";
    for (const DirEntry& e : files) {
        bool isDir = (e.attributes & gladfs::ATTR_DIRECTORY) != 0;
        std::string name = gladfs::entryName(e) + (isDir ? "/" : "");
        std::cout << padRight(name, 24)
                  << padLeft(std::to_string(e.size), 8) << "  "
                  << padLeft(gladfs::dateToString(e.created), 10) << " "
                  << padLeft(std::to_string(e.accessCount), 6) << " "
                  << padLeft(attrString(e.attributes), 4) << " "
                  << padRight(ownerName(e.ownerUid), 10) << " "
                  << gladfs::permString(e.permissions) << "\n";
    }
    std::cout << "--------------------------------------------------------------------------------\n";
    std::cout << "Записей: " << files.size() << "\n";
    if (longForm) {
        for (const DirEntry& e : files) {
            bool isDir = (e.attributes & gladfs::ATTR_DIRECTORY) != 0;
            std::cout << "  " << gladfs::entryName(e) << (isDir ? "/ (каталог)" : "")
                      << "\n    создан " << gladfs::dateToString(e.created)
                      << ", изменён " << gladfs::dateToString(e.modified)
                      << ", обращение " << gladfs::dateToString(e.lastAccess)
                      << ", атрибуты " << attrString(e.attributes)
                      << ", владелец " << ownerName(e.ownerUid)
                      << ", права " << gladfs::permString(e.permissions) << "\n";
        }
    }
}

void Shell::cmdLs(const Args& args) {
    if (!needMount()) return;
    bool longForm = false, all = false;
    std::string path;
    std::string sortKey = "name";
    for (const std::string& a : args) {
        if (a == "-l") longForm = true;
        else if (a == "-a") all = true;
        else if (a.rfind("--sort=", 0) == 0) {
            sortKey = a.substr(7);
            if (sortKey != "name" && sortKey != "date" && sortKey != "access") {
                std::cout << "Ошибка: сортировка возможна по name, date или access.\n";
                return;
            }
        } else if (path.empty()) {
            path = a;
        } else {
            std::cout << "Неизвестный аргумент " << a << ".\n";
            return;
        }
    }

    std::vector<DirEntry> files;
    ErrorCode r = fs.listDir(argPath({path.empty() ? std::string(".") : path}, 0),
                             files, all);
    if (r == gladfs::ERR_NOT_DIRECTORY) {
        // путь указывает на файл — покажем одну запись
        DirEntry e;
        if (fs.statFile(argPath({path}, 0), e) == OK) {
            printDirList({e}, longForm);
            return;
        }
        printError(r);
        return;
    }
    if (r != OK) { printError(r); return; }
    if (files.empty()) {
        std::cout << "Каталог пуст.\n";
        return;
    }

    if (sortKey == "date")
        std::sort(files.begin(), files.end(), [](const DirEntry& a, const DirEntry& b) {
            return gladfs::cmpDate(a.created, b.created) < 0;
        });
    else if (sortKey == "access")
        std::sort(files.begin(), files.end(), [](const DirEntry& a, const DirEntry& b) {
            return a.accessCount > b.accessCount;
        });
    else
        std::sort(files.begin(), files.end(), [](const DirEntry& a, const DirEntry& b) {
            // каталоги первыми, потом по алфавиту
            bool da = a.attributes & gladfs::ATTR_DIRECTORY;
            bool db = b.attributes & gladfs::ATTR_DIRECTORY;
            if (da != db) return da;
            return gladfs::entryName(a) < gladfs::entryName(b);
        });
    printDirList(files, longForm);
}

void Shell::cmdCd(const Args& args) {
    if (!needMount()) return;
    if (args.size() > 1) {
        std::cout << "Использование: cd [путь]\n";
        return;
    }
    std::vector<std::string> target =
        args.empty() ? std::vector<std::string>() : resolveArg(args[0]);
    ErrorCode r = fs.testDirectory(pathFromComps(target));
    if (r == OK) cwd = target;
    else printError(r);
}

void Shell::cmdPwd(const Args&) {
    std::cout << pathFromComps(cwd) << "\n";
}

void Shell::cmdMkdir(const Args& args) {
    if (!needMount() || args.size() != 1) {
        if (args.size() != 1) std::cout << "Использование: mkdir <имя>\n";
        return;
    }
    ErrorCode r = fs.makeDirectory(argPath(args, 0));
    if (r == OK) std::cout << "Каталог «" << args[0] << "» создан.\n";
    else printError(r);
}

void Shell::cmdRmdir(const Args& args) {
    if (!needMount() || args.size() != 1) {
        if (args.size() != 1) std::cout << "Использование: rmdir <имя>\n";
        return;
    }
    std::vector<std::string> target = resolveArg(args[0]);
    // нельзя удалить текущий каталог или его предка
    if (target.size() <= cwd.size() &&
        std::equal(target.begin(), target.end(), cwd.begin())) {
        std::cout << "Ошибка: это текущий каталог или его предок (сначала cd /).\n";
        return;
    }
    ErrorCode r = fs.removeDirectory(pathFromComps(target));
    if (r == OK) std::cout << "Каталог «" << args[0] << "» удалён.\n";
    else printError(r);
}

void Shell::cmdCreate(const Args& args) {
    if (!needMount() || args.size() != 1) {
        if (args.size() != 1) std::cout << "Использование: create <имя>\n";
        return;
    }
    ErrorCode r = fs.createFile(argPath(args, 0));
    if (r == OK) std::cout << "Файл «" << args[0] << "» создан.\n";
    else printError(r);
}

void Shell::cmdWrite(const Args& args) {
    if (!needMount() || args.empty()) {
        if (args.empty()) std::cout << "Использование: write <имя> [-a] [текст]\n";
        return;
    }
    std::string path = argPath(args, 0);
    // Сначала убеждаемся, что файл существует (и не защищён от записи)
    DirEntry probe;
    ErrorCode st = fs.statFile(path, probe);
    if (st != OK) { printError(st); return; }
    if (probe.attributes & gladfs::ATTR_DIRECTORY) {
        std::cout << "Ошибка: " << gladfs::errorText(ERR_IS_DIRECTORY) << ".\n";
        return;
    }
    if (probe.attributes & gladfs::ATTR_SYSTEM) {
        std::cout << "Ошибка: " << gladfs::errorText(gladfs::ERR_ACCESS_DENIED)
                  << " (системный файл).\n";
        return;
    }
    if (probe.attributes & gladfs::ATTR_READONLY) {
        std::cout << "Ошибка: " << gladfs::errorText(gladfs::ERR_READ_ONLY) << ".\n";
        return;
    }
    bool append = false;
    std::string text;
    for (size_t i = 1; i < args.size(); ++i) {
        if (i == 1 && args[i] == "-a") { append = true; continue; }
        if (!text.empty()) text += ' ';
        text += args[i];
    }
    if (text.empty()) {
        // Многострочный ввод до строки «.»
        std::cout << (append ? "Дописать строки (окончание ввода — строка из одной точки):\n"
                             : "Ввод текста (окончание ввода — строка из одной точки):\n");
        std::string line;
        while (std::getline(std::cin, line)) {
            if (line == ".") break;
            text += line;
            text += '\n';
        }
    }

    ErrorCode r = fs.writeFile(path, text, append);
    if (r == OK) {
        DirEntry e;
        fs.statFile(path, e);
        std::vector<uint32_t> chain;
        fs.chainOf(path, chain);
        std::cout << "Записано байт: " << e.size
                  << ", цепочка блоков: " << blocksString(chain) << "\n";
    } else {
        printError(r);
    }
}

void Shell::cmdCat(const Args& args) {
    if (!needMount() || args.size() != 1) {
        if (args.size() != 1) std::cout << "Использование: cat <имя>\n";
        return;
    }
    std::string path = argPath(args, 0);
    std::string data;
    ErrorCode r = fs.readFile(path, data);
    if (r == ERR_IS_DIRECTORY) {
        std::cout << "Ошибка: это каталог (содержимое покажет ls "
                  << args[0] << ").\n";
        return;
    }
    if (r != OK) { printError(r); return; }
    std::cout << data;
    if (data.empty() || data.back() != '\n') std::cout << "\n";
    DirEntry e;
    fs.statFile(path, e);
    std::cout << "[прочитано " << data.size() << " байт; обращений к файлу: "
              << e.accessCount << "]\n";
}

void Shell::cmdRm(const Args& args) {
    if (!needMount() || args.empty() || args.size() > 2) {
        if (args.empty() || args.size() > 2)
            std::cout << "Использование: rm <имя> [-f|-w]\n"
                         "  без флага — в корзину; -f — сразу; -w — с затиранием нулями\n"
                         "  каталоги удаляет rmdir\n";
        return;
    }
    std::string path = argPath(args, 0);
    std::string mode;
    if (args.size() == 2) mode = args[1];
    if (args.size() == 2 && mode != "-f" && mode != "-w") {
        std::cout << "Использование: rm <имя> [-f|-w]\n";
        return;
    }

    ErrorCode r = OK;
    if (mode == "-w") {
        r = fs.removeFile(path, /*wipe=*/true);
        if (r == OK)
            std::cout << "Файл «" << args[0] << "» удалён с затиранием данных нулями.\n";
    } else if (mode == "-f") {
        r = fs.removeFile(path, /*wipe=*/false);
        if (r == OK)
            std::cout << "Файл «" << args[0] << "» удалён (данные не затёрты — как в настоящих ФС).\n";
    } else {
        r = fs.trashFile(path);
        if (r == OK)
            std::cout << "Файл «" << args[0] << "» перемещён в корзину (restore — вернуть).\n";
    }
    if (r == ERR_IS_DIRECTORY)
        std::cout << "Ошибка: это каталог — используйте rmdir " << args[0] << ".\n";
    else if (r != OK) printError(r);
}

void Shell::cmdTrash(const Args& args) {
    if (!needMount()) return;
    if (!args.empty()) {
        std::cout << "Использование: trash (без параметров)\n";
        return;
    }
    std::vector<std::pair<std::string, DirEntry>> items;
    ErrorCode r = fs.listTrash(items);
    if (r != OK) { printError(r); return; }
    if (items.empty()) {
        std::cout << "Корзина пуста.\n";
        return;
    }
    std::cout << "Корзина (" << items.size() << "):\n";
    for (const auto& item : items) {
        const DirEntry& e = item.second;
        std::cout << "  " << padRight("/" + item.first, 32)
                  << padLeft(std::to_string(e.size), 8) << " байт  "
                  << "владелец " << ownerName(e.ownerUid) << "\n";
    }
    std::cout << "restore <имя> — вернуть; purge — очистить навсегда.\n";
}

void Shell::cmdRestore(const Args& args) {
    if (!needMount() || args.size() != 1) {
        if (args.size() != 1) std::cout << "Использование: restore <имя>\n";
        return;
    }
    ErrorCode r = fs.restoreFile(argPath(args, 0));
    if (r == OK) std::cout << "Файл «" << args[0] << "» восстановлен из корзины.\n";
    else printError(r);
}

void Shell::cmdPurge(const Args& args) {
    if (!needMount()) return;
    if (!args.empty()) {
        std::cout << "Использование: purge (без параметров)\n";
        return;
    }
    std::vector<std::pair<std::string, DirEntry>> items;
    fs.listTrash(items);
    if (items.empty()) {
        std::cout << "Корзина пуста.\n";
        return;
    }
    if (!confirm("Очистить корзину с безвозвратным затиранием " +
                 std::to_string(items.size()) + " файл(ов)?"))
        return;
    ErrorCode r = fs.purgeTrash();
    if (r == OK) std::cout << "Корзина очищена, данные затёрты нулями.\n";
    else printError(r);
}

void Shell::cmdMv(const Args& args) {
    if (!needMount() || args.size() != 2) {
        if (args.size() != 2) std::cout << "Использование: mv <старое_имя> <новое_имя>\n";
        return;
    }
    ErrorCode r = fs.renameFile(argPath(args, 0), argPath(args, 1));
    if (r == OK) std::cout << "Переименован: " << args[0] << " -> " << args[1] << "\n";
    else printError(r);
}

void Shell::cmdStat(const Args& args) {
    if (!needMount() || args.size() != 1) {
        if (args.size() != 1) std::cout << "Использование: stat <имя>\n";
        return;
    }
    std::string path = argPath(args, 0);
    DirEntry e;
    ErrorCode r = fs.statFile(path, e);
    if (r != OK) { printError(r); return; }
    std::vector<uint32_t> chain;
    fs.chainOf(path, chain);
    bool isDir = (e.attributes & gladfs::ATTR_DIRECTORY) != 0;
    std::cout << (isDir ? "Каталог:   " : "Файл:      ") << args[0] << "\n"
              << "Размер:    " << e.size << " байт"
              << (isDir ? " (" + std::to_string(e.size / gladfs::DIRENTRY_SIZE) +
                            " записей)" : "") << "\n"
              << "Создан:    " << gladfs::dateToString(e.created) << "\n"
              << "Изменён:   " << gladfs::dateToString(e.modified) << "\n"
              << "Обращение: " << gladfs::dateToString(e.lastAccess)
              << " (всего обращений: " << e.accessCount << ")\n"
              << "Владелец:  " << ownerName(e.ownerUid)
              << ", права: " << gladfs::permString(e.permissions) << "\n"
              << "Атрибуты:  " << attrString(e.attributes)
              << "  (R — только чтение, H — скрытый, S — системный)\n"
              << "Блоки:     " << blocksString(chain) << "\n";
}

void Shell::cmdAttr(const Args& args) {
    if (!needMount() || args.size() != 2) {
        if (args.size() != 2)
            std::cout << "Использование: attr <имя> <+|-><ro|hid|sys>  (например: attr x.txt +ro)\n";
        return;
    }
    const std::string& spec = args[1];
    if (spec.size() < 3 || (spec[0] != '+' && spec[0] != '-')) {
        std::cout << "Ошибка: ожидается +ro, -ro, +hid, -hid, +sys или -sys.\n";
        return;
    }
    bool set = spec[0] == '+';
    std::string what = spec.substr(1);
    uint8_t mask;
    if      (what == "ro")  mask = gladfs::ATTR_READONLY;
    else if (what == "hid") mask = gladfs::ATTR_HIDDEN;
    else if (what == "sys") mask = gladfs::ATTR_SYSTEM;
    else { std::cout << "Ошибка: неизвестный атрибут " << what << ".\n"; return; }

    ErrorCode r = fs.setAttributes(argPath(args, 0), mask, set);
    if (r == OK) {
        DirEntry e;
        fs.statFile(argPath(args, 0), e);
        std::cout << "Атрибуты «" << args[0] << "»: " << attrString(e.attributes) << "\n";
    } else printError(r);
}

void Shell::cmdDf(const Args&) {
    if (!needMount()) return;
    gladfs::DiskStats st = fs.stats();
    std::cout << "Том «" << st.volumeName << "», создан " << st.createdText << "\n"
              << (st.encrypted ? "Шифрование данных: включено (XTEA-CTR)\n"
                               : "Шифрование данных: выключено\n")
              << "Блоков: " << st.totalBlocks << " по " << st.blockSize
              << " байт, из них данных: " << gladfs::DATA_BLOCKS << "\n"
              << "Занято блоков данных: " << st.usedBlocks
              << ", свободно: " << st.freeBlocks << " ("
              << st.freeBlocks * st.blockSize / 1024 << " КиБ)\n"
              << "Файлов: " << st.fileCount << ", каталогов: " << st.dirCount
              << ", фрагментировано: " << st.fragmentedFiles << "\n";
}

void Shell::cmdFsck(const Args& args) {
    if (!needMount()) return;
    bool repair = !args.empty() && args[0] == "--repair";
    if (!args.empty() && !repair) {
        std::cout << "Использование: fsck [--repair]\n";
        return;
    }
    std::cout << "Проверка целостности" << (repair ? " (режим восстановления)\n" : "\n");
    gladfs::FsckReport rep = fs.fsck(repair);
    for (const std::string& line : rep.log) std::cout << "  ! " << line << "\n";
    if (rep.problems == 0)
        std::cout << "Ошибок не найдено: файловая система консистентна (проверено блоков: "
                  << rep.checkedBlocks << ").\n";
    else
        std::cout << "Проблем: " << rep.problems
                  << ", исправлено: " << rep.repaired
                  << (repair ? "" : " (fsck --repair — исправить)") << ".\n";
}

void Shell::cmdHexdump(const Args& args) {
    if (!needMount() || args.size() != 1) {
        if (args.size() != 1) std::cout << "Использование: hexdump <номер_блока | имя_файла>\n";
        return;
    }
    uint32_t block;
    if (isUnsignedNumber(args[0])) {
        if (!parseUnsigned(args[0], block) || block >= gladfs::TOTAL_BLOCKS) {
            std::cout << "Ошибка: номер блока — число от 0 до "
                      << gladfs::TOTAL_BLOCKS - 1 << ".\n";
            return;
        }
    } else {
        std::string path = argPath(args, 0);
        std::vector<uint32_t> chain;
        ErrorCode r = fs.chainOf(path, chain);
        if (r != OK) { printError(r); return; }
        if (chain.empty()) { std::cout << "Файл пуст — дампить нечего.\n"; return; }
        block = chain[0];
        std::cout << "Первый блок «" << args[0] << "»: " << blocksString(chain) << "\n";
        if (fs.encryptionEnabled())
            std::cout << "(том зашифрован: в дампе виден шифротекст, а не текст)\n";
    }

    std::string buf(gladfs::BLOCK_SIZE, '\0');
    if (!fs.rawRead(block, &buf[0])) { printError(ERR_IO); return; }
    std::cout << "Блок " << block << " (0x" << std::hex << block << std::dec << "):\n";
    for (uint32_t off = 0; off < gladfs::BLOCK_SIZE; off += 16) {
        std::cout << std::hex << std::setw(4) << std::setfill('0') << off << ": ";
        std::string ascii;
        for (uint32_t i = 0; i < 16; ++i) {
            unsigned char c = static_cast<unsigned char>(buf[off + i]);
            std::cout << std::hex << std::setw(2) << std::setfill('0')
                      << static_cast<unsigned>(c) << ' ';
            ascii += (c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : '.';
        }
        std::cout << " |" << ascii << "|\n";
    }
    std::cout << std::dec << std::setfill(' ');
}

void Shell::cmdBlocks(const Args& args) {
    if (!needMount() || args.size() != 1) {
        if (args.size() != 1) std::cout << "Использование: blocks <имя>\n";
        return;
    }
    std::vector<uint32_t> chain;
    ErrorCode r = fs.chainOf(argPath(args, 0), chain);
    if (r != OK) { printError(r); return; }
    if (chain.empty()) { std::cout << "Не занимает блоков (пустой файл или каталог).\n"; return; }
    std::cout << "Цепочка FAT (" << chain.size() << " бл.): ";
    for (size_t i = 0; i < chain.size(); ++i)
        std::cout << chain[i] << (i + 1 < chain.size() ? " -> " : " -> EOF\n");
}

void Shell::cmdFmap(const Args&) {
    if (!needMount()) return;
    std::string map = fs.diskMap();
    std::cout << "Карта диска: S суперблок, F FAT, R каталог, # занятые данные, . свободные\n";
    constexpr uint32_t kBlocksPerRow = 64;
    for (uint32_t row = 0; row < gladfs::TOTAL_BLOCKS / kBlocksPerRow; ++row) {
        uint32_t first = row * kBlocksPerRow;
        std::cout << std::setw(4) << first << ": "
                  << map.substr(first, kBlocksPerRow) << "\n";
    }
}

void Shell::cmdImport(const Args& args) {
    if (!needMount() || args.size() < 1 || args.size() > 2) {
        if (args.size() < 1 || args.size() > 2)
            std::cout << "Использование: import <путь_к_файлу> [имя_в_GladFS]\n";
        return;
    }
    // Если имя не задано, берём часть пути после последнего разделителя
    std::string name = args.size() == 2 ? args[1] : args[0];
    if (args.size() == 1) {
        size_t slash = name.find_last_of("/\\");
        if (slash != std::string::npos) name = name.substr(slash + 1);
    }
    ErrorCode r = fs.importFile(args[0], name);
    if (r == OK) {
        DirEntry e;
        fs.statFile(name, e);
        std::vector<uint32_t> chain;
        fs.chainOf(name, chain);
        std::cout << "Импортирован «" << args[0] << "» -> " << name
                  << " (" << e.size << " байт, блоки: " << blocksString(chain) << ")\n";
    } else printError(r);
}

void Shell::cmdExport(const Args& args) {
    if (!needMount() || args.size() != 2) {
        if (args.size() != 2) std::cout << "Использование: export <имя> <путь_на_большом_диске>\n";
        return;
    }
    ErrorCode r = fs.exportFile(argPath(args, 0), args[1]);
    if (r == OK) std::cout << "Экспортирован «" << args[0] << "» -> " << args[1] << "\n";
    else printError(r);
}

void Shell::cmdTop(const Args&) {
    if (!needMount()) return;
    std::string path;
    DirEntry best;
    ErrorCode r = fs.findMostAccessed(path, best);
    if (r != OK) { std::cout << "Файлов нет.\n"; return; }
    std::cout << "Наибольшее количество обращений: /" << path
              << " (" << best.accessCount << ")\n";
}

void Shell::cmdRmOld(const Args& args) {
    if (!needMount() || args.size() != 1) {
        if (args.size() != 1) std::cout << "Использование: rmold <дд.мм.гггг>\n";
        return;
    }
    gladfs::Date border;
    if (!gladfs::parseDate(args[0], border)) {
        std::cout << "Ошибка: дата задаётся как дд.мм.гггг (например 01.01.2025).\n";
        return;
    }
    // Сначала показываем, что будет удалено (по всему дереву)
    std::vector<std::pair<std::string, DirEntry>> all;
    fs.walkTree(all);
    std::vector<std::string> victims;
    for (const auto& item : all) {
        const DirEntry& e = item.second;
        if (e.attributes & (gladfs::ATTR_DIRECTORY | gladfs::ATTR_TRASHED |
                            gladfs::ATTR_SYSTEM))
            continue;
        if (gladfs::cmpDate(e.created, border) < 0) victims.push_back(item.first);
    }
    if (victims.empty()) {
        std::cout << "Файлов, созданных раньше " << args[0] << ", нет.\n";
        return;
    }
    std::cout << "Будут удалены (" << victims.size() << "): ";
    for (size_t i = 0; i < victims.size() && i < 10; ++i)
        std::cout << victims[i] << (i + 1 < std::min(victims.size(), size_t(10)) ? ", " : "");
    if (victims.size() > 10) std::cout << "…";
    std::cout << "\n";
    if (!confirm("Удалить эти файлы?")) return;

    std::vector<std::string> removed, skipped;
    ErrorCode r = fs.removeOlderThan(border, removed, skipped);
    if (r != OK) { printError(r); return; }
    std::cout << "Удалено файлов: " << removed.size() << ".\n";
    for (const std::string& name : skipped)
        std::cout << "Пропущен (только чтение или нет прав): " << name << "\n";
}

void Shell::cmdDebug(const Args& args) {
    if (args.size() == 1 && args[0] == "crypto") {
        std::string report;
        bool ok = gladfs::xteaSelfTest(report);
        std::cout << report << (ok ? "Самопроверка XTEA пройдена.\n"
                                   : "Самопроверка XTEA НЕ ПРОЙДЕНА!\n");
        return;
    }
    if (!needMount()) return;
    if (args.size() != 4 || args[0] != "write") {
        std::cout << "Использование: debug write <блок> <смещение> <байт_в_hex>\n"
                     "       или:     debug crypto\n"
                     "Прямая правка байта образа — для демонстрации восстановления (fsck).\n";
        return;
    }
    uint32_t block = 0, offset = 0, value = 0;
    if (!parseUnsigned(args[1], block) || !parseUnsigned(args[2], offset)) {
        std::cout << "Ошибка: блок и смещение — целые числа.\n";
        return;
    }
    value = std::strtoul(args[3].c_str(), nullptr, 16);
    if (value > 0xFF) {
        std::cout << "Ошибка: байт задаётся двумя hex-цифрами (например FF).\n";
        return;
    }
    ErrorCode r = fs.rawPatch(block, offset, static_cast<uint8_t>(value));
    if (r == OK)
        std::cout << "Байт записан, структуры перечитаны с диска. Проверьте fsck.\n";
    else if (r == gladfs::ERR_BAD_FORMAT)
        std::cout << "Правка сломала сигнатуру: образ больше не читается (в учебной\n"
                     "версии суперблок не восстанавливается — поможет format).\n";
    else printError(r);
}

// ----------------------------------------------------------------------------
// Пользователи и права
// ----------------------------------------------------------------------------

void Shell::cmdWhoAmI(const Args&) {
    std::cout << "Текущий пользователь: " << fs.currentUser()
              << (fs.currentIsAdmin() ? " (администратор)" : "") << "\n";
}

void Shell::cmdUsers(const Args& args) {
    if (!needMount()) return;
    if (!args.empty()) {
        std::cout << "Использование: users (без параметров)\n";
        return;
    }
    if (!fs.currentIsAdmin()) {
        std::cout << "Ошибка: нет прав на операцию (только администратор).\n";
        return;
    }
    std::cout << "Пользователи:\n";
    for (const auto& u : fs.users()) {
        if (!(u.flags & gladfs::USER_USED)) continue;
        std::string name(u.name, strnlen(u.name, gladfs::NAME_MAX_USERS + 1));
        std::cout << "  " << padRight(name, 20)
                  << "uid " << static_cast<int>(u.uid)
                  << ((u.flags & gladfs::USER_ADMIN) ? "  администратор" : "") << "\n";
    }
}

void Shell::cmdAddUser(const Args& args) {
    if (!needMount() || args.size() != 1) {
        if (args.size() != 1) std::cout << "Использование: adduser <имя>\n";
        return;
    }
    std::string password = promptLine("Пароль нового пользователя: ");
    std::string password2 = promptLine("Повторите пароль: ");
    if (password != password2) {
        std::cout << "Ошибка: пароли не совпадают.\n";
        return;
    }
    ErrorCode r = fs.addUser(args[0], password);
    if (r == OK) std::cout << "Пользователь «" << args[0] << "» добавлен.\n";
    else printError(r);
}

void Shell::cmdPasswd(const Args& args) {
    if (!needMount() || args.size() > 1) {
        if (args.size() > 1) std::cout << "Использование: passwd [имя]\n";
        return;
    }
    std::string name = args.empty() ? fs.currentUser() : args[0];
    std::string password = promptLine("Новый пароль: ");
    std::string password2 = promptLine("Повторите пароль: ");
    if (password != password2) {
        std::cout << "Ошибка: пароли не совпадают.\n";
        return;
    }
    ErrorCode r = fs.setPassword(name, password);
    if (r == OK) std::cout << "Пароль пользователя «" << name << "» изменён.\n";
    else printError(r);
}

void Shell::cmdChown(const Args& args) {
    if (!needMount() || args.size() != 2) {
        if (args.size() != 2) std::cout << "Использование: chown <файл> <пользователь>\n";
        return;
    }
    ErrorCode r = fs.changeOwner(argPath(args, 0), args[1]);
    if (r == OK)
        std::cout << "Владелец «" << args[0] << "» изменён на " << args[1] << ".\n";
    else printError(r);
}

void Shell::cmdChmod(const Args& args) {
    if (!needMount() || args.size() != 2) {
        if (args.size() != 2)
            std::cout << "Использование: chmod <файл> <права>\n"
                         "  права — три восьмеричные цифры (например 640):\n"
                         "  первая — владелец, вторая — остальные (r=4, w=2, x=1)\n";
        return;
    }
    // Права — три восьмеричные цифры (например 640)
    if (args[1].size() != 3) {
        std::cout << "Ошибка: права задаются тремя восьмеричными цифрами (например 640).\n";
        return;
    }
    uint32_t perms = 0;
    for (char c : args[1]) {
        if (c < '0' || c > '7') {
            std::cout << "Ошибка: права задаются тремя восьмеричными цифрами (например 640).\n";
            return;
        }
        perms = perms * 8 + static_cast<uint32_t>(c - '0');
    }
    uint8_t owner = static_cast<uint8_t>((perms >> 6) & 7);
    uint8_t other = static_cast<uint8_t>(perms & 7);
    uint8_t packed = static_cast<uint8_t>((owner << gladfs::PERM_OWNER_SHIFT) | other);
    ErrorCode r = fs.setPermissions(argPath(args, 0), packed);
    if (r == OK) {
        DirEntry e;
        fs.statFile(argPath(args, 0), e);
        std::cout << "Права «" << args[0] << "»: "
                  << gladfs::permString(e.permissions) << "\n";
    } else printError(r);
}

}  // namespace

// Запуск оболочки; вызывается из main
int runShell(const std::string& imagePath) {
    Shell shell;
    shell.imagePath = imagePath;
    shell.run();
    return 0;
}
