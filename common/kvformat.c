#include "common/kvformat.h"

/* Helper: emit unsigned integer in given base */
static int kvfmt_uint(void (*putc)(char c, void *arg), void *arg,
                      unsigned long val, int base, int uppercase,
                      int width, char pad, int left_align) {
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[20];
    int pos = 0;

    if (val == 0) {
        buf[pos++] = '0';
    } else {
        while (val) {
            buf[pos++] = digits[val % base];
            val /= base;
        }
    }

    int ndigits = pos;

    if (left_align) {
        while (--pos >= 0) putc(buf[pos], arg);
        for (int i = ndigits; i < width; i++) putc(' ', arg);
    } else {
        while (pos < width) buf[pos++] = pad;
        while (--pos >= 0) putc(buf[pos], arg);
    }
    return ndigits > width ? ndigits : width;
}

/* Helper: emit signed integer — correctly handles space-padded negatives */
static int kvfmt_int(void (*putc)(char c, void *arg), void *arg,
                     long val, int width, char pad, int left_align) {
    unsigned long uval;
    int neg = 0;

    if (val < 0) {
        neg = 1;
        uval = -(unsigned long)val;     /* well-defined for all negative long */
    } else {
        uval = (unsigned long)val;
    }

    if (!neg)
        return kvfmt_uint(putc, arg, uval, 10, 0, width, pad, left_align);

    /* Count decimal digits of |val| */
    unsigned long tmp = uval;
    int ndigits = 1;
    while (tmp >= 10) { ndigits++; tmp /= 10; }

    int total  = ndigits + 1;          /* digits + '-' sign */
    int pcount = width > total ? width - total : 0;

    if (left_align) {
        /* minus, digits, then spaces on the right */
        putc('-', arg);
        int r = 1 + kvfmt_uint(putc, arg, uval, 10, 0, 0, ' ', 0);
        for (int i = 0; i < pcount; i++) { putc(' ', arg); r++; }
        return r;
    } else if (pad == '0') {
        /* zero-fill: minus, then zeros, then digits */
        putc('-', arg);
        return 1 + kvfmt_uint(putc, arg, uval, 10, 0,
                               ndigits + pcount, '0', 0);
    } else {
        /* space-pad: spaces on the left, then minus, then digits */
        for (int i = 0; i < pcount; i++) putc(' ', arg);
        putc('-', arg);
        return pcount + 1 + kvfmt_uint(putc, arg, uval, 10, 0, 0, ' ', 0);
    }
}

int kvformat(void (*putc)(char c, void *arg), void *arg,
             const char *fmt, va_list ap) {
    int count = 0;

    while (*fmt) {
        if (*fmt != '%') {
            putc(*fmt++, arg);
            count++;
            continue;
        }

        fmt++; /* skip '%' */

        /* --- flags --- */
        int left_align = 0;
        if (*fmt == '-') { left_align = 1; fmt++; }

        /* --- width and pad character --- */
        int width = 0;
        char pad = ' ';
        if (*fmt == '0' && !left_align) {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* --- length modifier --- */
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; }

        /* --- specifier --- */
        switch (*fmt) {
        case '%':
            putc('%', arg);
            count++;
            break;

        case 'c': {
            char c = (char)va_arg(ap, int);
            putc(c, arg);
            count++;
            break;
        }

        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int slen = 0;
            while (s[slen]) slen++;
            if (left_align) {
                for (int i = 0; i < slen; i++) putc(s[i], arg);
                for (int i = slen; i < width; i++) putc(' ', arg);
            } else {
                for (int i = slen; i < width; i++) putc(' ', arg);
                for (int i = 0; i < slen; i++) putc(s[i], arg);
            }
            count += slen > width ? slen : width;
            break;
        }

        case 'd': {
            long val;
            if (is_long)
                val = va_arg(ap, long);
            else
                val = (long)va_arg(ap, int);
            count += kvfmt_int(putc, arg, val, width, pad, left_align);
            break;
        }

        case 'u': {
            unsigned long val;
            if (is_long)
                val = va_arg(ap, unsigned long);
            else
                val = (unsigned long)va_arg(ap, unsigned int);
            count += kvfmt_uint(putc, arg, val, 10, 0, width, pad, left_align);
            break;
        }

        case 'o': {
            unsigned long val;
            if (is_long)
                val = va_arg(ap, unsigned long);
            else
                val = (unsigned long)va_arg(ap, unsigned int);
            count += kvfmt_uint(putc, arg, val, 8, 0, width, pad, left_align);
            break;
        }

        case 'x':
        case 'X': {
            int upper = (*fmt == 'X');
            unsigned long val;
            if (is_long)
                val = va_arg(ap, unsigned long);
            else
                val = (unsigned long)va_arg(ap, unsigned int);
            count += kvfmt_uint(putc, arg, val, 16, upper, width, pad, left_align);
            break;
        }

        case 'p': {
            unsigned long val = (unsigned long)va_arg(ap, void *);
            putc('0', arg);
            putc('x', arg);
            count += 2 + kvfmt_uint(putc, arg, val, 16, 0, 0, '0', 0);
            break;
        }

        default:
            /* unknown specifier: output % + char as-is */
            putc('%', arg);
            count++;
            if (is_long) { putc('l', arg); count++; }
            putc(*fmt, arg);
            count++;
            break;
        }

        fmt++;
    }

    return count;
}
