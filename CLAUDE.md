# Project conventions

## Build / test policy

Never invoke compilers or test runners. No `gcc`, `clang`, `cl.exe`, `cmake`, `ctest`, `make`, no `-fsyntax-only` probes, no `run` of any built binary. User runs builds and tests themselves and pastes errors back when needed. Skip syntax-check shortcuts too — they count as testing.

## No explicit `_pad` fields in structs

Never write `_pad` / `_padN` / `_r` / similar dummy filler fields inside structs. The C compiler inserts the same padding bytes automatically to satisfy member alignment — the explicit field adds noise without changing layout.

Applies to:
- Trailing padding after the last field (the compiler rounds the struct size up to alignment).
- Middle padding between a smaller type and a more-strictly-aligned successor (e.g. `uint8_t kind;` followed by `uint32_t name_hash;` — compiler emits 3 bytes between them automatically).
- Union variants — anonymous unions follow the same auto-padding rules.

If a specific layout matters (wire format, ABI, cross-language struct sharing), use `static_assert(offsetof(...) == N)` and `static_assert(sizeof(T) == N)` to lock it down — those assertions verify the compiler's automatic padding produced the intended layout, instead of an explicit `_pad` field pretending to control it.

Document hot/cold ordering with comments at the field site, not with named padding.

## Serializer

Prefer `ecs_serializer_write_bits(s, value, n_bits)` / `ecs_deserializer_read_bits(d, n_bits)` when:
- payload size is fixed at compile time
- payload is `<= 64 bits`
- caller does not require byte alignment after the write

`write_bytes` / `read_bytes` carry overhead (alignment fixup + memcpy machinery) that is wasted on tiny fixed-size fields. Using `write_bits` for single-byte headers (version, flags) also avoids GCC `-Wstringop-overread` false positives on stack-local 1-byte sources passed to the inlined memcpy path.
