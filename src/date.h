#pragma once
// ============================================================================
//  Дата. Та же структура, что и в лабораторной работе №1 (вариант 6):
//  в GladFS она хранится в каждой записи каталога — дата создания,
//  дата изменения и дата последнего обращения к файлу.
// ============================================================================

#include <cstdint>
#include <string>

namespace gladfs {

#pragma pack(push, 1)  // точная раскладка на диске, без выравнивания
struct Date {
    uint16_t day;
    uint16_t month;
    uint16_t year;
};
#pragma pack(pop)

static_assert(sizeof(Date) == 6, "Date на диске должна занимать 6 байт");

// Сравнение дат: < 0, если a раньше b; 0, если равны; > 0, если a позже b
inline int cmpDate(const Date& a, const Date& b) {
    if (a.year != b.year)   return static_cast<int>(a.year)   - static_cast<int>(b.year);
    if (a.month != b.month) return static_cast<int>(a.month) - static_cast<int>(b.month);
    return static_cast<int>(a.day) - static_cast<int>(b.day);
}

// Текущая дата системы (для меток времени при создании/изменении файлов)
Date today();

// Дата в виде "дд.мм.гггг"
std::string dateToString(const Date& d);

// Разбор "дд.мм.гггг" или "дд-мм-гггг"; false — если строка не является датой
bool parseDate(const std::string& text, Date& out);

}  // namespace gladfs
