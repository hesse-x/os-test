#include <errno.h>
#include <sys/ipc.h>
#include <time.h>
#include <unistd.h>

unsigned int sleep(unsigned seconds) {
  // Use sys_recv with timeout in milliseconds
  struct recv_msg msg;
  int r = recv(&msg, NULL, 0, seconds * 1000);
  if (r == -ETIMEDOUT)
    return 0; // normal timeout
  return 0;   // interrupted (ignore for now)
}

int usleep(unsigned usec) {
  unsigned ms = usec / 1000;
  if (ms == 0)
    ms = 1;
  struct recv_msg msg;
  recv(&msg, NULL, 0, ms);
  return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem) {
  if (!req)
    return -1;
  unsigned ms = (unsigned)(req->tv_sec * 1000 + req->tv_nsec / 1000000);
  if (ms == 0 && req->tv_nsec > 0)
    ms = 1;
  struct recv_msg msg;
  recv(&msg, NULL, 0, ms);
  if (rem) {
    rem->tv_sec = 0;
    rem->tv_nsec = 0;
  }
  return 0;
}
