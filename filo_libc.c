/* Host hooks for a libc host: number formatting that matches the Go engine's
   FormatFloat(x, 'g', -1, 64) and parsing that matches its ParseFloat. A
   freestanding host (the msh wasm build, a microcontroller without stdio)
   supplies its own or leaves them NULL. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "filo_libc.h"

/* sci, as %e writes it, one unit of its last digit away from zero (up) or
   toward it, with the same number of digits: 9.99e+05 up is 1.00e+06, 1.00e+06
   down is 9.99e+05. */
static void other_side(char *sci, size_t cap, bool up) {
    char *e = strchr(sci, 'e');
    int exp = (int)strtol(e + 1, NULL, 10);
    char *first = sci[0] == '-' ? sci + 1 : sci;
    char *d = e;
    while (d > first) {
        d--;
        if (*d == '.') {
            continue;
        }
        if (*d != (up ? '9' : '0')) {
            *d = (char)(*d + (up ? 1 : -1));
            break;
        }
        *d = up ? '0' : '9';
        if (d == first) {
            *d = up ? '1' : '9'; /* carried past the first digit */
            exp += up ? 1 : -1;
        }
    }
    if (*first == '0') {
        *first = '9'; /* 1.00 down: 0.99 is 9.9 one power lower */
        exp--;
    }
    (void)snprintf(e, cap - (size_t)(e - sci), "e%d", exp);
}

/* The shortest digit string that round-trips, then Go's placement rule for
   FormatFloat(x, 'g', -1, 64): plain decimal when the exponent is in
   [-4, 6), else d.ddde±XX — so 999999 prints as is and 1000000 as 1e+06. */
size_t filo_libc_num_to_str(void *user, double x, char *dst, size_t cap) {
    (void)user;
    if (isnan(x)) {
        return (size_t)snprintf(dst, cap, "NaN");
    }
    if (isinf(x)) {
        return (size_t)snprintf(dst, cap, x > 0 ? "+Inf" : "-Inf");
    }
    if (x == 0) {
        return (size_t)snprintf(dst, cap, signbit(x) ? "-0" : "0");
    }
    char sci[40];
    int prec = 0;
    for (prec = 0; prec < 17; prec++) {
        (void)snprintf(sci, sizeof(sci), "%.*e", prec, x);
        double back = strtod(sci, NULL);
        if (back == x) {
            break;
        }
        /* at a power of two the gap above x is twice the one below, so the
           digits on the far side may read back when the nearest do not:
           2^-1007 is 7.291122019556398e-304, not ...397 nor ...3975 */
        other_side(sci, sizeof(sci), fabs(back) < fabs(x));
        if (strtod(sci, NULL) == x) {
            break;
        }
    }
    /* sci is like -1.2345e+05: split into sign, digits and exponent */
    const char *p = sci;
    bool neg = false;
    if (*p == '-') {
        neg = true;
        p++;
    }
    char digits[24];
    size_t nd = 0;
    while (*p != 'e' && *p != '\0') {
        if (*p != '.') {
            digits[nd] = *p;
            nd++;
        }
        p++;
    }
    digits[nd] = '\0';
    int exp = (int)strtol(p + 1, NULL, 10); /* exponent of the first digit */
    char out[48];
    size_t n = 0;
    if (neg) {
        out[n] = '-';
        n++;
    }
    if (exp < -4 || exp >= 6) {
        out[n] = digits[0];
        n++;
        if (nd > 1) {
            out[n] = '.';
            n++;
            memcpy(out + n, digits + 1, nd - 1);
            n += nd - 1;
        }
        n += (size_t)snprintf(out + n, sizeof(out) - n, "e%c%02d", exp < 0 ? '-' : '+',
                              exp < 0 ? -exp : exp);
    } else if (exp < 0) {
        out[n] = '0';
        out[n + 1] = '.';
        n += 2;
        for (int i = 0; i < -exp - 1; i++) {
            out[n] = '0';
            n++;
        }
        memcpy(out + n, digits, nd);
        n += nd;
    } else {
        size_t intlen = (size_t)exp + 1;
        for (size_t i = 0; i < intlen; i++) {
            out[n] = i < nd ? digits[i] : '0';
            n++;
        }
        if (nd > intlen) {
            out[n] = '.';
            n++;
            memcpy(out + n, digits + intlen, nd - intlen);
            n += nd - intlen;
        }
    }
    if (n > cap - 1) {
        return 0;
    }
    memcpy(dst, out, n);
    dst[n] = '\0';
    return n;
}

/* Go's ParseFloat: decimal only (no hex), underscores allowed only between
   digits, inf/infinity/nan accepted case-insensitively, the whole string
   must be consumed. */
/* s as strtod reads it, the separators between digits taken out; any
   length, as the Go engine reads it (a literal of a hundred digits is a
   number, not a name). */
bool filo_libc_str_to_num(void *user, const uint8_t *s, size_t len, double *out) {
    (void)user;
    if (len == 0) {
        return false;
    }
    char small[64];
    char *buf = small;
    if (len >= sizeof(small)) {
        buf = malloc(len + 1);
        if (buf == NULL) {
            return false;
        }
    }
    size_t n = 0;
    bool ok = true;
    for (size_t i = 0; i < len && ok; i++) {
        char c = (char)s[i];
        if (c == 'x' || c == 'X') {
            ok = false;
        } else if (c == '_') {
            /* a separator only between digits, as Go allows it */
            if (i == 0 || i + 1 >= len || s[i - 1] < '0' || s[i - 1] > '9' || s[i + 1] < '0' ||
                s[i + 1] > '9') {
                ok = false;
            }
        } else {
            buf[n] = c;
            n++;
        }
    }
    buf[n] = '\0';
    char *end = NULL;
    double x = 0;
    if (ok) {
        x = strtod(buf, &end);
        if (end == buf || *end != '\0') {
            ok = false;
        }
    }
    if (buf != small) {
        free(buf);
    }
    if (ok) {
        *out = x;
    }
    return ok;
}

/* Installs libm's pow for fractional exponents. */
void filo_libc_install(void) {
    filo_set_pow(pow);
}

const filo_host filo_libc_host = {
    NULL, filo_libc_num_to_str, filo_libc_str_to_num, NULL, NULL,
};

const filo_math_fns filo_libc_math = {sqrt, sin, cos, tan, log, log10, exp};

size_t filo_libc_fmt_fixed(double x, uint32_t prec, char *dst, size_t cap) {
    int n = snprintf(dst, cap, "%.*f", (int)prec, x);
    if (n < 0 || (size_t)n >= cap) {
        return 0;
    }
    return (size_t)n;
}

const filo_strings_fns filo_libc_strings = {filo_libc_fmt_fixed};
