// Компілятор: g++ 15.2.0 (MSYS2 UCRT64), C++20
// Програмний проєкт 4, варіант 5: структура даних із m = 3 цілих полів для багатопотокової програми.
//
// Схема захисту: ОДИН std::mutex на всю структуру.
// Обґрунтування коротко (детально з вимірами - у README.md):
//  * string (20 % операцій) має повернути узгоджений знімок усіх трьох полів, тож при
//    окремому м'ютексі на поле він блокував би 2-3 м'ютекси замість одного;
//  * поле 1 бере участь у 70 % операцій (read 40 % + write 10 % + string 20 %), тож
//    навіть окремий м'ютекс для кожного поля серіалізує більшість операцій на м'ютексі поля 1;
//  * критична секція - копіювання одного int (~1 нс), а читання-запис-блокування
//    (std::shared_mutex) у MinGW реалізоване через pthread_rwlock і в 4-5 разів дорожче
//    за std::mutex, тож перевага паралельних читань (59 % read + 20 % string) не окупається.
#pragma once

#include <mutex>
#include <stdexcept>
#include <string>

inline std::string formatFields(int v0, int v1, int v2)
{
    std::string result = "[";
    result += std::to_string(v0);
    result += ", ";
    result += std::to_string(v1);
    result += ", ";
    result += std::to_string(v2);
    result += ']';
    return result;
}

class ThreeFields
{
public:
    static constexpr int kFieldCount = 3;

    int get(int field) const
    {
        checkField(field);
        std::lock_guard lock(mutex_);
        return fields_[field];
    }

    void set(int field, int value)
    {
        checkField(field);
        std::lock_guard lock(mutex_);
        fields_[field] = value;
    }

    explicit operator std::string() const
    {
        int v0, v1, v2;
        {
            std::lock_guard lock(mutex_);
            v0 = fields_[0];
            v1 = fields_[1];
            v2 = fields_[2];
        }
        return formatFields(v0, v1, v2);
    }

private:
    static void checkField(int field)
    {
        if (field < 0 || field >= kFieldCount)
            throw std::out_of_range("field index " + std::to_string(field) + " is out of range [0, 3)");
    }

    mutable std::mutex mutex_;
    int fields_[kFieldCount] = {};
};
