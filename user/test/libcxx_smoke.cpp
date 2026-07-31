// libcxx_smoke.cpp — libc++ 端到端回归冒烟（refact_cmake.md §3.6）
//
// libc++ 历史上反复在四类缺口断链：int128 compiler runtime、catgets POSIX 符号、
// TLS（异常 __cxa_thread_atexit）、fused libc.so POSIX 符号。本程序各覆盖一条已修
// 路径，运行通过即回归通过。无 Unity——每步打印标记，任一失败 _exit 非零，
// test_runner 报 [FAIL]。
//
// 链接：-stdlib=libc++ + sysroot 的 libc++/libc++abi/libunwind（build_libcxx.sh 装入）。
// 仅在 libc++ 启用（-DLIBCXX=1）时构建。

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
    // 1. 基础 STL：<vector> + std::string（heap/异常基础路径）
    std::vector<std::string> v;
    for (int i = 0; i < 8; ++i) v.push_back(std::string("elem") + std::to_string(i));
    std::string joined;
    for (const auto &s : v) { joined += s; joined += "|"; }
    printf("STL: %zu items, joined[%zu]\n", v.size(), joined.size());
    if (v.size() != 8 || joined.empty()) return fail("vector/string");

    // 2. 异常 → libc++abi → TLS __cxa_thread_atexit 路径
    try {
        throw std::runtime_error("boom");
    } catch (const std::exception &e) {
        printf("exception: %s\n", e.what());
        if (std::string(e.what()) != "boom") return fail("throw/catch");
    } catch (...) {
        return fail("exception caught wrong handler");
    }

    // 3. std::filesystem → int128 compiler runtime（__divti3/__muloti4）路径
    std::error_code ec;
    auto cwd = std::filesystem::current_path(ec);
    if (ec) return fail("filesystem current_path");
    printf("filesystem: cwd length %zu\n", std::string(cwd.string()).size());
    // file_size on a *regular file* exercises operations.cpp's __file_size (int128
    // timestamp arithmetic in posix_stat). Note: file_size() on a directory is an
    // error by libc++ design (errc::is_a_directory), so test against a real file.
    // /etc/README ships in the image (mkdisk root README); fall back to argv[0]
    // (the smoke ELF itself) if absent.
    auto sz = std::filesystem::file_size("/README", ec);
    if (ec) {
        // Image layout may put README elsewhere; retry against the program path.
        sz = std::filesystem::file_size("/test/libcxx_smoke.elf", ec);
    }
    if (ec) return fail("filesystem file_size");
    printf("filesystem: file size %lld\n", (long long)sz);

    // 4. std::messages facet → catgets 路径（musl catopen 无 catalog 时降级）
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
