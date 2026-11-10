// Cortrace — SymbolTable implementation.
//
// SPDX-License-Identifier: MIT
#include "cortrace/symbols.hpp"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

#if defined(__has_include)
#if __has_include(<cxxabi.h>)
#include <cstdlib>
#include <cxxabi.h>
#define CORTRACE_HAVE_CXXABI 1
#endif
#endif

namespace cortrace {

std::string demangle_name(const std::string& name)
{
#if defined(CORTRACE_HAVE_CXXABI)
    if (name.compare(0, 2, "_Z") != 0)
        return name;
    int status = 0;
    char* out = abi::__cxa_demangle(name.c_str(), nullptr, nullptr, &status);
    if (status != 0 || !out) {
        std::free(out);
        return name;
    }
    std::string demangled(out);
    std::free(out);
    return demangled;
#else
    return name;
#endif
}

void SymbolTable::add(uint32_t addr, std::string name)
{
    syms_.push_back({ addr, std::move(name) });
    finalized_ = false;
}

void SymbolTable::finalize()
{
    std::sort(syms_.begin(), syms_.end(),
        [](const Symbol& a, const Symbol& b) { return a.addr < b.addr; });
    finalized_ = true;
}

const std::string& SymbolTable::function_at(uint32_t addr) const
{
    static const std::string unknown = "?";
    if (syms_.empty())
        return unknown;
    // greatest symbol with addr <= `addr`
    int lo = 0, hi = static_cast<int>(syms_.size()) - 1, res = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (syms_[mid].addr <= addr) {
            res = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return res >= 0 ? syms_[res].name : unknown;
}

bool SymbolTable::is_function_entry(uint32_t addr) const
{
    int lo = 0, hi = static_cast<int>(syms_.size()) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (syms_[mid].addr == addr)
            return true;
        if (syms_[mid].addr < addr)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return false;
}

std::size_t SymbolTable::load_nm(const std::string& path, bool demangle)
{
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f)
        throw std::runtime_error("cannot open nm file: " + path);
    std::size_t added = 0;
    char line[4096]; // mangled template names are long
    while (std::fgets(line, sizeof line, f)) {
        unsigned long addr = 0;
        char type = 0;
        char name[2048] = { 0 };
        if (std::sscanf(line, "%lx %c %2047s", &addr, &type, name) == 3) {
            if (type == 't' || type == 'T' || type == 'w' || type == 'W') {
                add(static_cast<uint32_t>(addr), demangle ? demangle_name(name) : name);
                ++added;
            }
        }
    }
    std::fclose(f);
    finalize();
    return added;
}

} // namespace cortrace
