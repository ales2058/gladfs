// ============================================================================
//  GladFS — учебная файловая система. Точка входа.
//
//  Использование:
//      gladfs.exe                — работать с образом disk.img в текущей папке
//      gladfs.exe <образ.img>    — указать другой образ
//      gladfs.exe --help         — справка
// ============================================================================

#include <iostream>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

int runShell(const std::string& imagePath);

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // Консоль Windows: включаем UTF-8 для корректного русского текста
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif

    std::string imagePath = "disk.img";
    if (argc > 2) {
        std::cout << "Использование: gladfs [образ] | --help\n";
        return 1;
    }
    if (argc == 2) {
        std::string arg = argv[1];
        if (arg == "--help" || arg == "-h" || arg == "/?") {
            std::cout <<
                "GladFS — учебная файловая система в файле-образе.\n\n"
                "Использование:\n"
                "  gladfs              работать с образом disk.img\n"
                "  gladfs <образ.img>  работать с указанным образом\n\n"
                "Внутри программы: help — список команд файловой системы.\n";
            return 0;
        }
        imagePath = arg;
    }
    return runShell(imagePath);
}
