# Input + Command Layout

Bit-level reference for the scripting layer's input + command data —
both on the wire and in memory. Companion to [scripting.md §2.6](scripting.md);
this file is the single place to look up exact bit positions, sizes,
and pack/unpack ordering.

All packing is integer-only and bit-deterministic across architectures
and rollback. Wire = pure bit stream (no byte padding inside records).
Memory = byte-stride C structs (trailing pad bits zero, never read).

---

## 1. Player input

One `player_input_t` record per connected player. Layout shared across
all players in the game (single `input { }` decl).

### 1.1 Game-defined layout

```
input {
    button fire
    button jump
    stick  move
    stick  aim
}
```

- `N` = button count (e.g. 2)
- `M` = stick  count (e.g. 2)

Slot index inside each section = declaration order (0-based).

### 1.2 Memory layout (per player)

Byte-aligned record, stride = `ceil((2*N + 32*M) / 8)` bytes.

```
bit  0          N         2N                                       2N + 32M
 |          |          |                                                |
 v          v          v                                                v
 [ down N ][ was_dn N ][ x0:16  y0:16  x1:16  y1:16  ...  xM-1  yM-1 ][ pad : 0..7 ]
```

Bit offsets:

| Slot                  | Bit offset            | Width | Encoding         |
|-----------------------|-----------------------|-------|------------------|
| `button.down[i]`      | `i`                   | 1     | bit              |
| `button.was_down[i]`  | `N + i`               | 1     | bit              |
| `stick[i].x`          | `2*N + 32*i`          | 16    | signed Q1.15     |
| `stick[i].y`          | `2*N + 32*i + 16`     | 16    | signed Q1.15     |

`was_down` is **engine-local memory only** — never crossed the network.

### 1.3 Wire layout (per player)

**Wire = memory.** Identical bit layout, no transform.

`bit_len = 2*N + 32*M`. Both `down` and `was_down` carried.

```
bit  0          N         2N                                       2N + 32M
 |          |          |                                                |
 v          v          v                                                v
 [ down N ][ was_dn N ][ x0:16  y0:16  x1:16  y1:16  ...  xM-1  yM-1 ]
```

Registered with the input type registry; `cmd_type_bits[input_type_id] = 2*N + 32*M`.

### 1.4 Tick-roll (sender-side only)

Sender computes the next frame's `was_down` from its own previous `down`
before serializing:

- `N % 8 == 0`: single `memcpy(record + N/8, record, N/8)`.
- Otherwise: bit-copy `[0..N) → [N..2N)`. Helpers in
  `ecs_serializer.h` + `ecs_player_input.h`.

After roll, the sender writes the current frame's new `down` bits + sticks,
then ships the whole record (both sections) over the wire. The receiver
does **no** tick-roll — `was_down` arrives explicitly each tick.

### 1.5 Derived button states

Pure functions of the two memory bits:

| Access                 | Formula                |
|------------------------|------------------------|
| `input.X.down`         | `down`                 |
| `input.X.pressed`      | `down & !was_down`     |
| `input.X.released`     | `!down & was_down`     |

State table:

| down | was_down | label   | pressed | down (out) | released |
|:----:|:--------:|---------|:-------:|:----------:|:--------:|
| 0    | 0        | idle    |    0    |     0      |    0     |
| 1    | 0        | rising  |    1    |     1      |    0     |
| 1    | 1        | held    |    0    |     1      |    0     |
| 0    | 1        | falling |    0    |     0      |    1     |

### 1.6 Stick encoding

Sticks are signed Q1.15 `(x, y)` — range `[-1.0, +0.99997]`, step `1/32768`.
Q1.15 → Q16.16 on read = sign-extending left-shift by 1.

| Q1.15 value | Q16.16 value | Meaning           |
|-------------|---------------|-------------------|
|   32767     |     65534     | nearly +1         |
|       0     |         0     | center            |
|  -32768     |    -65536     | exactly -1 (= -FIXED_ONE) |

Magnitude / angle derived lazily (integer sqrt / atan2 from `ecs_fixed.h`),
cached per tick.

### 1.7 Sizing examples

| Layout                | Wire bits | Memory bytes |
|-----------------------|----------:|-------------:|
| 1 btn, 0 sticks       |         1 |          1 B |
| 8 btn, 0 sticks       |         8 |          2 B |
| 4 btn, 1 stick        |        36 |          5 B |
| 8 btn, 1 stick        |        40 |          6 B |
| 8 btn, 2 sticks       |        72 |         10 B |
| 16 btn, 2 sticks      |        80 |         12 B |
| 1 btn, 1 stick        |        33 |          5 B |

### 1.8 Multi-player packet packing

Wire is bit-contiguous across players; only the whole packet rounds up
to a byte boundary at the very end.

```
bit 0      16          16+L      16+2L                    16+P*L
 |          |             |          |                        |
 v          v             v          v                        v
 [tick_id:16][player_0:L ][player_1:L]...[player_P-1:L      ][pad:0..7]
```

Where `L = bit_len`, `P = player count`. Packet bytes =
`ceil((16 + P * L) / 8)`.

Sizing at 60 Hz, 16 players × (16 btn + 2 sticks + 3D pos Q.16):
- Per-player L = 80 + 48 = 128 bits
- Packet = 290 B
- 290 × 60 ≈ 17.4 KB/s = 139 kbit/s. No ack channel exists in
  `ecs_input.h`, so no per-receiver delta encoding — every packet is
  self-contained within its R+1 cascade window. The chained-delta
  cascade IS the redundancy mechanism (each cascade tick diffed
  against the previously-emitted tick in the same packet).

---

## 2. Commands

Script commands ride `ecs_input.h`'s existing command-stream channel.
**Single** outer `ecs_input` type_id is reserved for all script commands;
the script's own command id is packed *inside* the payload as variable
bits. `ecs_input.h` / `ecs_input.c` unchanged.

### 2.1 Game-defined layout

```
command cast_ability {
    entity target_entity;
}
command cast_ability_ground {
    point position;
}
```

- `C` = number of `command` decls (e.g. 2)
- `K = ceil(log2(C))` = script-command id width in bits (e.g. 1)

Param types:

| Param type | Wire bits | Encoding                                       |
|------------|----------:|------------------------------------------------|
| `entity`   |        32 | `entity_t` (uint32 bitfield id:18 + version:12) |
| `point`    |        64 | x:32 Q16.16 + y:32 Q16.16                      |

### 2.2 Wire layout (script payload inside ecs_input's outer slot)

`ecs_input.h` provides the outer framing (`8-bit outer_type_id +
fixed_payload_bits`). The scripting layer puts its own discriminator
+ params into that payload:

```
bit  0          K                  K + params_bits     SLOT_BITS
 |          |                              |                |
 v          v                              v                v
 [ script_cmd_id : K ][ params : variable ][ pad : 0..7   ]
```

- `script_cmd_id`: this game's command id, 0..C-1. K bits wide.
- `params`: each `command NAME { }` decl's params concatenated in
  declaration order, no padding between fields.
- `pad`: zero bits filling up to the registered `SLOT_BITS`.

Registered `SLOT_BITS` (one fixed bit_len for the script's outer slot):

```
SLOT_BITS = K + max(params_bits(cmd_i) for all i)
```

### 2.3 Memory layout (per command, decoded)

Per-command typed struct, byte-aligned:

```c
typedef struct {
    entity_t target_entity;     /* 4 B */
} cmd_cast_ability_t;            /* 4 B */

typedef struct {
    fixed_t x;                   /* 4 B */
    fixed_t y;                   /* 4 B */
} cmd_cast_ability_ground_t;     /* 8 B */
```

Param order in struct = param order in `command { }` decl. No bit
packing in memory — full Q16.16 per fixed_t, full `entity_t` per
entity. Bit packing is wire-only.

### 2.4 Sample game (C = 2)

```
K               = 1
cast_ability         params = 32 bits   (1 entity)
cast_ability_ground  params = 64 bits   (1 point = 2 fixed_t)
SLOT_BITS       = 1 + max(32, 64) = 65 bits
```

Per-emission wire (inside ecs_input's payload):

| Command                    | K | Params | Pad | Total |
|----------------------------|--:|-------:|----:|------:|
| `cast_ability`             | 1 | 32     |  32 |  65   |
| `cast_ability_ground`      | 1 | 64     |   0 |  65   |

Bit offsets inside the 65-bit payload, `cast_ability`:

| Field                   | Bit offset | Bits |
|-------------------------|-----------:|-----:|
| `script_cmd_id`         |          0 |    1 |
| `target_entity` (id:18) |          1 |   18 |
| `target_entity` (ver:12)|         19 |   12 |
| pad                     |         33 |   32 |

Bit offsets inside the 65-bit payload, `cast_ability_ground`:

| Field           | Bit offset | Bits |
|-----------------|-----------:|-----:|
| `script_cmd_id` |          0 |    1 |
| `position.x`    |          1 |   32 |
| `position.y`    |         33 |   32 |

### 2.5 K table

| C (command count) | K (bits) |
|------------------:|---------:|
|   1               | 0 (or 1 if reserving an "unknown" id) |
|   2               | 1        |
|   3..4            | 2        |
|   5..8            | 3        |
|   9..16           | 4        |
|  17..32           | 5        |
| 33..256           | 6..8     |
| 257..4096         | 9..12    |

Upper bound: `K + max_params ≤ ECS_INPUT_CMD_MAX_BITS (4095)`.

### 2.6 Pack flow (codegen-emitted)

For `cast_ability`:

1. Initialize bit writer on a `ceil(SLOT_BITS/8)`-byte buffer.
2. `write_bits(script_cmd_id = 0, K)`
3. `write_bits(args.target_entity, 32)`
4. `write_bits(0, pad_bits)` — zero-fill up to `SLOT_BITS`
5. `flush_bits()`
6. `ecs_input_cmd_append(it, tick, slot, SCRIPT_OUTER_TYPE_ID, buf)`

### 2.7 Unpack flow (codegen-emitted)

In the per-tick dispatch:

1. `ecs_input_cmd_iter_*` yields `(outer_type, bytes, bit_len)`
2. If `outer_type != SCRIPT_OUTER_TYPE_ID` → skip
3. Initialize bit reader on `bytes`
4. `script_cmd_id = read_bits(K)`
5. `switch (script_cmd_id)` → unpack into matching typed struct:
   - `0` → read 32-bit `target_entity` → `cmd_cast_ability_t`
   - `1` → read 32-bit `x`, 32-bit `y` → `cmd_cast_ability_ground_t`
6. Pad bits remain unread (ignored).
7. Dispatch to the matching `on cast_ability { }` handler.

### 2.8 Inherited from `ecs_input.h`

- Per-(tick, slot) FIFO order preserved across serialize/deserialize.
- Idempotent under duplicate-packet receive.
- Authoritative-only: scripts must only emit commands the sim will
  replay deterministically.
- Outer type_id slot = 1 of 255 available.
- Per-command total bits ≤ 4095.

---

## 3. Multi-record packet (input + commands)

`ecs_input_serialize_tick` produces one packet that interleaves input
frames + command streams across `R+1` cascade ticks per slot. See
`ecs_input.h` for full wire detail. Summary per-(slot, cascade-tick):

```
[ 1 bit  diff_prev ]
[ if diff_prev = 1: raw input stride bytes (bit-packed) ]
[ 1 bit  has_cmds_at_t' ]
[ if has_cmds = 1, loop:
    [ 8 bits  outer_type_id ]
    [ registry[outer_type_id] bits  outer_payload ]   ← script's K + params + pad lives here
    [ 1 bit  more_in_slot_tick ]
]
```

Headers per packet: `u32 tick_T + u4 R + u10 first_slot` = 46 bits.

End-of-packet sentinel: the deserializer is initialized with the exact
packet bit count; loop terminates when bits remaining = 0.

---

## 4. Files

- [src/ecs_player_input.h](src/ecs_player_input.h) — reference impl for
  player input pack/unpack/tick-roll/accessors.
- [src/ecs_input.h](src/ecs_input.h) / [.c](src/ecs_input.c) — outer
  command-stream channel (unmodified by the scripting layer).
- [src/ecs_serializer.h](src/ecs_serializer.h) — bit-packed read/write.
- [scripting.md §2.6](scripting.md) — language-level description.
- [tests/test_player_input.h](tests/test_player_input.h) — input pack tests.
