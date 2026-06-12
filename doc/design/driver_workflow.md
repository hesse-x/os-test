# 驱动工作流重构设计

## 目标

重构 kbd_driver 和 kms_driver 的 IPC 模型，从"硬编码共享页 + 每次 sys_notify"改为"动态共享内存 + 环形缓冲区 + sleeping flag + 轮询窗口"，实现**快速路径零内核入口**（驱动醒着时不走 syscall）。

同时增加内核基础设施：TSC 时钟源、定时等待队列、sys_wait 超时、动态共享内存、sys_fb_info。

**不动**：disk_driver 和 fs_driver 的工作流，保留硬编码共享页。

## 1. TSC 时钟基础设施

### 1.1 rdtsc64() 内联函数

在 `arch/x64/utils.h` 新增：

```c
static inline uint64_t rdtsc64() {
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}
```

### 1.2 TSC 校准

在 `apic_init()` 的 `calibrate_lapic_timer()` 之后，复用 PIT 10ms 测量窗口校准 TSC：

```
tsc_start = rdtsc64()
// calibrate_lapic_timer() 内部 PIT 等待 10ms
tsc_end = rdtsc64()
tsc_freq = (tsc_end - tsc_start) * 100    // 10ms → 1s
tsc_per_ms = tsc_freq / 1000
tsc_base = rdtsc64()                       // 记录启动基线
```

全局变量（`arch/x64/apic.h` 声明，`arch/x64/apic.cc` 定义）：

```c
uint64_t tsc_freq;     // TSC ticks per second
uint64_t tsc_per_ms;   // TSC ticks per millisecond
```

### 1.3 sched_clock()

```c
// 返回自启动以来的纳秒数
uint64_t sched_clock() {
  return (rdtsc64() - tsc_base) * 1000000000ULL / tsc_freq;
}
```

### 1.4 修复 udelay() 精度

`arch/x64/smp.cc` 中 `udelay()` 的 `lapic_timer_ticks_calibrated / 10000 * us` 先除后乘截断，改为：

```c
uint32_t ticks = lapic_timer_ticks_calibrated * us / 10000;
```

后续可将 udelay 改为 TSC 忙等（不再破坏 LAPIC 定时器配置），但 AP 启动阶段 TSC 未校准，需保留 LAPIC 回退。当前仅修精度 bug。

## 2. 定时等待队列

### 2.1 proc_t 新增字段

```c
uint64_t wait_deadline;    // sched_clock() 纳秒截止时间，0=无限等待
uint8_t  wait_timed_out;   // 1=超时唤醒，0=notify 唤醒
```

### 2.2 cpu_local_t 新增字段

```c
list_node_t timer_queue;   // 定时等待队列哨兵（按 wait_deadline 升序排列）
```

初始化：`smp_init_cpu()` 和 BSP `isr_init()` 路径中 `list_init(&timer_queue)`。

### 2.3 队列操作

- `timer_queue_insert(proc)`：在 `scheduler_lock` 下，按 `wait_deadline` 升序插入（用 `wait_node`）
- `timer_queue_remove(proc)`：从定时队列移除（用于 notify 取消定时等待）

### 2.4 定时器到期检查

在 `timer_handler()` 中，EOI 之后遍历本 CPU timer_queue，将 `wait_deadline <= sched_clock()` 的进程唤醒（设 READY + `wait_timed_out = 1` + 入 run_queue + run_count++）。由于队列有序，遇到 `wait_deadline > now` 即停。

## 3. sys_wait(timeout_ms)

### 3.1 签名变更

```
sys_wait(uint64_t timeout_ms)   // arg1 = 超时毫秒数
```

- `timeout_ms == 0`：无限等待（向后兼容）
- `timeout_ms > 0`：设 `wait_deadline = sched_clock() + timeout_ms * 1000000`，插入定时队列

### 3.2 返回值

- `0`：被 notify 唤醒
- `1`：超时唤醒

实现：`schedule()` 返回后检查 `current_proc->wait_timed_out`。

### 3.3 sys_notify 联动

当 `sys_notify` 唤醒一个有 `wait_deadline != 0` 的进程时：
1. 从定时队列移除（`timer_queue_remove`）
2. 设 `wait_timed_out = 0`
3. 设 READY + 入 run_queue

### 3.4 用户态封装

```c
static inline int sys_wait(uint32_t timeout_ms) {
  return (int)__syscall1(SYS_WAIT, (int64_t)timeout_ms);
}
```

现有 `sys_wait()` 无参调用处改为 `sys_wait(0)`（无限等待）。

## 4. sys_fb_info(buf)

- **Syscall 号**：14
- **功能**：将 `g_fb_info`（`kms_fb_info` 结构）拷贝到用户态缓冲区
- **实现**：校验用户指针范围，`__memcpy(user_buf, &g_fb_info, sizeof(kms_fb_info))`
- **返回**：0=成功，正数=errno
- **替代**：KMS_INFO 硬编码共享页（0x508000）

## 5. sys_shm_create / sys_shm_attach

### 5.1 proc_t 新增

```c
#define MAX_SHM_PER_PROC 4
#define SHM_VADDR_BASE   0x510000   // 动态共享内存虚拟地址起始

struct shm_region {
  uint64_t vaddr;       // 本进程中的虚拟地址
  uint64_t phys;        // 物理页起始地址
  size_t   npages;      // 页数
  uint32_t ref_count;   // 引用计数
};

// proc_t 中新增：
shm_region shm_regions[MAX_SHM_PER_PROC];
```

### 5.2 sys_shm_create(size)

- **Syscall 号**：15
- **功能**：创建共享内存区域
- **实现**：
  1. `size` 向上取整到 PAGE_SIZE
  2. `bfc_alloc.alloc_page(npages)` 分配物理页，清零
  3. 在 `current_proc->shm_regions[]` 找空闲槽位
  4. 虚拟地址分配：从 `SHM_VADDR_BASE` 开始，扫描已有 region 找空位
  5. `map_user_page_direct()` 映射到调用者 PML4
  6. 设 `ref_count = 1`
- **返回**：虚拟地址（成功），0（失败）

### 5.3 sys_shm_attach(target_pid)

- **Syscall 号**：16
- **功能**：附加到目标进程的共享内存
- **实现**：
  1. `procs_lock` 下查找 `procs[target_pid].shm_regions[0]`（取第一个有 ref_count > 0 的）
  2. 在 `current_proc->shm_regions[]` 找空闲槽位
  3. 相同虚拟地址分配策略（从 SHM_VADDR_BASE 扫描）
  4. `map_user_page_direct()` 映射相同物理页到调用者 PML4
  5. 原子递增 `ref_count`
- **返回**：虚拟地址（成功），0（失败）

### 5.4 引用计数与回收

`proc_reap()` 中：
- PML4 遍历时：跳过 `proc->shm_regions[]` 中匹配的物理页（不释放）
- PML4 遍历后：对每个 `shm_region`，`procs_lock` 下递减 `ref_count`，归零时释放物理页
- PCB 清零时清空 `shm_regions`

## 6. 共享页重构

### 6.1 移除的硬编码映射

| 地址 | 原用途 | 替代方案 |
|------|--------|---------|
| 0x500000 | KBD_SHM | sys_shm_create（kbd_driver 创建） |
| 0x508000 | KMS_INFO | sys_fb_info syscall |
| 0x509000 | KMS_REQ | 同一 shm 页中 kms_ring 区域 |

保留 7 页硬编码映射：DISK_REQ(0x501000-0x502000)、DISK_RESP(0x503000-0x504000)、FS_REQ(0x505000)、FS_RESP(0x506000-0x507000)。

### 6.2 新共享页布局

kbd_driver 通过 `sys_shm_create(4096)` 创建一页，kbd_driver、kms_driver、shell 通过 `sys_shm_attach` 共享。

```
偏移    内容                        大小
0       driver_shm_header           8B
          uint8_t kbd_sleeping      (1=kbd 驱动在睡)
          uint8_t consumer_sleeping  (1=消费者[shell]在睡)
          uint8_t kms_sleeping      (1=kms 驱动在睡)
          uint8_t reserved[5]
8       kbd ring buffer
          uint32_t kbd_head         (写位置, 0..7)
          uint32_t kbd_tail         (读位置, 0..7)
16      kbd_msg kbd_msgs[8]         (8×8B = 64B)
80      padding                     (到偏移 128 对齐)
128     kms ring buffer
          uint32_t kms_head         (写位置, 0..1)
          uint32_t kms_tail         (读位置, 0..1)
136     kms_msg kms_msgs[2]         (2×256B = 512B)
648     (剩余 3448B 可用)
```

消息结构：

```c
struct kbd_msg {
  uint8_t type;       // 1=key event
  uint8_t ch;         // ASCII 字符
  uint8_t reserved[6];
};

struct kms_msg {
  uint32_t cmd;       // KMS_CMD_PUTC/CLEAR/SCROLL/CURSOR_MOVE
  uint32_t arg1;
  uint32_t arg2;
  uint32_t arg3;
  uint8_t  data[240]; // 扩展载荷（未来用）
};
```

### 6.3 common/shm.h 变更

- 删除 `KBD_SHM_ADDR`、`KMS_INFO_ADDR`、`KMS_REQ_ADDR` 定义
- 删除 `kbd_shm`、`kms_cmd`、`kms_req_shm` 结构体（保留 `kms_fb_info`，sys_fb_info 仍需）
- 新增 `driver_shm_header`、`kbd_msg`、`kms_msg` 结构体
- 新增偏移常量 `KBD_RING_OFFSET 0`、`KMS_RING_OFFSET 128`

### 6.4 kernel/proc.cc 变更

- `shm_init()`：10 页 → 7 页（删除 kbd_shm_phys、kms_info_shm_phys、kms_req_shm_phys 的分配和清零；删除 g_fb_info 拷贝）
- `map_shared_pages()`：删除 KBD_SHM_ADDR、KMS_INFO_ADDR、KMS_REQ_ADDR 三个映射
- `proc_reap()`：is_shared 检查中删除对这三个物理页的比较，增加对 `shm_regions[]` 的检查

## 7. Sleeping Flag 协议

### 7.1 核心原理

驱动在共享页中声明 `sleeping=1` 后才进入 `sys_wait(0)` 深度睡眠。发送方写入环形缓冲区后检查 sleeping flag：

- **快速路径**（`sleeping==0`）：驱动正在轮询/处理，会自行发现新数据，**无需 syscall**
- **慢速路径**（`sleeping==1`）：驱动在睡眠，调用 `sys_notify()` 唤醒

### 7.2 时序安全

驱动端：
```
1. 检查 ring 是否为空
2. 若空：设 sleeping=1（写共享内存，x86 store 可见性由 cache coherence 保证）
3. 再次检查 ring（防止写入方在步骤1-2之间写入）
4. 若仍空：sys_wait(0)（深度睡眠）
5. 唤醒后：设 sleeping=0
```

发送方：
```
1. 写入 ring（store）
2. 读 sleeping flag（load）
3. 若 sleeping==1：sys_notify()
```

竞态窗口：发送方在步骤2读到 `sleeping==0`，但驱动紧接着设 `sleeping=1` 并睡眠。此时发送方不 notify，但数据已在 ring 中。驱动将在下一个轮询周期（1ms 后 sys_wait(1) 超时）醒来发现数据。**最坏延迟 1ms，可接受**。

## 8. 驱动轮询窗口模式

### 8.1 模式描述

```
while (1) {
    while (ring 有消息) {
        处理一条消息;
        idle_polls = 0;
    }
    idle_polls++;
    if (idle_polls >= 8) {         // 约 8ms 无工作
        sleeping = 1;
        sys_wait(0);               // 深度睡眠
        sleeping = 0;
        idle_polls = 0;
    } else {
        sys_wait(1);               // 短暂等待 1ms 后重新轮询
    }
}
```

- `sys_wait(1)` 返回 1（超时）或 0（被 notify/IRQ 唤醒）
- 连续 8 次 `sys_wait(1)` 超时（约 8ms）后进入深度睡眠
- IRQ 绑定的驱动（kbd）在深度睡眠时由硬件中断唤醒

### 8.2 kbd_driver 流程

```
_start():
  sys_irq_bind(33)
  shm_addr = sys_shm_create(4096)       // 创建共享页
  kbd_ring = (kbd_ring *)(shm_addr + KBD_RING_OFFSET)
  初始化 head=tail=0, sleeping flags=0

  idle_polls = 0
  while (1):
    sys_wait(1)                          // 等 IRQ 或 1ms 超时
    scancode = inb(0x60)
    // scancode → ASCII (同现有逻辑)
    if 产生了字符 ch:
      写入 kbd_ring（head 推进）
      if consumer_sleeping:
        sys_notify(SHELL_PID)            // 慢路径：唤醒消费者
      idle_polls = 0
    else:
      idle_polls++
      if idle_polls >= 8:
        kbd_sleeping = 1
        sys_wait(0)                      // 深度睡眠，IRQ 唤醒
        kbd_sleeping = 0
        idle_polls = 0
```

注意：kbd_driver 自身不需要 sleeping flag 的快速路径（它的写入目标是 shell 读取的 kbd_ring，shell 检查 consumer_sleeping）。kbd_sleeping 标志是给发送方（shell）用的——当前没有进程主动写数据给 kbd_driver，此标志预留未来使用。

### 8.3 kms_driver 流程

```
_start():
  shm_addr = sys_shm_attach(KBD_DRIVER_PID)  // 重试直到成功
  kms_ring = (kms_ring *)(shm_addr + KMS_RING_OFFSET)
  sys_fb_info(&fb_info)                       // 获取 framebuffer 信息
  fb_vaddr = fb_info.fb_vaddr                 // 0x700000 (map_fb 仍由内核映射)

  idle_polls = 0
  while (1):
    if kms_head != kms_tail:                  // ring 非空
      msg = kms_msgs[tail]
      处理 msg (同现有渲染逻辑)
      tail = (tail + 1) % 2
      idle_polls = 0
    else:
      idle_polls++
      if idle_polls >= 8:
        kms_sleeping = 1
        sys_wait(0)
        kms_sleeping = 0
        idle_polls = 0
      else:
        sys_wait(1)
```

### 8.4 Shell 适配

**键盘输入**：
```
static char getc() {
  while (kbd_head == kbd_tail) {
    consumer_sleeping = 1;
    sys_wait(0);                    // 深度睡眠，等 kbd_driver notify
    consumer_sleeping = 0;
  }
  msg = kbd_msgs[tail]
  tail = (tail + 1) % 8
  return msg.ch;
}
```

**KMS 输出**（kms_flush）：
```
static void kms_flush() {
  for each buffered cmd:
    写入 kms_ring (head 推进)
    if kms_ring 满了:
      if kms_sleeping: sys_notify(KMS_DRIVER_PID)
      sys_wait(1)                  // 等 KMS 消费
  if kms_sleeping:
    sys_notify(KMS_DRIVER_PID)     // 慢路径
  // 快速路径：kms_sleeping==0 时不走 syscall
}
```

## 9. libc driver_loop 框架

### 9.1 接口

`user/include/driver.h`：

```c
typedef bool (*driver_poll_fn)(void *ctx);
typedef void (*driver_process_fn)(void *ctx);

struct driver_config {
  volatile uint8_t *sleeping_flag;  // 本驱动的 sleeping flag 指针
  int notify_pid;                   // 唤醒目标 PID，-1=不 notify
  driver_poll_fn    poll;           // 检查是否有待处理工作
  driver_process_fn process;        // 处理一条消息
  void *ctx;                        // 上下文
};

void driver_loop(struct driver_config *cfg);
```

### 9.2 实现

`user/lib/driver.cc`：

```c
void driver_loop(struct driver_config *cfg) {
  int idle_polls = 0;
  while (1) {
    bool had_work = false;
    while (cfg->poll(cfg->ctx)) {
      cfg->process(cfg->ctx);
      had_work = true;
    }
    if (had_work) {
      idle_polls = 0;
    } else {
      idle_polls++;
      if (idle_polls >= 8) {
        if (cfg->sleeping_flag) *cfg->sleeping_flag = 1;
        sys_wait(0);
        if (cfg->sleeping_flag) *cfg->sleeping_flag = 0;
        idle_polls = 0;
      } else {
        sys_wait(1);
      }
    }
  }
}
```

### 9.3 使用示例

kbd_driver：
```c
static bool kbd_poll(void *ctx) { /* 检查 IRQ 产生了字符 */ }
static void kbd_process(void *ctx) { /* 读 scancode → 写 ring → notify */ }
driver_config cfg = { .sleeping_flag = &hdr->kbd_sleeping, .notify_pid = SHELL_PID, ... };
driver_loop(&cfg);
```

kms_driver：
```c
static bool kms_poll(void *ctx) { return kms->head != kms->tail; }
static void kms_process(void *ctx) { /* 读一条 msg → 渲染 */ }
driver_config cfg = { .sleeping_flag = &hdr->kms_sleeping, .notify_pid = -1, ... };
driver_loop(&cfg);
```

## 10. libc kms_write_flush 适配

`user/lib/stdio.cc` 中：

- 新增全局变量 `g_kms_ring`、`g_shm_hdr`（由 shell 启动时通过 `kms_shm_init(addr)` 设置）
- `kms_write_flush`：写入 kms_ring（240 slot），ring 满时 notify KMS driver drain
- 快速路径：KMS 驱动醒着 → 写 ring 后直接返回，零 syscall

## 11. Framebuffer 映射

KMS 驱动的 framebuffer 映射（`map_fb=true`，虚拟地址 0x700000）**不变**，仍在 `process_create_elf()` 中由内核映射。`sys_fb_info()` 只负责传递元信息（宽高/深度/虚拟地址），不做映射。

## 12. 启动时序

kbd_driver（PID 3）先运行 → `sys_shm_create(4096)` → 共享页就绪。
kms_driver（PID 4）和 shell（PID 5）→ `sys_shm_attach(KBD_DRIVER_PID)` 重试循环（`sys_wait(1)` 间隔），直到 kbd_driver 创建 shm。

## 13. 依赖图

```
1. TSC + sched_clock + udelay 修复
   ↓
2. 定时等待队列（proc_t + cpu_local_t + timer_handler）
   ↓
3. sys_wait(timeout_ms)
   ↓
4. sys_fb_info               （独立，可并行）
   ↓
5. sys_shm_create / attach   （依赖 4 的 syscall 模式）
   ↓
6. 共享页重构                 （依赖 5）
   ↓
7. kbd_driver 重写            （依赖 3, 5, 6）
8. kms_driver 重写            （依赖 3, 4, 5, 6）
9. Shell 适配                 （依赖 3, 5, 6）
10. proc_reap 动态 shm 处理   （依赖 5, 6）
11. kms_write_flush 适配      （依赖 5, 6）
12. KMS 帧调度优化             （依赖 8, 11）
13. 构建集成 + 验证            （依赖所有）
```

注：Step 11（libc driver_loop 框架）已跳过，kbd/kms 各自内联 loop 逻辑，功能等价。

## 14. 实际实现与设计差异

### kbd_driver：纯中断驱动（非轮询窗口）

设计文档 8.2 节描述的轮询窗口模式（idle_polls + sys_wait(1) + deep sleep）未采用。实际实现为纯中断驱动：

```
while (1) {
    sys_wait(0);               // 深度睡眠，等 IRQ 唤醒
    while (inb(0x64) & 0x01) { // drain 所有 scancode
        scancode = inb(0x60);
        // 处理 → 写 ring → notify shell
    }
}
```

理由：kbd 数据源是硬件 IRQ，不需要超时轮询。`kbd_sleeping` flag 未使用。

### kms_driver：帧调度模式（非轮询窗口）

设计文档 8.3 节的轮询窗口模式已改为定时刷帧：

- 每次醒来 drain 整个 ring（而非只处理 1 条消息）
- `sys_wait(16)` 定时 16ms（~60fps），而非 idle_polls + deep sleep
- 设置 `kms_sleeping=1` 后二次检查 ring（防止 lost-wakeup）

### kms_ring：240 slot（非 2 slot）

`kms_msg` 从 256B 缩小到 16B（删除未使用的 `data[240]`），ring slot 从 2 增加到 240。减少 shell 写入时的阻塞和上下文切换。

### Sleeping flag 双检查协议

所有 sleeping flag 使用点均实现双检查（设 flag 后重新检查数据），防止 lost-wakeup 竞态：
- shell `getc()`：设 `consumer_sleeping=1` 后重查 kbd_ring
- kms_driver：设 `kms_sleeping=1` 后重查 kms_ring

## 15. 后续：DRM/KMS compositor 重构

当前逐字符 KMS_CMD_PUTC 消息模型是过渡方案。后续需重构为共享文本 buffer + compositor 模型：

- **共享 char grid**：80x25 的 char+attr 数组（~4KB），shell 直接写入（纯内存，零 syscall）
- **compositor 帧调度**：KMS driver 定时扫描 char grid，按 dirty region 重绘 framebuffer
- **即时响应**：用户输入回显等场景设 dirty flag 通知 KMS 立即刷帧
- **与 Linux DRM 模型对齐**：应用写 buffer → 显示服务合成输出 → page flip
