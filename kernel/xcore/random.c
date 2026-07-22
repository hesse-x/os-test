/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// kernel/xcore/random.c — ChaCha20 CSPRNG（per-CPU state，对齐 Linux crng
// 极简版）
//
// 熵源：RDRAND（有则用之），兜底 RDTSC 抖动 + cpu_id + 启动时间混合。
// 三条对外路径（sys_getrandom / /dev/random / /dev/urandom）共用 csprng_read。
//
// 并发模型：取数时关中断（防同核嵌套 + 防迁移），临界区内只做
// 播种检查 → 生成到栈上固定小块 → reseed 判定；copy_to_user 在临界区外。
//
// 熵源相关故障一律 WARN 不 panic：随机数降级是可运行状态。

#include "kernel/xcore/random.h"

#include <stdint.h>

#include "arch/x64/apic.h"
#include "arch/x64/rdrand.h"
#include "arch/x64/smp.h"
#include "arch/x64/utils.h"
#include "kernel/xcore/log.h"

// ===================== ChaCha20 块函数 =====================

#define CHACHA_ROTL32(x, r) (((x) << (r)) | ((x) >> (32 - (r))))
#define CHACHA_QR(a, b, c, d)                                                  \
  do {                                                                         \
    a += b;                                                                    \
    d ^= a;                                                                    \
    d = CHACHA_ROTL32(d, 16);                                                  \
    c += d;                                                                    \
    b ^= c;                                                                    \
    b = CHACHA_ROTL32(b, 12);                                                  \
    a += b;                                                                    \
    d ^= a;                                                                    \
    d = CHACHA_ROTL32(d, 8);                                                   \
    c += d;                                                                    \
    b ^= c;                                                                    \
    b = CHACHA_ROTL32(b, 7);                                                   \
  } while (0)

// out[64] = chacha20_block(state[16])；调用方保证 out/state 不重叠
static void chacha20_block(const uint32_t state[16], uint32_t out[16]) {
  uint32_t w[16];
  for (int i = 0; i < 16; i++)
    w[i] = state[i];
  for (int i = 0; i < 10; i++) { // 20 轮 = 10 次双轮
    CHACHA_QR(w[0], w[4], w[8], w[12]);
    CHACHA_QR(w[1], w[5], w[9], w[13]);
    CHACHA_QR(w[2], w[6], w[10], w[14]);
    CHACHA_QR(w[3], w[7], w[11], w[15]);
    CHACHA_QR(w[0], w[5], w[10], w[15]);
    CHACHA_QR(w[1], w[6], w[11], w[12]);
    CHACHA_QR(w[2], w[7], w[8], w[13]);
    CHACHA_QR(w[3], w[4], w[9], w[14]);
  }
  for (int i = 0; i < 16; i++)
    out[i] = w[i] + state[i];
}

// ===================== per-CPU state =====================

typedef struct {
  uint32_t state[16];      // 常量4 + key8 + counter2 + nonce2
  uint64_t generated;      // 已输出字节数（reseed 字节触发）
  uint64_t last_reseed_ns; // 上次 reseed 时间（sched_clock）
  uint8_t initialized;     // 惰性播种标志
} csprng_state;

static csprng_state rng_states[MAX_CPUS];

#define RESEED_BYTES (1u << 20)                     // 1 MiB
#define RESEED_INTERVAL_NS (300ULL * 1000000000ULL) // 5 分钟
#define CHUNK_SIZE 256 // 临界区内单次生成的栈上小块上限

static void secure_zero(void *p, size_t n) {
  __memset(p, 0, n);
  __asm__ volatile("" ::: "memory");
}

// ===================== 熵源 =====================

static int fallback_warned;

// 兜底混合源（仅用于播种/reseed，不直接输出）：TSC 抖动 + cpu_id + 时间。
// 明确降级语义：只保证"可用、不崩、每次启动不同"，不宣称密码学强度。
static void fallback_entropy(uint8_t *out, size_t len) {
  if (!fallback_warned) {
    fallback_warned = 1;
    printk(LOG_WARN, "random: no RDRAND, using fallback entropy source\n");
  }
  size_t off = 0;
  while (off < len) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) // 采样 8 次取低 8 bit 异或
      v ^= rdtsc64();
    v ^= (uint64_t)get_cpu_local()->cpu_id * 0x9e3779b97f4a7c15ULL;
    v ^= sched_clock();
    size_t n = len - off < 8 ? len - off : 8;
    __memcpy(out + off, &v, n);
    off += n;
  }
}

static void get_entropy(uint8_t *out, size_t len) {
  size_t off = 0;
  while (off < len) {
    uint64_t v;
    if (rdrand64(&v) != 0) {
      fallback_entropy(out + off, len - off);
      return;
    }
    size_t n = len - off < 8 ? len - off : 8;
    __memcpy(out + off, &v, n);
    off += n;
  }
}

// ===================== 播种 / reseed =====================

// 调用方须持本核临界区（关中断）
static void seed_cpu(csprng_state *s) {
  uint8_t seed[40]; // 32B key + 8B nonce
  get_entropy(seed, sizeof(seed));
  s->state[0] = 0x61707865;
  s->state[1] = 0x3320646e;
  s->state[2] = 0x79622d32;
  s->state[3] = 0x6b206574;
  __memcpy(&s->state[4], seed, 32);      // key
  s->state[12] = 0;                      // counter lo
  s->state[13] = 0;                      // counter hi
  __memcpy(&s->state[14], seed + 32, 8); // nonce
  s->generated = 0;
  s->last_reseed_ns = sched_clock();
  secure_zero(seed, sizeof(seed));
  // key 全零检测：实现 bug 信号，WARN_ON 提示开发期介入
  uint32_t acc = 0;
  for (int i = 4; i < 12; i++)
    acc |= s->state[i];
  WARN_ON(acc == 0);
  s->initialized = 1;
}

// reseed：块函数输出前 32B 作新 key，再混入 32B 新鲜熵（前向安全）
static void reseed_cpu(csprng_state *s) {
  uint32_t out[16];
  chacha20_block(s->state, out);
  uint8_t fresh[32];
  get_entropy(fresh, sizeof(fresh));
  for (int i = 0; i < 8; i++)
    s->state[4 + i] = out[i] ^ ((uint32_t *)fresh)[i];
  s->state[12] = 0;
  s->state[13] = 0;
  s->generated = 0;
  s->last_reseed_ns = sched_clock();
  secure_zero(out, sizeof(out));
  secure_zero(fresh, sizeof(fresh));
}

// ===================== csprng_read =====================

// 关中断保护的 per-CPU 临界区：定位本核 state + 生成到内核 buf
static void generate_chunk(void *buf, size_t n) {
  uint64_t flags;
  __asm__ volatile("pushfq; popq %0; cli" : "=r"(flags));

  csprng_state *s = &rng_states[get_cpu_local()->cpu_id];
  if (!s->initialized)
    seed_cpu(s);
  if (s->generated >= RESEED_BYTES ||
      sched_clock() - s->last_reseed_ns >= RESEED_INTERVAL_NS)
    reseed_cpu(s);

  uint8_t *p = (uint8_t *)buf;
  size_t off = 0;
  while (off < n) {
    uint32_t block[16];
    chacha20_block(s->state, block);
    // counter 自增（64 bit，跨 state[12]/[13]）
    if (++s->state[12] == 0)
      s->state[13]++;
    size_t k = n - off < 64 ? n - off : 64;
    __memcpy(p + off, block, k);
    secure_zero(block, sizeof(block));
    off += k;
  }
  s->generated += n;

  __asm__ volatile("pushq %0; popfq" : : "r"(flags));
}

void csprng_read(void *buf, size_t len) {
  uint8_t *p = (uint8_t *)buf;
  while (len > 0) {
    size_t n = len < CHUNK_SIZE ? len : CHUNK_SIZE;
    generate_chunk(p, n);
    p += n;
    len -= n;
  }
}

// ===================== init（BSP） =====================

// RFC 8439 §2.3.2 测试向量：key=00..1f, counter=1,
// nonce=00:00:00:09:00:00:00:4a:00:00:00:00（小端 word: 09000000 4a000000
// 00000000）
static const uint32_t selftest_state[16] = {
    0x61707865, 0x3320646e, 0x79622d32, 0x6b206574, 0x03020100, 0x07060504,
    0x0b0a0908, 0x0f0e0d0c, 0x13121110, 0x17161514, 0x1b1a1918, 0x1f1e1d1c,
    0x00000001, 0x09000000, 0x4a000000, 0x00000000};
static const uint8_t selftest_expected[64] = {
    0x10, 0xf1, 0xe7, 0xe4, 0xd1, 0x3b, 0x59, 0x15, 0x50, 0x0f, 0xdd,
    0x1f, 0xa3, 0x20, 0x71, 0xc4, 0xc7, 0xd1, 0xf4, 0xc7, 0x33, 0xc0,
    0x68, 0x03, 0x04, 0x22, 0xaa, 0x9a, 0xc3, 0xd4, 0x6c, 0x4e, 0xd2,
    0x82, 0x64, 0x46, 0x07, 0x9f, 0xaa, 0x09, 0x14, 0xc2, 0xd7, 0x05,
    0xd9, 0x8b, 0x02, 0xa2, 0xb5, 0x12, 0x9c, 0xd1, 0xde, 0x16, 0x4e,
    0xb9, 0xcb, 0xd0, 0x83, 0xe8, 0xa2, 0x50, 0x3c, 0x4e};

static void chacha20_selftest(void) {
  uint32_t out[16];
  chacha20_block(selftest_state, out);
  if (__memcmp(out, selftest_expected, 64) != 0) {
    const uint8_t *got = (const uint8_t *)out;
    printk(LOG_WARN,
           "random: chacha20 selftest FAILED, got %02x%02x%02x%02x"
           "%02x%02x%02x%02x expected %02x%02x%02x%02x%02x%02x%02x%02x\n",
           got[0], got[1], got[2], got[3], got[4], got[5], got[6], got[7],
           selftest_expected[0], selftest_expected[1], selftest_expected[2],
           selftest_expected[3], selftest_expected[4], selftest_expected[5],
           selftest_expected[6], selftest_expected[7]);
  }
  secure_zero(out, sizeof(out));
}

void xcore_random_init(void) {
  chacha20_selftest();
  rdrand_init();
  // CPU0（BSP）立即播种；AP 首次 csprng_read 时惰性播种
  seed_cpu(&rng_states[0]);
}
