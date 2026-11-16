/* libFuzzer target: jiffy's JSON encoder, run in-process on the mock NIF host.
 * Input: one flag byte selecting encode options, then an Erlang external term
 * (term_to_binary format) that is handed to the encoder as the value to encode. */
#include "nif_host.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die(const char* msg, const unsigned char* out, size_t outn, ERL_NIF_TERM t)
{
    size_t n;
    char* tt = term_to_text(t, &n);
    fprintf(stderr, "fuzz_encode: %s\n  term  : %.1500s\n  output: %.1500s\n", msg, tt,
            out ? (const char*)out : "");
    (void)outn;
    free(tt);
    abort();
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if(size < 2) return -1;
    host_init();

    unsigned flags = data[0];
    ErlNifEnv* env = mock_env_new();
    ERL_NIF_TERM term = etf_decode(env, data + 1, size - 1);
    if(term == 0) {
        mock_env_free(env);
        return -1;
    }

    const char* names[8];
    int n = 0;
    if(flags & 1) names[n++] = "uescape";
    if(flags & 2) names[n++] = "pretty";
    if(flags & 4) names[n++] = "escape_forward_slashes";
    if(flags & 8) names[n++] = "use_nil";
    if(flags & 16) names[n++] = "bytes_per_red=1";
    if(flags & 32) names[n++] = "force_utf8";

    ERL_NIF_TERM r = host_encode(env, term, host_opts(env, names, n));
    if(!mock_badarg_raised(env)) {
        unsigned arity;
        const ERL_NIF_TERM* el;
        int is_err = mock_is_tuple(r, &arity, &el) && arity == 2 && mock_atom_is(el[0], "error");
        size_t tn;
        if(is_err) {
            free(term_to_text(r, &tn));
        } else {
            unsigned char* out = NULL;
            size_t outn = 0;
            if(!encoded_to_bytes(env, r, &out, &outn)) {
                /* {partial, _} hands terms the NIF can't encode (bignums, odd tuples) back to Erlang. */
                if(mock_is_tuple(r, &arity, &el) && arity == 2 && mock_atom_is(el[0], "partial")) {
                    free(term_to_text(r, &tn));
                    mock_env_free(env);
                    return 0;
                }
                die("unflattenable encode result", NULL, 0, r);
            }

            /* Whatever the encoder emits (short of spliced {json, _} fragments) must parse. */
            if(!term_contains_tagged(term, "json", NULL)) {
                ERL_NIF_TERM d = host_decode(env, out, outn, mock_nil());
                if(mock_is_tuple(d, &arity, &el) && arity == 2 && mock_atom_is(el[0], "error")) {
                    die("encoder output does not decode", out, outn, term);
                }
            }
            free(out);
        }
    }
    mock_env_free(env);
    return 0;
}
