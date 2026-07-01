#ifndef USER_SYS_TLS_H
#define USER_SYS_TLS_H

#include <stddef.h>

// TLS 模板信息（variant II 布局）
// 阶段一裁剪到最小：pthread_create 只需拷贝合并后的模板，
// 不需 per-object 偏移。未来 dlopen + __tls_get_addr 再加 per-object 信息。
struct tls_info {
    void *tdata_template;       // TLS 模板（合并后的 tdata 拷贝源）
    size_t tdata_size;          // tdata 总大小
    size_t tbss_size;           // tbss 总大小
    size_t alignment;           // 最大对齐
    size_t size;                // 总大小（tdata + tbss + padding，variant II 块大小）
};

// 全局单例，pthread_create 读取
extern struct tls_info __g_tls_info;

// 初始化函数（静态路径：读链接器符号填 __g_tls_info）
void __libc_tls_init(void);

#endif // USER_SYS_TLS_H
