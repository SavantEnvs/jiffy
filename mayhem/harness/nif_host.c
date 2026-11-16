/* Hosts jiffy's NIF (compiled in as one unity translation unit) on top of the mock VM. */
#include "../../c_src/jiffy.c"

#include "nif_host.h"

#include <ctype.h>
#include <stdlib.h>

static ErlNifEntry* g_entry;

void host_init(void)
{
    if(g_entry != NULL) return;
    g_entry = nif_init();
    ErlNifEnv* load_env = mock_env_new();
    void* priv = NULL;
    if(g_entry->load(load_env, &priv, mock_nil()) != 0) abort();
    mock_set_priv(priv);
}

static ERL_NIF_TERM host_call(ErlNifEnv* env, const char* name, ERL_NIF_TERM a0, ERL_NIF_TERM a1)
{
    for(int i = 0; i < g_entry->num_of_funcs; i++) {
        if(strcmp(g_entry->funcs[i].name, name) == 0) {
            ERL_NIF_TERM argv[2] = { a0, a1 };
            return mock_trampoline(env, g_entry->funcs[i].fptr(env, 2, argv));
        }
    }
    abort();
}

ERL_NIF_TERM host_opts(ErlNifEnv* env, const char* const* names, int n)
{
    ERL_NIF_TERM l = mock_nil();
    for(int i = n; i > 0; i--) {
        const char* s = names[i - 1];
        const char* eq = strchr(s, '=');
        ERL_NIF_TERM t;
        if(eq == NULL) {
            t = enif_make_atom(env, s);
        } else {
            char key[64];
            size_t kn = (size_t)(eq - s);
            if(kn >= sizeof key) abort();
            memcpy(key, s, kn);
            key[kn] = 0;
            ERL_NIF_TERM v = isdigit((unsigned char)eq[1]) ? enif_make_long(env, atol(eq + 1))
                                                           : enif_make_atom(env, eq + 1);
            t = enif_make_tuple2(env, enif_make_atom(env, key), v);
        }
        l = enif_make_list_cell(env, t, l);
    }
    return l;
}

ERL_NIF_TERM host_decode(ErlNifEnv* env, const unsigned char* json, size_t n, ERL_NIF_TERM opts)
{
    ERL_NIF_TERM bin;
    unsigned char* d = enif_make_new_binary(env, n, &bin);
    if(n) memcpy(d, json, n);
    return host_call(env, "nif_decode_init", bin, opts);
}

ERL_NIF_TERM host_encode(ErlNifEnv* env, ERL_NIF_TERM term, ERL_NIF_TERM opts)
{
    return host_call(env, "nif_encode_init", term, opts);
}
