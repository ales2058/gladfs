@echo off
rem ============================================================
rem  Сборка GladFS. Компилятор: g++ (MinGW-w64).
rem  Если g++ не найден в PATH, используется D:\mingw64\mingw64\bin
rem ============================================================
where g++ >nul 2>nul
if errorlevel 1 set PATH=D:\mingw64\mingw64\bin;%PATH%

g++ -std=c++17 -O2 -Wall -Wextra -o gladfs.exe src\main.cpp src\blockdevice.cpp src\crypto.cpp src\gladfs.cpp src\shell.cpp
if errorlevel 1 (
    echo Сборка НЕ удалась.
    exit /b 1
)
echo Сборка успешна: gladfs.exe
