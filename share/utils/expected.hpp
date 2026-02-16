#pragma once

#include <variant>
#include <string>
#include <optional>

namespace crossscale {

template<typename T, typename E>
class expected {
public:
    expected(T value) : data_(std::move(value)) {}
    expected(E error) : data_(std::move(error)) {}
    
    bool has_value() const { return std::holds_alternative<T>(data_); }
    bool ok() const { return has_value(); }
    
    T& value() & { return std::get<T>(data_); }
    const T& value() const& { return std::get<T>(data_); }
    T&& value() && { return std::get<T>(std::move(data_)); }
    
    E& error() & { return std::get<E>(data_); }
    const E& error() const& { return std::get<E>(data_); }
    E&& error() && { return std::get<E>(std::move(data_)); }
    
    T& operator*() & { return value(); }
    const T& operator*() const& { return value(); }
    T&& operator*() && { return std::move(value()); }
    
    T* operator->() { return &value(); }
    const T* operator->() const { return &value(); }
    
    explicit operator bool() const { return has_value(); }

private:
    std::variant<T, E> data_;
};

template<typename E>
class expected<void, E> {
public:
    expected() : has_value_(true) {}
    expected(E error) : has_value_(false), error_(std::move(error)) {}
    
    bool has_value() const { return has_value_; }
    bool ok() const { return has_value_; }
    
    E& error() & { return error_; }
    const E& error() const& { return error_; }
    E&& error() && { return std::move(error_); }
    
    explicit operator bool() const { return has_value_; }

private:
    bool has_value_;
    E error_;
};

}
