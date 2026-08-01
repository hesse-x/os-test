// libcxx_smoke.cpp — libc++ end-to-end regression smoke (refact_cmake.md §3.6)
//
// libc++ has historically broken linkage on four gaps: int128 compiler runtime,
// catgets POSIX symbol, TLS (exception __cxa_thread_atexit), and fused libc.so
// POSIX symbols. This program covers one fixed path each; passing = regression
// passing. No Unity — each step prints a marker, any failure _exit's non-zero,
// test_runner reports [FAIL].
//
// Links -stdlib=libc++ against sysroot libc++/libc++abi/libunwind (installed by
// build_libcxx.sh). Built only when libc++ is enabled (-DLIBCXX=1).

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <stdexcept>
#include <filesystem>
#include <locale>

static int fail(const char *what) {
    printf("[FAIL] %s\n", what);
    return 1;
}

int main() {
    // 1. Basic STL: <vector> + std::string (heap/exception base path)
    std::vector<std::string> v;
    for (int i = 0; i < 8; ++i) v.push_back(std::string("elem") + std::to_string(i));
    std::string joined;
    for (const auto &s : v) { joined += s; joined += "|"; }
    printf("STL: %zu items, joined[%zu]\n", v.size(), joined.size());
    if (v.size() != 8 || joined.empty()) return fail("vector/string");

    // 2. Exception → libc++abi → TLS __cxa_thread_atexit path
    try {
        throw std::runtime_error("boom");
    } catch (const std::exception &e) {
        printf("exception: %s\n", e.what());
        if (std::string(e.what()) != "boom") return fail("throw/catch");
    } catch (...) {
        return fail("exception caught wrong handler");
    }

    // 3. std::filesystem → int128 compiler runtime (__divti3/__muloti4) path
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (ec) return fail("filesystem current_path");
    printf("filesystem: cwd length %zu\n", std::string(cwd.string()).size());
    // file_size on a regular file exercises int128 timestamp arithmetic in
    // posix_stat. file_size() on a directory errors by libc++ design, so test
    // against a real file. Fall back to the smoke ELF itself if /README is absent.
    auto sz = std::filesystem::file_size("/README", ec);
    if (ec) {
        // Image layout may put README elsewhere; retry against the program path.
        sz = std::filesystem::file_size("/test/libcxx_smoke.elf", ec);
    }
    if (ec) return fail("filesystem file_size");
    printf("filesystem: file size %lld\n", (long long)sz);

    // 4. std::messages facet → catgets path (musl catopen degrades when no catalog)
    try {
        std::locale loc;
        const std::messages<char> &msg = std::use_facet<std::messages<char>>(loc);
        std::messages<char>::catalog cat = msg.open("libcxx_smoke", std::locale());
        if (cat < 0) {
            printf("messages: catalog not opened (C-locale no-catalog degrade, ok)\n");
        } else {
            printf("messages: catalog opened id=%d\n", (int)cat);
            msg.close(cat);
        }
    } catch (const std::exception &e) {
        printf("messages facet threw: %s\n", e.what());
        return fail("messages facet");
    }

    printf("[PASS] libcxx_smoke\n");
    return 0;
}
