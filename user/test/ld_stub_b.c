// ld.so 多依赖单元测试 stub：libb.so
// 导出 ldb_chain()（线性链场景用）、ldb_via_a()（调 liba 符号，验证 b→a 解析）
int lda_answer(void);  // liba.so 符号（JUMP_SLOT）

int ldb_chain(void) {
    return 42;  // 线性链场景主 ELF 期望 42
}

int ldb_via_a(void) {
    return lda_answer() + 1;  // 调 liba，验证 b→a 跨模块 JUMP_SLOT
}
