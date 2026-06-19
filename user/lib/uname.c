#include <sys/utsname.h>
#include <string.h>

int uname(struct utsname *buf) {
    if (!buf) return -1;
    strcpy(buf->sysname, "Xos");
    strcpy(buf->nodename, "(none)");
    strcpy(buf->release, "0.1");
    strcpy(buf->version, __DATE__);
    strcpy(buf->machine, "x86_64");
    return 0;
}
