# Encoding an internal marker atom aborts the VM

`jiffy:encode('$object_ref$')` (likewise `'$array_ref$'` and `'$map_ref$'`) hits
`assert(stack->top > 0 && stack->top <= stack->size)` in `termstack_pop` (`c_src/encoder.c:156`) and
aborts the whole Erlang VM (verified against the real NIF on OTP 27: `beam.smp: ... Assertion ... failed`).

## Cause
`enc_iter` (`c_src/encoder.c`, around line 906) pushes work items and the three marker atoms on the
same term stack and tells them apart with `enif_is_identical(curr, e->atoms->ref_object|ref_array|ref_map)`.
A caller-supplied atom with the same name is indistinguishable from the marker, so the encoder pops
state that was never pushed.

## Impact
A user-controlled atom (e.g. from `binary_to_atom/2` on untrusted input, or any data path that reaches
`jiffy:encode/1`) crashes the node (jiffy's rebar build leaves asserts enabled).

## Reproducers
`object_ref.bin`, `array_ref.bin`, `map_ref.bin`: input for `fuzz_encode` (flag byte `00`, then the
external term format of the atom). `./fuzz_encode-standalone mayhem/fuzz_encode/known-findings/internal-marker-atom-assert/object_ref.bin`

## Upstream fix
Use markers a caller cannot produce, e.g. a private tagged tuple or a resource/integer sentinel kept in
the `Encoder` struct, or reject those three atoms in the term-dispatch before the marker checks.
