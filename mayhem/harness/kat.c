/* Known-answer probe: decode JSON (stdin) or encode an Erlang external term (stdin) with the
 * real jiffy c_src running on the mock NIF host, and print the result.
 *   kat decode [opt|opt=v ...] < json     -> Erlang-style term text
 *   kat encode [opt|opt=v ...] < etf      -> JSON text */
#include "nif_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned char* slurp(size_t* n)
{
    size_t cap = 4096, len = 0;
    unsigned char* b = malloc(cap);
    size_t got;
    while((got = fread(b + len, 1, cap - len, stdin)) > 0) {
        len += got;
        if(len == cap) {
            cap *= 2;
            b = realloc(b, cap);
        }
    }
    *n = len;
    return b;
}

int main(int argc, char** argv)
{
    if(argc < 2) return 64;
    host_init();
    size_t n;
    unsigned char* in = slurp(&n);
    ErlNifEnv* env = mock_env_new();
    ERL_NIF_TERM opts = host_opts(env, (const char* const*)argv + 2, argc - 2);

    if(strcmp(argv[1], "decode") == 0) {
        ERL_NIF_TERM r = host_decode(env, in, n, opts);
        size_t tn;
        char* t = term_to_text(r, &tn);
        printf("%s\n", t);
        free(t);
    } else if(strcmp(argv[1], "encode") == 0) {
        ERL_NIF_TERM term = etf_decode(env, in, n);
        if(term == 0) {
            fprintf(stderr, "invalid external term\n");
            return 65;
        }
        ERL_NIF_TERM r = host_encode(env, term, opts);
        unsigned char* out;
        size_t on;
        if(encoded_to_bytes(env, r, &out, &on)) {
            fwrite(out, 1, on, stdout);
            printf("\n");
            free(out);
        } else {
            size_t tn;
            char* t = term_to_text(r, &tn);
            printf("%s\n", t);
            free(t);
        }
    } else {
        return 64;
    }
    mock_env_free(env);
    free(in);
    return 0;
}
