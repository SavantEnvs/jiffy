/* In-process model of the subset of erl_nif.h that jiffy's c_src uses. */
#include "enif_mock.h"

#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    T_ATOM = 1, T_INT, T_BIG, T_FLOAT, T_BIN, T_TUPLE, T_NIL, T_CONS, T_MAP,
    T_RES, T_PID, T_REF, T_BADARG, T_SCHED
};

typedef struct Term Term;
typedef ERL_NIF_TERM (*NifFp)(ErlNifEnv*, int, const ERL_NIF_TERM[]);

struct Term {
    int type;
    Term* chain;
    union {
        struct { size_t len; unsigned char* name; } atom;
        long i;
        struct { int neg; size_t n; unsigned char* mag; } big;
        double d;
        struct { size_t size; unsigned char* data; int owned; unsigned lastbits; } bin;
        struct { unsigned arity; ERL_NIF_TERM* el; } tup;
        struct { ERL_NIF_TERM head, tail; } cons;
        struct { size_t size; ERL_NIF_TERM* keys; ERL_NIF_TERM* vals; } map;
        struct Res* res;
        struct { NifFp fp; int argc; ERL_NIF_TERM* argv; } sched;
    } u;
};

typedef struct Res {
    long refs;
    struct enif_resource_type_t* type;
    size_t size;
    size_t pad;
} Res;

struct enif_resource_type_t {
    const char* name;
    void (*dtor)(ErlNifEnv*, void*);
};

struct enif_environment_t {
    Term* chain;
    int badarg;
};

static Term NIL_TERM = { T_NIL, NULL, { .i = 0 } };
static Term BADARG_TERM = { T_BADARG, NULL, { .i = 0 } };
static void* g_priv;

#define TP(t) ((Term*)(uintptr_t)(t))
#define TT(p) ((ERL_NIF_TERM)(uintptr_t)(p))

ERL_NIF_TERM mock_nil(void) { return TT(&NIL_TERM); }
void mock_set_priv(void* priv) { g_priv = priv; }
int mock_badarg_raised(ErlNifEnv* env) { return env->badarg; }

ErlNifEnv* mock_env_new(void)
{
    ErlNifEnv* env = calloc(1, sizeof(*env));
    if(env == NULL) abort();
    return env;
}

static void res_unref(Res* r)
{
    if(--r->refs == 0) {
        if(r->type->dtor != NULL) {
            struct enif_environment_t scratch = { NULL, 0 };
            r->type->dtor(&scratch, (char*)r + sizeof(Res));
        }
        free(r);
    }
}

void mock_env_free(ErlNifEnv* env)
{
    Term* t = env->chain;
    while(t != NULL) {
        Term* next = t->chain;
        switch(t->type) {
            case T_ATOM: free(t->u.atom.name); break;
            case T_BIG: free(t->u.big.mag); break;
            case T_BIN:
            case T_PID:
            case T_REF:
                if(t->u.bin.owned) free(t->u.bin.data);
                break;
            case T_TUPLE: free(t->u.tup.el); break;
            case T_MAP: free(t->u.map.keys); free(t->u.map.vals); break;
            case T_RES: res_unref(t->u.res); break;
            case T_SCHED: free(t->u.sched.argv); break;
            default: break;
        }
        free(t);
        t = next;
    }
    free(env);
}

static Term* tnew(ErlNifEnv* env, int type)
{
    Term* t = calloc(1, sizeof(Term));
    if(t == NULL) abort();
    t->type = type;
    t->chain = env->chain;
    env->chain = t;
    return t;
}

static void* xmalloc(size_t n)
{
    void* p = malloc(n ? n : 1);
    if(p == NULL) abort();
    return p;
}

/* ---- ordering ---------------------------------------------------------- */

typedef struct { Term* a; Term* b; } Pair;
typedef struct { Pair* v; size_t n, cap; Pair small[32]; } PStack;

static void ps_push(PStack* s, Term* a, Term* b)
{
    if(s->n == s->cap) {
        size_t nc = s->cap * 2;
        Pair* nv = xmalloc(nc * sizeof(Pair));
        memcpy(nv, s->v, s->n * sizeof(Pair));
        if(s->v != s->small) free(s->v);
        s->v = nv;
        s->cap = nc;
    }
    s->v[s->n].a = a;
    s->v[s->n].b = b;
    s->n++;
}

static int rank_of(const Term* t)
{
    switch(t->type) {
        case T_INT: case T_BIG: case T_FLOAT: return 0;
        case T_ATOM: return 1;
        case T_REF: return 2;
        case T_PID: return 5;
        case T_TUPLE: return 6;
        case T_MAP: return 7;
        case T_NIL: return 8;
        case T_CONS: return 9;
        case T_BIN: return 10;
        default: return 11;
    }
}

static double big_to_double(const Term* t)
{
    double d = 0;
    for(size_t i = t->u.big.n; i > 0; i--) d = d * 256.0 + t->u.big.mag[i - 1];
    return t->u.big.neg ? -d : d;
}

static int num_cmp(const Term* a, const Term* b, int exact)
{
    if(exact && a->type != b->type) return a->type < b->type ? -1 : 1;
    if(a->type == T_INT && b->type == T_INT) {
        return a->u.i < b->u.i ? -1 : a->u.i > b->u.i;
    }
    if(a->type == T_BIG && b->type == T_BIG) {
        if(a->u.big.neg != b->u.big.neg) return a->u.big.neg ? -1 : 1;
        int s = a->u.big.neg ? -1 : 1;
        if(a->u.big.n != b->u.big.n) return a->u.big.n < b->u.big.n ? -s : s;
        for(size_t i = a->u.big.n; i > 0; i--) {
            if(a->u.big.mag[i - 1] != b->u.big.mag[i - 1]) {
                return a->u.big.mag[i - 1] < b->u.big.mag[i - 1] ? -s : s;
            }
        }
        return 0;
    }
    double x = a->type == T_INT ? (double)a->u.i
             : a->type == T_BIG ? big_to_double(a) : a->u.d;
    double y = b->type == T_INT ? (double)b->u.i
             : b->type == T_BIG ? big_to_double(b) : b->u.d;
    return x < y ? -1 : x > y;
}

static int bytes_cmp(const unsigned char* a, size_t an, const unsigned char* b, size_t bn)
{
    size_t m = an < bn ? an : bn;
    int c = m ? memcmp(a, b, m) : 0;
    if(c != 0) return c < 0 ? -1 : 1;
    return an < bn ? -1 : an > bn;
}

static int tcmp(Term* a0, Term* b0, int exact)
{
    PStack st;
    int result = 0;
    st.v = st.small;
    st.n = 0;
    st.cap = 32;
    ps_push(&st, a0, b0);

    while(st.n > 0 && result == 0) {
        Pair p = st.v[--st.n];
        Term* a = p.a;
        Term* b = p.b;
        if(a == b) continue;
        int ra = rank_of(a), rb = rank_of(b);
        if(ra != rb) { result = ra < rb ? -1 : 1; break; }
        switch(a->type) {
            case T_INT: case T_BIG: case T_FLOAT:
                result = num_cmp(a, b, exact);
                break;
            case T_ATOM:
                result = bytes_cmp(a->u.atom.name, a->u.atom.len,
                                   b->u.atom.name, b->u.atom.len);
                break;
            case T_BIN:
            case T_PID:
            case T_REF:
                result = bytes_cmp(a->u.bin.data, a->u.bin.size,
                                   b->u.bin.data, b->u.bin.size);
                if(result == 0 && a->type == T_BIN) {
                    result = a->u.bin.lastbits < b->u.bin.lastbits ? -1
                           : a->u.bin.lastbits > b->u.bin.lastbits;
                }
                break;
            case T_TUPLE:
                if(a->u.tup.arity != b->u.tup.arity) {
                    result = a->u.tup.arity < b->u.tup.arity ? -1 : 1;
                    break;
                }
                for(unsigned i = a->u.tup.arity; i > 0; i--) {
                    ps_push(&st, TP(a->u.tup.el[i - 1]), TP(b->u.tup.el[i - 1]));
                }
                break;
            case T_MAP:
                if(a->u.map.size != b->u.map.size) {
                    result = a->u.map.size < b->u.map.size ? -1 : 1;
                    break;
                }
                for(size_t i = a->u.map.size; i > 0; i--) {
                    ps_push(&st, TP(a->u.map.vals[i - 1]), TP(b->u.map.vals[i - 1]));
                }
                for(size_t i = a->u.map.size; i > 0; i--) {
                    ps_push(&st, TP(a->u.map.keys[i - 1]), TP(b->u.map.keys[i - 1]));
                }
                break;
            case T_CONS:
                ps_push(&st, TP(a->u.cons.tail), TP(b->u.cons.tail));
                ps_push(&st, TP(a->u.cons.head), TP(b->u.cons.head));
                break;
            default:
                break;
        }
    }
    if(st.v != st.small) free(st.v);
    return result;
}

int enif_compare(ERL_NIF_TERM lhs, ERL_NIF_TERM rhs)
{
    return tcmp(TP(lhs), TP(rhs), 0);
}

int enif_is_identical(ERL_NIF_TERM lhs, ERL_NIF_TERM rhs)
{
    return lhs == rhs || tcmp(TP(lhs), TP(rhs), 1) == 0;
}

static uint64_t hash_bytes(uint64_t h, const unsigned char* p, size_t n)
{
    for(size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ULL;
    return h;
}

static uint64_t thash(Term* t, int depth, uint64_t h)
{
    h = (h ^ (uint64_t)rank_of(t)) * 0x100000001b3ULL;
    if(depth > 8) return h;
    switch(t->type) {
        case T_INT: return hash_bytes(h, (unsigned char*)&t->u.i, sizeof(long));
        case T_ATOM: return hash_bytes(h, t->u.atom.name, t->u.atom.len);
        case T_BIN: return hash_bytes(h, t->u.bin.data, t->u.bin.size);
        case T_TUPLE:
            for(unsigned i = 0; i < t->u.tup.arity && i < 8; i++) {
                h = thash(TP(t->u.tup.el[i]), depth + 1, h);
            }
            return h;
        default: return h;
    }
}

ErlNifUInt64 enif_hash(ErlNifHash type, ERL_NIF_TERM term, ErlNifUInt64 salt)
{
    (void)type;
    return thash(TP(term), 0, 0xcbf29ce484222325ULL ^ salt);
}

/* ---- memory ------------------------------------------------------------ */

void* enif_priv_data(ErlNifEnv* env) { (void)env; return g_priv; }
void* enif_alloc(size_t size) { return malloc(size); }
void enif_free(void* ptr) { free(ptr); }
void* enif_realloc(void* ptr, size_t size) { return realloc(ptr, size); }

ErlNifTime enif_monotonic_time(ErlNifTimeUnit u) { (void)u; return 123456789; }
int enif_consume_timeslice(ErlNifEnv* env, int percent)
{
    (void)env; (void)percent;
    return 0;
}

/* ---- type tests -------------------------------------------------------- */

int enif_is_atom(ErlNifEnv* env, ERL_NIF_TERM t) { (void)env; return TP(t)->type == T_ATOM; }
int enif_is_binary(ErlNifEnv* env, ERL_NIF_TERM t)
{
    (void)env;
    return TP(t)->type == T_BIN && TP(t)->u.bin.lastbits == 8;
}
int enif_is_list(ErlNifEnv* env, ERL_NIF_TERM t)
{
    (void)env;
    return TP(t)->type == T_CONS || TP(t)->type == T_NIL;
}
int enif_is_empty_list(ErlNifEnv* env, ERL_NIF_TERM t) { (void)env; return TP(t)->type == T_NIL; }
int enif_is_map(ErlNifEnv* env, ERL_NIF_TERM t) { (void)env; return TP(t)->type == T_MAP; }

int mock_is_tuple(ERL_NIF_TERM t, unsigned* arity, const ERL_NIF_TERM** el)
{
    if(TP(t)->type != T_TUPLE) return 0;
    *arity = TP(t)->u.tup.arity;
    *el = TP(t)->u.tup.el;
    return 1;
}

int mock_atom_is(ERL_NIF_TERM t, const char* name)
{
    size_t n = strlen(name);
    return TP(t)->type == T_ATOM && TP(t)->u.atom.len == n &&
           memcmp(TP(t)->u.atom.name, name, n) == 0;
}

/* ---- atoms ------------------------------------------------------------- */

ERL_NIF_TERM mock_make_atom_utf8(ErlNifEnv* env, const unsigned char* s, size_t n)
{
    Term* t = tnew(env, T_ATOM);
    t->u.atom.name = xmalloc(n);
    if(n) memcpy(t->u.atom.name, s, n);
    t->u.atom.len = n;
    return TT(t);
}

ERL_NIF_TERM mock_make_atom_latin1(ErlNifEnv* env, const unsigned char* s, size_t n)
{
    Term* t = tnew(env, T_ATOM);
    size_t extra = 0;
    for(size_t i = 0; i < n; i++) extra += s[i] >= 0x80;
    t->u.atom.name = xmalloc(n + extra);
    size_t o = 0;
    for(size_t i = 0; i < n; i++) {
        if(s[i] < 0x80) {
            t->u.atom.name[o++] = s[i];
        } else {
            t->u.atom.name[o++] = (unsigned char)(0xC0 | (s[i] >> 6));
            t->u.atom.name[o++] = (unsigned char)(0x80 | (s[i] & 0x3F));
        }
    }
    t->u.atom.len = o;
    return TT(t);
}

ERL_NIF_TERM enif_make_atom(ErlNifEnv* env, const char* name)
{
    size_t n = strlen(name);
    if(n > 255) return enif_make_badarg(env);
    return mock_make_atom_latin1(env, (const unsigned char*)name, n);
}

int enif_make_existing_atom(ErlNifEnv* env, const char* name, ERL_NIF_TERM* atom,
                            ErlNifCharEncoding enc)
{
    (void)enc;
    ERL_NIF_TERM t = enif_make_atom(env, name);
    if(TP(t)->type != T_ATOM) return 0;
    *atom = t;
    return 1;
}

int enif_get_atom(ErlNifEnv* env, ERL_NIF_TERM term, char* buf, unsigned len,
                  ErlNifCharEncoding enc)
{
    (void)env;
    Term* t = TP(term);
    if(t->type != T_ATOM || len == 0) return 0;
    const unsigned char* s = t->u.atom.name;
    size_t n = t->u.atom.len;
    if(enc == ERL_NIF_UTF8) {
        if(n + 1 > len) return 0;
        memcpy(buf, s, n);
        buf[n] = 0;
        return (int)(n + 1);
    }
    size_t o = 0;
    for(size_t i = 0; i < n;) {
        unsigned cp;
        if(s[i] < 0x80) { cp = s[i]; i += 1; }
        else if((s[i] & 0xE0) == 0xC0 && i + 1 < n) {
            cp = ((unsigned)(s[i] & 0x1F) << 6) | (s[i + 1] & 0x3F);
            i += 2;
        } else {
            return 0;
        }
        if(cp > 255 || o + 1 >= len) return 0;
        buf[o++] = (char)cp;
    }
    buf[o] = 0;
    return (int)(o + 1);
}

/* ---- numbers ----------------------------------------------------------- */

ERL_NIF_TERM enif_make_int(ErlNifEnv* env, int i)
{
    Term* t = tnew(env, T_INT);
    t->u.i = i;
    return TT(t);
}

ERL_NIF_TERM enif_make_long(ErlNifEnv* env, long i)
{
    Term* t = tnew(env, T_INT);
    t->u.i = i;
    return TT(t);
}

ERL_NIF_TERM mock_make_big(ErlNifEnv* env, int neg, const unsigned char* mag, size_t n)
{
    while(n > 0 && mag[n - 1] == 0) n--;
    if(n <= 8) {
        unsigned long m = 0;
        for(size_t i = n; i > 0; i--) m = (m << 8) | mag[i - 1];
        if(!neg && m <= (unsigned long)LONG_MAX) return enif_make_long(env, (long)m);
        if(neg && m <= (unsigned long)LONG_MAX + 1UL) return enif_make_long(env, (long)(0UL - m));
    }
    Term* t = tnew(env, T_BIG);
    t->u.big.neg = neg;
    t->u.big.n = n;
    t->u.big.mag = xmalloc(n);
    memcpy(t->u.big.mag, mag, n);
    return TT(t);
}

ERL_NIF_TERM enif_make_double(ErlNifEnv* env, double d)
{
    if(!isfinite(d)) return enif_make_badarg(env);
    Term* t = tnew(env, T_FLOAT);
    t->u.d = d;
    return TT(t);
}

int enif_get_long(ErlNifEnv* env, ERL_NIF_TERM term, long* ip)
{
    (void)env;
    if(TP(term)->type != T_INT) return 0;
    *ip = TP(term)->u.i;
    return 1;
}

int enif_get_uint(ErlNifEnv* env, ERL_NIF_TERM term, unsigned* ip)
{
    (void)env;
    if(TP(term)->type != T_INT) return 0;
    long v = TP(term)->u.i;
    if(v < 0 || v > (long)UINT_MAX) return 0;
    *ip = (unsigned)v;
    return 1;
}

int enif_get_double(ErlNifEnv* env, ERL_NIF_TERM term, double* dp)
{
    (void)env;
    if(TP(term)->type != T_FLOAT) return 0;
    *dp = TP(term)->u.d;
    return 1;
}

/* ---- binaries ---------------------------------------------------------- */

int enif_inspect_binary(ErlNifEnv* env, ERL_NIF_TERM term, ErlNifBinary* bin)
{
    (void)env;
    Term* t = TP(term);
    if(t->type != T_BIN || t->u.bin.lastbits != 8) return 0;
    memset(bin, 0, sizeof(*bin));
    bin->size = t->u.bin.size;
    bin->data = t->u.bin.data;
    return 1;
}

int enif_alloc_binary(size_t size, ErlNifBinary* bin)
{
    memset(bin, 0, sizeof(*bin));
    bin->data = xmalloc(size);
    bin->size = size;
    bin->ref_bin = bin->data;
    return 1;
}

int enif_realloc_binary(ErlNifBinary* bin, size_t size)
{
    unsigned char* p = realloc(bin->data, size ? size : 1);
    if(p == NULL) return 0;
    bin->data = p;
    bin->size = size;
    bin->ref_bin = p;
    return 1;
}

void enif_release_binary(ErlNifBinary* bin)
{
    if(bin->ref_bin != NULL) free(bin->data);
    bin->data = NULL;
    bin->ref_bin = NULL;
    bin->size = 0;
}

ERL_NIF_TERM enif_make_binary(ErlNifEnv* env, ErlNifBinary* bin)
{
    Term* t = tnew(env, T_BIN);
    t->u.bin.data = bin->data;
    t->u.bin.size = bin->size;
    t->u.bin.lastbits = 8;
    t->u.bin.owned = bin->ref_bin != NULL;
    return TT(t);
}

unsigned char* enif_make_new_binary(ErlNifEnv* env, size_t size, ERL_NIF_TERM* termp)
{
    Term* t = tnew(env, T_BIN);
    t->u.bin.data = xmalloc(size);
    t->u.bin.size = size;
    t->u.bin.lastbits = 8;
    t->u.bin.owned = 1;
    *termp = TT(t);
    return t->u.bin.data;
}

ERL_NIF_TERM enif_make_sub_binary(ErlNifEnv* env, ERL_NIF_TERM bin_term, size_t pos,
                                  size_t size)
{
    Term* p = TP(bin_term);
    if(p->type != T_BIN || pos > p->u.bin.size || size > p->u.bin.size - pos) {
        fprintf(stderr, "enif_make_sub_binary: range %zu+%zu outside binary of %zu bytes\n",
                pos, size, p->type == T_BIN ? p->u.bin.size : (size_t)0);
        abort();
    }
    Term* t = tnew(env, T_BIN);
    t->u.bin.data = p->u.bin.data + pos;
    t->u.bin.size = size;
    t->u.bin.lastbits = 8;
    t->u.bin.owned = 0;
    return TT(t);
}

ERL_NIF_TERM mock_make_bits(ErlNifEnv* env, const unsigned char* s, size_t n, unsigned lastbits)
{
    Term* t = tnew(env, T_BIN);
    t->u.bin.data = xmalloc(n);
    if(n) memcpy(t->u.bin.data, s, n);
    t->u.bin.size = n;
    t->u.bin.lastbits = lastbits;
    t->u.bin.owned = 1;
    return TT(t);
}

ERL_NIF_TERM mock_make_blob(ErlNifEnv* env, int is_ref, const unsigned char* s, size_t n)
{
    Term* t = tnew(env, is_ref ? T_REF : T_PID);
    t->u.bin.data = xmalloc(n);
    if(n) memcpy(t->u.bin.data, s, n);
    t->u.bin.size = n;
    t->u.bin.owned = 1;
    return TT(t);
}

/* ---- tuples and lists -------------------------------------------------- */

static ERL_NIF_TERM tuple_from(ErlNifEnv* env, const ERL_NIF_TERM* arr, unsigned cnt)
{
    Term* t = tnew(env, T_TUPLE);
    t->u.tup.arity = cnt;
    t->u.tup.el = xmalloc(cnt * sizeof(ERL_NIF_TERM));
    if(cnt) memcpy(t->u.tup.el, arr, cnt * sizeof(ERL_NIF_TERM));
    return TT(t);
}

ERL_NIF_TERM enif_make_tuple_from_array(ErlNifEnv* env, const ERL_NIF_TERM arr[], unsigned cnt)
{
    return tuple_from(env, arr, cnt);
}

ERL_NIF_TERM enif_make_tuple(ErlNifEnv* env, unsigned cnt, ...)
{
    ERL_NIF_TERM* tmp = xmalloc(cnt * sizeof(ERL_NIF_TERM));
    va_list ap;
    va_start(ap, cnt);
    for(unsigned i = 0; i < cnt; i++) tmp[i] = va_arg(ap, ERL_NIF_TERM);
    va_end(ap);
    ERL_NIF_TERM r = tuple_from(env, tmp, cnt);
    free(tmp);
    return r;
}

int enif_get_tuple(ErlNifEnv* env, ERL_NIF_TERM tpl, int* arity, const ERL_NIF_TERM** array)
{
    (void)env;
    Term* t = TP(tpl);
    if(t->type != T_TUPLE) return 0;
    *arity = (int)t->u.tup.arity;
    *array = t->u.tup.el;
    return 1;
}

ERL_NIF_TERM enif_make_list_cell(ErlNifEnv* env, ERL_NIF_TERM car, ERL_NIF_TERM cdr)
{
    Term* t = tnew(env, T_CONS);
    t->u.cons.head = car;
    t->u.cons.tail = cdr;
    return TT(t);
}

ERL_NIF_TERM enif_make_list_from_array(ErlNifEnv* env, const ERL_NIF_TERM arr[], unsigned cnt)
{
    ERL_NIF_TERM l = TT(&NIL_TERM);
    for(unsigned i = cnt; i > 0; i--) l = enif_make_list_cell(env, arr[i - 1], l);
    return l;
}

ERL_NIF_TERM enif_make_list(ErlNifEnv* env, unsigned cnt, ...)
{
    ERL_NIF_TERM* tmp = xmalloc(cnt * sizeof(ERL_NIF_TERM));
    va_list ap;
    va_start(ap, cnt);
    for(unsigned i = 0; i < cnt; i++) tmp[i] = va_arg(ap, ERL_NIF_TERM);
    va_end(ap);
    ERL_NIF_TERM r = enif_make_list_from_array(env, tmp, cnt);
    free(tmp);
    return r;
}

int enif_get_list_cell(ErlNifEnv* env, ERL_NIF_TERM term, ERL_NIF_TERM* head,
                       ERL_NIF_TERM* tail)
{
    (void)env;
    Term* t = TP(term);
    if(t->type != T_CONS) return 0;
    *head = t->u.cons.head;
    *tail = t->u.cons.tail;
    return 1;
}

int enif_get_list_length(ErlNifEnv* env, ERL_NIF_TERM term, unsigned* len)
{
    (void)env;
    unsigned n = 0;
    Term* t = TP(term);
    while(t->type == T_CONS) {
        n++;
        t = TP(t->u.cons.tail);
    }
    if(t->type != T_NIL) return 0;
    *len = n;
    return 1;
}

/* ---- maps -------------------------------------------------------------- */

ERL_NIF_TERM enif_make_new_map(ErlNifEnv* env)
{
    return TT(tnew(env, T_MAP));
}

typedef struct { ERL_NIF_TERM k, v; } KV;

static int kv_cmp(const void* a, const void* b)
{
    const KV* x = a;
    const KV* y = b;
    int c = tcmp(TP(x->k), TP(y->k), 0);
    if(c != 0) return c;
    return TP(x->k)->type < TP(y->k)->type ? -1 : TP(x->k)->type > TP(y->k)->type;
}

ERL_NIF_TERM mock_make_map_checked(ErlNifEnv* env, ERL_NIF_TERM* keys, ERL_NIF_TERM* vals,
                                   size_t cnt)
{
    ERL_NIF_TERM out;
    if(!enif_make_map_from_arrays(env, keys, vals, cnt, &out)) return 0;
    return out;
}

int enif_make_map_from_arrays(ErlNifEnv* env, ERL_NIF_TERM keys[], ERL_NIF_TERM values[],
                              size_t cnt, ERL_NIF_TERM* map_out)
{
    KV* tmp = xmalloc(cnt * sizeof(KV));
    for(size_t i = 0; i < cnt; i++) { tmp[i].k = keys[i]; tmp[i].v = values[i]; }
    qsort(tmp, cnt, sizeof(KV), kv_cmp);
    for(size_t i = 1; i < cnt; i++) {
        if(enif_is_identical(tmp[i - 1].k, tmp[i].k)) {
            free(tmp);
            return 0;
        }
    }
    Term* t = tnew(env, T_MAP);
    t->u.map.size = cnt;
    t->u.map.keys = xmalloc(cnt * sizeof(ERL_NIF_TERM));
    t->u.map.vals = xmalloc(cnt * sizeof(ERL_NIF_TERM));
    for(size_t i = 0; i < cnt; i++) {
        t->u.map.keys[i] = tmp[i].k;
        t->u.map.vals[i] = tmp[i].v;
    }
    free(tmp);
    *map_out = TT(t);
    return 1;
}

int enif_get_map_size(ErlNifEnv* env, ERL_NIF_TERM term, size_t* size)
{
    (void)env;
    if(TP(term)->type != T_MAP) return 0;
    *size = TP(term)->u.map.size;
    return 1;
}

int enif_map_iterator_create(ErlNifEnv* env, ERL_NIF_TERM map, ErlNifMapIterator* iter,
                             ErlNifMapIteratorEntry entry)
{
    (void)env;
    Term* t = TP(map);
    if(t->type != T_MAP) return 0;
    memset(iter, 0, sizeof(*iter));
    iter->map = map;
    iter->size = t->u.map.size;
    iter->u.flat.ks = t->u.map.keys;
    iter->u.flat.vs = t->u.map.vals;
    iter->idx = entry == ERL_NIF_MAP_ITERATOR_FIRST ? 0 : t->u.map.size - 1;
    return 1;
}

void enif_map_iterator_destroy(ErlNifEnv* env, ErlNifMapIterator* iter)
{
    (void)env;
    memset(iter, 0, sizeof(*iter));
}

int enif_map_iterator_get_pair(ErlNifEnv* env, ErlNifMapIterator* iter, ERL_NIF_TERM* key,
                               ERL_NIF_TERM* value)
{
    (void)env;
    if(iter->idx >= iter->size) return 0;
    *key = iter->u.flat.ks[iter->idx];
    *value = iter->u.flat.vs[iter->idx];
    return 1;
}

int enif_map_iterator_next(ErlNifEnv* env, ErlNifMapIterator* iter)
{
    (void)env;
    if(iter->idx < iter->size) iter->idx++;
    return iter->idx < iter->size;
}

/* ---- resources --------------------------------------------------------- */

ErlNifResourceType* enif_open_resource_type(ErlNifEnv* env, const char* module_str,
                                            const char* name_str,
                                            void (*dtor)(ErlNifEnv*, void*),
                                            ErlNifResourceFlags flags,
                                            ErlNifResourceFlags* tried)
{
    (void)env; (void)module_str; (void)flags; (void)tried;
    struct enif_resource_type_t* rt = xmalloc(sizeof(*rt));
    rt->name = name_str;
    rt->dtor = dtor;
    return rt;
}

void* enif_alloc_resource(ErlNifResourceType* type, size_t size)
{
    Res* r = xmalloc(sizeof(Res) + size);
    r->refs = 1;
    r->type = type;
    r->size = size;
    r->pad = 0;
    return (char*)r + sizeof(Res);
}

void enif_release_resource(void* obj)
{
    res_unref((Res*)((char*)obj - sizeof(Res)));
}

ERL_NIF_TERM enif_make_resource(ErlNifEnv* env, void* obj)
{
    Res* r = (Res*)((char*)obj - sizeof(Res));
    r->refs++;
    Term* t = tnew(env, T_RES);
    t->u.res = r;
    return TT(t);
}

int enif_get_resource(ErlNifEnv* env, ERL_NIF_TERM term, ErlNifResourceType* type, void** objp)
{
    (void)env;
    Term* t = TP(term);
    if(t->type != T_RES || t->u.res->type != type) return 0;
    *objp = (char*)t->u.res + sizeof(Res);
    return 1;
}

/* ---- scheduling and errors --------------------------------------------- */

ERL_NIF_TERM enif_make_badarg(ErlNifEnv* env)
{
    env->badarg = 1;
    return TT(&BADARG_TERM);
}

ERL_NIF_TERM enif_schedule_nif(ErlNifEnv* env, const char* fun_name, int flags, NifFp fp,
                               int argc, const ERL_NIF_TERM argv[])
{
    (void)fun_name; (void)flags;
    Term* t = tnew(env, T_SCHED);
    t->u.sched.fp = fp;
    t->u.sched.argc = argc;
    t->u.sched.argv = xmalloc(argc * sizeof(ERL_NIF_TERM));
    memcpy(t->u.sched.argv, argv, argc * sizeof(ERL_NIF_TERM));
    return TT(t);
}

ERL_NIF_TERM mock_trampoline(ErlNifEnv* env, ERL_NIF_TERM r)
{
    while(TP(r)->type == T_SCHED) {
        Term* s = TP(r);
        r = s->u.sched.fp(env, s->u.sched.argc, s->u.sched.argv);
    }
    return r;
}

#include "term_util.inc"
