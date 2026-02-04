#include "core/util/dynlib.h"

#include <dlfcn.h>

#include <sstream>
#include <utility>

namespace studiocast::util {

namespace {

int FlagsForScope(DynLib::Scope scope) {
    const int base = RTLD_NOW;
    if (scope == DynLib::Scope::Global) {
        return base | RTLD_GLOBAL;
    }
    return base | RTLD_LOCAL;
}

const char* ScopeName(DynLib::Scope scope) {
    return scope == DynLib::Scope::Global ? "global" : "local";
}

}  // namespace

DynLib::DynLib(DynLib&& other) noexcept {
    *this = std::move(other);
}

DynLib& DynLib::operator=(DynLib&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    Close();
    path_ = std::move(other.path_);
    handle_ = other.handle_;
    scope_ = other.scope_;
    other.handle_ = nullptr;
    other.scope_ = Scope::Local;
    return *this;
}

DynLib::~DynLib() {
    Close();
}

bool DynLib::Open(const std::filesystem::path& path, Scope scope, std::string* error_out) {
    path_ = path;
    return Open(scope, error_out);
}

bool DynLib::Open(Scope scope, std::string* error_out) {
    if (error_out) {
        error_out->clear();
    }

    Close();
    scope_ = scope;

    if (path_.empty()) {
        if (error_out) {
            *error_out = "dlopen failed: empty library path.";
        }
        return false;
    }

    dlerror();
    handle_ = dlopen(path_.c_str(), FlagsForScope(scope_));
    if (!handle_) {
        const char* e = dlerror();
        if (error_out) {
            *error_out = DescribeOpenError(path_, scope_, e);
        }
        return false;
    }

    return true;
}

void DynLib::Close() {
    if (handle_) {
        dlclose(handle_);
        handle_ = nullptr;
    }
}

void* DynLib::GetSymbolRaw(const char* name, std::string* error_out) const {
    if (error_out) {
        error_out->clear();
    }

    if (!handle_) {
        if (error_out) {
            std::ostringstream oss;
            oss << "dlsym failed: library not open (requested='" << path_.string() << "').";
            *error_out = oss.str();
        }
        return nullptr;
    }

    dlerror();
    void* sym = dlsym(handle_, name);
    const char* e = dlerror();
    if (e != nullptr || sym == nullptr) {
        if (error_out) {
            *error_out = DescribeMissingSymbol(path_, name, e);
        }
        return nullptr;
    }
    return sym;
}

std::string DynLib::DescribeOpenError(const std::filesystem::path& path, Scope scope, const char* dlerror_text) {
    std::ostringstream oss;
    oss << "dlopen failed: attempted '" << path.string() << "'";
    if (path.is_absolute()) {
        oss << " (absolute path)";
    }
    oss << " scope=" << ScopeName(scope);
    if (dlerror_text && *dlerror_text) {
        oss << ": " << dlerror_text;
    }
    return oss.str();
}

std::string DynLib::DescribeMissingSymbol(const std::filesystem::path& path, const char* symbol, const char* dlerror_text) {
    std::ostringstream oss;
    oss << "dlsym failed: missing symbol '" << (symbol ? symbol : "") << "' in '" << path.string() << "'";
    if (path.is_absolute()) {
        oss << " (absolute path)";
    }
    if (dlerror_text && *dlerror_text) {
        oss << ": " << dlerror_text;
    }
    return oss.str();
}

}  // namespace studiocast::util
