#ifndef NIF_HOST_H
#define NIF_HOST_H

#include "enif_mock.h"

/* Loads jiffy's NIF library into the mock VM (idempotent). */
void host_init(void);

/* Builds an option list from "name" and "name=value" strings (value: integer or atom). */
ERL_NIF_TERM host_opts(ErlNifEnv* env, const char* const* names, int n);

/* Calls nif_decode_init/nif_encode_init and drives any yielded continuation to completion. */
ERL_NIF_TERM host_decode(ErlNifEnv* env, const unsigned char* json, size_t n, ERL_NIF_TERM opts);
ERL_NIF_TERM host_encode(ErlNifEnv* env, ERL_NIF_TERM term, ERL_NIF_TERM opts);

#endif
