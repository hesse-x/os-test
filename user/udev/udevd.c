/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// Minimal udevd — subscribes to NETLINK_KOBJECT_UEVENT via epoll,
// receives uevent messages and prints them to stdout.
//
// socket activation (dep0 1.1)：init 可经 fd 3 传入一个已 listen 的
// AF_UNIX socket（/run/udev/socket）。本进程开头探测 fd 3：
//   - 是 listen socket → 纳入 epoll，accept 客户端连接（udevd
//   主体方案接管处理）
//   - 非 socket / 无效 → 跳过，仅跑 netlink（降级，udevd 仍能起）
// 探测用 try-accept（本 OS 无 getsockname）。

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/netlink.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xos/input.h>
#include <xos/input_key.h>
#include <xos/ioctl.h>

#define UDEV_LISTEN_FD 3

/* udevd monitor client 表(单线程,无锁)。每个 client = accept 出的 conn +
 * udevd 为其建的 pipe(rd 给 client / wr udevd 持)。conn fd 拿到 pipe rd fd
 * 后即关闭(§4.4 step 8),仅 pipe wr 端进 epoll(可写时排空)。 */
#define UDEV_MAX_CLIENTS 8

struct udev_client {
  int pipe_wr; /* udevd 持有的写端(收 SCM_RIGHTS 给 client 的 rd)。<0=空闲槽 */
  char wbuf[4096]; /* 待写 pipe 缓冲(pipe 满则丢,Q2) */
  int wlen;        /* wbuf 已用字节 */
};

static struct udev_client udev_clients[UDEV_MAX_CLIENTS];

/* 单字符进度链(§8.6 调试纪律):u=uevent a=accept c=coldplug r=remove w=write */

#define EVDEV_BITS_PER_LONG (sizeof(long) * 8)
#define NBITS(x) ((((x) - 1) / EVDEV_BITS_PER_LONG) + 1)
#define LONG(x) ((x) / EVDEV_BITS_PER_LONG)
#define OFF(x) ((x) % EVDEV_BITS_PER_LONG)
#define test_bit(bit, bits) (!!(bits[LONG(bit)] & (1UL << OFF(bit))))

/* 探测 fd 是否为已 listen 的 AF_UNIX socket。
 * 返 >=0（== fd）表示是 listen socket；-1 表示非 socket / 无效。 */
static int probe_listen_fd(int fd) {
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0)
    return -1; /* fd 无效 */
  /* 置非阻塞后 try-accept：成功/ EAGAIN 都说明是 listen socket */
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int probe = accept(fd, NULL, NULL);
  int saved = errno;
  fcntl(fd, F_SETFL, flags); /* 还原 */
  if (probe >= 0) {
    close(probe); /* 排队的 client 立即关，真实 client 重连 */
    return fd;
  }
  if (saved == EAGAIN)
    return fd; /* 无排队，socket activation 成立 */
  /* ENOTSOCK / EINVAL 等 → 非 listen socket */
  return -1;
}

/* client 表初始化(全部空闲)。 */
static void clients_init(void) {
  for (int i = 0; i < UDEV_MAX_CLIENTS; i++)
    udev_clients[i].pipe_wr = -1;
}

/* 分配一个空闲 client 槽,返回下标;<0 表满。 */
static int client_alloc(void) {
  for (int i = 0; i < UDEV_MAX_CLIENTS; i++) {
    if (udev_clients[i].pipe_wr < 0) {
      udev_clients[i].pipe_wr = -2; /* 占位防重入,稍后填真 fd */
      udev_clients[i].wlen = 0;
      return i;
    }
  }
  return -1;
}

/* 通过 pipe_wr fd 反查 client 槽下标;<0 表未找到。
 * M1+M2 暂未接入(按 index 管理即可);shim monitor 客户端(第二步)经 fd
 * 反查时启用。标 __attribute__((unused)) 过 -Werror=unused-function。 */
static __attribute__((unused)) int client_find_by_wr(int pipe_wr) {
  for (int i = 0; i < UDEV_MAX_CLIENTS; i++)
    if (udev_clients[i].pipe_wr == pipe_wr)
      return i;
  return -1;
}

/* 降级自 bind+listen：init 未传 fd 3 或探测失败时，udevd 自建 listen socket。
 * 保证 udevd 永远能 accept 客户端连接，不硬依赖 init socket activation。 */
static int fallback_self_bind_listen(void) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, "/run/udev/socket", sizeof(addr.sun_path) - 1);
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  if (listen(fd, 8) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

/* udevd db 原子写(user 态 C,int 代 bool,freestanding 无 stdbool) */
static int db_write_property(uint32_t devnum, const char *kv_str,
                             size_t kv_len) {
  char key[32], tmp_path[80], final_path[80];
  /* key = devnum 十进制(对齐 Linux 用 devnum 寻址,本 OS 无 major/minor) */
  int klen = snprintf(key, sizeof(key), "%u", devnum);
  if (klen <= 0 || klen >= (int)sizeof(key))
    return -EINVAL;
  snprintf(final_path, sizeof(final_path), "/run/udev/data/%s", key);
  snprintf(tmp_path, sizeof(tmp_path), "/run/udev/data/%s.tmp", key);

  /* 1. 写 tmp 文件 */
  int fd = open(tmp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0)
    return -errno;
  ssize_t off = 0;
  while ((size_t)off < kv_len) {
    ssize_t n = write(fd, kv_str + off, kv_len - off);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      close(fd);
      unlink(tmp_path);
      return -errno;
    }
    off += n;
  }
  close(fd);

  /* 2. rename 原子覆盖(依赖本方案 §3.1 SYS_RENAME) */
  if (rename(tmp_path, final_path) < 0) {
    int saved = errno;
    unlink(tmp_path); /* 清理 tmp */
    return -saved;
  }
  return 0;
}

/* 读 db 全量到 buf(client 侧,shim 用)。返回读到的字节数 / 负 errno。 */
ssize_t db_read_all(uint32_t devnum, char *buf, size_t bufcap) {
  if (!devnum || !buf || bufcap == 0)
    return -EINVAL;
  char key[32], path[80];
  int klen = snprintf(key, sizeof(key), "%u", devnum);
  if (klen <= 0 || klen >= (int)sizeof(key))
    return -EINVAL;
  snprintf(path, sizeof(path), "/run/udev/data/%s", key);
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return -ENOENT;
  ssize_t n = read(fd, buf, bufcap - 1);
  close(fd);
  if (n < 0)
    return -errno;
  buf[n] = '\0';
  return n;
}

/* 删 db 文件(remove 两阶段真删步骤用,父文档 §4.6)。 */
int db_remove(uint32_t devnum) {
  if (!devnum)
    return -EINVAL;
  char key[32], path[80];
  int klen = snprintf(key, sizeof(key), "%u", devnum);
  if (klen <= 0 || klen >= (int)sizeof(key))
    return -EINVAL;
  snprintf(path, sizeof(path), "/run/udev/data/%s", key);
  if (unlink(path) < 0)
    return -errno;
  return 0;
}

/* device 补全:收 uevent(含 name=DEVPATH)→ 构造完整 KV 写入 out(buf)。
 * 标识 KV 由 name+subsystem 构造;property(ID_INPUT_*)查 db 取值追加(乙)。
 * out_nul 为 out 容量。返回写入字节数(含末尾 \0 分隔),<0 跳过。
 * KV 格式(对齐 §4.3, \0 分隔):
 *   "<action>@<name>\0ACTION=<action>\0DEVNAME=/dev/<name>\0
 *    DEVPATH=/sys/class/<subsys>/<name>\0SUBSYSTEM=<subsys>\0
 *    DEVTYPE=<devtype>\0DEVNUM=<n>\0ID_INPUT=1\0...\0"
 * db 文件格式为 KEY=VALUE\n(见 input_id_compute 写入);本函数读 db 把每行转成
 * \0 分隔 KV 追加。 */
static int device_complete_kv(const char *action, const char *name,
                              const char *subsystem, const char *devtype,
                              char *out, size_t out_nul) {
  /* 取 devnum(三边一致 = ino)作 db key + DEVNUM 字段 */
  char devnode[64];
  snprintf(devnode, sizeof(devnode), "/dev/%s", name);
  struct stat st;
  if (stat(devnode, &st) < 0)
    return -1;
  uint32_t devnum = (uint32_t)st.st_rdev;
  if (devnum == 0)
    return -1;

  int len = 0;
#define APPEND_KV0(fmt, ...)                                                   \
  do {                                                                         \
    int n = snprintf(out + len, out_nul - (size_t)len, fmt, ##__VA_ARGS__);    \
    if (n < 0 || (size_t)n >= out_nul - (size_t)len)                           \
      return -1;                                                               \
    len += n;                                                                  \
  } while (0)

  APPEND_KV0("%s@%s", action, name);
  APPEND_KV0("ACTION=%s", action);
  APPEND_KV0("DEVNAME=/dev/%s", name);
  APPEND_KV0("DEVPATH=/sys/class/%s/%s", subsystem, name);
  APPEND_KV0("SUBSYSTEM=%s", subsystem);
  if (devtype && devtype[0])
    APPEND_KV0("DEVTYPE=%s", devtype);
  APPEND_KV0("DEVNUM=%u", devnum);
#undef APPEND_KV0

  /* 查 db 拿 property(乙)。db 文件 KEY=VALUE\n,转成 \0 分隔 KV 追加。 */
  char dbbuf[1024];
  ssize_t dn = db_read_all(devnum, dbbuf, sizeof(dbbuf));
  if (dn > 0) {
    char *line = dbbuf;
    char *end = dbbuf + dn;
    while (line < end) {
      char *eol = line;
      while (eol < end && *eol != '\n')
        eol++;
      int llen = (int)(eol - line);
      if (llen > 0 && line[0] != '\0') {
        if ((size_t)len + (size_t)llen + 1 > out_nul)
          return -1;
        memcpy(out + len, line, (size_t)llen);
        len += llen;
        out[len++] = '\0';
      }
      line = (eol < end) ? eol + 1 : end;
    }
  }
  return len;
}

/* 把 KV(olen 字节,\0 分隔)写进每个活跃 client 的 wbuf,并尽量排空到 pipe。
 * pipe 满则丢该 client 此事件(Q2 非阻塞)。单线程,遍历期间不改表结构。 */
static void clients_broadcast(const char *kv, int olen) {
  if (olen <= 0)
    return;
  for (int i = 0; i < UDEV_MAX_CLIENTS; i++) {
    if (udev_clients[i].pipe_wr < 0)
      continue;
    struct udev_client *c = &udev_clients[i];
    int space = (int)sizeof(c->wbuf) - c->wlen;
    if (olen > space) {
      /* 缓冲满:先尝试排空,再决定丢/留 */
      int w = write(c->pipe_wr, c->wbuf, (size_t)c->wlen);
      if (w > 0) {
        memmove(c->wbuf, c->wbuf + w, (size_t)(c->wlen - w));
        c->wlen -= w;
        space = (int)sizeof(c->wbuf) - c->wlen;
      }
    }
    if (olen <= space) {
      memcpy(c->wbuf + c->wlen, kv, (size_t)olen);
      c->wlen += olen;
    }
    /* 排空(非阻塞):EAGAIN 时留 wbuf 等下次 EPOLLOUT/下次广播 */
    if (c->wlen > 0) {
      int w = write(c->pipe_wr, c->wbuf, (size_t)c->wlen);
      if (w > 0) {
        memmove(c->wbuf, c->wbuf + w, (size_t)(c->wlen - w));
        c->wlen -= w;
      }
    }
  }
}

/* 前置声明:accept_client 在 coldplug_trigger 定义之前调用它(冷启动补发)。 */
static void coldplug_trigger(void);

/* accept 新 client:建单向 pipe(rd 给 client, wr udevd 持),经 conn fd 用
 * SCM_RIGHTS 回传 pipe rd fd(§4.4)。回传后关 conn fd(connect 即订阅,Q5)。
 * accept/pipe/sendmsg 任一失败 → 关 fd 跳过该 client。 */
static void accept_client(int listen_fd) {
  int cfd = accept(listen_fd, NULL, NULL);
  if (cfd < 0)
    return;

  int pfd[2];
  if (pipe(pfd) < 0) {
    close(cfd);
    return;
  }
  int pipe_rd = pfd[0]; /* 给 client */
  int pipe_wr = pfd[1]; /* udevd 持 */

  /* SCM_RIGHTS 回传 pipe_rd。带 1 字节 dummy data(iov 必须非空)。 */
  char dummy = 'u';
  struct iovec iov;
  iov.iov_base = &dummy;
  iov.iov_len = 1;
  char cmsgbuf[CMSG_SPACE(sizeof(int))];
  struct msghdr msg;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsgbuf;
  msg.msg_controllen = sizeof(cmsgbuf);
  struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cmsg), &pipe_rd, sizeof(int));

  if (sendmsg(cfd, &msg, 0) < 0) {
    /* 回传失败:关 pipe rd(避免泄漏),留 wr 待清理 */
    close(pipe_rd);
    close(pipe_wr);
    close(cfd);
    return;
  }

  /* 登记到 client 表 */
  int slot = client_alloc();
  if (slot < 0) {
    /* 表满:关 pipe + conn,client 连接断(§8.3 accept backlog 不会满,极少) */
    close(pipe_wr);
    close(pipe_rd);
    close(cfd);
    return;
  }
  udev_clients[slot].pipe_wr = pipe_wr;

  close(cfd); /* connect 即订阅,关 conn(§4.4 step 8) */
  putchar('a');

  /* 首个 client 补 coldplug(§4.5):为现有设备合成 add 转发给该 client。
   * 复用 coldplug_trigger 内核重广播路径——重广播的 uevent 回到 udevd 自身
   * netlink fd,经 handle_uevent 写各 client(含新 client),无需此处单独转发。
   * 故此处仅再触发一次 coldplug_trigger(若设备仍在)。 */
  coldplug_trigger();
}

/* udevd input_id builtin(user 态 C,int 代 bool)
 * 开 /dev/input/eventX 探 caps,合成键盘类 ID_INPUT_* 写 db。
 * 对齐 Linux src/udev/udev-builtin-input_id.c(键盘路径子集)。 */
static int input_id_compute(const char *devnode, uint32_t devnum) {
  int fd = open(devnode, O_RDONLY);
  if (fd < 0)
    return -errno;

  /* 事件类型位图:EV_KEY/EV_REL/EV_ABS 等。本轮 event0 纯键盘(只 EV_KEY),
   * 但仍探完整 evbits 以对齐 Linux input_id 的 ID_INPUT 判定(任何
   * EV_KEY/EV_REL/EV_ABS → ID_INPUT=1),为 B6 真实多设备预留。 */
  unsigned long evbits[NBITS(EV_MAX + 1)];
  memset(evbits, 0, sizeof(evbits));
  if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) {
    close(fd);
    return -errno;
  }

  int is_input = 0;    /* ID_INPUT=1 */
  int is_keyboard = 0; /* ID_INPUT_KEYBOARD=1 */
  int is_key = 0;      /* ID_INPUT_KEY=1 */

  /* ID_INPUT:任何 EV_KEY/EV_REL/EV_ABS 设备(对齐 Linux input_id 主开关) */
  if (test_bit(EV_KEY, evbits) || test_bit(EV_REL, evbits) ||
      test_bit(EV_ABS, evbits))
    is_input = 1;

  /* ID_INPUT_KEYBOARD / KEY:EV_KEY 且有键盘类按键
   * (对齐 Linux:有 KEY_A..KEY_Z / KEY_ENTER / KEY_SPACE 等任一即 keyboard) */
  if (test_bit(EV_KEY, evbits)) {
    unsigned long keybits[NBITS(KEY_MAX + 1)];
    memset(keybits, 0, sizeof(keybits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) >= 0) {
      /* keyboard:字母/数字/控制键 */
      if (test_bit(KEY_A, keybits) || test_bit(KEY_ENTER, keybits) ||
          test_bit(KEY_SPACE, keybits) || test_bit(KEY_LEFTCTRL, keybits)) {
        is_keyboard = 1;
        is_key = 1;
      }
    }
    /* B6 延后(§3.4):MOUSE(EV_REL+REL_X/Y)/ TOUCHPAD(BTN_TOOL_FINGER+ABS_X/Y)
     * 判定本轮不做 —— 依赖真实多设备 caps,本轮 event0 纯键盘不触发。 */
  }

  close(fd);

  /* 合成 KV 写 db(本轮只键盘类 property;ID_SEAT 对齐 Linux 永远 seat0) */
  char kv[512];
  int len = 0;
  len += snprintf(kv + len, sizeof(kv) - len, "ID_INPUT=%d\n", is_input);
  if (is_keyboard)
    len += snprintf(kv + len, sizeof(kv) - len, "ID_INPUT_KEYBOARD=1\n");
  if (is_key)
    len += snprintf(kv + len, sizeof(kv) - len, "ID_INPUT_KEY=1\n");
  len += snprintf(kv + len, sizeof(kv) - len, "ID_SEAT=seat0\n");

  return db_write_property(devnum, kv, (size_t)len);
}

/* udevd 收 add uevent 后入口(整合到现有 main 循环 netlink fd 分支) */
static void handle_uevent_add(const char *devname /* DEVPATH=<name> */,
                              const char *subsystem) {
  /* 1. device 补全:取 devnum(三边一致 = ino)作 db key */
  char devnode[64];
  snprintf(devnode, sizeof(devnode), "/dev/%s", devname);
  struct stat st;
  if (stat(devnode, &st) < 0)
    return;                               /* /dev 节点未就绪,跳过 */
  uint32_t devnum = (uint32_t)st.st_rdev; /* = ino */
  if (devnum == 0)
    return;

  /* 2. 仅 input 子系统跑 input_id(对齐 Linux input_id 只处理 input 设备) */
  if (strcmp(subsystem, "input") != 0)
    return;

  /* 3. 规则引擎:探 caps + 算 ID_INPUT_* + 写 db */
  input_id_compute(devnode, devnum);

  /* 4. monitor 转发归父文档 §4(device 补全查 db 取 property 塞 pipe KV);
   *    本方案只保证 db 写好,不碰 monitor 管道。 */
}

/* process_one_uevent:解析 \0 分隔 uevent payload 取 ACTION/DEVPATH/SUBSYSTEM,
 * 构造完整 KV(device 补全:标识 KV + 查 db property)广播给所有 client。
 * add → handle_uevent_add 写 db 后再补全转发;remove → 两阶段(§4.6,直接用
 * db 快照补全转发,不在此删 db——remove 时 db 可能已被设备清理路径删,
 * 现状无 remove 设备来源,本路径仅占位)。drain 与主循环共用。
 * 返回是否处理了事件(主循环用于单字符进度)。 */
static int process_one_uevent(const char *payload, int payload_len) {
  char action[16] = {0}, devname[64] = {0}, subsys[16] = {0};
  const char *pp = payload;
  int rem = payload_len;
  while (rem > 0 && *pp) {
    if (strncmp(pp, "ACTION=", 7) == 0)
      snprintf(action, sizeof(action), "%s", pp + 7);
    else if (strncmp(pp, "DEVPATH=", 8) == 0)
      snprintf(devname, sizeof(devname), "%s", pp + 8);
    else if (strncmp(pp, "SUBSYSTEM=", 10) == 0)
      snprintf(subsys, sizeof(subsys), "%s", pp + 10);
    int seg_len = (int)strlen(pp) + 1;
    pp += seg_len;
    rem -= seg_len;
  }
  if (!action[0] || !devname[0] || !subsys[0])
    return 0;

  /* add:先写 db(规则引擎),再补全转发。input_id 仅 input 子系统。 */
  if (strcmp(action, "add") == 0) {
    if (strcmp(subsys, "input") == 0)
      handle_uevent_add(devname, subsys);
  }

  /* device 补全(标识 KV + 查 db property)→ 广播给所有 client。
   * devtype 现状 uevent payload 不含(§3.1 仅 3 键),传 NULL → KV 不带 DEVTYPE。
   * monitor client 据此构造 udev_device。 */
  char kv[1024];
  int klen = device_complete_kv(action, devname, subsys, NULL, kv, sizeof(kv));
  if (klen > 0) {
    clients_broadcast(kv, klen);
    putchar(strcmp(action, "remove") == 0 ? 'r' : 'u');
  }
  return 1;
}

/* coldplug_trigger:对齐 Linux udevadm trigger——扫 /sys/class/input,对每个
 * 设备写 "add" 到 /sys/class/input/<sysname>/uevent 触发内核重广播(经 netlink
 * 走与热插拔同路径)。本轮只 input 子系统需 coldplug(terminal 唯一依赖)。
 * 参照 shim udev.c:534-558 同款 opendir/readdir 枚举。 */
static void coldplug_trigger(void) {
  DIR *dir = opendir("/sys/class/input");
  if (!dir)
    return;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] == '.')
      continue;
    char path[96];
    snprintf(path, sizeof(path), "/sys/class/input/%s/uevent", entry->d_name);
    int fd = open(path, O_WRONLY);
    if (fd < 0)
      continue;
    write(fd, "add", 3);
    close(fd);
  }
  closedir(dir);
}

/* coldplug_drain_settle:非阻塞排干 trigger 产生的 uevent。bind 后 udevd socket
 * 已在 nl_groups[0],trigger 的 write → 内核 nl_group_broadcast(0,...,-1) 不
 * exclude 任何 pid,skb 入 udevd 自己 recv_queue(上限 256)。用 MSG_DONTWAIT 同步
 * 处理完每个 add(handle_uevent_add 写 db),直到 EAGAIN,db 必已写。然后建
 * /run/udev/settled 标志供 init gate。本轮 trigger 产 1 个 uevent。 */
static void coldplug_drain_settle(int nl_fd) {
  for (;;) {
    char buf[4096];
    struct iovec iov = {.iov_base = buf, .iov_len = sizeof(buf)};
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    ssize_t len = recvmsg(nl_fd, &msg, MSG_DONTWAIT);
    if (len < 0) {
      if (errno == EINTR)
        continue;
      break; /* EAGAIN/ENOMSG:排干完成 */
    }
    if (len < (ssize_t)sizeof(struct nlmsghdr))
      continue;
    struct nlmsghdr *nh = (struct nlmsghdr *)buf;
    if (!NLMSG_OK(nh, len))
      continue;
    char *payload = (char *)NLMSG_DATA(nh);
    int payload_len = (int)(nh->nlmsg_len) - NLMSG_HDRLEN;
    process_one_uevent(payload, payload_len);
  }
  /* 建 settled 标志(init 轮询 F_OK gate spawn terminal)。O_CREAT|O_TRUNC,
   * 不写内容(存在即就绪)。失败不致命(init 超时仍 spawn)。 */
  int sfd = open("/run/udev/settled", O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (sfd >= 0)
    close(sfd);
}

int main(void) {
  int listen_fd = probe_listen_fd(UDEV_LISTEN_FD);
  if (listen_fd < 0)
    listen_fd = fallback_self_bind_listen();

  /* mkdir /run/udev/data(db 落点,init 只建到 /run/udev)。
   * 幂等(EEXIST 忽略)。 */
  mkdir("/run/udev/data", 0755);

  // Create netlink socket
  int nl_fd = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_KOBJECT_UEVENT);
  if (nl_fd < 0) {
    printf("udevd: socket(AF_NETLINK) failed: errno=%d\n", errno);
    return 1;
  }

  // Bind: subscribe to uevent group (bit 0 = group 1)
  struct sockaddr_nl addr;
  memset(&addr, 0, sizeof(addr));
  addr.nl_family = AF_NETLINK;
  addr.nl_pid = 0;    // auto-assign PID
  addr.nl_groups = 1; // subscribe to group 1 (UEVENT)
  if (bind(nl_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    printf("udevd: bind failed: errno=%d\n", errno);
    close(nl_fd);
    return 1;
  }

  // Create epoll fd
  int epfd = epoll_create1(0);
  if (epfd < 0) {
    printf("udevd: epoll_create1 failed: errno=%d\n", errno);
    close(nl_fd);
    return 1;
  }

  // Register netlink fd with epoll
  struct epoll_event ev;
  ev.events = EPOLLIN;
  ev.data.fd = nl_fd;
  if (epoll_ctl(epfd, EPOLL_CTL_ADD, nl_fd, &ev) < 0) {
    printf("udevd: epoll_ctl ADD failed: errno=%d\n", errno);
    close(epfd);
    close(nl_fd);
    return 1;
  }

  /* coldplug:trigger 写各 /sys/class/input/<sysname>/uevent 触发内核重广播,
   * drain 同步处理完写 db,然后建 /run/udev/settled。须在 nl_fd 入 epoll 之后
   * (trigger 产的 uevent 入 recv_queue 不丢)且 listen_fd 注册之前(先 settle
   * 再服务)。 */
  /* 先初始化 monitor client 表(全部空闲 pipe_wr=-1):coldplug drain 会经
   * process_one_uevent → clients_broadcast,若表未初始化(BSS pipe_wr==0)会被
   * <0 guard 误判为活跃槽向 fd 0(stdin)写垃圾。coldplug 启动广播本就无 client
   * 接收(真实 coldplug 转发在 accept_client 首连时重触发),故先 init 再
   * coldplug。 */
  clients_init();
  coldplug_trigger();
  coldplug_drain_settle(nl_fd);
  printf("udevd: coldplug trigger done\n");

  /* socket activation：将 init 传入的 listen fd 纳入 epoll（非阻塞）。
   * udevd 主体方案在此 accept 客户端并处理 udev 协议；本轮只保活 + accept。
   * listen_fd < 0（无 socket activation）时跳过，udevd 仍跑 netlink。 */
  if (listen_fd >= 0) {
    struct epoll_event lev;
    lev.events = EPOLLIN;
    lev.data.fd = listen_fd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, listen_fd, &lev);
    printf("udevd: socket activation on fd=%d\n", listen_fd);
  } else {
    printf("udevd: no socket activation fd, netlink-only\n");
  }

  printf("udevd: listening for uevents on fd=%d\n", nl_fd);

  // Event loop
  while (1) {
    struct epoll_event events[4];
    int n = epoll_wait(epfd, events, 4, -1);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      printf("udevd: epoll_wait error: errno=%d\n", errno);
      break;
    }

    for (int i = 0; i < n; i++) {
      if (events[i].data.fd == nl_fd) {
        // Read uevent from netlink socket
        char buf[4096];
        struct iovec iov;
        iov.iov_base = buf;
        iov.iov_len = sizeof(buf);
        struct msghdr msg;
        memset(&msg, 0, sizeof(msg));
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        ssize_t len = recvmsg(nl_fd, &msg, 0);
        if (len < 0) {
          printf("udevd: recvmsg error: errno=%d\n", errno);
          continue;
        }

        // Parse nlmsghdr
        if (len < (ssize_t)sizeof(struct nlmsghdr))
          continue;

        struct nlmsghdr *nh = (struct nlmsghdr *)buf;
        if (!NLMSG_OK(nh, len))
          continue;

        // Extract payload
        char *payload = (char *)NLMSG_DATA(nh);
        int payload_len = (int)(nh->nlmsg_len) - NLMSG_HDRLEN;

        // Print uevent info
        printf("udevd: uevent type=%d pid=%u len=%d: ", nh->nlmsg_type,
               nh->nlmsg_pid, payload_len);

        // Payload is \0-separated key-value pairs; print first segment
        // (the "action@devpath" line) then remaining pairs
        char *p = payload;
        int remaining = payload_len;
        while (remaining > 0 && *p) {
          printf("[%s] ", p);
          int seg_len = (int)strlen(p) + 1;
          p += seg_len;
          remaining -= seg_len;
        }
        printf("\n");
        /* 解析 KV + add 处理(drain 与主循环共用 process_one_uevent) */
        process_one_uevent(payload, payload_len);
      } else if (events[i].data.fd == listen_fd) {
        /* 新 client 连接:accept → 建 pipe → SCM_RIGHTS 回传 pipe rd fd
         * → 首个 client 补 coldplug(§4.4/§4.5)。 */
        accept_client(listen_fd);
      }
    }

    /* 兜底排空:本轮可能写满 wbuf(pipe EAGAIN),尝试再排一次。
     * 现状 pipe wr 未入 epoll EPOLLOUT,靠每次广播自排空;满则丢(Q2)。 */
    for (int i = 0; i < UDEV_MAX_CLIENTS; i++) {
      if (udev_clients[i].pipe_wr >= 0 && udev_clients[i].wlen > 0) {
        struct udev_client *c = &udev_clients[i];
        int w = write(c->pipe_wr, c->wbuf, (size_t)c->wlen);
        if (w > 0) {
          memmove(c->wbuf, c->wbuf + w, (size_t)(c->wlen - w));
          c->wlen -= w;
        }
      }
    }
  }

  close(epfd);
  close(nl_fd);
  return 0;
}
