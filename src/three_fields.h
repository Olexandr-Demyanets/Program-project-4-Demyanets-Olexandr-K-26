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
            throw std::out_of_range("field index " + std::to_string(field) + " is out of range [0, " +
                                    std::to_string(kFieldCount) + ")");
    }

    mutable std::mutex mutex_;
    int fields_[kFieldCount] = {};
};
