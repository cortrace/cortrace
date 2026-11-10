// Cortrace — SymbolTable unit tests.
//
// SPDX-License-Identifier: MIT
#include "cortrace/symbols.hpp"
#include "test_framework.hpp"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <string>

using cortrace::SymbolTable;

TEST(symbols_function_at_basic)
{
    SymbolTable t;
    t.add(0x1000, "alpha");
    t.add(0x2000, "beta");
    t.add(0x3000, "gamma");
    t.finalize();

    CHECK_EQ(t.function_at(0x1000), std::string("alpha"));
    CHECK_EQ(t.function_at(0x1004), std::string("alpha")); // inside alpha
    CHECK_EQ(t.function_at(0x1fff), std::string("alpha"));
    CHECK_EQ(t.function_at(0x2000), std::string("beta"));
    CHECK_EQ(t.function_at(0x3500), std::string("gamma"));
}

TEST(symbols_below_first_is_unknown)
{
    SymbolTable t;
    t.add(0x1000, "alpha");
    t.finalize();
    CHECK_EQ(t.function_at(0x0800), std::string("?"));
}

TEST(symbols_empty_is_unknown)
{
    SymbolTable t;
    CHECK(t.empty());
    CHECK_EQ(t.function_at(0x1234), std::string("?"));
}

TEST(symbols_is_function_entry)
{
    SymbolTable t;
    t.add(0x2000, "beta");
    t.add(0x1000, "alpha"); // out of order on purpose
    t.finalize();

    CHECK(t.is_function_entry(0x1000));
    CHECK(t.is_function_entry(0x2000));
    CHECK(!t.is_function_entry(0x1004)); // mid-function, not an entry
    CHECK(!t.is_function_entry(0x0fff));
}

TEST(symbols_unsorted_add_then_finalize)
{
    SymbolTable t;
    t.add(0x3000, "gamma");
    t.add(0x1000, "alpha");
    t.add(0x2000, "beta");
    t.finalize();
    CHECK_EQ(t.size(), static_cast<std::size_t>(3));
    CHECK_EQ(t.function_at(0x2500), std::string("beta"));
}

namespace {
// Write `text` to a scratch file and return its path.
std::string write_file(const std::string& name, const std::string& text)
{
    const std::string path = cortrace_test::temp_path(name);
    FILE* f = std::fopen(path.c_str(), "wb");
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    return path;
}
} // namespace

TEST(symbols_load_nm_keeps_only_text_and_weak_symbols)
{
    const std::string path = write_file("nm_ok.txt",
        "08000100 T reset_handler\n"
        "08000200 t local_helper\n"
        "08000300 W weak_hook\n"
        "08000400 w weak_local\n"
        "20000000 D data_symbol\n" // data: dropped
        "20000100 B bss_symbol\n" // bss: dropped
        "         U undefined_symbol\n" // no address: unparsable, dropped
        "not an nm line at all\n");
    SymbolTable t;
    CHECK_EQ(t.load_nm(path), static_cast<std::size_t>(4));
    CHECK_EQ(t.size(), static_cast<std::size_t>(4));
    CHECK_EQ(t.function_at(0x08000150), std::string("reset_handler"));
    CHECK_EQ(t.function_at(0x08000450), std::string("weak_local"));
    CHECK(t.is_function_entry(0x08000300));
    CHECK(!t.is_function_entry(0x20000000)); // data symbol never became a function
    std::remove(path.c_str());
}

TEST(symbols_load_nm_sorts_an_unsorted_file)
{
    const std::string path = write_file("nm_unsorted.txt", "08000300 T gamma\n08000100 T alpha\n");
    SymbolTable t;
    t.load_nm(path);
    CHECK_EQ(t.function_at(0x08000200), std::string("alpha")); // finalize() ran
    std::remove(path.c_str());
}

TEST(symbols_load_nm_missing_file_throws)
{
    SymbolTable t;
    bool threw = false;
    try {
        t.load_nm("/nonexistent/dir/no.nm");
    } catch (const std::runtime_error& e) {
        threw = std::string(e.what()).find("no.nm") != std::string::npos;
    }
    CHECK(threw);
}

TEST(symbols_load_nm_empty_file_adds_nothing)
{
    const std::string path = write_file("nm_empty.txt", "");
    SymbolTable t;
    CHECK_EQ(t.load_nm(path), static_cast<std::size_t>(0));
    CHECK(t.empty());
    std::remove(path.c_str());
}

TEST(symbols_demangle_name)
{
    using cortrace::demangle_name;
    CHECK_EQ(
        demangle_name("_ZN8touchgfx3HAL8endFrameEv"), std::string("touchgfx::HAL::endFrame()"));
    CHECK_EQ(demangle_name("_ZNK8touchgfx5Image4drawERKNS_4RectE"),
        std::string("touchgfx::Image::draw(touchgfx::Rect const&) const"));
    CHECK_EQ(demangle_name("_Z4leafj"), std::string("leaf(unsigned int)"));
    // not mangled, or not a valid mangled name: unchanged
    CHECK_EQ(demangle_name("SysTick_Handler"), std::string("SysTick_Handler"));
    CHECK_EQ(demangle_name("_start"), std::string("_start"));
    CHECK_EQ(demangle_name("_Zbroken"), std::string("_Zbroken"));
    CHECK_EQ(demangle_name(""), std::string());
}
TEST(symbols_load_nm_demangles_on_request)
{
    const std::string path = cortrace_test::temp_path("demangle.nm");
    FILE* f = std::fopen(path.c_str(), "w");
    std::fputs("08000100 T _ZN8touchgfx3HAL8endFrameEv\n08000200 T main\n", f);
    std::fclose(f);
    SymbolTable mangled, plain;
    CHECK_EQ(mangled.load_nm(path, true), 2u);
    CHECK_EQ(plain.load_nm(path), 2u);
    CHECK_EQ(mangled.function_at(0x08000104), std::string("touchgfx::HAL::endFrame()"));
    CHECK_EQ(mangled.function_at(0x08000200), std::string("main"));
    CHECK_EQ(plain.function_at(0x08000104), std::string("_ZN8touchgfx3HAL8endFrameEv"));
    std::remove(path.c_str());
}
