#pragma once

// Small helpers for the browser: a growable array and string functions.

extern "C" {
void* memcpy(void* dest, void const* src, unsigned long n);
void* memset(void* dest, int value, unsigned long n);
int memcmp(void const* a, void const* b, unsigned long n);
}

namespace Browser {

using u8 = unsigned char;
using u32 = unsigned;
using u64 = unsigned long;

// A growable array of trivially copyable values.
template <typename T>
class Vector {
public:
    Vector() = default;
    Vector(Vector const&) = delete;
    Vector& operator=(Vector const&) = delete;
    ~Vector() { delete[] _data; }

    u64 size() const { return _size; }
    bool empty() const { return _size == 0; }
    T* data() { return _data; }
    T const* data() const { return _data; }
    T& operator[](u64 i) { return _data[i]; }
    T const& operator[](u64 i) const { return _data[i]; }
    T& back() { return _data[_size - 1]; }

    bool push(T const& value) {
        if (_size == _capacity and not grow(_size + 1))
            return false;
        _data[_size++] = value;
        return true;
    }

    bool append(T const* values, u64 n) {
        if (_size + n > _capacity and not grow(_size + n))
            return false;
        memcpy(_data + _size, values, n * sizeof(T));
        _size += n;
        return true;
    }

    void clear() { _size = 0; }
    void shrink(u64 n) {
        if (n < _size)
            _size = n;
    }

    // Swaps contents with another vector (cheap).
    void swap(Vector& other) {
        T* d = _data;
        u64 s = _size, c = _capacity;
        _data = other._data;
        _size = other._size;
        _capacity = other._capacity;
        other._data = d;
        other._size = s;
        other._capacity = c;
    }

private:
    bool grow(u64 needed) {
        u64 capacity = _capacity ? _capacity * 2 : 64;
        while (capacity < needed)
            capacity *= 2;
        T* data = new T[capacity];
        if (not data)
            return false;
        if (_data)
            memcpy(data, _data, _size * sizeof(T));
        delete[] _data;
        _data = data;
        _capacity = capacity;
        return true;
    }

    T* _data = nullptr;
    u64 _size = 0, _capacity = 0;
};

inline u64 length(char const* s) {
    u64 n = 0;
    while (s[n])
        n++;
    return n;
}

inline char lower(char c) { return c >= 'A' and c <= 'Z' ? static_cast<char>(c + 32) : c; }

inline bool isSpace(char c) { return c == ' ' or c == '\n' or c == '\r' or c == '\t' or c == '\f'; }

// Case-insensitive: does `text` start with `prefix`?
inline bool startsWith(char const* text, char const* prefix) {
    for (; *prefix; text++, prefix++) {
        if (lower(*text) != lower(*prefix))
            return false;
    }
    return true;
}

inline bool equals(char const* a, char const* b) {
    for (; *a and *b; a++, b++) {
        if (lower(*a) != lower(*b))
            return false;
    }
    return *a == *b;
}

// Copies at most capacity - 1 characters and terminates.
inline void copy(char* dest, char const* src, u64 capacity, u64 n = ~0ul) {
    u64 i = 0;
    for (; i + 1 < capacity and i < n and src[i]; i++)
        dest[i] = src[i];
    dest[i] = '\0';
}

// Appends to a terminated string within `capacity`.
inline void append(char* dest, char const* src, u64 capacity) {
    u64 n = length(dest);
    copy(dest + n, src, capacity > n ? capacity - n : 0);
}

} // namespace Browser
