#include "sys/tls.h"
#include <string.h>

// 全局 TLS 信息单例
struct tls_info __g_tls_info = {0};

// 链接器提供的 TLS 段符号（user_linker.ld 定义）
extern "C" char __tls_template_start[];
extern "C" char __tls_template_end[];
extern "C" char __tls_tdata_size[];
extern "C" char __tls_tbss_size[];
extern "C" char __tls_align[];

// 静态路径：读链接器符号填 tls_info（单对象）
// 对应 ld.md §3.5.3 collect_tls_from_linker_symbols
void __libc_tls_init(void) {
    __g_tls_info.tdata_template = (void *)__tls_template_start;
    __g_tls_info.tdata_size = (size_t)__tls_tdata_size;
    __g_tls_info.tbss_size = (size_t)__tls_tbss_size;
    __g_tls_info.alignment = (size_t)__tls_align;
    __g_tls_info.size = __g_tls_info.tdata_size + __g_tls_info.tbss_size;
}
