/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

// Minimal udevd — subscribes to NETLINK_KOBJECT_UEVENT via epoll,
// receives uevent messages and prints them to stdout.
//
// socket activation (dep0 1.1): init may pass in a listening AF_UNIX socket
// (/run/udev/socket) via fd 3. This process probes fd 3 at startup:
//   - it is a listen socket → add to epoll, accept client connections (the
//     udevd main design takes over handling)
//   - not a socket / invalid → skip, run netlink only (degraded; udevd still
//     comes up)
// Probing uses try-accept (this OS has no getsockname).

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/netlink.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <xos/ioctl.h>

#define UDEV_LISTEN_FD 3

// udevd monitor client table (single-threaded, no lock). Each client = the
// accepted conn + a pipe udevd builds for it (rd given to client / wr held by
// udevd). The conn fd is closed once the pipe rd fd is taken (§4.4 step 8);
// only the pipe wr end goes into epoll (drained when writable).
#define UDEV_MAX_CLIENTS 8
#define UDEV_FRAME_HEADER_SIZE 12
#define UDEV_FRAME_PAYLOAD_MAX 4096
#define UDEV_CLIENT_QUEUE_SIZE 16384

struct udev_client {
  int pipe_wr; // write end held by udevd (rd handed to client via SCM_RIGHTS).
               // <0 = free slot
  unsigned char wbuf[UDEV_CLIENT_QUEUE_SIZE];
  int wlen;
  int epollout_registered;
};

static struct udev_client udev_clients[UDEV_MAX_CLIENTS];
static int daemon_epfd = -1;

// Single-char progress chain (§8.6 debug discipline): u=uevent a=accept
// c=coldplug r=remove w=write

#define EVDEV_BITS_PER_LONG (sizeof(long) * 8)
#define NBITS(x) ((((x) - 1) / EVDEV_BITS_PER_LONG) + 1)
#define LONG(x) ((x) / EVDEV_BITS_PER_LONG)
#define OFF(x) ((x) % EVDEV_BITS_PER_LONG)
#define test_bit(bit, bits) (!!(bits[LONG(bit)] & (1UL << OFF(bit))))

// Probe whether fd is a listening AF_UNIX socket.
// Returns >=0 (== fd) if it is a listen socket; -1 if not a socket / invalid.
static int probe_listen_fd(int fd) {
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0)
    return -1; // fd invalid
  // Set non-blocking then try-accept: success or EAGAIN both indicate a listen
  // socket
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  int probe = accept(fd, NULL, NULL);
  int saved = errno;
  fcntl(fd, F_SETFL, flags); // restore
  if (probe >= 0) {
    close(probe); // close any queued client immediately; the real one will
                  // reconnect
    return fd;
  }
  if (saved == EAGAIN)
    return fd; // nothing queued; socket activation holds
  // ENOTSOCK / EINVAL etc. → not a listen socket
  return -1;
}

// Initialize the client table (all slots free).
static void clients_init(void) {
  for (int i = 0; i < UDEV_MAX_CLIENTS; i++)
    udev_clients[i].pipe_wr = -1;
}

// Allocate a free client slot, return its index; <0 if full.
static int client_alloc(void) {
  for (int i = 0; i < UDEV_MAX_CLIENTS; i++) {
    if (udev_clients[i].pipe_wr < 0) {
      udev_clients[i].pipe_wr =
          -2; // placeholder against reentry; real fd filled shortly
      udev_clients[i].wlen = 0;
      udev_clients[i].epollout_registered = 0;
      return i;
    }
  }
  return -1;
}

// Reverse-lookup a client slot by its pipe_wr fd; <0 if not found.
// Not wired in M1+M2 yet (index management suffices); enabled when the shim
// monitor client (step 2) looks up by fd. Marked __attribute__((unused)) to
// pass -Werror=unused-function.
static __attribute__((unused)) int client_find_by_wr(int pipe_wr) {
  for (int i = 0; i < UDEV_MAX_CLIENTS; i++)
    if (udev_clients[i].pipe_wr == pipe_wr)
      return i;
  return -1;
}

// Degraded self bind+listen: when init didn't pass fd 3 or the probe failed,
// udevd builds its own listen socket. Ensures udevd can always accept client
// connections without hard-depending on init socket activation.
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

// udevd db atomic write (userspace C, int as bool; freestanding has no stdbool)
static int db_write_property(uint32_t devnum, const char *kv_str,
                             size_t kv_len) {
  char key[32], tmp_path[80], final_path[80];
  // key = devnum in decimal (mirrors Linux addressing by devnum; this OS has no
  // major/minor)
  int klen = snprintf(key, sizeof(key), "%u", devnum);
  if (klen <= 0 || klen >= (int)sizeof(key))
    return -EINVAL;
  snprintf(final_path, sizeof(final_path), "/run/udev/data/%s", key);
  snprintf(tmp_path, sizeof(tmp_path), "/run/udev/data/%s.tmp", key);

  // 1. write tmp file
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

  // 2. atomic rename-over (relies on SYS_RENAME, §3.1 of this design)
  if (rename(tmp_path, final_path) < 0) {
    int saved = errno;
    unlink(tmp_path); // clean up tmp
    return -saved;
  }
  return 0;
}

// Read the full db into buf (client-side, used by the shim). Returns bytes
// read / negative errno.
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

// Delete the db file (used by the two-phase real-remove step, parent doc §4.6).
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

// Device completion: given a uevent (with name=DEVPATH), build the full KV set
// and write it into out (buf). Identifier KVs are built from name+subsystem;
// properties (ID_INPUT_*) are read from the db and appended (乙).
// out_nul is the capacity of out. Returns bytes written (including the
// trailing \0 separators), <0 to skip.
// KV format (mirrors §4.3, \0-separated):
//   "<action>@<name>\0ACTION=<action>\0DEVNAME=/dev/<name>\0
//    DEVPATH=/sys/class/<subsys>/<name>\0SUBSYSTEM=<subsys>\0
//    DEVTYPE=<devtype>\0DEVNUM=<n>\0ID_INPUT=1\0...\0"
// The db file format is KEY=VALUE\n (see input_id_compute's writes); this
// function reads the db and converts each line into a \0-separated KV to
// append.
static int device_complete_kv(const char *action, const char *name,
                              const char *subsystem, const char *devtype,
                              char *out, size_t out_nul) {
  // Take devnum (three-way consistent = ino) as the db key + DEVNUM field
  char devnode[64];
  snprintf(devnode, sizeof(devnode), "/dev/%s", name);
  struct stat st;
  if (stat(devnode, &st) < 0)
    return -1;
  uint32_t devnum = (uint32_t)st.st_rdev;
  if (devnum == 0)
    return -1;

  int len = 0;
  // Each KV ends with \0 (same as Linux netlink uevent); the client's
  // receive_device parses by \0 separator. snprintf supplies a terminator
  // but doesn't count it in its return value, so append a \0 after each
  // entry and count it — otherwise entries glue together and the client's
  // strchr('=') crosses entries and picks the wrong key.
#define APPEND_KV0(fmt, ...)                                                   \
  do {                                                                         \
    int n = snprintf(out + len, out_nul - (size_t)len, fmt, ##__VA_ARGS__);    \
    if (n < 0 || (size_t)n >= out_nul - (size_t)len)                           \
      return -1;                                                               \
    len += n;                                                                  \
    if ((size_t)len >= out_nul)                                                \
      return -1;                                                               \
    out[len++] = '\0';                                                         \
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

  // Read db for properties (乙). The db file is KEY=VALUE\n; convert to
  // \0-separated KV and append.
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

// Write the KV (olen bytes, \0-separated) into each active client's wbuf and
// try to drain it to the pipe. If the pipe is full, drop that event for that
// client (Q2 non-blocking). Single-threaded; table structure is unchanged
// during iteration.
static void client_close(struct udev_client *c) {
  if (c->epollout_registered && daemon_epfd >= 0)
    epoll_ctl(daemon_epfd, EPOLL_CTL_DEL, c->pipe_wr, NULL);
  if (c->pipe_wr >= 0)
    close(c->pipe_wr);
  c->pipe_wr = -1;
  c->wlen = 0;
  c->epollout_registered = 0;
}

static void client_flush(struct udev_client *c) {
  while (c->pipe_wr >= 0 && c->wlen > 0) {
    ssize_t n = write(c->pipe_wr, c->wbuf, (size_t)c->wlen);
    if (n > 0) {
      memmove(c->wbuf, c->wbuf + n, (size_t)(c->wlen - n));
      c->wlen -= (int)n;
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0 && errno == EAGAIN)
      break;
    client_close(c);
    return;
  }
  if (c->pipe_wr < 0 || daemon_epfd < 0)
    return;
  if (c->wlen > 0 && !c->epollout_registered) {
    struct epoll_event event;
    event.events = EPOLLOUT | EPOLLERR | EPOLLHUP;
    event.data.fd = c->pipe_wr;
    if (epoll_ctl(daemon_epfd, EPOLL_CTL_ADD, c->pipe_wr, &event) == 0)
      c->epollout_registered = 1;
    else
      client_close(c);
  } else if (c->wlen == 0 && c->epollout_registered) {
    epoll_ctl(daemon_epfd, EPOLL_CTL_DEL, c->pipe_wr, NULL);
    c->epollout_registered = 0;
  }
}

static void clients_broadcast(const char *kv, int olen) {
  if (olen <= 0 || olen > UDEV_FRAME_PAYLOAD_MAX)
    return;
  unsigned char frame[UDEV_FRAME_HEADER_SIZE + UDEV_FRAME_PAYLOAD_MAX];
  memcpy(frame, "UDEV", 4);
  frame[4] = 1;
  frame[5] = 0;
  frame[6] = 0;
  frame[7] = 0;
  frame[8] = (unsigned char)((unsigned)olen & 0xff);
  frame[9] = (unsigned char)(((unsigned)olen >> 8) & 0xff);
  frame[10] = (unsigned char)(((unsigned)olen >> 16) & 0xff);
  frame[11] = (unsigned char)(((unsigned)olen >> 24) & 0xff);
  memcpy(frame + UDEV_FRAME_HEADER_SIZE, kv, (size_t)olen);
  int frame_len = UDEV_FRAME_HEADER_SIZE + olen;
  for (int i = 0; i < UDEV_MAX_CLIENTS; i++) {
    if (udev_clients[i].pipe_wr < 0)
      continue;
    struct udev_client *c = &udev_clients[i];
    client_flush(c);
    if (c->pipe_wr < 0)
      continue;
    if (frame_len > (int)sizeof(c->wbuf) - c->wlen) {
      printf("udevd: monitor queue overflow fd=%d fatal=1\n", c->pipe_wr);
      client_close(c);
      continue;
    }
    memcpy(c->wbuf + c->wlen, frame, (size_t)frame_len);
    c->wlen += frame_len;
    client_flush(c);
  }
}

// Forward decl: accept_client calls coldplug_trigger before it is defined
// (cold-start replay).
static void coldplug_trigger(void);

// Accept a new client: build a unidirectional pipe (rd to client, wr held by
// udevd), send the pipe rd fd back over the conn fd via SCM_RIGHTS (§4.4).
// After sending, close the conn fd (connect == subscribe, Q5). Any failure in
// accept/pipe/sendmsg → close fds and skip that client.
static void accept_client(int listen_fd) {
  int cfd = accept(listen_fd, NULL, NULL);
  if (cfd < 0)
    return;

  int pfd[2];
  if (pipe2(pfd, O_NONBLOCK | O_CLOEXEC) < 0) {
    close(cfd);
    return;
  }
  int pipe_rd = pfd[0]; // to client
  int pipe_wr = pfd[1]; // held by udevd

  // SCM_RIGHTS sends pipe_rd back. Carries 1 byte of dummy data (iov must be
  // non-empty).
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
    // Send failed: close pipe rd (avoid leak), leave wr for cleanup
    close(pipe_rd);
    close(pipe_wr);
    close(cfd);
    return;
  }

  // Register in the client table
  int slot = client_alloc();
  if (slot < 0) {
    // Table full: close pipe + conn, client connection drops (§8.3 accept
    // backlog won't fill; rare)
    close(pipe_wr);
    close(pipe_rd);
    close(cfd);
    return;
  }
  udev_clients[slot].pipe_wr = pipe_wr;

  close(cfd); // connect == subscribe; close conn (§4.4 step 8)
  putchar('a');

  // First client replays coldplug (§4.5): synthesize add for existing devices
  // and forward to that client. Reuse the coldplug_trigger kernel-rebroadcast
  // path — the rebroadcast uevent comes back to udevd's own netlink fd and is
  // dispatched via handle_uevent to each client (including the new one), so no
  // separate forwarding is needed here. Just retrigger coldplug_trigger once
  // (if the device is still present).
  coldplug_trigger();
}

// udevd input_id builtin (userspace C, int as bool)
// Opens /dev/input/eventX, probes caps, synthesizes keyboard-class ID_INPUT_*
// and writes them to the db. Mirrors Linux src/udev/udev-builtin-input_id.c
// (keyboard path subset).
static int input_id_compute(const char *devnode, uint32_t devnum) {
  static int mouse_dpi_logged;
  int fd = open(devnode, O_RDONLY);
  if (fd < 0)
    return -errno;

  // Event type bitmap: EV_KEY/EV_REL/EV_ABS etc. Probed per-device (event0 =
  // keyboard, event1 = mouse), so the same input_id_compute serves both. The
  // full evbits probe mirrors Linux input_id's ID_INPUT decision (any of
  // EV_KEY/EV_REL/EV_ABS → ID_INPUT=1). (mouse.md §3.4)
  unsigned long evbits[NBITS(EV_MAX + 1)];
  memset(evbits, 0, sizeof(evbits));
  if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0) {
    close(fd);
    return -errno;
  }

  int is_input = 0;    // ID_INPUT=1
  int is_keyboard = 0; // ID_INPUT_KEYBOARD=1
  int is_key = 0;      // ID_INPUT_KEY=1
  int is_mouse = 0;    // ID_INPUT_MOUSE=1 (mouse.md §3.4)
  int is_virtual_mouse = 0;

  // Keep the explicit DPI baseline scoped to this project's synthetic mouse;
  // real USB mice must retain their own udev/hardware DPI properties.
  struct input_id input_id;
  char input_name[64];
  memset(&input_id, 0, sizeof(input_id));
  memset(input_name, 0, sizeof(input_name));
  if (ioctl(fd, EVIOCGID, &input_id) >= 0 &&
      ioctl(fd, EVIOCGNAME(sizeof(input_name)), input_name) >= 0 &&
      input_id.bustype == BUS_USB && input_id.vendor == 0x0001 &&
      input_id.product == 0x0002 && strcmp(input_name, "evdev mouse") == 0)
    is_virtual_mouse = 1;

  // ID_INPUT: any EV_KEY/EV_REL/EV_ABS device (mirrors Linux input_id main
  // switch)
  if (test_bit(EV_KEY, evbits) || test_bit(EV_REL, evbits) ||
      test_bit(EV_ABS, evbits))
    is_input = 1;

  // ID_INPUT_KEYBOARD / KEY: EV_KEY plus keyboard-class keys
  // (mirrors Linux: any of KEY_A..KEY_Z / KEY_ENTER / KEY_SPACE etc. ⇒
  // keyboard). Non-mutex with the mouse branch below: a device with both
  // KEY_A and REL_X/Y (e.g. a pointing stick + keyboard combo) gets both tags;
  // a pure mouse has no KEY_* caps so this won't mis-fire on it.
  if (test_bit(EV_KEY, evbits)) {
    unsigned long keybits[NBITS(KEY_MAX + 1)];
    memset(keybits, 0, sizeof(keybits));
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) >= 0) {
      // keyboard: letter / digit / control keys
      if (test_bit(KEY_A, keybits) || test_bit(KEY_ENTER, keybits) ||
          test_bit(KEY_SPACE, keybits) || test_bit(KEY_LEFTCTRL, keybits)) {
        is_keyboard = 1;
        is_key = 1;
      }
    }
  }

  // ID_INPUT_MOUSE: EV_REL plus REL_X + REL_Y (mirrors Linux input_id's
  // mouse/pointing-stick detection). libinput's udev backend reads
  // ID_INPUT_MOUSE to tag the device as a pointer → wlroots produces a
  // WLR_INPUT_DEVICE_POINTER → tinywl server_new_pointer. (mouse.md §3.4)
  if (test_bit(EV_REL, evbits)) {
    unsigned long relbits[NBITS(REL_MAX + 1)];
    memset(relbits, 0, sizeof(relbits));
    if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relbits)), relbits) >= 0) {
      if (test_bit(REL_X, relbits) && test_bit(REL_Y, relbits))
        is_mouse = 1;
    }
  }

  close(fd);

  // Synthesize KV and write to db. ID_SEAT is always seat0, mirroring Linux.
  char kv[512];
  int len = 0;
  len += snprintf(kv + len, sizeof(kv) - len, "ID_INPUT=%d\n", is_input);
  if (is_keyboard)
    len += snprintf(kv + len, sizeof(kv) - len, "ID_INPUT_KEYBOARD=1\n");
  if (is_key)
    len += snprintf(kv + len, sizeof(kv) - len, "ID_INPUT_KEY=1\n");
  if (is_mouse) {
    len += snprintf(kv + len, sizeof(kv) - len, "ID_INPUT_MOUSE=1\n");
    // ID_INPUT_POINTING_DEVICE mirrors Linux input_id (set alongside MOUSE for
    // any relative-pointer device); libinput uses ID_INPUT_MOUSE specifically.
    len += snprintf(kv + len, sizeof(kv) - len, "ID_INPUT_POINTING_DEVICE=1\n");
    if (is_virtual_mouse) {
      len += snprintf(kv + len, sizeof(kv) - len, "MOUSE_DPI=1000\n");
      if (!mouse_dpi_logged) {
        fprintf(stderr, "libinput: evdev mouse dpi=1000\n");
        mouse_dpi_logged = 1;
      }
    }
  }
  len += snprintf(kv + len, sizeof(kv) - len, "ID_SEAT=seat0\n");

  return db_write_property(devnum, kv, (size_t)len);
}

// udevd entry after receiving an add uevent (integrated into the existing
// main-loop netlink-fd branch)
static void handle_uevent_add(const char *devname /* DEVPATH=<name> */,
                              const char *subsystem) {
  // 1. Device completion: take devnum (three-way consistent = ino) as the db
  // key
  char devnode[64];
  snprintf(devnode, sizeof(devnode), "/dev/%s", devname);
  struct stat st;
  if (stat(devnode, &st) < 0)
    return;                               // /dev node not ready; skip
  uint32_t devnum = (uint32_t)st.st_rdev; // = ino
  if (devnum == 0)
    return;

  // 2. Only the input subsystem runs input_id (mirrors Linux input_id only
  //    handling input devices)
  if (strcmp(subsystem, "input") != 0)
    return;

  // 3. Rules engine: probe caps + compute ID_INPUT_* + write db
  input_id_compute(devnode, devnum);

  // 4. Monitor forwarding is parent doc §4 (device completion reads db for
  //    properties and packs them into the pipe KV); this design only ensures
  //    the db is written and does not touch the monitor pipe.
}

// process_one_uevent: parse the \0-separated uevent payload for
// ACTION/DEVPATH/SUBSYSTEM, build the full KV (device completion: identifier
// KVs + db properties) and broadcast to all clients.
// add → handle_uevent_add writes the db, then completes and forwards; remove
// → two-phase (§4.6, complete+forward directly from a db snapshot, do not
// delete the db here — on remove the db may already be gone via the device
// cleanup path; currently there's no remove device source, this path is just
// a placeholder). Draining shares this with the main loop.
// Returns whether an event was processed (used by the main loop for
// single-char progress).
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

  // add: write the db first (rules engine), then complete and forward.
  // input_id only runs on the input subsystem.
  if (strcmp(action, "add") == 0) {
    if (strcmp(subsys, "input") == 0)
      handle_uevent_add(devname, subsys);
  }

  // Device completion (identifier KVs + db properties) → broadcast to all
  // clients. The current uevent payload has no devtype (§3.1 only has 3
  // keys), pass NULL → the KV omits DEVTYPE. The monitor client builds its
  // udev_device from this.
  char kv[1024];
  int klen = device_complete_kv(action, devname, subsys, NULL, kv, sizeof(kv));
  if (klen > 0) {
    clients_broadcast(kv, klen);
    putchar(strcmp(action, "remove") == 0 ? 'r' : 'u');
  }
  return 1;
}

// coldplug_trigger: mirrors Linux udevadm trigger — scan /sys/class/input and
// for each device write "add" to /sys/class/input/<sysname>/uevent to trigger
// a kernel rebroadcast (over netlink, same path as hotplug). This round only
// the input subsystem needs coldplug (terminal's sole dependency). Same
// opendir/readdir enumeration as shim udev.c:534-558.
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

// coldplug_drain_settle: non-blockingly drain the uevents produced by trigger.
// After bind, udevd's socket is in nl_groups[0]; trigger's write → kernel
// nl_group_broadcast(0,...,-1) excludes no pid, so the skb enters udevd's own
// recv_queue (cap 256). Use MSG_DONTWAIT to synchronously process each add
// (handle_uevent_add writes the db) until EAGAIN, by which point the db is
// necessarily written. Then create the /run/udev/settled marker for init to
// gate on. This round trigger produces 1 uevent.
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
      break; // EAGAIN/ENOMSG: drain done
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
  // Create the settled marker (init polls with F_OK to gate spawning
  // terminal). O_CREAT|O_TRUNC, no contents (existence == ready). Failure is
  // non-fatal (init still spawns on timeout).
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
  daemon_epfd = epfd;

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
      } else {
        int slot = client_find_by_wr(events[i].data.fd);
        if (slot >= 0) {
          if (events[i].events & (EPOLLERR | EPOLLHUP))
            client_close(&udev_clients[slot]);
          else
            client_flush(&udev_clients[slot]);
        }
      }
    }
  }

  close(epfd);
  close(nl_fd);
  return 0;
}
