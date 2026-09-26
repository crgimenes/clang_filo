#include "filo_nolibc.h"

#include <string.h>

/* Exactly representable as doubles, which is what makes the short path here
   correctly rounded: one multiply or one divide, no accumulated error. */
static const double pow10_tab[23] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22,
};

static const double inf_value = 1e308 * 10;

enum {
    POW10_MAX = 22,
    MANT_DIGITS_MAX = 19, /* what fits in a uint64 without overflow */
};

static bool is_digit(uint8_t c) {
    if (c >= '0' && c <= '9') {
        return true;
    }
    return false;
}

static uint8_t lower(uint8_t c) {
    if (c >= 'A' && c <= 'Z') {
        return (uint8_t)(c + 32);
    }
    return c;
}

static bool word_at(const uint8_t *s, size_t len, size_t *i, const char *word) {
    size_t k = 0;
    while (word[k] != '\0') {
        if (*i + k >= len || lower(s[*i + k]) != (uint8_t)word[k]) {
            return false;
        }
        k++;
    }
    *i += k;
    return true;
}

/* mant * 10^exp. Exact when the mantissa fits a double and the power is in
   the table; otherwise scaled in steps, which can land one unit off in the
   last place for extreme inputs. */
static double scale(double mant, int exp) {
    if (exp == 0) {
        return mant;
    }
    bool up = exp > 0;
    int n = up ? exp : -exp;
    while (n > POW10_MAX) {
        mant = up ? mant * pow10_tab[POW10_MAX] : mant / pow10_tab[POW10_MAX];
        n -= POW10_MAX;
        if (mant == 0 || mant >= inf_value || mant <= -inf_value) {
            return mant; /* saturated: further steps cannot bring it back */
        }
    }
    return up ? mant * pow10_tab[n] : mant / pow10_tab[n];
}

/* ------------------------------------------------------ exact decimals

   Outside the one-step range a decimal is converted on big integers: all
   its significant digits (up to DIGITS_KEPT, then a marker digit for "and
   more"), times or divided by the power of ten, then rounded half to even
   to 53 bits — what Go's strconv.ParseFloat does. 767 digits is the most a
   halfway point between two doubles can need, so the marker never changes
   a rounding. The widest number on the way is 10^1124 shifted by 56 bits. */

enum {
    BN_LIMBS = 128, /* 4096 bits */
    DIGITS_KEPT = 800,
};

typedef struct {
    uint32_t d[BN_LIMBS];
    size_t n; /* limbs in use, the top one nonzero; 0 is the value zero */
} bignum;

static void bn_trim(bignum *a) {
    while (a->n > 0 && a->d[a->n - 1] == 0) {
        a->n--;
    }
}

static void bn_set(bignum *a, uint64_t v) {
    a->d[0] = (uint32_t)v;
    a->d[1] = (uint32_t)(v >> 32U);
    a->n = 2;
    bn_trim(a);
}

/* a*mul + add; the sizes above keep every caller inside BN_LIMBS */
static void bn_mul_add(bignum *a, uint32_t mul, uint32_t add) {
    uint64_t carry = add;
    for (size_t i = 0; i < a->n; i++) {
        uint64_t t = ((uint64_t)a->d[i] * mul) + carry;
        a->d[i] = (uint32_t)t;
        carry = t >> 32U;
    }
    if (carry != 0 && a->n < BN_LIMBS) {
        a->d[a->n] = (uint32_t)carry;
        a->n++;
    }
}

static void bn_shl(bignum *a, size_t bits) {
    size_t n = a->n;
    size_t words = bits / 32U;
    unsigned b = (unsigned)(bits % 32U);
    if (n == 0 || n + words + 1 > BN_LIMBS) {
        return;
    }
    a->d[n + words] = 0;
    for (size_t i = n; i > 0; i--) {
        uint32_t cur = a->d[i - 1];
        if (b != 0) {
            a->d[i + words] |= cur >> (32U - b);
        }
        a->d[i - 1 + words] = cur << b;
    }
    for (size_t i = 0; i < words; i++) {
        a->d[i] = 0;
    }
    a->n = n + words + 1;
    bn_trim(a);
}

static void bn_shr1(bignum *a) {
    for (size_t i = 0; i < a->n; i++) {
        uint32_t next = 0;
        if (i + 1 < a->n) {
            next = a->d[i + 1];
        }
        a->d[i] = (a->d[i] >> 1U) | (next << 31U);
    }
    bn_trim(a);
}

static int bn_cmp(const bignum *a, const bignum *b) {
    if (a->n != b->n) {
        return a->n < b->n ? -1 : 1;
    }
    for (size_t i = a->n; i > 0; i--) {
        if (a->d[i - 1] != b->d[i - 1]) {
            return a->d[i - 1] < b->d[i - 1] ? -1 : 1;
        }
    }
    return 0;
}

/* a -= b, with a >= b */
static void bn_sub(bignum *a, const bignum *b) {
    uint64_t borrow = 0;
    for (size_t i = 0; i < a->n; i++) {
        uint64_t sub = borrow;
        if (i < b->n) {
            sub += b->d[i];
        }
        uint64_t t = (uint64_t)a->d[i] - sub;
        a->d[i] = (uint32_t)t;
        borrow = (t >> 63U) & 1U;
    }
    bn_trim(a);
}

static size_t bn_bitlen(const bignum *a) {
    if (a->n == 0) {
        return 0;
    }
    size_t bits = (a->n - 1) * 32U;
    uint32_t top = a->d[a->n - 1];
    while (top != 0) {
        bits++;
        top >>= 1U;
    }
    return bits;
}

static bool bn_bit(const bignum *a, size_t i) {
    if (i / 32U >= a->n) {
        return false;
    }
    return ((a->d[i / 32U] >> (i % 32U)) & 1U) != 0;
}

/* whether any of the bits below position count is set */
static bool bn_low_nonzero(const bignum *a, size_t count) {
    for (size_t i = 0; i < count && i / 32U < a->n; i++) {
        if (bn_bit(a, i)) {
            return true;
        }
    }
    return false;
}

static uint64_t bn_bits(const bignum *a, size_t lo, size_t count) {
    uint64_t v = 0;
    for (size_t j = count; j > 0; j--) {
        v <<= 1U;
        if (bn_bit(a, lo + j - 1)) {
            v |= 1U;
        }
    }
    return v;
}

/* (q + something below one when sticky) * 2^t, rounded half to even to a
   double: 53 bits, or fewer where the result is subnormal. */
static double round_big(const bignum *q, long t, bool sticky) {
    long bl = (long)bn_bitlen(q);
    long drop = bl - 53;
    long e2 = t + drop;
    if (e2 < -1074) {
        drop += -1074 - e2;
        e2 = -1074;
    }
    uint64_t mant = 0;
    if (drop <= 0) {
        mant = bn_bits(q, 0, (size_t)bl) << (unsigned)(-drop);
    } else {
        if (drop < bl) {
            mant = bn_bits(q, (size_t)drop, (size_t)(bl - drop));
        }
        bool rest = sticky;
        if (bn_low_nonzero(q, (size_t)(drop - 1))) {
            rest = true;
        }
        if (bn_bit(q, (size_t)(drop - 1)) && (rest || (mant & 1U) != 0)) {
            mant++;
        }
        if (mant == (1ULL << 53U)) {
            mant >>= 1U;
            e2++;
        }
    }
    uint64_t bits = mant; /* below 2^52 only as a subnormal, where e2 is -1074 */
    if (mant >= (1ULL << 52U)) {
        long biased = e2 + 1075;
        if (biased >= 2047) {
            return inf_value;
        }
        bits = ((uint64_t)biased << 52U) | (mant & ((1ULL << 52U) - 1U));
    }
    double out = 0;
    memcpy(&out, &bits, sizeof(out));
    return out;
}

/* The digits of m (with '.' and '_' as the text had them) times 10^eexp,
   correctly rounded; the sign is the caller's. */
static double decimal_exact(const uint8_t *m, size_t len, long eexp) {
    bignum a;
    bignum b;
    bn_set(&a, 0);
    size_t sig = 0;
    long frac = 0;
    long over = 0;
    bool dot = false;
    bool sticky = false;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = m[i];
        if (c == '_') {
            continue;
        }
        if (c == '.') {
            dot = true;
            continue;
        }
        uint32_t d = (uint32_t)(c - '0');
        if (sig == 0 && d == 0) {
            if (dot) {
                frac++; /* a leading zero after the point only shifts */
            }
            continue;
        }
        if (sig < DIGITS_KEPT) {
            bn_mul_add(&a, 10, d);
            sig++;
            if (dot) {
                frac++;
            }
            continue;
        }
        if (d != 0) {
            sticky = true;
        }
        if (!dot) {
            over++;
        }
    }
    if (sig == 0) {
        return 0;
    }
    long e10 = eexp - frac + over;
    if (sticky) {
        bn_mul_add(&a, 10, 1); /* "and more": below any halfway point */
        sig++;
        e10--;
    }
    long mag = (long)sig + e10; /* the value lies in [10^(mag-1), 10^mag) */
    if (mag > 310) {
        return inf_value;
    }
    if (mag < -324) {
        return 0;
    }
    if (e10 >= 0) {
        for (long i = 0; i < e10; i++) {
            bn_mul_add(&a, 10, 0);
        }
        return round_big(&a, 0, false);
    }
    bn_set(&b, 1);
    for (; e10 < 0; e10++) {
        bn_mul_add(&b, 10, 0); /* b = 10^-e10, one power at a time */
    }
    /* align so that the quotient has 55 to 57 bits, then long division */
    long shift = (long)bn_bitlen(&b) - (long)bn_bitlen(&a) + 56;
    if (shift >= 0) {
        bn_shl(&a, (size_t)shift);
    } else {
        bn_shl(&b, (size_t)(-shift));
    }
    bn_shl(&b, 56);
    uint64_t q = 0;
    for (unsigned i = 57; i > 0; i--) {
        if (bn_cmp(&a, &b) >= 0) {
            bn_sub(&a, &b);
            q |= 1ULL << (i - 1U);
        }
        bn_shr1(&b);
    }
    bool rest = a.n != 0;
    bn_set(&b, q);
    return round_big(&b, -shift, rest);
}

bool filo_nolibc_str_to_num(void *user, const uint8_t *s, size_t len, double *out) {
    (void)user;
    size_t i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
        i++;
    }
    bool neg = false;
    if (i < len && (s[i] == '+' || s[i] == '-')) {
        neg = s[i] == '-';
        i++;
    }
    if (word_at(s, len, &i, "infinity") || word_at(s, len, &i, "inf")) {
        if (i != len) {
            return false;
        }
        *out = neg ? -inf_value : inf_value;
        return true;
    }
    if (word_at(s, len, &i, "nan")) {
        if (i != len) {
            return false;
        }
        *out = inf_value - inf_value;
        return true;
    }

    /* One pass validates and keeps what the one-step path needs: the first
       significant digits and how many of the digits came after the point. */
    size_t ms = i;
    uint64_t mant = 0;
    long frac = 0;
    size_t sig = 0;
    size_t digits = 0;
    bool seen_dot = false;
    while (i < len) {
        uint8_t c = s[i];
        if (c == '_') {
            /* Go allows a separator only between digits */
            if (i == 0 || i + 1 >= len || !is_digit(s[i - 1]) || !is_digit(s[i + 1])) {
                return false;
            }
            i++;
            continue;
        }
        if (c == '.') {
            if (seen_dot) {
                return false;
            }
            seen_dot = true;
            i++;
            continue;
        }
        if (!is_digit(c)) {
            break;
        }
        digits++;
        if (sig > 0 || c != '0') {
            if (sig < MANT_DIGITS_MAX) {
                mant = (mant * 10) + (uint64_t)(c - '0');
            }
            sig++;
        }
        if (seen_dot) {
            frac++;
        }
        i++;
    }
    size_t me = i;
    if (digits == 0) {
        return false;
    }
    long eexp = 0;
    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        bool eneg = false;
        if (i < len && (s[i] == '+' || s[i] == '-')) {
            eneg = s[i] == '-';
            i++;
        }
        size_t edigits = 0;
        int e = 0;
        while (i < len && is_digit(s[i])) {
            if (e < 100000) {
                e = (e * 10) + (s[i] - '0');
            }
            edigits++;
            i++;
        }
        if (edigits == 0) {
            return false;
        }
        eexp = eneg ? -e : e;
    }
    if (i != len) {
        return false;
    }
    /* At most 15 digits and a power of ten a double holds exactly: one
       multiply or divide, correctly rounded. Anything else, exactly. */
    double v = 0;
    long e10 = eexp - frac;
    if (sig > 0 && sig <= 15 && e10 >= -22 && e10 <= 22) {
        v = scale((double)mant, (int)e10);
    } else if (sig > 0) {
        v = decimal_exact(s + ms, me - ms, eexp);
    }
    *out = neg ? -v : v;
    return true;
}

/* ---- formatting ---- */

static size_t put_u64(char *dst, size_t cap, uint64_t v) {
    char tmp[24];
    size_t n = 0;
    do {
        tmp[n] = (char)('0' + (v % 10));
        n++;
        v /= 10;
    } while (v > 0);
    if (n > cap) {
        return 0;
    }
    size_t k = 0;
    while (k < n) {
        dst[k] = tmp[n - 1 - k];
        k++;
    }
    return n;
}

/* Lays out digits/exponent as text: plain when the exponent lands in
   [-4, 6), scientific otherwise, which is Go's rule for %g. */
static size_t render(bool neg, const char *digits, size_t nd, int e10, char *dst, size_t cap) {
    size_t out = 0;
    if (neg) {
        dst[out] = '-';
        out++;
    }
    if (e10 < -4 || e10 >= 6) {
        dst[out] = digits[0];
        out++;
        if (nd > 1) {
            dst[out] = '.';
            out++;
            memcpy(dst + out, digits + 1, nd - 1);
            out += nd - 1;
        }
        dst[out] = 'e';
        out++;
        int e = e10;
        if (e < 0) {
            dst[out] = '-';
            e = -e;
        } else {
            dst[out] = '+';
        }
        out++;
        if (e < 10) {
            dst[out] = '0';
            out++;
        }
        return out + put_u64(dst + out, cap - out, (uint64_t)e);
    }
    if (e10 >= 0) {
        size_t whole = (size_t)e10 + 1;
        size_t k = 0;
        while (k < whole) {
            dst[out] = k < nd ? digits[k] : '0';
            out++;
            k++;
        }
        if (nd > whole) {
            dst[out] = '.';
            out++;
            memcpy(dst + out, digits + whole, nd - whole);
            out += nd - whole;
        }
        return out;
    }
    dst[out] = '0';
    out++;
    dst[out] = '.';
    out++;
    int z = -e10 - 1;
    while (z > 0) {
        dst[out] = '0';
        out++;
        z--;
    }
    memcpy(dst + out, digits, nd);
    return out + nd;
}

/* ------------------------------------------------------------ %f, exact

   A double is m * 2^e with m an integer of at most 53 bits, so its decimal
   expansion is finite: an integer part of at most 309 digits and a fraction
   that ends within -e digits. Both are computed exactly on small big
   integers — arrays of 32-bit limbs — and rounded to the precision half to
   even, as Go's strconv and a C library's printf round. The short path this
   replaced scaled by a power of ten in double arithmetic: it cut the
   precision to 17 without saying so and failed once the scaled value passed
   1e19, so (str-fmt "%f" 9007199254740992) was an error. */

enum {
    BIG_LIMBS = 36,  /* 1074 fraction bits, or a 1024-bit integer, as 32-bit limbs */
    BIG_CHUNKS = 40, /* 309 integer digits in chunks of nine */
};

/* The limbs times ten; what spills out of the top limb is the next digit. */
static uint32_t times_ten(uint32_t *limb, size_t n) {
    uint64_t carry = 0;
    for (size_t i = 0; i < n; i++) {
        uint64_t t = ((uint64_t)limb[i] * 10U) + carry;
        limb[i] = (uint32_t)t;
        carry = t >> 32U;
    }
    return (uint32_t)carry;
}

static bool all_zero(const uint32_t *limb, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (limb[i] != 0) {
            return false;
        }
    }
    return true;
}

/* m << shift into limbs: m has at most 53 bits and shift % 32 leaves at
   most 31 more, so three limbs from shift / 32 hold it. */
static void put_shifted(uint32_t *limb, uint64_t m, unsigned shift) {
    size_t word = shift / 32U;
    unsigned bit = shift % 32U;
    uint64_t lo = m << bit;
    uint64_t hi = 0;
    if (bit != 0) {
        hi = m >> (64U - bit);
    }
    limb[word] = (uint32_t)lo;
    limb[word + 1] = (uint32_t)(lo >> 32U);
    limb[word + 2] = (uint32_t)hi;
}

/* The digits of m << e, by long division by 1e9; 0 when they do not fit. */
static size_t big_int_text(uint64_t m, unsigned e, char *dst, size_t cap) {
    uint32_t limb[BIG_LIMBS] = {0};
    put_shifted(limb, m, e);
    size_t used = (e / 32U) + 3U;
    uint32_t chunk[BIG_CHUNKS] = {0};
    size_t nchunks = 0;
    while (used > 0 && limb[used - 1] == 0) {
        used--;
    }
    while (used > 0 && nchunks < BIG_CHUNKS) {
        uint64_t rem = 0;
        for (size_t i = used; i > 0; i--) {
            uint64_t cur = (rem << 32U) | limb[i - 1];
            limb[i - 1] = (uint32_t)(cur / 1000000000U);
            rem = cur % 1000000000U;
        }
        chunk[nchunks] = (uint32_t)rem;
        nchunks++;
        while (used > 0 && limb[used - 1] == 0) {
            used--;
        }
    }
    if (nchunks == 0) {
        return put_u64(dst, cap, 0);
    }
    size_t out = put_u64(dst, cap, chunk[nchunks - 1]);
    if (out == 0) {
        return 0;
    }
    for (size_t c = nchunks - 1; c > 0; c--) {
        if (cap - out < 9) {
            return 0;
        }
        uint32_t v = chunk[c - 1];
        for (size_t d = 9; d > 0; d--) {
            dst[out + d - 1] = (char)('0' + (v % 10U));
            v /= 10U;
        }
        out += 9;
    }
    return out;
}

/* Rounding may carry into the slot kept in front of the leading digit;
   when it did not, the slot goes. */
static size_t drop_lead(char *dst, size_t lead, size_t out) {
    if (dst[lead] != '0') {
        return out;
    }
    for (size_t j = lead; j + 1 < out; j++) {
        dst[j] = dst[j + 1];
    }
    return out - 1;
}

/* The decimals of frac / 2^k, rounded; 0 when they do not fit. The binary
   point is put on a limb boundary, so each multiplication by ten carries
   the next digit out of the top limb, and the fraction runs out of digits
   within k of them — past that, the rest are zeros, however many. */
static size_t fraction_text(uint64_t frac, unsigned k, uint32_t prec, char *dst, size_t lead,
                            size_t out, size_t cap) {
    uint32_t limb[BIG_LIMBS] = {0};
    unsigned s = (32U - (k % 32U)) % 32U;
    size_t n = (k + s) / 32U;
    put_shifted(limb, frac, s);
    if (prec > 0) {
        if (cap - out < (size_t)prec + 1U) {
            return 0;
        }
        dst[out] = '.';
        out++;
    }
    uint32_t written = 0;
    while (written < prec && !all_zero(limb, n)) {
        dst[out] = (char)('0' + times_ten(limb, n));
        out++;
        written++;
    }
    memset(dst + out, '0', (size_t)(prec - written));
    out += (size_t)(prec - written);
    uint32_t next = times_ten(limb, n);
    bool rest = true;
    if (all_zero(limb, n)) {
        rest = false;
    }
    uint32_t last = (uint32_t)(dst[out - 1] - '0');
    bool up = next > 5U;
    if (next == 5U && (rest || (last % 2U) == 1U)) {
        up = true;
    }
    size_t j = out;
    while (up && j > lead) {
        j--;
        if (dst[j] == '.') {
            continue;
        }
        if (dst[j] == '9') {
            dst[j] = '0';
            continue;
        }
        dst[j] = (char)(dst[j] + 1);
        up = false;
    }
    return drop_lead(dst, lead, out);
}

static size_t nolibc_fmt_fixed(double x, uint32_t prec, char *dst, size_t cap) {
    uint64_t bits = 0;
    memcpy(&bits, &x, sizeof(bits));
    unsigned ex = (unsigned)((bits >> 52U) & 0x7FFU);
    if (ex == 0x7FFU || cap < 2) {
        return 0; /* NaN and the infinities are spelled by str-fmt itself */
    }
    uint64_t m = bits & ((1ULL << 52U) - 1U);
    int e = -1074;
    if (ex != 0) {
        m |= 1ULL << 52U;
        e = (int)ex - 1075;
    }
    size_t out = 0;
    if ((bits >> 63U) != 0U) {
        dst[out] = '-'; /* -0 included, as Go and printf write it */
        out++;
    }
    size_t lead = out;
    dst[out] = '0';
    out++;
    if (e >= 0) {
        size_t n = big_int_text(m, (unsigned)e, dst + out, cap - out);
        if (n == 0) {
            return 0;
        }
        out += n;
        if (prec > 0) {
            if (cap - out < (size_t)prec + 1U) {
                return 0;
            }
            dst[out] = '.';
            memset(dst + out + 1, '0', prec);
            out += (size_t)prec + 1U;
        }
        return drop_lead(dst, lead, out); /* an integer: nothing to round */
    }
    unsigned k = (unsigned)(-e);
    uint64_t whole = 0;
    uint64_t frac = m;
    if (k < 64U) {
        whole = m >> k;
        frac = m & ((1ULL << k) - 1U);
    }
    size_t n = put_u64(dst + out, cap - out, whole);
    if (n == 0) {
        return 0;
    }
    return fraction_text(frac, k, prec, dst, lead, out + n, cap);
}

/* ------------------------------------------------------ shortest, exact

   The shortest digits that read back as the same double, and of those the
   closest: what Go's strconv.FormatFloat(x, 'g', -1, 64) writes. For each
   length the two candidates around the exact value are tried, the nearer
   first, and the exact reader above decides — so which ends of the
   rounding interval count is whatever the reader rounds to, and the two
   can never disagree about a round trip. */

enum { SHORT_DIGITS = 18 }; /* 17 always read back; one more decides the rounding */

/* The first SHORT_DIGITS significant digits of x > 0, exactly, with the
   decimal exponent of the first and whether anything nonzero follows. */
static size_t leading_digits(double x, char *dig, int *e10, bool *more) {
    uint64_t bits = 0;
    memcpy(&bits, &x, sizeof(bits));
    unsigned ex = (unsigned)((bits >> 52U) & 0x7FFU);
    uint64_t m = bits & ((1ULL << 52U) - 1U);
    int e = -1074;
    if (ex != 0) {
        m |= 1ULL << 52U;
        e = (int)ex - 1075;
    }
    *more = false;
    if (e >= 0) {
        char whole[340];
        size_t n = big_int_text(m, (unsigned)e, whole, sizeof(whole));
        size_t take = n < SHORT_DIGITS ? n : SHORT_DIGITS;
        memcpy(dig, whole, take);
        for (size_t i = take; i < n; i++) {
            if (whole[i] != '0') {
                *more = true;
            }
        }
        *e10 = (int)n - 1;
        return take;
    }
    unsigned k = (unsigned)(-e);
    uint64_t whole = 0;
    uint64_t frac = m;
    if (k < 64U) {
        whole = m >> k;
        frac = m & ((1ULL << k) - 1U);
    }
    size_t nd = 0;
    if (whole > 0) {
        nd = put_u64(dig, SHORT_DIGITS, whole); /* at most 16 digits */
        *e10 = (int)nd - 1;
    }
    uint32_t limb[BIG_LIMBS] = {0};
    unsigned s = (32U - (k % 32U)) % 32U;
    size_t n = (k + s) / 32U;
    put_shifted(limb, frac, s);
    int zeros = 0;
    while (nd < SHORT_DIGITS && !all_zero(limb, n)) {
        uint32_t d = times_ten(limb, n);
        if (nd == 0 && d == 0) {
            zeros++; /* below one: the zeros after the point set the exponent */
            continue;
        }
        if (nd == 0) {
            *e10 = -(zeros + 1);
        }
        dig[nd] = (char)('0' + d);
        nd++;
    }
    if (!all_zero(limb, n)) {
        *more = true;
    }
    return nd;
}

/* Whether dig (nd digits, the first at 10^e10) reads back as x. */
static bool reads_back(const char *dig, size_t nd, int e10, double x) {
    long q = (long)e10 - (long)nd + 1;
    double v = 0;
    if (nd <= 15 && q >= -22 && q <= 22) {
        uint64_t mant = 0;
        for (size_t i = 0; i < nd; i++) {
            mant = (mant * 10U) + (uint64_t)(dig[i] - '0');
        }
        v = scale((double)mant, (int)q);
    } else {
        v = decimal_exact((const uint8_t *)dig, nd, q);
    }
    return v == x;
}

/* The next p-digit number up; 99 becomes 10 one power higher. */
static void bump(char *dig, size_t p, int *e10) {
    size_t i = p;
    while (i > 0) {
        i--;
        if (dig[i] != '9') {
            dig[i] = (char)(dig[i] + 1);
            return;
        }
        dig[i] = '0';
    }
    dig[0] = '1';
    *e10 += 1;
}

static size_t put_shortest(bool neg, const char *dig, size_t nd, int e10, char *dst, size_t cap) {
    while (nd > 1 && dig[nd - 1] == '0') {
        nd--; /* trailing zeros carry no information */
    }
    return render(neg, dig, nd, e10, dst, cap);
}

size_t filo_nolibc_num_to_str(void *user, double x, char *dst, size_t cap) {
    (void)user;
    if (cap < 32) {
        return 0;
    }
    if (x != x) {
        memcpy(dst, "NaN", 4); /* with its terminator: cap is at least 32 */
        return 3;
    }
    if (x >= inf_value) {
        memcpy(dst, "+Inf", 5);
        return 4;
    }
    if (x <= -inf_value) {
        memcpy(dst, "-Inf", 5);
        return 4;
    }
    bool neg = false;
    if (x < 0 || (x == 0 && 1 / x < 0)) {
        neg = true;
        x = -x;
    }
    if (x == 0) {
        size_t n = 0;
        if (neg) {
            dst[0] = '-';
            n = 1;
        }
        dst[n] = '0';
        return n + 1;
    }
    char dig[SHORT_DIGITS];
    int e10 = 0;
    bool more = false;
    size_t have = leading_digits(x, dig, &e10, &more);
    for (size_t p = 1; p < SHORT_DIGITS; p++) {
        char lo[SHORT_DIGITS];
        char hi[SHORT_DIGITS];
        for (size_t i = 0; i < p; i++) {
            lo[i] = i < have ? dig[i] : '0';
        }
        memcpy(hi, lo, p);
        int e_lo = e10;
        int e_hi = e10;
        bump(hi, p, &e_hi);
        uint32_t next = 0;
        if (p < have) {
            next = (uint32_t)(dig[p] - '0');
        }
        bool beyond = more;
        for (size_t i = p + 1; i < have; i++) {
            if (dig[i] != '0') {
                beyond = true;
            }
        }
        bool up = next > 5U;
        if (next == 5U && (beyond || ((uint32_t)(lo[p - 1] - '0') % 2U) == 1U)) {
            up = true;
        }
        if (up && reads_back(hi, p, e_hi, x)) {
            return put_shortest(neg, hi, p, e_hi, dst, cap);
        }
        if (reads_back(lo, p, e_lo, x)) {
            return put_shortest(neg, lo, p, e_lo, dst, cap);
        }
        if (!up && reads_back(hi, p, e_hi, x)) {
            return put_shortest(neg, hi, p, e_hi, dst, cap);
        }
    }
    return put_shortest(neg, dig, SHORT_DIGITS - 1, e10, dst, cap); /* not reached */
}

const filo_strings_fns filo_nolibc_strings = {nolibc_fmt_fixed};

const filo_host filo_nolibc_host = {
    NULL, filo_nolibc_num_to_str, filo_nolibc_str_to_num, NULL, NULL,
};
