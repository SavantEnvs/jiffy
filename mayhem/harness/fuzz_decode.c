/* libFuzzer target: jiffy's JSON decoder, run in-process on the mock NIF host.
 * Input: one flag byte selecting decode options, then the JSON text. */
#include "nif_host.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die(const char* msg, ERL_NIF_TERM a, ERL_NIF_TERM b)
{
    size_t n;
    char* ta = term_to_text(a, &n);
    fprintf(stderr, "fuzz_decode: %s\n  first : %.1500s\n", msg, ta);
    free(ta);
    if(b != 0) {
        char* tb = term_to_text(b, &n);
        fprintf(stderr, "  second: %.1500s\n", tb);
        free(tb);
    }
    abort();
}

static int is_error_or_partial(ERL_NIF_TERM r)
{
    unsigned arity;
    const ERL_NIF_TERM* el;
    return mock_is_tuple(r, &arity, &el) && arity == 2 &&
           (mock_atom_is(el[0], "error") || mock_atom_is(el[0], "partial"));
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if(size < 1) return -1;
    host_init();

    unsigned flags = data[0];
    const char* names[8];
    int n = 0;
    if(flags & 1) names[n++] = "return_maps";
    if(flags & 2) names[n++] = "return_trailer";
    if(flags & 4) names[n++] = "dedupe_keys";
    if(flags & 8) names[n++] = "copy_strings";
    if(flags & 16) names[n++] = "use_nil";
    if(flags & 32) names[n++] = "bytes_per_red=1";
    if(flags & 64) names[n++] = "null_term=foo";

    ErlNifEnv* env = mock_env_new();
    ERL_NIF_TERM opts = host_opts(env, names, n);
    ERL_NIF_TERM r = host_decode(env, data + 1, size - 1, opts);

    if(!mock_badarg_raised(env)) {
        size_t tn;
        free(term_to_text(r, &tn));

        /* decode -> encode -> decode must reproduce the first result. */
        if(!(flags & (2 | 64)) && !is_error_or_partial(r) &&
           !term_contains_tagged(r, "bignum", "bigdbl")) {
            const char* enames[2];
            int en = 0;
            if(flags & 16) enames[en++] = "use_nil";
            if(flags & 32) enames[en++] = "bytes_per_red=1";
            ERL_NIF_TERM e = host_encode(env, r, host_opts(env, enames, en));
            unsigned char* out = NULL;
            size_t outn = 0;
            if(!is_error_or_partial(e) || mock_atom_is(e, "partial")) {
                unsigned arity;
                const ERL_NIF_TERM* el;
                int is_err = mock_is_tuple(e, &arity, &el) && arity == 2 &&
                             mock_atom_is(el[0], "error");
                if(!is_err) {
                    if(!encoded_to_bytes(env, e, &out, &outn)) die("unflattenable encode result", e, 0);
                    ERL_NIF_TERM r2 = host_decode(env, out, outn, opts);
                    free(out);
                    if(enif_compare(r, r2) != 0) die("decode(encode(decode(x))) != decode(x)", r, r2);
                }
            }
        }
    }
    mock_env_free(env);
    return 0;
}
