// Cortrace — ELF symbol table: address -> function name, and entry test.
//
// SPDX-License-Identifier: MIT
#ifndef CORTRACE_SYMBOLS_HPP
#define CORTRACE_SYMBOLS_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace cortrace {

// The C++ name for a mangled symbol (`_ZN8touchgfx3HAL8endFrameEv` becomes
// `touchgfx::HAL::endFrame()`). A name that is not an Itanium-ABI mangled name,
// or that cannot be demangled, is returned unchanged.
std::string demangle_name(const std::string& name);
// A sorted function symbol table. Built from `nm`-style entries (address, name)
// for text/weak symbols. Provides address->function lookup and an exact
// function-entry test used by the call-stack machine to confirm a callee.
class SymbolTable {
public:
    struct Symbol {
        uint32_t addr;
        std::string name;
    };

    // Add a function symbol. Call finalize() once after all adds.
    void add(uint32_t addr, std::string name);

    // Sort for binary search. Idempotent.
    void finalize();

    // Function containing `addr` (greatest symbol with addr <= a). Returns
    // "?" if none / table empty.
    const std::string& function_at(uint32_t addr) const;

    // True if `addr` is exactly a function's first address (entry point).
    bool is_function_entry(uint32_t addr) const;

    // Parse an `nm` output file (lines: "<hex-addr> <type> <name>"), keeping
    // only text/weak symbols (t/T/w/W). Returns number of symbols added.
    // With `demangle`, C++ symbols are stored under their demangled names.
    // Throws std::runtime_error if the file cannot be opened.
    std::size_t load_nm(const std::string& path, bool demangle = false);

    std::size_t size() const { return syms_.size(); }
    bool empty() const { return syms_.empty(); }

private:
    std::vector<Symbol> syms_;
    bool finalized_ = false;
};

} // namespace cortrace

#endif // CORTRACE_SYMBOLS_HPP
