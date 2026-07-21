/*
 * Copyright (c) 2026 hesse
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef COMMON_ERRNO_H
#define COMMON_ERRNO_H

// Linux x86-64 errno values (aligned to asm-generic/errno-base.h + errno.h).
// Former misaligned values (ENOMEM=3, EINVAL=4, etc.) are replaced wholesale.
// Collision items (EMFILE old=13 vs EACCES=13; ENFILE old=23 vs
// ECONNREFUSED=23) are resolved: EACCES=13, EMFILE=24, ENFILE=23,
// ECONNREFUSED=111.

#define EPERM 1          /* Operation not permitted */
#define ENOENT 2         /* No such file or directory */
#define ESRCH 3          /* No such process */
#define EINTR 4          /* Interrupted system call */
#define EIO 5            /* I/O error */
#define ENXIO 6          /* No such device or address */
#define E2BIG 7          /* Argument list too long */
#define ENOEXEC 8        /* Exec format error */
#define EBADF 9          /* Bad file number */
#define ECHILD 10        /* No child processes */
#define EAGAIN 11        /* Try again */
#define ENOMEM 12        /* Out of memory */
#define EACCES 13        /* Permission denied */
#define EFAULT 14        /* Bad address */
#define ENOTBLK 15       /* Block device required */
#define EBUSY 16         /* Device or resource busy */
#define EEXIST 17        /* File exists */
#define EXDEV 18         /* Cross-device link */
#define ENODEV 19        /* No such device */
#define ENOTDIR 20       /* Not a directory */
#define EISDIR 21        /* Is a directory */
#define EINVAL 22        /* Invalid argument */
#define ENFILE 23        /* File table overflow */
#define EMFILE 24        /* Too many open files */
#define ENOTTY 25        /* Not a typewriter */
#define ETXTBSY 26       /* Text file busy */
#define EFBIG 27         /* File too large */
#define ENOSPC 28        /* No space left on device */
#define ESPIPE 29        /* Illegal seek */
#define EROFS 30         /* Read-only file system */
#define EMLINK 31        /* Too many links */
#define EPIPE 32         /* Broken pipe */
#define EDOM 33          /* Math argument out of domain */
#define ERANGE 34        /* Math result not representable */
#define EDEADLK 35       /* Resource deadlock would occur */
#define ENAMETOOLONG 36  /* File name too long */
#define ENOLCK 37        /* No record locks available */
#define ENOSYS 38        /* Function not implemented */
#define ENOTEMPTY 39     /* Directory not empty */
#define ELOOP 40         /* Too many symbolic links encountered */
#define ENOMSG 42        /* No message of desired type */
#define EIDRM 43         /* Identifier removed */
#define ECHRNG 44        /* Channel number out of range */
#define EL2NSYNC 45      /* Level 2 not synchronized */
#define EL3HLT 46        /* Level 3 halted */
#define EL3RST 47        /* Level 3 reset */
#define ELNRNG 48        /* Link number out of range */
#define EUNATCH 49       /* Protocol driver not attached */
#define ENOCSI 50        /* No CSI structure available */
#define EL2HLT 51        /* Level 2 halted */
#define EBADE 52         /* Invalid exchange */
#define EBADR 53         /* Invalid request descriptor */
#define EXFULL 54        /* Exchange full */
#define ENOANO 55        /* No anode */
#define EBADRQC 56       /* Invalid request code */
#define EBADSLT 57       /* Invalid slot */
#define EDEADLOCK 58     /* EDEADLK (same value on x86-64) */
#define EBFONT 59        /* Bad font file format */
#define ENOSTR 60        /* Device not a stream */
#define ENODATA 61       /* No data available */
#define ETIME 62         /* Timer expired */
#define ENOSR 63         /* Out of streams resources */
#define ENONET 64        /* Machine is not on the network */
#define ENOPKG 65        /* Package not installed */
#define EREMOTE 66       /* Object is remote */
#define ENOLINK 67       /* Link has been severed */
#define EADV 68          /* Advertise error */
#define ESRMNT 69        /* Srmount error */
#define ECOMM 70         /* Communication error on send */
#define EPROTO 71        /* Protocol error */
#define EMULTIHOP 72     /* Multihop attempted */
#define EDOTDOT 73       /* RFS specific error */
#define EBADMSG 74       /* Not a data message */
#define EOVERFLOW 75     /* Value too large for defined data type */
#define ENOTUNIQ 76      /* Name not unique on network */
#define EBADFD 77        /* File descriptor in bad state */
#define EREMCHG 78       /* Remote address changed */
#define ELIBACC 79       /* Can not access a needed shared library */
#define ELIBBAD 80       /* Accessing a corrupted shared library */
#define ELIBSCN 81       /* .lib section in a.out corrupted */
#define ELIBMAX 82       /* Attempting to link in too many shared libraries */
#define ELIBEXEC 83      /* Cannot exec a shared library directly */
#define EILSEQ 84        /* Illegal byte sequence */
#define ERESTART 85      /* Interrupted system call should be restarted */
#define ESTRPIPE 86      /* Streams pipe error */
#define ENOTCONN 87      /* Transport endpoint is not connected */
#define ESHUTDOWN 88     /* Cannot send after transport endpoint shutdown */
#define ETOOMANYREFS 89  /* Too many references: cannot splice */
#define ETIMEDOUT 90     /* Connection timed out */
#define ECONNREFUSED 91  /* Connection refused */
#define EHOSTDOWN 92     /* Host is down */
#define EHOSTUNREACH 93  /* No route to host */
#define EALREADY 94      /* Operation already in progress */
#define EINPROGRESS 95   /* Operation now in progress */
#define ESTALE 96        /* Stale NFS file handle */
#define EUCLEAN 97       /* Structure needs cleaning */
#define ENOTNAM 98       /* Not a XENIX named type file */
#define ENAVAIL 99       /* No XENIX semaphores available */
#define EISNAM 100       /* Is a named type file */
#define EREMOTEIO 101    /* Remote I/O error */
#define EDQUOT 102       /* Quota exceeded */
#define ENOMEDIUM 103    /* No medium found */
#define EMEDIUMTYPE 104  /* Wrong medium type */
#define ECANCELED 105    /* Operation Canceled */
#define ENOKEY 106       /* Required key not available */
#define EKEYEXPIRED 107  /* Key has expired */
#define EKEYREVOKED 108  /* Key has been revoked */
#define EKEYREJECTED 109 /* Key was rejected by service */
#define EOWNERDEAD 110   /* Owner died */
#define ENOTRECOVERABLE 111 /* State not recoverable */
#define ERFKILL 112         /* Operation not possible due to RF-kill */
#define EHWPOISON 113       /* Memory page has hardware error */

// Socket/networking errnos (Linux x86-64 values)
#define ESOCKTNOSUPPORT                                                        \
  94 /* Socket type not supported (dup of EALREADY=94, Linux has this at 94)   \
      */
// Note: ESOCKTNOSUPPORT shares value 94 with EALREADY in Linux asm-generic.
// In practice glibc assigns distinct values but kernel uses asm-generic.
// For this OS, we keep Linux asm-generic values as-is.
#define EOPNOTSUPP 95      /* Operation not supported on transport endpoint */
#define ENOTSUP EOPNOTSUPP /* ENOTSUP same as EOPNOTSUPP (Linux convention) */
#define EADDRINUSE 98      /* Address already in use */
#define EADDRNOTAVAIL 99   /* Cannot assign requested address */
#define EAFNOSUPPORT 97    /* Address family not supported by protocol */
#define EPROTONOSUPPORT 93 /* Protocol not supported */
#define EMSGSIZE 90        /* Message too long */
#define ENOTSOCK 88        /* Socket operation on non-socket */
#define ECONNRESET 104     /* Connection reset by peer */
#define EWOULDBLOCK EAGAIN /* Same as EAGAIN (Linux convention) */
#define EDESTADDRREQ 89    /* Destination address required */
#define EPROTOTYPE 91      /* Protocol wrong type for socket */
#define ENOPROTOOPT 92     /* Protocol not available */

#endif // COMMON_ERRNO_H
