/* Included at the end of enif_mock.c: ETF reader, term printer, iolist flattening. */

/* ---- ETF reader -------------------------------------------------------- */

typedef struct {
    ErlNifEnv* env;
    const unsigned char* p;
    const unsigned char* end;
} Rd;

#define ETF_MAX_DEPTH 200

static size_t rd_left(const Rd* r) { return (size_t)(r->end - r->p); }

static int rd_u(Rd* r, unsigned nbytes, uint32_t* out)
{
    if(rd_left(r) < nbytes) return 0;
    uint32_t v = 0;
    for(unsigned i = 0; i < nbytes; i++) v = (v << 8) | *r->p++;
    *out = v;
    return 1;
}

/* Strict UTF-8; returns number of code points, or -1 when malformed. */
static long utf8_count(const unsigned char* s, size_t n)
{
    long cps = 0;
    for(size_t i = 0; i < n; cps++) {
        unsigned c = s[i];
        size_t extra;
        unsigned cp;
        if(c < 0x80) { extra = 0; cp = c; }
        else if(c >= 0xC2 && c <= 0xDF) { extra = 1; cp = c & 0x1F; }
        else if(c >= 0xE0 && c <= 0xEF) { extra = 2; cp = c & 0x0F; }
        else if(c >= 0xF0 && c <= 0xF4) { extra = 3; cp = c & 0x07; }
        else return -1;
        if(extra > n - i - 1) return -1;
        for(size_t k = 1; k <= extra; k++) {
            if((s[i + k] & 0xC0) != 0x80) return -1;
            cp = (cp << 6) | (s[i + k] & 0x3F);
        }
        if(extra == 2 && cp < 0x800) return -1;
        if(extra == 3 && (cp < 0x10000 || cp > 0x10FFFF)) return -1;
        if(cp >= 0xD800 && cp <= 0xDFFF) return -1;
        i += extra + 1;
    }
    return cps;
}

static ERL_NIF_TERM rd_term(Rd* r, int depth);

static ERL_NIF_TERM rd_atom_term(Rd* r, unsigned lenbytes, int utf8)
{
    uint32_t len;
    if(!rd_u(r, lenbytes, &len) || rd_left(r) < len) return 0;
    const unsigned char* s = r->p;
    r->p += len;
    if(utf8) {
        long cps = utf8_count(s, len);
        if(cps < 0 || cps > 255) return 0;
        return mock_make_atom_utf8(r->env, s, len);
    }
    if(len > 255) return 0;
    return mock_make_atom_latin1(r->env, s, len);
}

static ERL_NIF_TERM rd_term(Rd* r, int depth)
{
    uint32_t tag, a, b;
    if(depth > ETF_MAX_DEPTH || !rd_u(r, 1, &tag)) return 0;
    switch(tag) {
    case 97:
        if(!rd_u(r, 1, &a)) return 0;
        return enif_make_long(r->env, (long)a);
    case 98:
        if(!rd_u(r, 4, &a)) return 0;
        return enif_make_long(r->env, (long)(int32_t)a);
    case 70: {
        if(rd_left(r) < 8) return 0;
        uint64_t bits = 0;
        for(int i = 0; i < 8; i++) bits = (bits << 8) | *r->p++;
        double d;
        memcpy(&d, &bits, sizeof d);
        if(!isfinite(d)) return 0;
        return enif_make_double(r->env, d);
    }
    case 110:
    case 111: {
        if(!rd_u(r, tag == 110 ? 1 : 4, &a) || !rd_u(r, 1, &b) || b > 1) return 0;
        if(rd_left(r) < a) return 0;
        const unsigned char* mag = r->p;
        r->p += a;
        return mock_make_big(r->env, (int)b, mag, a);
    }
    case 100: return rd_atom_term(r, 2, 0);
    case 115: return rd_atom_term(r, 1, 0);
    case 118: return rd_atom_term(r, 2, 1);
    case 119: return rd_atom_term(r, 1, 1);
    case 106: return mock_nil();
    case 109: {
        if(!rd_u(r, 4, &a) || rd_left(r) < a) return 0;
        ERL_NIF_TERM t;
        unsigned char* d = enif_make_new_binary(r->env, a, &t);
        if(a) memcpy(d, r->p, a);
        r->p += a;
        return t;
    }
    case 77: {
        if(!rd_u(r, 4, &a) || !rd_u(r, 1, &b) || b < 1 || b > 8 || a == 0) return 0;
        if(rd_left(r) < a) return 0;
        ERL_NIF_TERM t = mock_make_bits(r->env, r->p, a, b);
        r->p += a;
        return t;
    }
    case 107: {
        if(!rd_u(r, 2, &a) || rd_left(r) < a) return 0;
        ERL_NIF_TERM l = mock_nil();
        for(uint32_t i = a; i > 0; i--) {
            l = enif_make_list_cell(r->env, enif_make_long(r->env, r->p[i - 1]), l);
        }
        r->p += a;
        return l;
    }
    case 108: {
        if(!rd_u(r, 4, &a) || rd_left(r) < a) return 0;
        ERL_NIF_TERM* el = xmalloc((size_t)a * sizeof(ERL_NIF_TERM));
        ERL_NIF_TERM tail = 0;
        int ok = 1;
        for(uint32_t i = 0; i < a && ok; i++) {
            el[i] = rd_term(r, depth + 1);
            ok = el[i] != 0;
        }
        if(ok) {
            tail = rd_term(r, depth + 1);
            ok = tail != 0;
        }
        if(ok) {
            for(uint32_t i = a; i > 0; i--) tail = enif_make_list_cell(r->env, el[i - 1], tail);
        }
        free(el);
        return ok ? tail : 0;
    }
    case 104:
    case 105: {
        if(!rd_u(r, tag == 104 ? 1 : 4, &a) || rd_left(r) < a) return 0;
        ERL_NIF_TERM* el = xmalloc((size_t)a * sizeof(ERL_NIF_TERM));
        int ok = 1;
        for(uint32_t i = 0; i < a && ok; i++) {
            el[i] = rd_term(r, depth + 1);
            ok = el[i] != 0;
        }
        ERL_NIF_TERM t = ok ? enif_make_tuple_from_array(r->env, el, a) : 0;
        free(el);
        return t;
    }
    case 116: {
        if(!rd_u(r, 4, &a) || rd_left(r) / 2 < a) return 0;
        ERL_NIF_TERM* ks = xmalloc((size_t)a * sizeof(ERL_NIF_TERM));
        ERL_NIF_TERM* vs = xmalloc((size_t)a * sizeof(ERL_NIF_TERM));
        int ok = 1;
        for(uint32_t i = 0; i < a && ok; i++) {
            ks[i] = rd_term(r, depth + 1);
            ok = ks[i] != 0;
            if(ok) {
                vs[i] = rd_term(r, depth + 1);
                ok = vs[i] != 0;
            }
        }
        ERL_NIF_TERM t = ok ? mock_make_map_checked(r->env, ks, vs, a) : 0;
        free(ks);
        free(vs);
        return t;
    }
    case 88: {
        ERL_NIF_TERM node = rd_term(r, depth + 1);
        if(node == 0 || TP(node)->type != T_ATOM || rd_left(r) < 12) return 0;
        ERL_NIF_TERM t = mock_make_blob(r->env, 0, r->p, 12);
        r->p += 12;
        return t;
    }
    case 90: {
        if(!rd_u(r, 2, &a) || a > 5) return 0;
        ERL_NIF_TERM node = rd_term(r, depth + 1);
        if(node == 0 || TP(node)->type != T_ATOM) return 0;
        size_t body = 4 + (size_t)a * 4;
        if(rd_left(r) < body) return 0;
        ERL_NIF_TERM t = mock_make_blob(r->env, 1, r->p, body);
        r->p += body;
        return t;
    }
    default:
        return 0;
    }
}

ERL_NIF_TERM etf_decode(ErlNifEnv* env, const unsigned char* p, size_t n)
{
    Rd r = { env, p, p + n };
    if(n > 0 && p[0] == 131) r.p++;
    return rd_term(&r, 0);
}

/* ---- text output ------------------------------------------------------- */

typedef struct { char* s; size_t n, cap; } Buf;

static void buf_put(Buf* b, const char* s, size_t n)
{
    if(b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 256;
        while(nc < b->n + n + 1) nc *= 2;
        char* ns = realloc(b->s, nc);
        if(ns == NULL) abort();
        b->s = ns;
        b->cap = nc;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

static void buf_str(Buf* b, const char* s) { buf_put(b, s, strlen(s)); }

static void put_quoted(Buf* b, const unsigned char* s, size_t n, char q)
{
    char tmp[8];
    buf_put(b, &q, 1);
    for(size_t i = 0; i < n; i++) {
        unsigned c = s[i];
        if(c == (unsigned)q || c == '\\') {
            tmp[0] = '\\';
            tmp[1] = (char)c;
            buf_put(b, tmp, 2);
        } else if(c >= 0x20 && c < 0x7F) {
            tmp[0] = (char)c;
            buf_put(b, tmp, 1);
        } else {
            snprintf(tmp, sizeof tmp, "\\x%02X", c);
            buf_put(b, tmp, 4);
        }
    }
    buf_put(b, &q, 1);
}

static int atom_is_plain(const Term* t)
{
    if(t->u.atom.len == 0) return 0;
    unsigned char c0 = t->u.atom.name[0];
    if(!(c0 >= 'a' && c0 <= 'z')) return 0;
    for(size_t i = 0; i < t->u.atom.len; i++) {
        unsigned char c = t->u.atom.name[i];
        if(!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
             c == '_' || c == '@')) return 0;
    }
    return 1;
}

typedef struct { int lit; Term* t; const char* s; } Item;
typedef struct { Item* v; size_t n, cap; } IStack;

static void is_push(IStack* s, int lit, Term* t, const char* str)
{
    if(s->n == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 64;
        s->v = realloc(s->v, s->cap * sizeof(Item));
        if(s->v == NULL) abort();
    }
    s->v[s->n].lit = lit;
    s->v[s->n].t = t;
    s->v[s->n].s = str;
    s->n++;
}

char* term_to_text(ERL_NIF_TERM root, size_t* len)
{
    Buf b = { NULL, 0, 0 };
    IStack st = { NULL, 0, 0 };
    char tmp[64];
    buf_put(&b, "", 0);
    is_push(&st, 0, TP(root), NULL);

    while(st.n > 0) {
        Item it = st.v[--st.n];
        if(it.lit) { buf_str(&b, it.s); continue; }
        Term* t = it.t;
        switch(t->type) {
        case T_ATOM:
            if(atom_is_plain(t)) buf_put(&b, (char*)t->u.atom.name, t->u.atom.len);
            else put_quoted(&b, t->u.atom.name, t->u.atom.len, '\'');
            break;
        case T_INT:
            snprintf(tmp, sizeof tmp, "%ld", t->u.i);
            buf_str(&b, tmp);
            break;
        case T_BIG:
            snprintf(tmp, sizeof tmp, "<<big %s%zu bytes>>", t->u.big.neg ? "-" : "", t->u.big.n);
            buf_str(&b, tmp);
            break;
        case T_FLOAT:
            for(int prec = 15; prec <= 17; prec++) {
                snprintf(tmp, sizeof tmp, "%.*g", prec, t->u.d);
                if(strtod(tmp, NULL) == t->u.d) break;
            }
            if(strpbrk(tmp, ".en") == NULL) strcat(tmp, ".0");
            buf_str(&b, tmp);
            break;
        case T_BIN:
            buf_str(&b, "<<");
            put_quoted(&b, t->u.bin.data, t->u.bin.size, '"');
            if(t->u.bin.lastbits != 8) {
                snprintf(tmp, sizeof tmp, ":%u", t->u.bin.lastbits);
                buf_str(&b, tmp);
            }
            buf_str(&b, ">>");
            break;
        case T_PID: buf_str(&b, "<pid>"); break;
        case T_REF: buf_str(&b, "<ref>"); break;
        case T_NIL: buf_str(&b, "[]"); break;
        case T_BADARG: buf_str(&b, "<badarg>"); break;
        case T_TUPLE:
            buf_str(&b, "{");
            is_push(&st, 1, NULL, "}");
            for(unsigned i = t->u.tup.arity; i > 0; i--) {
                is_push(&st, 0, TP(t->u.tup.el[i - 1]), NULL);
                if(i > 1) is_push(&st, 1, NULL, ",");
            }
            break;
        case T_MAP:
            buf_str(&b, "#{");
            is_push(&st, 1, NULL, "}");
            for(size_t i = t->u.map.size; i > 0; i--) {
                is_push(&st, 0, TP(t->u.map.vals[i - 1]), NULL);
                is_push(&st, 1, NULL, " => ");
                is_push(&st, 0, TP(t->u.map.keys[i - 1]), NULL);
                if(i > 1) is_push(&st, 1, NULL, ", ");
            }
            break;
        case T_CONS: {
            size_t count = 0;
            Term* c = t;
            while(c->type == T_CONS) { count++; c = TP(c->u.cons.tail); }
            buf_str(&b, "[");
            is_push(&st, 1, NULL, "]");
            if(c->type != T_NIL) {
                is_push(&st, 0, c, NULL);
                is_push(&st, 1, NULL, "|");
            }
            /* push elements last-to-first: collect then iterate backwards */
            Term** cells = xmalloc(count * sizeof(Term*));
            c = t;
            for(size_t i = 0; i < count; i++) { cells[i] = c; c = TP(c->u.cons.tail); }
            for(size_t i = count; i > 0; i--) {
                is_push(&st, 0, TP(cells[i - 1]->u.cons.head), NULL);
                if(i > 1) is_push(&st, 1, NULL, ",");
            }
            free(cells);
            break;
        }
        default:
            buf_str(&b, "<?>");
            break;
        }
    }
    free(st.v);
    *len = b.n;
    return b.s;
}

/* ---- encoder output flattening ----------------------------------------- */

static int append_iodata(Buf* out, ERL_NIF_TERM root)
{
    IStack st = { NULL, 0, 0 };
    int ok = 1;
    is_push(&st, 0, TP(root), NULL);
    while(st.n > 0 && ok) {
        Term* t = st.v[--st.n].t;
        switch(t->type) {
        case T_BIN:
            if(t->u.bin.lastbits != 8) { ok = 0; break; }
            buf_put(out, (char*)t->u.bin.data, t->u.bin.size);
            break;
        case T_INT:
            if(t->u.i < 0 || t->u.i > 255) { ok = 0; break; }
            { char c = (char)t->u.i; buf_put(out, &c, 1); }
            break;
        case T_NIL: break;
        case T_CONS:
            is_push(&st, 0, TP(t->u.cons.tail), NULL);
            is_push(&st, 0, TP(t->u.cons.head), NULL);
            break;
        default: ok = 0; break;
        }
    }
    free(st.v);
    return ok;
}

int encoded_to_bytes(ErlNifEnv* env, ERL_NIF_TERM res, unsigned char** out, size_t* n)
{
    (void)env;
    Term* t = TP(res);
    if(t->type == T_TUPLE && t->u.tup.arity == 2 && mock_atom_is(t->u.tup.el[0], "partial")) {
        t = TP(t->u.tup.el[1]);
    }
    size_t count = 0;
    for(Term* c = t; c->type == T_CONS; c = TP(c->u.cons.tail)) count++;
    Term** cells = xmalloc(count * sizeof(Term*));
    Term* c = t;
    for(size_t i = 0; i < count; i++) { cells[i] = c; c = TP(c->u.cons.tail); }
    int ok = c->type == T_NIL;
    Buf b = { NULL, 0, 0 };
    buf_put(&b, "", 0);
    /* the list is in reverse output order */
    for(size_t i = count; i > 0 && ok; i--) {
        Term* e = TP(cells[i - 1]->u.cons.head);
        if(e->type == T_BIN || e->type == T_CONS || e->type == T_NIL) {
            ok = append_iodata(&b, TT(e));
        } else if(e->type == T_INT) {
            char tmp[32];
            snprintf(tmp, sizeof tmp, "%ld", e->u.i);
            buf_str(&b, tmp);
        } else if(e->type == T_TUPLE && e->u.tup.arity == 2 && mock_atom_is(e->u.tup.el[0], "json")) {
            ok = append_iodata(&b, e->u.tup.el[1]);
        } else {
            ok = 0;
        }
    }
    free(cells);
    if(!ok) { free(b.s); return 0; }
    *out = (unsigned char*)b.s;
    *n = b.n;
    return 1;
}

int term_contains_tagged(ERL_NIF_TERM root, const char* a, const char* b)
{
    IStack st = { NULL, 0, 0 };
    int found = 0;
    is_push(&st, 0, TP(root), NULL);
    while(st.n > 0 && !found) {
        Term* t = st.v[--st.n].t;
        switch(t->type) {
        case T_TUPLE:
            if(t->u.tup.arity == 2 &&
               (mock_atom_is(t->u.tup.el[0], a) || (b && mock_atom_is(t->u.tup.el[0], b)))) {
                found = 1;
                break;
            }
            for(unsigned i = 0; i < t->u.tup.arity; i++) is_push(&st, 0, TP(t->u.tup.el[i]), NULL);
            break;
        case T_CONS:
            is_push(&st, 0, TP(t->u.cons.tail), NULL);
            is_push(&st, 0, TP(t->u.cons.head), NULL);
            break;
        case T_MAP:
            for(size_t i = 0; i < t->u.map.size; i++) {
                is_push(&st, 0, TP(t->u.map.keys[i]), NULL);
                is_push(&st, 0, TP(t->u.map.vals[i]), NULL);
            }
            break;
        default: break;
        }
    }
    free(st.v);
    return found;
}
