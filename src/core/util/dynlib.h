#pragma once

#include <filesystem>
#include <string>
#include <type_traits>
#include <utility>

namespace studiocast::util {

class DynLib {
public:
    enum class Scope {
        Local,
        Global,
    };

    DynLib() = default;
    explicit DynLib(std::filesystem::path path) : path_(std::move(path)) {}

    DynLib(const DynLib&) = delete;
    DynLib& operator=(const DynLib&) = delete;

    DynLib(DynLib&& other) noexcept;
    DynLib& operator=(DynLib&& other) noexcept;

    ~DynLib();

    bool Open(const std::filesystem::path& path, Scope scope, std::string* error_out);
    bool Open(Scope scope, std::string* error_out);

    void Close();

    bool IsOpen() const { return handle_ != nullptr; }
    explicit operator bool() const { return IsOpen(); }

    const std::filesystem::path& path() const { return path_; }

    void* GetSymbolRaw(const char* name, std::string* error_out) const;

    template <typename T>
    bool GetSymbol(const char* name, T* out, std::string* error_out) const {
        static_assert(std::is_pointer_v<T>, "DynLib::GetSymbol<T> requires a pointer type.");
        if (!out) {
            if (error_out) {
                *error_out = "GetSymbol called with null out pointer.";
            }
            return false;
        }

        *out = nullptr;
        void* sym = GetSymbolRaw(name, error_out);
        if (!sym) {
            return false;
        }

        *out = reinterpret_cast<T>(sym);
        return *out != nullptr;
    }

private:
    static std::string DescribeOpenError(const std::filesystem::path& path, Scope scope, const char* dlerror_text);
    static std::string DescribeMissingSymbol(const std::filesystem::path& path, const char* symbol, const char* dlerror_text);

    std::filesystem::path path_;
    void* handle_ = nullptr;
    Scope scope_ = Scope::Local;
};

}  // namespace studiocast::util
