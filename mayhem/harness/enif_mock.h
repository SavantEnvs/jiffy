/* Minimal in-process model of the Erlang NIF API, enough to run jiffy's c_src
 * (decoder/encoder) without a BEAM. Terms are heap nodes owned by an env. */
#ifndef ENIF_MOCK_H
#define ENIF_MOCK_H

#include "erl_nif.h"
#include <stddef.h>
#include <stdint.h>

ErlNifEnv* mock_env_new(void);
void mock_env_free(ErlNifEnv* env);
int mock_badarg_raised(ErlNifEnv* env);
void mock_set_priv(void* priv);
ERL_NIF_TERM mock_nil(void);

/* Re-enters scheduled NIF continuations until a real result is returned. */
ERL_NIF_TERM mock_trampoline(ErlNifEnv* env, ERL_NIF_TERM t);

ERL_NIF_TERM mock_make_atom_utf8(ErlNifEnv* env, const unsigned char* s, size_t n);
ERL_NIF_TERM mock_make_atom_latin1(ErlNifEnv* env, const unsigned char* s, size_t n);
ERL_NIF_TERM mock_make_bits(ErlNifEnv* env, const unsigned char* s, size_t n, unsigned lastbits);
ERL_NIF_TERM mock_make_blob(ErlNifEnv* env, int is_ref, const unsigned char* s, size_t n);
ERL_NIF_TERM mock_make_big(ErlNifEnv* env, int neg, const unsigned char* le_mag, size_t n);
ERL_NIF_TERM mock_make_map_checked(ErlNifEnv* env, ERL_NIF_TERM* keys, ERL_NIF_TERM* vals,
                                   size_t cnt);

int mock_is_tuple(ERL_NIF_TERM t, unsigned* arity, const ERL_NIF_TERM** el);
int mock_atom_is(ERL_NIF_TERM t, const char* name);


/* ETF input decoding (version byte optional); 0 if invalid. Trailing bytes are ignored. */
ERL_NIF_TERM etf_decode(ErlNifEnv* env, const unsigned char* p, size_t n);

/* Erlang-style text of a term (malloc'd, NUL terminated). Iterative: any depth. */
char* term_to_text(ERL_NIF_TERM t, size_t* len);

/* Flatten a jiffy encode result (reversed iolist or {partial, IOList}) to bytes. */
int encoded_to_bytes(ErlNifEnv* env, ERL_NIF_TERM res, unsigned char** out, size_t* n);

/* True if some tuple {Tag, _} with Tag == a or b occurs anywhere inside t. */
int term_contains_tagged(ERL_NIF_TERM t, const char* a, const char* b);

#endif
