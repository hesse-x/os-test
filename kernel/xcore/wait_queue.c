/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "arch/x64/smp.h"
#include "kernel/xcore/log.h"
#include "kernel/xcore/wait_queue.h"
#include "kernel/xcore/xtask.h" // current_task + struct xtask (remove_wait_queue diagnostic)

void init_wait_queue_head(wait_queue_head *wq) {
  wq->lock = SPINLOCK_INIT;
  list_init(&wq->head);
}

void add_wait_queue(wait_queue_head *wq, wait_queue_t *wait) {
  uint64_t flags;
  spin_lock_irqsave(&wq->lock, &flags);
  list_push_back(&wq->head, &wait->node);
  spin_unlock_irqrestore(&wq->lock, flags);
}

void remove_wait_queue(wait_queue_head *wq, wait_queue_t *wait) {
  uint64_t flags;
  spin_lock_irqsave(&wq->lock, &flags);
  if (wait->node.prev == &wait->node && wait->node.next == &wait->node)
    printk(LOG_WARN, "remove_wait_queue: self-ref node=%p wq=%p pid=%d\n",
           (void *)&wait->node, (void *)wq, current_task->pid);
  list_remove(&wait->node);
  spin_unlock_irqrestore(&wq->lock, flags);
}

// 持 wq->lock 遍历并在锁内回调。这是根治栈上 wait 节点 UAF 的关键：wait 节点
// 常是调用者栈上变量（sys_epoll_wait/timerfd/signalfd/eventfd/poll/ring），
// remove_wait_queue 同样取 wq->lock，二者互斥 ⇒ 回调期间 waiter 无法摘除/销毁
// 节点，回调用的节点必定有效。锁序见 doc/design/kernel/epoll.md：
//   A 类 waiter 回调取 scheduler_lock（wq->lock → scheduler_lock，无反向边——
//     持 scheduler_lock 的代码段内不调用任何 wait_queue 操作）；
//   B 类 epitem 回调（ep_poll_callback）持 ep->lock 再嵌套 __wake_up(&ep->wq)，
//     锁序 W_fd → ep->lock → W_ep → scheduler_lock，全程单向无环。
// 嵌套 __wake_up 是不同 wq 实例，非同锁重入；各层 irqsave 保存各自栈上 flags，
// irqrestore 正确恢复。
// flags 透传给每个回调，xcore 不解释其含义（poll 掩码语义由 bsd 回调解释）。
void __wake_up(wait_queue_head *wq, unsigned long flags) {
  uint64_t irqflags;
  spin_lock_irqsave(&wq->lock, &irqflags);
  list_node *it = wq->head.next;
  while (it != &wq->head) {
    wait_queue_t *wq_entry = LIST_ENTRY(it, wait_queue_t, node);
    // 先取 next：回调（wake_with_event）只改 wq_entry->state 不摘节点；
    // 摘节点仅由 remove_wait_queue 做，而它要等我们放锁，故遍历期间链表稳定。
    list_node *next = it->next;
    // 防御：自指节点（next==self）是不在任何链表里的已摘除节点。正常链表里不会
    // 出现它；一旦出现说明有调用者把栈上 wait 节点残留在了 wq 上且栈已被复用
    // （见 sys_poll 的 wait 节点泄漏修复）。此时若继续遍历会因 it==next 死循环
    // 把整机卡死，故告警并提前结束本次唤醒——根治仍是不让节点残留。
    if (next == it) {
      WARN_ON_ONCE(1);
      break;
    }
    if (wq_entry->func)
      wq_entry->func(wq_entry, flags);
    it = next;
  }
  spin_unlock_irqrestore(&wq->lock, irqflags);
}
