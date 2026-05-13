# HibitEcs Scripting Language — Design

A declarative gameplay scripting layer for HibitEcs. Models prefabs, abilities,
effects, and attributes as a single hierarchical tag space, with bytecode-driven
event reactions that run deterministically inside the predict/rollback ECS.

Inspiration: Unreal GAS (GameplayTag containers, GameplayEffect, GameplayAbility).
Difference: tags, scripts, and storage layout are derived **entirely** from
script source — there is no separate `tags.txt`, no engine-side enum, no runtime
string lookup.

---

## 1. Top-level forms

Three top-level decls. Every decl name becomes a tag in the global tag table.
There is one flat global tag namespace; `prefab player` and `ability fireball`
register tags `player` and `fireball` directly.

```
prefab   player    { ... }   // entity template (declares attribute values)
ability  fireball  { ... }   // async state machine, granted to entities
effect   burning   { ... }   // duration / periodic / event-reactive modifier
```

Decl name = tag path. Dotted names allowed: `ability spell.fire.fireball`.

Plain tags (no def) need no decl — they are harvested automatically from
every position where a tag literal appears (owning containers, query
clauses, `emit`, `apply`, `on TAG` hooks). Listing a tag in `owned_tags`
or referencing it in a query is enough to register it.

**No `attribute` decl.** Attributes are not first-class. An attribute is just
a `(tag, fixed_t value)` pair stored on an entity. Any tag mentioned in an
entity's `attributes` block (or written by a script) becomes an attribute on
that entity. There is no schema, no default, no clamp, no derived — scripts
read and write values directly.

If you want both a current and a max, declare two attribute tags:
`health` and `health.max`. They are separate entries; the hierarchical match
of `health` against `health.max` is irrelevant to attribute storage.

---

## 2. Blocks inside a decl

All bracket blocks. **No parentheses for tag containers.** Tag containers are
comma- or newline-separated lists of tag literals inside `{ }`.

### 2.1 Containers — owning vs query

**Owning containers (flat tag lists).** Comma- or newline-separated tag
literals. The decl declaratively *owns* these tags.

| Block         | Where             | Meaning                                                       |
|---------------|-------------------|---------------------------------------------------------------|
| `tags`        | all decls         | Innate tags this decl carries. Used by external queries.      |
| `owned_tags`  | ability, effect   | Granted to the owning entity while ability is active / effect is applied. |

**Ancestor normalization.** Hierarchical match means a subject holding
`status.burning` already satisfies any query for `status`. Therefore listing
both is redundant. The compiler:

1. Drops any tag in an owning container if a strict descendant of it is also
   listed (`status` removed when `status.burning` is present).
2. Warns on exact duplicates.
3. Stores only the *minimal antichain* of tags in the runtime
   `entity_tags` buffer — keeps buffer small without losing matchability.

Authors are free to write redundant ancestors for documentation; the
compiler normalizes silently.

**Query containers (unified DSL).** Body is a query (§ 7). Leaves are either
*tag clauses* (hierarchical "has descendant of") or *predicate clauses*
(formula comparisons like `self.strength >= 50`). All four containers below
share the same grammar and evaluator; only the subject differs.

| Block          | Where           | When evaluated                                  | Subject(s) in scope                       |
|----------------|-----------------|-------------------------------------------------|-------------------------------------------|
| `requirements` | ability, effect | once at activation (ability) / application (effect) | `self`, `target` (ability), `source` (effect) |
| `ongoing`      | effect          | suspends/resumes effect while applied (record stays alive) | `self`, `source`                |
| `cancel`       | ability         | once at activation, matched against *each* ability/effect record on `self` | the candidate record's `tags ∪ owned_tags` |

`requirements` and `ongoing` are *positive* gates — they must hold. `cancel`
is a *match selector* — when it holds against a candidate record, that record
is cancelled.

There is no separate `immunity` block. Protective effects use a convention:
they declare protection tags via `owned_tags { immunity.<bucket> }`, and
offensive abilities/effects gate themselves with
`requirements { none { target.immunity.<bucket> } }`. Hierarchical matching
makes this composable: `owned_tags { immunity.fire }` blocks anything
checking `target.immunity.fire`, `target.immunity.fire.minor`, etc.

`ongoing` does **not** remove the effect record when false. It *disables* the effect:

- Periodic body (`every`) does not fire.
- Attribute modifiers contributed by the effect are excluded from recompute.
- `on TAG` event handlers belonging to the effect do not run.
- Duration timer continues to tick (the effect still expires when `end_tick <= now`).
- Record stays in `entity_effects`; re-enables automatically when `ongoing` becomes true again.
- **`owned_tags` of a disabled effect are NOT in `entity_tags`.** Only enabled
  effects contribute owned_tags. Tags are rebuilt at tick-begin from the enabled
  set (§ 13.3).

**Re-evaluation.** `ongoing` is evaluated once per tick at **tick-begin**
(§ 13.3 step 1), before `entity_tags` is rebuilt. It reads the previous
tick's finalized `entity_tags` and attribute values — the state that was
in place when the last tick ended. `enabled` is always recomputed from
scratch; the stored value from a prior tick is never trusted.
This eliminates circular dependency: no `ongoing` evaluation reads another
effect's `enabled` state computed in the same pass.
1-tick cascade lag: if effect A's `ongoing` flips and changes its
owned_tags contribution, effect B's `ongoing` will not see that change
until the following tick-begin.

There is no separate `block_tags` / `required_tags` / `ongoing_tags`. To
forbid a clause, write `requirements { none { ... } }` or place a `none`
inside an `all`. There is one gate per concern.

### 2.2 Body blocks

| Block         | Where             | Contents                                                              |
|---------------|-------------------|-----------------------------------------------------------------------|
| `attributes`  | prefab            | `tag = <formula>` pairs — one fixed_t value per attribute tag         |
| `effects`     | prefab            | List of effect tags applied at spawn                                  |
| `abilities`   | prefab            | List of ability tags granted at spawn                                 |
| `costs`       | ability           | `attr_tag = <formula>` pairs; checked + deducted on activation        |
| `cooldowns`   | ability           | `duration = <formula>` plus optional `tags { ... }`. Repeatable.      |
| `duration`    | effect            | `<formula>` resolving to ticks (`5s`, `30t`, `infinite`, or expr)     |
| `period`      | effect            | `<formula>` tick interval for `every`                                 |
| `every`       | effect            | Body run each `period` while active                                   |
| `source`      | effect            | `{ attr, ... }` — attrs to snapshot from source entity at application time; stored inline in the effect record; compiler resolves `source.X` for captured X to `LOAD_CAP_SRC` |
| `target`      | effect            | `{ attr, ... }` — attrs to snapshot from target (`self`) entity at application time; stored inline in the effect record; compiler resolves `self.X` for captured X to `LOAD_CAP_TGT` |
| `on <name>`   | any decl          | Hook body. `<name>` is either a lifecycle keyword (§ 2.3) or a tag literal (§ 2.4). |

Every numeric position above accepts a **formula** (§ 6.4) — not just a literal.

### 2.3 Lifecycle hooks (`on <keyword>`)

A small set of reserved keywords selects lifecycle hooks instead of event tags.
Reserved keywords cannot be used as user tag names (compiler error).

| Form              | Where    | Fires                                                |
|-------------------|----------|------------------------------------------------------|
| `on activate`     | ability  | After costs paid + requirements satisfied            |
| `on end`          | ability  | When ability ends (success, cancel, or interrupt)    |
| `on init`         | prefab   | At spawn, after attributes initialised               |
| `on despawn`      | prefab   | When entity is despawned                             |

Reserved set: `activate`, `end`, `init`, `despawn`.

### 2.4 Event hooks (`on TAG`)

Any non-reserved name in `on <name>` position is a tag literal — the hook
listens for events whose tag is a descendant of `<name>` (hierarchical match).

```
on damage         { self.health -= event.amount }      // catches all damage.*
on damage.fire    { ... }                              // only fire damage
on damage         { if self.health <= 0 { despawn self } }
```

### 2.5 Cooldowns

The `cooldowns { }` block is a list of `<tag> = <duration_formula>` entries.
Each entry declares one cooldown bucket: while active, the caster owns the
tag for the duration. Multiple entries per ability — typical mix is
self-cooldown, GCD, and school cooldown.

```
cooldowns {
  ability.fireball = 30s                          // self cooldown
  ability          = 1.5s - self.haste * 0.05s   // GCD bucket
  ability.fire     = 8s                           // school cooldown
}
```

Each entry compiles to a synthetic cooldown **effect** with `duration =
<formula>`, `owned_tags { <tag> }`, empty body. Effect tag itself is the
bucket tag — no `cooldown.` prefix.

**Each tag expires at its own duration.** The synthetic effects are
independent records. `ability` drops at 1.5s, `ability.fire` at 8s,
`ability.fireball` at 30s.

**Hierarchical match composes them naturally.** The compiler auto-injects:

```
requirements { none { ability.fireball, ability, ability.fire } }
```

These are normal (hierarchical) tag clauses. While *any* descendant of a
queried tag is on the caster, the clause matches and activation is blocked.
With the example above, casting fireball blocks every ability checking
`none { ability }` for the full 30s — because `ability.fireball` is a
descendant of `ability` and stays for 30s. The GCD `ability` tag itself
drops at 1.5s, but the descendant prolongs the ancestor-query block to 30s.
This is the intended semantics: heavier cooldowns dominate.

To gate on a specific bucket only (no descendant prolongation), use
`exact` (§ 7.4): `requirements { none { exact ability } }` matches only
while the literal `ability` tag is on the caster.

Activation pipeline (§ 13.1) applies all cooldown effects after cost
deduction. Cooldown starts immediately and stays even if the ability body
is cancelled mid-`wait`.

Read remaining cooldown ticks with the builtin `cooldown_remaining(self,
TAG)` — returns 0 if the exact tag is not currently on the entity, else
`end_tick - now`.

### 2.6 Input + Commands

Gameplay scripts read player intent through two related surfaces:

- **`commands { }`** — top-level decl listing command tag names. Each entry
  registers a tag in the global namespace; commands are routable events,
  typically emitted by the input layer and consumed by `on <command>` hooks.
- **`input { }`** — top-level (per-game) block declaring the input layout
  the engine writes once per connected player per tick (before scripts
  run). Holds buttons + 2D sticks bit-packed in a deterministic record.

#### commands

```
commands {
    fire
    jump
    move
    ability.fireball
    ability.iceblast
}
```

Each entry becomes a tag (subject to ancestor promotion: `ability.fireball`
also promotes `ability`). Dotted paths follow the same hierarchical-match
rules as any other tag — `on ability { ... }` catches every ability command.

Commands are *just tags* — the `commands { }` block is documentation +
namespace anchor; no special wire format. The optional `command` keyword
before an entry is accepted as a style hint and ignored by codegen:

```
commands {
    command fire           // equivalent to bare `fire`
    command ability.fireball
}
```

Commands map to input slots (next section) or are emitted directly by
script code (`emit ability.fireball`). Either way they flow through the
normal event dispatch path (§ 13.3 step 5).

#### input

**Top-level decl, one per game.** Input is owned by the *player*, not by
any prefab — one connected player produces one input record per tick,
regardless of which entities they currently control. Multiple entities
may read the same player's input; one player can control many entities.

```
input {
    button fire
    button jump
    button crouch
    stick  move
    stick  aim
}
```

Slot names are tags (full dotted paths allowed: `button weapon.primary.fire`).
They live in the same global tag table as everything else.

The engine maintains one record per connected player, slotted into the
existing `ecs_input.h` table. Scripts read `input.<slot>.<sub>` from any
prefab body; the resolution `entity → player → input record` is performed
by engine code (e.g. an `owner_player_id` component on the entity, set
when the player possesses the prefab). The language is agnostic to that
mapping — `input.X` always means *the current entity's owning player*.

```
prefab player {
    on input.fire.pressed {       // resolves against self's owning player
        emit ability.fireball
    }

    on input.move.changed {
        // move.x / move.y now reflect the new stick position
    }
}
```

##### `button` slots — 1 wire bit, 2 memory bits, 3 derived states

Per button per tick:

- **Wire**: `1` bit — only `down` (current frame).
- **Memory**: `2` bits — `down` (current) + `was_down` (previous frame).
  `was_down` is engine-local; never crosses the network.

Three derived states cover all use cases:

| Access                  | Formula            | Notes                            |
|-------------------------|--------------------|----------------------------------|
| `input.fire.down`       | down               | held this tick                   |
| `input.fire.pressed`    | down & !was_down   | rising edge (0→1 this tick)      |
| `input.fire.released`   | !down & was_down   | falling edge (1→0 this tick)     |

State table for the raw bit-pair `(down, was_down)`:

| down | was_down | label   | pressed | down (out) | released |
|:----:|:--------:|---------|:-------:|:----------:|:--------:|
| 0    | 0        | idle    |    0    |     0      |    0     |
| 1    | 0        | rising  |    1    |     1      |    0     |
| 1    | 1        | held    |    0    |     1      |    0     |
| 0    | 1        | falling |    0    |     0      |    1     |

Returns `fixed_t` 0 / `FIXED_ONE`. Codegen emits one byte-load + bit-mask
+ compare per access.

**Memory layout** — buttons sit at exact bit offsets in the record. No
byte padding between sections:

```
bit-offset 0   → down section     [ N bits ]
bit-offset N   → was_down section [ N bits ]
```

Slot index inside each section = declaration order (0..N−1). Codegen
resolves each named slot to a bit offset at compile time:
- `down_bit_i  = i`
- `wasd_bit_i  = N + i`

**Edge derivation**: `pressed` / `released` are pure functions of the two
memory bits — no separate wire bits. Codegen emits one bit-extract per
side + AND/NAND.

**Tick-roll** (engine-side, before applying the new wire frame): copy
bits `[0..N)` to bits `[N..2N)` — `was_down := down` for all slots in one
bitstream-copy.
- N % 8 == 0 → single `memcpy(record + N/8, record, N/8)`.
- otherwise   → one `read_bits(N)` + `write_bits(N)` against
  `ecs_serializer.h` helpers (handles up to 64 bits in one shot;
  multi-word loop for larger N).

After tick-roll, the engine deserializes the wire's `N` `down` bits
directly into the down section, overwriting them. The was_down section
already holds last frame's down — left untouched by the wire decode.

##### `stick` slots — cartesian Q1.15, 32 bits, lossless

Sticks are **always normalized** (engine deadzones + radial-clamps at
the input boundary, magnitude ≤ 1) and encoded as a single fixed format:

```
input {
    stick move
    stick aim
}
```

Each stick is **32 bits** in declaration order:

| Field | Bits | Encoding                          | Range / resolution            |
|-------|-----:|-----------------------------------|-------------------------------|
| x     |   16 | signed Q1.15                      | `[-1, +1)`, step `1 / 32768`  |
| y     |   16 | signed Q1.15                      | `[-1, +1)`, step `1 / 32768`  |

**Lossless across the entire pipeline**: 16-bit signed-int captures the
full precision of any commodity joystick ADC (typically 8–12 bits).
Wire bits, in-memory bits, and ADC bits carry identical information —
no encoding transform, no rounding, no quantization beyond what the
hardware itself produces.

Single format. No precision classes, no polar / cartesian split, no
LUTs. Tightens codegen + decoder.

##### Stick script access

| Access                    | Type     | Notes                                                |
|---------------------------|----------|------------------------------------------------------|
| `input.move.x`            | fixed_t  | direct: signed Q1.15 sign-extended + shifted to Q16.16 |
| `input.move.y`            | fixed_t  | direct: same                                          |
| `input.move.magnitude`    | fixed_t  | `sqrt(x² + y²)`; lazy + cached per tick               |
| `input.move.angle`        | fixed_t  | `atan2(y, x)`, Q16.16 radians; lazy + cached          |
| `input.move.changed`      | fixed_t  | 0/1, set when raw bits differ from previous frame    |

`x` and `y` are **direct field reads — zero conversion cost** (one
signed shift). `magnitude` and `angle` are computed lazily on first
access per tick (integer sqrt + atan2, deterministic) and cached.

**Determinism**: integer-only — Q1.15 → Q16.16 is a sign-extending
shift; sqrt + atan2 use fixed-point integer implementations from
`ecs_fixed.h`. Same bits in → same fixed_t out across architectures
and rollback. No floating point, no transcendental LUTs.

##### Wire format

Buttons + sticks share one opaque payload per player, slotted into the
existing `ecs_input.h` table (one command type_id for the whole game's
input layout). The payload is a **pure bitstream — only the `down` bit
per button; no `was_down`, no padding between sections, no padding
inside sticks**:

```
bit 0      N                                                 N + 32M
 |          |                                                       |
 v          v                                                       v
 [ down N  ][ x0:16 | y0:16 | x1:16 | y1:16 | ... | xM-1:16 | yM-1:16 ]
```

- `N` = button count
- `M` = stick count
- Each stick contributes exactly **32 bits**: signed Q1.15 `x` (16 bits)
  followed by signed Q1.15 `y` (16 bits). Sticks concatenated in
  declaration order, no padding.
- Slot index within each section = declaration order

**Exact wire bit length**: `bit_len = N + 32*M`. Registered with
`ecs_input_register_cmd_type(registry, type_id, bit_len)` so the decoder
knows the size without a per-command header. `was_down` is reconstructed
locally each tick from last frame's `down`; never transmitted.

**Record layout in the input BUFFER tree** — `ceil((2*N + 32*M) / 8)`
bytes per player (memory holds both `down` and `was_down`). Bit offsets:

| Slot                  | Bit offset            | Bit width |
|-----------------------|-----------------------|-----------|
| `button.down[i]`      | `i`                   | 1         |
| `button.wasd[i]`      | `N + i`               | 1         |
| `stick[i].x`          | `2*N + 32*i`          | 16 (signed Q1.15) |
| `stick[i].y`          | `2*N + 32*i + 16`     | 16 (signed Q1.15) |

**Memory stride**: `ceil((2*N + 32*M) / 8)` bytes per player.
**Wire bit_len**: `N + 32*M` (down + sticks only).

A few concrete sizes:

| Layout                | wire bit_len | wire bytes (P=1, no framing) | memory stride |
|-----------------------|-------------:|-----------------------------:|--------------:|
| 1 button, 0 sticks    |            1 |                          1 B |           1 B |
| 3 buttons, 0 sticks   |            3 |                          1 B |           1 B |
| 4 buttons, 1 stick    |           36 |                          5 B |           5 B |
| 8 buttons, 1 stick    |           40 |                          5 B |           6 B |
| 8 buttons, 2 sticks   |           72 |                          9 B |          10 B |
| 16 buttons, 2 sticks  |           80 |                         10 B |          12 B |
| 1 button, 1 stick     |           33 |                          5 B |           5 B |

`N == 0` → button sections omitted; `M == 0` → stick section omitted.
An empty `input { }` block (no slots) produces no record at all.

**Unaligned bit-field reads**: stick `x` / `y` (16 bits each) rarely
land on byte boundaries when N is not a multiple of 8. Codegen emits
`read_bits(16)` + sign-extend per field; `ecs_serializer.h` already
implements this. Cost is ~2 byte-loads + shifts per read — same as a
plain unaligned `int16_t` load on x86.

Layout is purely a function of the (single) `input { }` decl →
deterministic across builds and identical for every connected player.

##### Codegen-emitted I/O

For the single `input { }` decl, the compiler emits four helpers + a
typed in-memory struct (one shared type for every player). All paths
are pure integer bit-ops → **bit-identical across architectures and
rollback**.

```c
/* In-memory representation: same bitstream as wire (zero-copy).
   Stride = ceil((2*N + 32*M) / 8). One instance per connected player. */
typedef struct { uint8_t bits[PLAYER_INPUT_STRIDE]; } player_input_t;

/* Wire I/O — both sides of the netcode boundary call these. */
void player_input_read (ecs_deserializer_t* d,       player_input_t* out);
void player_input_write(ecs_serializer_t*   s, const player_input_t* in);

/* Tick-begin roll: copies down section onto was_down section.
   Bit-copy when N is not byte-aligned, single memcpy when it is. */
void player_input_tick_roll(player_input_t* state);

/* Per-slot accessors — inlined into script bytecode lowering.
   `i` is the compile-time slot index baked by codegen. Called via
   `input.<slot>.<sub>` against the current entity's owning player. */
static inline uint32_t player_input_btn_down    (const player_input_t* s, uint32_t i);
static inline uint32_t player_input_btn_pressed (const player_input_t* s, uint32_t i);
static inline uint32_t player_input_btn_released(const player_input_t* s, uint32_t i);
static inline fixed_t  player_input_stick_x     (const player_input_t* s, uint32_t i);
static inline fixed_t  player_input_stick_y     (const player_input_t* s, uint32_t i);
static inline fixed_t  player_input_stick_mag   (const player_input_t* s, uint32_t i);
static inline fixed_t  player_input_stick_angle (const player_input_t* s, uint32_t i);
```

`*_read` / `*_write` use `ecs_serializer.h`'s `read_bits` / `write_bits`
on the same bitstream layout documented above. Because in-memory = wire
format, the implementation degenerates to a single `memcpy` once the
deserializer's bit cursor is byte-aligned (the common case for the head
of a multi-player packet).

###### Wire is bit-exact, memory is byte-stride

Two separate notions of "size":

- **Wire**: pure bit-stream. Each player's record occupies exactly
  `bit_len = 2*N + 32*M` bits — **no byte padding between players**.
  Multi-player packets concatenate records bit-contiguously; only the
  whole packet rounds up to a byte boundary at the very end.

- **Memory**: each `player_input_t` is a stand-alone record sized to
  `stride = ceil(bit_len / 8)` bytes (byte-aligned, normal C struct). The
  last byte may contain up to 7 zero padding bits in the slot positions
  past `bit_len` — they carry no information and are never read.

The two representations carry **identical information** (same bits in
slot order); only the framing differs. No quantization, no precision
loss between network and RAM.

`*_read` advances the deserializer by exactly `bit_len` bits per player,
not by `stride * 8` — the wire never wastes the trailing padding bits.
When `bit_len % 8 == 0` and the cursor happens to be byte-aligned, the
read degenerates to a `memcpy`; otherwise it's a few `read_bits` calls.

Rollback shadow per player = `stride` bytes (tight; trailing pad is
free).

##### Netcode packing

Records are bit-tight on the wire. Multi-player packets = **bit-contiguous
concatenation, no byte gaps between players, no per-player framing**.
Sender + receiver agree on `(player_count, input_layout)` via the session
handshake; ordering implies player ID. One `tick_id` per packet at the
front. Whole packet rounds up to byte boundary once at the end.

```
bit 0      16          16+bit_len    16+2*bit_len      16+P*bit_len
 |          |                |             |                 |
 v          v                v             v                 v
 [tick_id:16][player_0:bit_len][player_1:bit_len]...[player_P-1:bit_len][pad:0..7]
```

Packet byte size = `ceil((16 + P * bit_len) / 8)`.

For desync detection / server reconciliation, append per-player
coordinates after the input block (or interleave — choice of binding):

```
[ tick_id : 16 ]  [ inputs : P × bit_len ]  [ coords : P × coord_bits ]
```

Coordinates: typical encodings (Q.K signed fixed-point):

| Encoding             | Bits / axis | Range          | Resolution     | Use                  |
|----------------------|------------:|----------------|----------------|----------------------|
| Q.10                 |          10 | ±512           | 1.0            | grid / tile worlds   |
| Q.14                 |          14 | ±128.0         | 1 / 256        | bounded arena        |
| Q.16                 |          16 | ±256.0         | 1 / 256        | typical 2D / 3D play |
| Q.20                 |          20 | ±2048.0        | 1 / 256        | open-world           |
| Q16.16 (engine native)|        32 | full fixed_t   | 1 / 65536      | replay / authoritative |

Game picks `coord_bits` per axis and per dimension (2D = 2 × per axis,
3D = 3 ×). Deltas from last-acked reduce typical wire cost ~3×.

**Sizing table** — bits / bytes per player per tick, plus packet totals
at 60 Hz for P players. Tick framing + UDP/IP overhead not counted
(~28 B per datagram).

All numbers are **wire** bits per player (only `down` for buttons, no
`was_down`).

| Composition (per player)                | Wire bits | Bytes if standalone |
|------------------------------------------|----------:|---------------------:|
| 4 buttons                                |         4 |                 1 B |
| 8 buttons                                |         8 |                 1 B |
| 16 buttons                               |        16 |                 2 B |
| 1 stick                                  |        32 |                 4 B |
| 2 sticks                                 |        64 |                 8 B |
| 8 buttons + 1 stick                      |        40 |                 5 B |
| 8 buttons + 2 sticks                     |        72 |                 9 B |
| 16 buttons + 2 sticks                    |        80 |                10 B |
| 8 buttons + 2 sticks + 2D pos Q.14       |       100 |                13 B |
| 8 buttons + 2 sticks + 3D pos Q.16       |       120 |                15 B |
| 16 buttons + 2 sticks + 3D pos Q.20      |       140 |                18 B |

**Multi-player packets** — packet bytes for P players + 16-bit tick_id
(rounded up at the end, packing is bit-contiguous):

All players inside one packet are bit-contiguous (`(16 + P * bit_len)`
bits total, rounded up to bytes once). bit_len per row:
- 8 btn + 1 stick: 40 bits
- 16 btn + 2 sticks: 80 bits
- 16 btn + 2 sticks + 3D pos Q.16: 128 bits

| Players | 8 btn + 1 stick | 16 btn + 2 stick | + 3D pos Q.16 |
|--------:|----------------:|-----------------:|--------------:|
| 1       |             7 B |             12 B |          18 B |
| 2       |            12 B |             22 B |          34 B |
| 4       |            22 B |             42 B |          66 B |
| 8       |            42 B |             82 B |         130 B |
| 16      |            82 B |            162 B |         258 B |

At 60 Hz, 16 players × (16 buttons + 2 sticks + 3D pos Q.16): `258 × 60
≈ 15.5 KB/s = 124 kbit/s`. Halved with delta-from-last-acked
(≈ 62 kbit/s). Within a single UDP datagram (MTU ~1500 B) → no
fragmentation through ~92 players.

**Notes**:
- No per-player ID in packet — derived from ordering or session handshake.
- `ecs_input.h` already supports `bit_len`-exact command types + delta-stream
  compression (`in_write_payload_bits` / `in_write_cmd_bits`).
- Tick advance (delta-only frames where nothing changed) costs a single
  bit per player via the `1` ↔ `0` change-bit in the existing protocol.

##### Routing input → commands

The default pattern is `on input.<slot>.pressed` etc. — direct slot
edge hooks. For cross-prefab dispatch, scripts can convert any input edge
into a command tag via `emit`:

```
input { button fire }

prefab player {
    on input.fire.pressed {
        emit ability.fireball     // routes through normal event dispatch
    }
}
```

This keeps the wire surface (per-player input records) and the routing
surface (command tags) separable. Commands listed in `commands { }` are
expected to be reachable this way.

##### Determinism

Input frames roll back with the rest of the simulation — buttons + sticks
are deterministic per the engine's predict/rollback model. Re-simulated
frames see the same `pressed` / `released` / `x` / `y` values given the
same input stream. `magnitude` / `angle` cache is rebuilt per tick from
the raw `x` / `y` so it never diverges across rollback.

##### Reserved keywords

`commands`, `input`, `command`, `button`, `stick` — added to the
reserved-keyword set (§ 15). Cannot be used as user tag names.

---

## 3. Examples

### 3.1 Entity (prefab)

```
prefab player {
  tags { creature.humanoid }

  attributes {
    level         = 1
    intellect     = 10
    constitution  = 10
    spell_power   = self.intellect * 2
    health        = 100 + self.level * 10
    health.max    = self.health
    mana          = 50 + self.intellect * 3
    mana.max      = self.mana
    stamina       = 100
    stamina.max   = 100
  }

  effects   { regen.health.slow, status.well_fed }
  abilities { fireball, melee.basic }

  on damage      { self.health = max(0, self.health - event.amount) }
  on damage.fire { self.health = max(0, self.health - event.amount * 2) }

  on damage {
    if self.health <= 0 { despawn self }
  }
}
```

Each entry in `attributes` is one tag and one fixed_t value. Order matters
inside the block: a later entry can read earlier entries (`health.max =
self.health`). The compiler topo-sorts by formula dependency and rejects
cycles. There are no implicit `current` / `max` sub-fields — write them as
separate entries with explicit tags (`health` and `health.max`).

### 3.2 Ability

```
ability fireball {
  tags        { spell.fire, offensive }
  owned_tags  { state.casting }

  requirements {
    state.combat_ready                          // tag must hold
    self.intellect >= 10                         // predicate
    self.mana >= 30                              // predicate (also enforced by costs)
    target.creature                              // target tag
    any {
      weapon.staff
      weapon.wand
      self.spell_power > 0
    }
    none {
      status.silenced
      status.stunned
      status.dead
      target.status.invulnerable
      target.status.untargetable
      // cooldown.* checks are injected automatically by the cooldown blocks below
    }
  }

  cancel {
    any  { state.casting, channeled }            // cancel records with either tag
    none { ability.uninterruptible }             // unless they are uninterruptible
  }

  costs {
    mana    = 30 + self.level * 0.5
    stamina = 5
  }

  cooldowns {
    ability.fireball = 30s                            // self cooldown
    ability          = 1.5s - self.haste * 0.05s      // GCD bucket
    ability.fire     = 8s                             // school cooldown
  }

  // Compiler injects automatically (hierarchical match):
  //   requirements { none { ability.fireball, ability, ability.fire } }
  //
  // Each tag expires at its own duration. But because `ability.fireball`
  // is a descendant of `ability`, any other ability checking `none { ability }`
  // stays blocked for the full 30s — the heavier cooldown dominates.

  on activate {
    wait 0.5s
    target.emit damage.physical.fire {
      amount = (self.spell_power * 1.5 + 20) * (1 + self.level * 0.05)
    }
    apply burning -> target
  }

  on end {
    // nothing
  }
}
```

### 3.3 Effect

```
effect burning {
  // status.burning already matches `status` queries (hierarchical descendant test)
  tags       { debuff, status.burning }
  owned_tags { status.on_fire }

  requirements {
    self.armor <= 200                          // predicate (heavy armor immune)
    any  { creature, construct.flammable }      // must be flammable kind
    none { immunity.fire, immunity.all_status } // ward-style protection blocks apply
  }

  ongoing {
    // effect is disabled while any of these is true (ticking + modifiers paused).
    // duration keeps counting; effect resumes when query becomes true again.
    none { status.wet, status.frozen, status.fireproof_aura }
  }

  // Attrs snapshotted from source/target into the effect record at application time.
  // source.spell_power inside this body reads the snapshot, not the live entity.
  source { spell_power, level }
  target { armor, health }

  duration = 5s + source.spell_power * 0.05s   // reads snapshot
  period   = 1s

  every {
    self.emit damage.fire { amount = 5 + source.spell_power * 0.25 }
  }

  on damage.fire {
    // amplify incoming fire damage on burning targets
    event.amount = event.amount * (1 + 0.25 * stacks)
  }
}
```

### 3.4 Protective effect (ward via owned_tags convention)

```
effect fire_ward {
  tags       { protection, ward }
  owned_tags { immunity.fire, immunity.status.burning }
  duration = 30s
}
```

Offensive effects/abilities gate themselves on the matching bucket:

```
effect burning {
  requirements { none { target.immunity.status.burning } }
  // ...
}

ability fireball {
  requirements { none { target.immunity.fire } }
  // ...
}
```

`owned_tags` is hierarchical: a single `immunity.fire` covers
`target.immunity.fire`, `target.immunity.fire.minor`, etc. Add a new ward
type by giving it the right `owned_tags`; offensive code does not change.

### 3.5 Effect — event-reactive damage modifier

```
effect half_damage_from_behind {
  tags { defensive, perk }
  duration infinite

  // Listen on incoming physical damage. event.source = attacker entity.
  // If attacker is behind self (relative to self.facing), halve event.amount.
  // Mutating event.amount inside an `on TAG` body modifies the in-flight
  // event before later handlers see it.
  on damage.physical {
    if is_behind(self, event.source) {
      event.amount = event.amount * 0.5
    }
  }
}
```

Notes:

- `is_behind(viewer, src)` is a deterministic spatial builtin (§ 6.2). It
  reads `viewer.position`, `viewer.facing`, `src.position` and returns 1
  iff the source is in the viewer's rear half-space (dot of facing with
  `(src.position - viewer.position)` is `< 0`).
- Handlers fire in a defined order so multiple damage-modifying effects
  compose deterministically (sort by `(effect_tag preorder id, apply_tick,
  source.id)` — no race).
- `event.amount` is mutable; the final value seen by the recipient's own
  `on damage` handler is whatever the chain of effect handlers leaves.

### 3.6 Ability — transactional, atomic commit (trade)

Multi-step ability that proposes a trade, waits for both sides to accept, and
**either commits all changes atomically or makes no observable change at
all**. Pattern: stage every proposed change in the ability's coroutine
locals — never write to entity attributes until the commit branch.
Cancellation, peer disconnect, range break, or timeout all end the ability
with zero side effects on attributes.

```
ability trade {
  tags        { social, ability.trade }
  owned_tags  { state.trading }

  requirements {
    target.creature
    distance(self, target) <= 5
    none {
      state.trading                  // can't open a 2nd trade
      self.status.dead
      target.status.dead
      target.status.untradable
    }
  }

  // Auto-cancel mid-trade if either party becomes invalid. ongoing flips
  // `enabled` off, the wait wakes with `disabled = 1`, the body returns,
  // `on end` fires with no commit. No attribute writes were ever made.
  ongoing {
    distance(self, target) <= 5
    none {
      self.status.dead
      target.status.dead
      target.status.hostile
    }
  }

  // locals layout (4 × i32 — fits the inline coroutine frame, no spill):
  //   locals[0] = my_offer_gold       (staged, not yet deducted)
  //   locals[1] = peer_offer_gold     (staged, not yet credited)
  //   locals[2] = flags               (bit 0 = self accepted, bit 1 = peer accepted)
  //   locals[3] = peer_offer_version  (incremented on each peer offer change)

  on activate {
    locals[0] = 0
    locals[1] = 0
    locals[2] = 0
    locals[3] = 0

    // Open the trade window for both sides. UI / peer ability listens for this.
    target.emit trade.opened { peer = self }

    while true {
      // Hierarchical wait: wakes on any descendant of `trade`. 60s timeout.
      // Sets `timed_out = 1` if the wait expires; sets `disabled = 1` if
      // ongoing flipped off during the wait.
      wait_event trade timeout 60s

      if disabled or timed_out          { return }   // graceful end, no commit
      if event.tag == trade.cancel       { return }   // either side cancelled
      if event.source != self and event.source != target { continue }  // ignore strays

      if event.tag == trade.offer {
        // Peer (or self via UI) changed their offer. Reset both accept bits;
        // any prior accept is invalidated by a new offer.
        if event.source == self   { locals[0] = event.amount }
        if event.source == target { locals[1] = event.amount; locals[3] += 1 }
        locals[2] = 0
        target.emit trade.offer.refresh {
          mine   = locals[0]
          theirs = locals[1]
        }
        continue
      }

      if event.tag == trade.accept {
        // Accept must reference the current peer offer version (replay-safe
        // against the "accept arrives after offer change" race).
        if event.version != locals[3] { continue }

        if event.source == self   { locals[2] = locals[2] | 1 }
        if event.source == target { locals[2] = locals[2] | 2 }

        if locals[2] == 3 {
          // Both accepted at the same offer version — COMMIT.
          // First and only attribute writes in the entire ability.
          // Must also re-check funds at commit (peer may have spent gold
          // mid-trade via another script).
          if self.gold   < locals[0] { target.emit trade.failed { reason = insufficient_self };   return }
          if target.gold < locals[1] { target.emit trade.failed { reason = insufficient_target }; return }

          self.gold   -= locals[0]
          target.gold += locals[0]
          target.gold -= locals[1]
          self.gold   += locals[1]

          target.emit trade.committed { mine = locals[0], theirs = locals[1] }
          self.emit trade.committed { mine = locals[1], theirs = locals[0] }
          return
        }
      }
    }
  }

  on end {
    // No revert needed — no attribute writes happened on any non-commit
    // path. Just notify the peer that the trade window is gone, and let
    // owned_tags drop naturally with the record.
    if (locals[2] & 3) != 3 {
      target.emit trade.aborted { }
    }
  }
}
```

**Why this has rollback semantics:**

1. **No staged write touches the world.** `locals[0..3]` live inside the
   ability_record_t. They roll back automatically with the BUFFER tree's
   predicted/confirmed split — predicted ticks keep speculating, confirmed
   ticks promote — but more importantly, *no entity attribute is mutated*
   until the commit branch.
2. **Single commit point.** The four `gold` writes happen only when both
   sides have accepted at the same offer version. They are sequenced inside
   one tick (no `wait` between them), so even per-tick rollback sees them
   atomically: the tick either confirms with all four writes, or doesn't
   and they evaporate together.
3. **Cancel = `return`.** No reverse-write code path exists. If the ability
   ends from cancel, timeout, ongoing-fail, peer-death, or out-of-range, the
   `on end` body fires and only emits a notification event. No attribute
   was ever mutated.
4. **Re-check at commit.** Funds are verified again immediately before the
   transfer because another script could have spent the gold during the
   60s window. This guards against late-binding races without needing a
   lock.
5. **Replay-safe.** Both peers see the same `event.tag`, `event.amount`,
   `event.version` values because events are deterministic ECS records;
   the per-tick PREDICT/CONFIRMED scheme already handles network rollback
   without the script needing to know.

**Required language features used here** (all already in the spec):

- `wait_event TAG timeout <formula>` — hierarchical event wait with timer.
- `event.source`, `event.tag`, `event.amount`, `event.version` — payload fields.
- `disabled` — well-known coroutine local set when `ongoing` flipped off
  during a wait.
- `timed_out` — well-known coroutine local set when a wait's timer expired.
- `locals[0..3]` — inline coroutine frame on the ability_record_t.
- Bitwise `|` and `&` on i32 locals (Q16.16 fits 32-bit; bitops legal on the
  underlying integer).
- `or` / `and` boolean operators.
- `continue` inside `while`.

---

## 4. Tag harvest, sort, and ID assignment

### 4.1 Sources of tags

The compiler harvests tags from every position where a tag literal can appear:

- Decl names: `prefab X`, `ability X`, `effect X`.
- Owning tag containers: `tags`, `owned_tags`.
- Query blocks (every tag-clause leaf is a tag literal):
  `requirements`, `ongoing`, `cancel`.
- `match(<entity>, { ... })` inline query expressions.
- `on TAG` handler heads.
- `<entity>.emit TAG { ... }`
- `apply TAG -> ...`, `remove TAG`, `cancel TAG`
- `has_tag(self, TAG)` and other tag-builtin args
- Event payload field names: `event.amount` registers tag `amount`
- `effects { ... }`, `abilities { ... }` lists inside prefab decls

### 4.2 Implicit ancestor promotion

Mention of `damage.physical.fire` auto-declares `damage`, `damage.physical`,
`damage.physical.fire`. Every prefix is a real tag.

### 4.3 Sort and assign

Alphabetical sort of full paths is exactly DFS preorder when children are
sorted alphabetically (`.` = 0x2E < any letter). The compiler:

1. Scans every `.script` file in **sorted-by-path order**.
2. Collects unique tag literals + promoted ancestors → set.
3. Sorts the set lexicographically.
4. Assigns IDs by position → `tag_id = index in sorted list`.
5. Linear scan to compute `out` (subtree extent) and `parent` (longest strict prefix).
6. Emits `tag_defs.gen.c` and `tag_ids.gen.h`.

Same source → identical output, byte-stable.

### 4.4 Generated table

```c
typedef struct {
    uint16_t parent;       // 0xFFFF = root
    uint16_t out;          // first ID that is NOT a descendant of this tag
    uint32_t def_offset;   // blob offset to def struct (0 = plain tag)
    uint32_t name_offset;  // blob offset to path bytes (cold string table)
    uint16_t name_len;     // path byte length, no NUL
    uint8_t  kind;         // tag_kind_t
} tag_def_t;               // 16 bytes

extern tag_def_t tag_defs[N];   // N = total tags, <= 65536
```

`in` is implicit (equals the array index). 16 bytes × N. For N = 65536 → 1 MB
static data — fine. Path bytes (`name_offset`-addressed) live in the blob's
cold tail, concatenated raw without separators.

A parallel `tag_meta[]` table indexed by tag id holds decl-kind information
(prefab idx, ability idx, effect idx); most slots are zero.

### 4.5 Cross-build determinism

Every build that consumes the same source file set produces:

- Identical sorted tag list.
- Identical IDs.
- Identical `schema_crc = crc64(joined sorted paths)`.
- Identical bytecode (op IDs use same tag IDs).

`schema_crc` is stamped into world saves and the network handshake. Mismatch =
rebuild required; saves remap by comparing the saved path strings against
the new `tag_defs[].name_offset`/`name_len` table.

---

## 5. Determinism boundaries

These are language rules that exist purely to keep tag harvest pure:

- All tags must appear as literals. **No `string_to_tag(s)` runtime function.**
- No conditional compilation (`#if`, etc.) that gates tag literals.
- No string concatenation in tag position: `"damage." + element` is rejected.
- Comments are stripped before harvest.
- Whitespace is not allowed inside dotted tag literals: `damage.fire`, never
  `damage . fire`.
- Decl names must match `[a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)*`.
- Single global tag namespace. Two decls with the same name across kinds is a
  compile-time error (so `prefab health` and `effect health` cannot coexist).
- File enumeration is sorted by path before scan.
- No anonymous decls.

---

## 6. Expressions

Everything is an expression that returns a value. There is no separate
statement category. Side-effectful forms return **unit** (fixed-point `0`).
A block `{ e1; e2; … en }` evaluates all sub-expressions in order and
returns the value of the last one. Non-final expressions in a block have
their value discarded (compiler inserts `POP`).

### 6.1 Expression forms

| Form | Returns |
|---|---|
| `<integer>`, `1.5`, `30%`, `5s`, `30t` | fixed_t literal |
| `self.attr`, `target.attr`, `source.attr` | fixed_t attribute value |
| `event.X` | fixed_t payload field |
| `locals[n]` | fixed_t coroutine local |
| `stacks`, `now`, `timed_out`, `disabled` | fixed_t well-known |
| `e + e`, `e - e`, `e * e`, `e / e` | fixed_t arithmetic |
| `e < e`, `e <= e`, `e > e`, `e >= e`, `e == e`, `e != e` | fixed_t 0 or 1 |
| `e and e`, `e or e`, `not e` | fixed_t 0 or 1 |
| `has_tag(entity, TAG)` | fixed_t 0 or 1 |
| `match(entity, { query })` | fixed_t 0 or 1 |
| `clamp(e,lo,hi)`, `min(a,b)`, `max(a,b)`, `abs(e)` | fixed_t |
| `distance(a,b)`, `is_behind(v,s)`, `is_in_front(v,s)`, `angle_to(v,s)` | fixed_t |
| `random_range(lo,hi)` | fixed_t, deterministic (seeded by tick+entity+opcode) |
| `cooldown_remaining(entity, TAG)` | fixed_t ticks remaining |
| `attr = e`, `attr += e`, `attr -= e`, `attr *= e`, `attr /= e` | fixed_t (assigned value) |
| `locals[n] = e` | fixed_t (assigned value) |
| `if e { e } [else { e }]` | value of taken branch; unit if no else and false |
| `while e { e }` | unit |
| `{ e; e; … e }` | value of last sub-expression |
| `wait e` | unit; suspends coroutine `e` ticks |
| `wait_event TAG [timeout e]` | unit; suspends until matching event or timer |
| `<entity>.emit TAG { k = e, … }` | unit |
| `apply TAG -> <entity>` | unit |
| `remove TAG` | unit |
| `cancel TAG` | unit |
| `despawn <entity>` | unit |
| `spawn TAG [at e] [as <ident>]` | entity ref (or unit if no `as`) |
| `return [e]` | never (exits body) |
| `break [e]` | never (exits enclosing `while`; `while` returns `e` or unit) |
| `continue` | never (next iteration of enclosing `while`) |

`return`, `break`, `continue` have type **never** — they do not produce a
value that flows to the parent expression; the parent expression's value
comes from another branch or is unreachable.

Costs are *not* deducted by an explicit expression. The `costs` block is run
implicitly by the activation pipeline before `on activate`:

1. Evaluate every formula in the `costs` block.
2. For each `attr_tag = value` pair, refuse activation if `caster.<attr_tag> < value`.
3. If any cost fails, `on activate` does not run and no costs are deducted.
   Otherwise deduct atomically and run the body.

### 6.3 Attribute access

Attributes are plain `(tag, fixed_t)` values on an entity. Access by tag
path against an entity reference:

```
self.health
target.armor
source.spell_power
self.cooldown.fireball
```

Both reads and writes are by full attribute tag — there are no `current` /
`base` / `max` sub-fields. To model "current and max", use two separate
attribute tags: `health` and `health.max`. Write the clamp explicitly:
`self.health = clamp(self.health, 0, self.health.max)`.

Bare attribute path with no entity prefix resolves to `self.<path>`.

### 6.4 Formulas

A **formula** is a pure expression — any expression form from § 6.1 that
does not use side-effectful forms (`wait`, `wait_event`, `.emit`, `apply`,
`remove`, `cancel`, `despawn`, `spawn`, `return`, `break`, `continue`).
Formulas always resolve to a fixed-point value. The compiler rejects
side-effectful sub-expressions in formula positions at compile time.

Every numeric position in the language takes a formula:

| Position                                                | Available context                                  |
|---------------------------------------------------------|----------------------------------------------------|
| Entity `attributes { tag = … }` initializer             | `self`, attributes initialised earlier in same block |
| Ability `costs { mana = … }`                            | `self` (caster), `target`                          |
| Ability `on activate` body's nested formulas            | `self`, `target`, ability locals                   |
| Effect `duration = …`, `period = …`                     | `self` (target), `source` (applier), effect locals |
| Effect `every { <entity>.emit … { amount = … } }`        | `self`, `source`, `stacks`, effect locals          |
| Event handler `on TAG { … }` body's formulas            | `self`, `event`, attributes of `self`              |
| Event payload field formulas: `emit T { k = … }`        | scope where `emit` appears                         |
| Modifier values pushed via `apply_mod` / `MOD_ATTR`     | `self`, `source`, modifier locals                  |

Formulas can reference:

- Attributes of any in-scope entity: `self.health`, `target.armor`,
  `source.spell_power`, `self.health.max` (`.max` is just another tag).
- Bare attribute path → resolves to `self.<path>`.
- Ability/effect record fields: `stacks`, `duration_left`, `time_alive`.
  (Well-known fields of the calling record, NOT attributes on the entity.)
- Event payload: `event.amount`, `event.crit`, etc.
- `now` (current `predicted_tick`).
- Constants and operators per § 6.2.

#### Compilation

Each formula compiles to a small bytecode chunk and is stored as a
`formula_id` (16-bit index into `formula_pool[]`). Formulas share the same
op set as scripts (§ 10), but only the pure subset (`LOAD_*`, `STORE_LOCAL`,
arithmetic, `INTRINSIC`). Branches (`JUMP_IF_ZERO`) and intrinsics like
`clamp`, `min`, `max` are allowed; `WAIT_*`, `EMIT`, `APPLY_*`,
`STORE_ATTR` are rejected at compile time.

The compiler resolves identifiers at compile time: a reference to
`self.health` becomes `LOAD_ATTR tree=10` directly (no name lookup at
runtime). Hot attribute tags are pinned to fixed POD-tree indices by the
compiler; cold attributes resolve to `LOAD_ATTR_COLD <tag_id>` which does a
binary search in the entity's `attr_misc` BUFFER.

#### Evaluation

```c
fixed_t formula_eval(uint16_t formula_id, const ctx_t* ctx);
```

`ctx` packs pointers to the relevant entity records (`self`, `target`,
`source`), the calling coroutine record (for locals), and the event payload
(if any). Cost ~10–30 ns for a typical 3-op formula on the stack VM.

#### Cycle detection (init time only)

Prefab `attributes { ... }` initializers can reference other attributes of
the same entity. The compiler builds a per-prefab dependency graph and
emits a topologically-sorted init order. Cycles in attribute initializers
are a compile error: `prefab player attribute health depends on mana, which
depends on health`.

Other formulas (cost, duration, damage, etc.) are evaluated *post-spawn* and
have no init-order constraints — they read whatever value the attribute
holds at evaluation time.

---

## 7. Query DSL (flat, single-level)

The query DSL is the language of `requirements`, `ongoing`, `cancel`, and the
inline `match(<entity>, { ... })` builtin. **Queries are flat, not recursive.**
A query has at most three sections: an implicit *all* (bare clauses), an
optional `any { }`, and an optional `none { }`. The three sections are
combined with AND. There is no nesting.

### 7.1 Grammar

```
<query>       ::= <clause_list>
                  ( "any"  "{" <clause_list> "}" )*
                  ( "none" "{" <clause_list> "}" )*

<clause_list> ::= <clause> ("," <clause>)*           // commas + newlines OK

<clause>      ::= <tag_clause>
                | <predicate>

<tag_clause>  ::= <tag_path>                          // bare: subject has descendant of <tag_path>
                | <subject> "." <tag_path>            // qualified: query a different subject
                | "exact" <tag_path>                  // exact: subject has the tag literally (no descent)
                | "exact" <subject> "." <tag_path>

<predicate>   ::= <formula> <cmp> <formula>
<cmp>         ::= "==" | "!=" | "<" | "<=" | ">" | ">="
```

Order inside the body is fixed: bare clauses first (the implicit `all`),
then zero or more `any { }` groups, then zero or more `none { }` blocks.

### 7.2 Semantics

A query holds iff:

1. Every clause in the implicit `all` (the bare clauses at the body's top
   level) holds, AND
2. For **each** `any { }` group present, at least one of its clauses holds, AND
3. Across **all** `none { }` blocks combined, none of their clauses hold.

Multiple `any { }` groups are independent OR constraints — all must pass.
Multiple `none { }` blocks are sugar — equivalent to one unified `none { }`.

Empty implicit `all` (no bare clauses) → trivially true.
No `any { }` groups → trivially true.
No `none { }` blocks → trivially true.
Empty `any { }` → false; empty `none { }` → true.

### 7.3 No nesting

`all { }` is **not** a keyword (the implicit-all section already covers it).
`any { ... }` and `none { ... }` bodies are flat clause lists — they cannot
contain another `any`/`none`/`all` block. Each clause is a single tag clause
or a single predicate.

Multiple independent OR constraints (e.g. weapon type AND element type, both
requiring an OR match) are expressed as separate `any { }` groups. If you need
disjunction-of-conjunctions (e.g. `(A AND B) OR (C AND D)`), split across
multiple decls or precompute a derived tag.

### 7.4 Tag clauses

Tag clauses match against the *current subject's tag set* (`entity_tags`
buffer or candidate record's tag set, depending on container — see § 2.1).
Two match modes:

**Hierarchical (default).** Bare `T` or `subject.T`:

> Subject S contains a descendant of T iff some `X in S` satisfies
> `X in [T, tag_defs[T].out)`.

Implementation: `lower_bound(S, T)` then check `*it < tag_defs[T].out`. One
compare after a `bsearch`.

**Exact.** `exact T` or `exact subject.T`:

> Subject S contains exactly T iff `T in S` (no descent).

Implementation: `bsearch(S, T) != NULL`. Plain membership.

Use exact for cooldown buckets and any case where you need to gate on a
specific tag without conflating it with descendants. The default
hierarchical form is the right pick everywhere else.

### 7.5 Predicate clauses

Predicates are formula comparisons. Each side is a § 6.4 formula; both sides
are evaluated, then compared exactly (fixed-point).

```
self.strength >= 50
target.health <= target.health.max * 0.25
event.amount > self.armor
now - self.last_hit_tick > 30
```

Bare attribute path on either side resolves to `self.<path>` (§ 6.3).

### 7.6 Naming subjects

Tag clauses default to the *primary* subject of the container (usually
`self`). Prefix with `target.`, `source.`, or `record.` to query another
entity / record:

```
requirements {
  creature                       // self.creature
  target.creature                // target's tag set
  source.element.fire            // applier's tag set (effect requirements)
  self.health > 0                // predicate on self
  target.armor < 100             // predicate on target
}
```

Per-container subject availability:

| Container       | primary subject (`self`)  | also in scope                            |
|-----------------|---------------------------|-------------------------------------------|
| `requirements` (ability) | caster           | `target`                                 |
| `requirements` (effect)  | effect target    | `source`                                 |
| `ongoing`                | effect target    | `source`                                 |
| `cancel`                 | candidate record | `self` = activating caster (use prefix)  |

For `cancel`: bare tag clauses match the candidate record's `tags ∪ owned_tags`.
To match the caster, prefix `self.`.

### 7.7 Examples

```
requirements {
  state.combat_ready
  self.health > self.health.max * 0.25
  any  { weapon.melee, weapon.ranged }      // must have a melee or ranged weapon
  any  { element.fire, element.arcane }     // AND must have fire or arcane element
  none { status.silenced, status.stunned, target.status.invulnerable,
         self.cooldown.fireball > 0 }
}

ongoing {
  none { status.wet, status.frozen, status.fireproof_aura }
}

cancel {
  any  { state.casting, channeled }
  none { ability.uninterruptible }
}
```

### 7.8 Compiled representation

Queries flatten into a clause pool. Each query container in a decl_def
stores slices into `clause_pool[]`. Multiple `any { }` groups are stored as
entries in a separate `any_group_pool[]` indirection.

```c
typedef enum { CLAUSE_TAG = 0, CLAUSE_TAG_EXACT = 1, CLAUSE_PRED = 2 } clause_kind_t;
typedef enum { CMP_EQ=0, CMP_NE, CMP_LT, CMP_LE, CMP_GT, CMP_GE } cmp_op_t;
typedef enum { SUBJ_SELF=0, SUBJ_TARGET=1, SUBJ_SOURCE=2, SUBJ_RECORD=3 } subject_t;

typedef struct {
    uint8_t  kind;          // clause_kind_t
    uint8_t  subject;        // subject_t
    uint8_t  cmp;            // cmp_op_t (CLAUSE_PRED only)
    uint8_t  pad;
    union {
        uint16_t tag_id;     // CLAUSE_TAG / CLAUSE_TAG_EXACT
        struct { uint16_t lhs_formula, rhs_formula; } pred;
    } u;
} clause_t;

typedef struct {
    uint16_t offset, count;  // slice into clause_pool[] for this any-group
} any_group_t;

typedef struct {
    uint16_t all_offset,       all_count;    // implicit-all section
    uint16_t any_groups_offset, any_groups;  // slice into any_group_pool[]; 0 = trivially true
    uint16_t none_offset,      none_count;   // all none-clauses flattened; 0 = trivially true
} tag_query_t;

extern clause_t   clause_pool[];
extern any_group_t any_group_pool[];
```

No tree, no recursion, no allocation.

### 7.9 Evaluator

```c
static inline int eval_clause(const ctx_t* ctx, const clause_t* c) {
    if (c->kind == CLAUSE_TAG) {
        const uint16_t* subj; uint32_t subj_n;
        ctx_get_tag_set(ctx, (subject_t)c->subject, &subj, &subj_n);
        uint16_t T = c->u.tag_id;
        uint16_t out = tag_defs[T].out;
        const uint16_t* it = lower_bound_u16(subj, subj_n, T);
        return it != subj + subj_n && *it < out;
    } else if (c->kind == CLAUSE_TAG_EXACT) {
        const uint16_t* subj; uint32_t subj_n;
        ctx_get_tag_set(ctx, (subject_t)c->subject, &subj, &subj_n);
        return bsearch_u16(subj, subj_n, c->u.tag_id) != NULL;
    } else { /* CLAUSE_PRED */
        fixed_t l = formula_eval(c->u.pred.lhs_formula, ctx);
        fixed_t r = formula_eval(c->u.pred.rhs_formula, ctx);
        switch (c->cmp) {
            case CMP_EQ: return l == r;
            case CMP_NE: return l != r;
            case CMP_LT: return l <  r;
            case CMP_LE: return l <= r;
            case CMP_GT: return l >  r;
            case CMP_GE: return l >= r;
        }
        return 0;
    }
}

int tag_query_eval(const ctx_t* ctx, const tag_query_t* q) {
    const clause_t* p;
    p = &clause_pool[q->all_offset];
    for (uint16_t i = 0; i < q->all_count; i++)
        if (!eval_clause(ctx, &p[i])) return 0;

    const any_group_t* ag = &any_group_pool[q->any_groups_offset];
    for (uint16_t g = 0; g < q->any_groups; g++) {
        int hit = 0;
        p = &clause_pool[ag[g].offset];
        for (uint16_t i = 0; i < ag[g].count; i++)
            if (eval_clause(ctx, &p[i])) { hit = 1; break; }
        if (!hit) return 0;
    }

    p = &clause_pool[q->none_offset];
    for (uint16_t i = 0; i < q->none_count; i++)
        if (eval_clause(ctx, &p[i])) return 0;

    return 1;
}
```

Flat loops, all short-circuiting. No recursion, no stack. Each `any { }` group
adds one outer iteration; typical queries have 0–2 groups. Tag clauses stay
one-bsearch-plus-compare; predicate clauses cost a small formula bytecode each.
Typical query evaluates in well under a microsecond.

### 7.10 Determinism

`clause_pool[]` and `formula_pool[]` are emitted in source-walk order during
codegen. Tag IDs are a pure function of the source set (§ 4). Same source →
byte-identical pools.

---

## 8. Runtime data layout

The scripting layer reuses HibitEcs primitives. **Effects, abilities, and
handlers are stored as records inside BUFFER trees on the target entity, NOT
as separate entities.** This keeps the entity slot count flat regardless of
effect/ability churn.

| Tree                  | Kind     | Purpose                                                        |
|-----------------------|----------|----------------------------------------------------------------|
| `entity` (reserved)   | POD      | entity_t                                                       |
| `destroyed` (reserved)| tag TEMP | despawn marker                                                 |
| `entity_tags`         | BUFFER   | sorted u16 tag IDs                                             |
| `attr_health`, ...    | POD      | hot attributes, single fixed_t per slot                        |
| `attr_misc`           | BUFFER   | cold attributes `{tag_id, value}[]`                            |
| `attr_mods`           | BUFFER   | active modifier stacks `{attr_tag, op, value, source, active}[]` |
| `entity_effects`      | BUFFER   | active effect records; variable-size `effect_hdr_t`, stride-iterable |
| `entity_abilities`    | BUFFER   | granted ability state machines                                 |
| `entity_handlers`     | BUFFER   | event handler subscriptions                                    |
| `events_in`           | BUFFER TEMP | per-tick event inbox, auto-cleared at end_tick              |
| 6–12 category tag-trees | tag    | broad-phase tag presence bitmasks                              |

Record shapes (sketch):

```c
// Variable-size; lives inline in entity_effects BUFFER.
// Layout: [ effect_hdr_t | src_caps[] | tgt_caps[] ]
// Next record: (uint8_t*)r + r->stride.
typedef struct {
    uint16_t stride;           // total bytes of this record; next = (uint8_t*)r + stride
    uint16_t effect_tag;       // -> tag_meta[].effect_idx
    uint16_t asset_idx;        // index into effect_asset_pool[] (capture schema)
    uint16_t stacks;
    uint32_t apply_tick;       // tick when record was created (handler sort key)
    uint32_t end_tick;
    uint32_t source;           // entity_t
    uint32_t next_period_tick;
    uint8_t  enabled;          // set at tick-begin by ongoing eval; 0 = suspended
    uint8_t  n_src_caps;
    uint8_t  n_tgt_caps;
    // effect_cap_t src_caps[n_src_caps]  — snapshotted at application time
    // effect_cap_t tgt_caps[n_tgt_caps]  — snapshotted at application time
} effect_hdr_t;                // 28 bytes (compiler pads trailing u8s to u32 boundary)

typedef struct {
    int32_t  value;            // Q16.16 first — natural 4-byte alignment
    uint16_t attr_tag;
} effect_cap_t;                // 8 bytes (compiler adds 2 trailing bytes)
```

**Compile-time effect asset** — emitted as generated dense C arrays
(`effect_assets.gen.c`), compiled directly into the binary. No file load,
no pointers, cache-friendly. Same source → identical output.

```c
// Flat pool; effect_asset_t indexes into this by offset.
extern const uint16_t cap_slot_pool[];   // attr_tag values, packed u16

typedef struct {
    uint16_t src_caps_offset;  // index into cap_slot_pool[]
    uint16_t tgt_caps_offset;  // index into cap_slot_pool[]
    uint8_t  n_src_caps;
    uint8_t  n_tgt_caps;
} effect_asset_t;              // one per effect decl, sorted by effect_tag id

extern const effect_asset_t effect_assets[];
```

Cap lists derived from the effect's `source { }` / `target { }` blocks.
At application time, the runtime reads `effect_assets[asset_idx]`, snapshots
each declared attr inline into the `effect_hdr_t`. Caps are read-only after.

Inside effect script bodies, the compiler resolves `source.X` / `self.X` for
captured attrs to `LOAD_CAP_SRC` / `LOAD_CAP_TGT` opcodes (reads inline
snapshot). Non-captured attrs fall back to live entity read (`LOAD_ATTR`).
Transparent to the script author.

```c
typedef struct {
    uint16_t tag_id;
    uint16_t source_id;
    int32_t  payload[6];   // (tag_id, value) pairs; up to 3 explicit fields
} event_record_t;

typedef struct {
    uint16_t ability_tag;     // -> tag_meta[].ability_idx
    uint16_t state;            // FSM state
    uint32_t resume_tick;      // <= now means runnable
    uint16_t pc;               // bytecode offset
    uint16_t flags;            // waiting_event, etc.
    int32_t  locals[4];        // inline coroutine frame
} ability_record_t;

typedef struct {
    uint16_t tag_in;           // = tag_id; subtree start
    uint16_t tag_out;          // tag_defs[tag_id].out
    uint16_t script_id;
    uint16_t flags;
} handler_record_t;
```

All records are POD, layout fully owned by the compiler — entity_effects,
entity_abilities, entity_handlers, attr_mods all roll back automatically via
the existing BUFFER predicted/confirmed split, and `events_in` is wholesale
cleared each tick by the existing `ECS_TREE_FLAG_TEMPORARY` reap.

---

## 9. Numerics & determinism

- All numbers are 32-bit fixed-point (Q16.16) using `ecs_fixed.h`. No floats.
- Time literals (`5s`, `0.25s`) compile to integer tick counts using a
  compile-time `TICKS_PER_SECOND` constant.
- RNG uses a deterministic stream seeded by `(predicted_tick, entity_id,
  opcode_index)`. No global RNG state.

---

## 10. Bytecode VM

**Register-based, 32-bit instructions, computed-goto dispatch.**

- No stack. Each instruction encodes destination + source registers directly.
  Avoids push/pop traffic; registers map to a small fixed array in the frame.
- Instructions are one 32-bit word — single aligned fetch, predictable decode.
- Dispatch via computed goto (`goto *dispatch_table[op]`) — zero indirect-branch
  misprediction overhead vs switch-case.
- Bytecode lives in flat `const uint32_t script_pool[]` (generated C array).
  `script_id` = start index into that array.
- Coroutine state: `pc` (index into `script_pool`) + `R[0..N-1]` (register
  file) live in the calling record (`ability_record_t` / `effect_record_t`).
  Yield = save `pc`, return to caller. Resume = jump to saved `pc`.

### 10.1 Instruction encoding

```
 31      24 23    16 15     8 7      0
 [  op:8  ][  A:8  ][  B:8  ][  C:8  ]   format iABC
 [  op:8  ][  A:8  ][     Bx:16      ]   format iABx   (unsigned 16-bit immediate)
 [  op:8  ][  A:8  ][    sBx:16      ]   format iAsBx  (signed 16-bit offset)
 [  op:8  ][          Ax:24          ]   format iAx    (24-bit immediate / jump offset)
```

- `A`, `B`, `C` — register indices (0–255; typical script uses < 16)
- `Bx` — unsigned 16-bit index (tag id, tree index, const pool index, etc.)
- `sBx` — signed 16-bit PC-relative jump offset
- `Ax` — 24-bit immediate (large const, long jump)

All tag IDs and tree indices are baked as immediates at compile time —
zero runtime name lookup.

### 10.2 Instruction set

| Op | Format | Effect |
|---|---|---|
| `MOVE`          | iABC  | `R[A] = R[B]` |
| `LOAD_CONST`    | iABx  | `R[A] = K[Bx]` (const pool) |
| `LOAD_ATTR`     | iABx  | `R[A] = hot_attr(self, tree=Bx)` |
| `LOAD_ATTR_T`   | iABx  | `R[A] = hot_attr(target, tree=Bx)` |
| `LOAD_ATTR_S`   | iABx  | `R[A] = hot_attr(source, tree=Bx)` |
| `LOAD_COLD`     | iABx  | `R[A] = cold_attr(self, tag=Bx)` (bsearch attr_misc) |
| `LOAD_CAP_SRC`  | iABx  | `R[A] = src_caps[Bx].value` (effect snapshot) |
| `LOAD_CAP_TGT`  | iABx  | `R[A] = tgt_caps[Bx].value` (effect snapshot) |
| `LOAD_LOCAL`    | iABx  | `R[A] = locals[Bx]` |
| `LOAD_PAYLOAD`  | iABx  | `R[A] = event.payload[tag=Bx]` |
| `STORE_ATTR`    | iABx  | `hot_attr(self, tree=Bx) = R[A]` |
| `STORE_ATTR_T`  | iABx  | `hot_attr(target, tree=Bx) = R[A]` |
| `STORE_COLD`    | iABx  | `cold_attr(self, tag=Bx) = R[A]` (upsert attr_misc) |
| `STORE_LOCAL`   | iABx  | `locals[Bx] = R[A]` |
| `ADD`           | iABC  | `R[A] = R[B] + R[C]` |
| `SUB`           | iABC  | `R[A] = R[B] - R[C]` |
| `MUL`           | iABC  | `R[A] = R[B] * R[C]` (Q16.16 fixed mul) |
| `DIV`           | iABC  | `R[A] = R[B] / R[C]` (Q16.16 fixed div) |
| `ADD_K`         | iABx  | `R[A] = R[A] + K[Bx]` (fused load+add; avoids separate LOAD_CONST) |
| `MUL_K`         | iABx  | `R[A] = R[A] * K[Bx]` |
| `NEG`           | iABC  | `R[A] = -R[B]` |
| `CMP_LT`        | iABC  | `R[A] = R[B] <  R[C]` (0 or 1) |
| `CMP_LE`        | iABC  | `R[A] = R[B] <= R[C]` |
| `CMP_EQ`        | iABC  | `R[A] = R[B] == R[C]` |
| `CMP_NE`        | iABC  | `R[A] = R[B] != R[C]` |
| `AND`           | iABC  | `R[A] = R[B] && R[C]` |
| `OR`            | iABC  | `R[A] = R[B] \|\| R[C]` |
| `NOT`           | iABC  | `R[A] = !R[B]` |
| `JUMP`          | iAx   | `pc += Ax` (signed, relative) |
| `JUMP_F`        | iAsBx | `if !R[A]: pc += sBx` |
| `JUMP_T`        | iAsBx | `if  R[A]: pc += sBx` |
| `INTRINSIC`     | iABC  | `R[A] = builtin[B](R[C], …)` (clamp/min/max/abs/distance/…) |
| `HAS_TAG`       | iABC  | `R[A] = has_tag(entity=R[B], tag=C)` (hierarchical) |
| `QUERY_EVAL`    | iABx  | `R[A] = tag_query_eval(self, query_pool[Bx])` |
| `EMIT`          | iABC  | emit event tag=A; payload regs R[B..B+C-1] (alternating name/value pairs) |
| `APPLY_EFFECT`  | iABx  | apply effect tag=Bx to entity R[A] (enqueue pending) |
| `REMOVE_EFFECT` | iABx  | remove effect tag=Bx from self (enqueue pending) |
| `CANCEL_ABILITY`| iABx  | cancel ability tag=Bx on self |
| `DESPAWN`       | iABC  | despawn entity R[A] |
| `WAIT_TICKS`    | iABC  | save pc; `resume_tick = now + R[A]`; yield |
| `WAIT_EVENT`    | iABx  | save pc; set WAIT_EVENT flag; timeout reg R[A] if Bx != 0; yield |
| `RETURN`        | iABC  | return R[A]; end coroutine |

### 10.3 Dispatch loop

```c
// Computed-goto dispatch — one indirect branch per instruction, fully predicted.
static const void* dispatch_table[NUM_OPS] = { &&op_MOVE, &&op_LOAD_CONST, … };

#define NEXT  do { instr = script_pool[pc++]; goto *dispatch_table[instr >> 24]; } while(0)

void vm_resume(vm_frame_t* f) {
    uint32_t instr;
    uint32_t pc   = f->pc;
    int32_t* R    = f->regs;       // register file (stack-allocated, small, hot in L1)
    NEXT;

    op_ADD: R[A] = fx_add(R[B], R[C]); NEXT;
    op_MUL: R[A] = fx_mul(R[B], R[C]); NEXT;
    op_LOAD_ATTR: R[A] = *hot_attr_ptr(ctx, Bx); NEXT;
    // … one label per opcode
}
```

Register file `R[0..N-1]` is a small `int32_t` array in `vm_frame_t`
(typically N ≤ 16 for game scripts). Fits entirely in L1 cache with the
instruction stream. No heap allocation per invocation.

---

## 11. Compile pipeline

Compiler runs at startup (or hot-reload). No file output — all passes produce
in-memory tables that live in `script_db_t`. Same source → identical `script_db_t`
contents and identical `schema_crc`.

### 11.1 Pass 1 — Tag harvest (lightweight scan)

No full parse. Lex each file with a simple tokeniser (identifiers, dots,
braces, `=`, string literals, comments stripped). Walk token stream looking
only for positions where a tag literal can appear (§ 4.1). Collect every
tag string found. Promote ancestors inline: encountering `damage.fire` adds
`damage` and `damage.fire` to the set. Result: one `std::unordered_set<string>`
(or equivalent) of unique tag strings across all files.

Files enumerated in **sorted path order** before scanning begins. Order is
deterministic on every platform.

### 11.2 Pass 2 — ID assignment

1. Sort the tag string set lexicographically → `sorted_tags[]`.
2. Assign `tag_id = index` (0-based).
3. For each tag, compute:
   - `parent`: index of longest strict prefix in `sorted_tags[]`; `0xFFFF` if none.
   - `out`: first index whose string does NOT start with `this_tag + "."` — the
     exclusive end of this tag's subtree. Computed by linear scan forward from
     `tag_id + 1`.
4. Build `tag_meta[]` in parallel: for each tag that is also a decl name, record
   kind (`ENTITY/ABILITY/EFFECT/TAG`) and a slot index (filled in pass 4).
5. Emit `tag_ids.gen.h`: one `#define TAG_FOO <id>` per tag (screaming-snake
   of the dot-path).

### 11.3 Pass 3 — Storage layout

**Hot attribute selection.** Count how many times each attribute tag is
referenced across all script bodies (reads + writes). Pick the top N
(implementation constant, e.g. 64) most-referenced as *hot*: each gets a
dedicated POD tree. All remaining attributes are *cold* (`attr_misc` BUFFER,
looked up by tag ID at runtime). N and the selection are a pure function of
source → stable across builds.

**Record size computation.** For each effect decl, count `source { }` and
`target { }` capture slots → `n_src_caps` and `n_tgt_caps`. These are baked
into the effect asset and determine the allocation size of each `effect_hdr_t`
at application time.

### 11.4 Pass 4 — Full parse → AST → def tables

Full recursive-descent parse of every file (in sorted order) → per-file AST.
No global mutable state during parse; each file produces an isolated AST.

**AST node kinds (sketch):**
```
DeclNode        { kind, name_tag, children[] }
BlockNode       { block_kind, items[] }
QueryNode       { all[], any_groups[][], none[] }
FormulaNode     { op, children[] | literal | attr_ref | builtin_call }
StmtNode        { stmt_kind, operands[] }
OnHookNode      { tag_or_keyword, body: StmtNode[] }
```

After parse, walk all decl ASTs and build flat def tables:

| Table | Contents |
|---|---|
| `prefab_defs[]` | tag lists, attr init formulas (topo-sorted), effect/ability/handler lists |
| `ability_defs[]` | requirements query, costs, cooldown entries, cancel query, owned_tags, script refs |
| `effect_defs[]` | requirements query, ongoing query, duration/period formulas, owned_tags, attr mod specs, script refs |
| `effect_assets[]` | src/tgt cap slot lists (from `source { }` / `target { }` blocks) |
| `formula_pool[]` | deduplicated compiled formula chunks (see pass 5) |
| `clause_pool[]` | flattened query clauses |
| `any_group_pool[]` | any-group indirection for queries |

All tables use integer offsets — no pointers.

**Attribute initializer topo-sort.** For each prefab decl, build a dependency
graph among its `attributes { tag = formula }` entries (formula references
earlier tags in the same block). Topological sort; cycle → compile error.

### 11.5 Pass 5 — Codegen

Walk each script body (ability `on activate`, `on end`; effect `every`, `on TAG`;
prefab `on init`, `on despawn`) and emit bytecode into `script_pool[]`.

**Expression codegen** (recursive, post-order → stack machine):
- Literal → `LOAD_CONST`
- Attribute ref `self.X` → `LOAD_ATTR tree` (hot) or `LOAD_ATTR_COLD tag` (cold).
  For captured attrs in effect bodies → `LOAD_CAP_SRC` / `LOAD_CAP_TGT`.
- Binary op → recurse left, recurse right, emit `ADD`/`SUB`/`MUL`/`DIV`/`CMP_*`.
- Builtin call → recurse args, emit `INTRINSIC id`.

**Register allocation.** Simple linear-scan allocator over a small virtual
register file. Each sub-expression is assigned a destination register by the
codegen; the allocator tracks liveness and reuses freed registers. Typical
game script uses 4–12 registers total — no spill needed.

**Expression codegen** (each expression returns its result register `dst`):
- Literal → `LOAD_CONST dst, Kx`
- `self.attr` (hot) → `LOAD_ATTR dst, tree`; `source.attr` → `LOAD_ATTR_S`; `target.attr` → `LOAD_ATTR_T`
- `self.attr` (cold) → `LOAD_COLD dst, tag`
- Captured attr → `LOAD_CAP_SRC dst, slot` / `LOAD_CAP_TGT dst, slot`
- `a + b` → codegen `a`→rA, codegen `b`→rB, `ADD dst, rA, rB`; free rA, rB
- `a + K` (constant RHS) → codegen `a`→rA, `ADD_K dst, rA, Kx`; free rA
- `if e { a } [else { b }]` → codegen `e`→rC; `JUMP_F rC, Lelse`; codegen
  `a`→rA; `MOVE dst, rA`; `JUMP Lend`; Lelse: codegen `b`→rB (or `LOAD_CONST 0`);
  `MOVE dst, rB`; Lend:
- `while e { body }` → Ltop: codegen `e`→rC; `JUMP_F rC, Lend`; codegen body
  (result discarded); `JUMP Ltop`; Lend: `LOAD_CONST dst, 0`. `break e`:
  codegen `e`→rB; `MOVE dst, rB`; `JUMP Lend`.
- `{ e1; e2; … en }` → codegen each; all but last result discarded (register freed).
- `attr = e` → codegen `e`→rV; `STORE_ATTR tree, rV`; `MOVE dst, rV`
- `wait e` → codegen `e`→rT; `WAIT_TICKS rT`; `LOAD_CONST dst, 0`
- `<entity>.emit TAG { k=e,… }` → codegen each value into consecutive regs
  `R[base..base+n-1]`; `EMIT TAG, base, n`; `LOAD_CONST dst, 0`
- `apply TAG -> entity` → `APPLY_EFFECT entity_reg, tag`; `LOAD_CONST dst, 0`
- `return e` → codegen `e`→rV; `RETURN rV`

All tag IDs, tree indices, and formula IDs are baked as immediate operands —
zero runtime name lookup.

**Formula deduplication.** Identical formula bytecode sequences are merged into
one `formula_pool` entry. Comparison is byte-exact on the emitted bytecode.

### 11.6 Pass 5+6 — Codegen + blob fill

Unity DOTS BlobAsset strategy. Two passes:

1. **Measure pass** — walk all ASTs, sum byte requirements for every struct,
   clause array, bytecode block, and query without writing. Result: `total_bytes`.
2. **Write pass** — `malloc(total_bytes)`; place `script_blob_t` header at offset 0;
   replay same walk writing data via `blob_writer_t`. Set each `blob_arr_t` field
   with `blob_arr_set(field, target_ptr, count)` which stores
   `field->rel = target_ptr - &field->rel`.

**`blob_arr_t` primitive** (DOTS `BlobArray<T>` equivalent):

```c
typedef struct { int32_t rel; uint32_t count; } blob_arr_t;
#define BLOB_ARR(a, T)  ((T*)((uint8_t*)&(a)->rel + (a)->rel))
```

`rel` is a signed byte offset from `&rel` to the first element. Every ref inside
the blob is self-relative → the blob is fully relocatable, `memcpy`-safe, and
can be memory-mapped.

**`script_db_t` after build:**

```c
typedef struct { script_blob_t* root; uint32_t size; } script_db_t;
```

`root` points to the single malloc. `free(root)` = complete teardown.

`script_blob_t` (at offset 0 of blob):
- `blob_arr_t tag_defs` → `tag_def_t[]`
- `blob_arr_t tag_meta` → `tag_meta_t[]`
- `blob_arr_t prefab_defs` → `prefab_def_t[]` (each has blob_arr_t children)
- `blob_arr_t ability_defs` → `ability_def_t[]`
- `blob_arr_t effect_defs` → `effect_def_t[]`
- `uint64_t schema_crc`

`formula_t = blob_arr_t` (uint32_t bytecode). `tag_query_t` has `blob_arr_t all/any_groups/none`
pointing to `clause_t[]` / `any_group_t[]` inline in the same blob.

Everything is one contiguous allocation. Zero pointer chasing at runtime.

---

## 12. Spawning a prefab

```c
entity_t spawn_prefab(ecs_world_t* world, uint16_t prefab_tag) {
    const prefab_def_t* p = &prefab_defs[tag_meta[prefab_tag].prefab_idx];
    entity_t e = ecs_entity_spawn(world);

    add_tag(world, e, prefab_tag);                       // self tag
    for (i = 0; i < p->tags_count; i++) {                 // extra innate tags
        add_tag(world, e, prefab_extra_tags[p->tags_offset + i]);
    }
    // attribute initializers — evaluated in topo-sorted order
    ctx_t ctx = { .self = e };
    for (i = 0; i < p->attrs_count; i++) {
        const prefab_attr_init_t* a = &prefab_attr_inits[p->attrs_offset + i];
        fixed_t v = formula_eval(a->value_formula, &ctx);
        write_attr(world, e, a->attr_tag, v);
    }
    for (i = 0; i < p->effects_count; i++) {              // default effects
        apply_effect(world, e, prefab_effect_inits[p->effects_offset + i].effect_tag);
    }
    for (i = 0; i < p->abilities_count; i++) {            // granted abilities
        grant_ability(world, e, prefab_ability_inits[p->abilities_offset + i].ability_tag);
    }
    for (i = 0; i < p->handlers_count; i++) {             // event handlers
        const prefab_handler_init_t* h = &prefab_handler_inits[p->handlers_offset + i];
        push_handler(world, e, h->event_tag, h->script_id);
    }
    return e;
}
```

No script execution required for instantiation — pure data writes into the
existing trees.

---

## 13. Activation, application, tick pipeline

### 13.1 Ability activation

`activate(world, caster, ability_tag, target)` runs synchronously. ctx for
all queries/formulas: `{ self = caster, target = target }`.

1. Look up `ability_def_t* ad` from `tag_meta[ability_tag].ability_idx`.
2. **Requirements.** Evaluate `requirements` query. Refuse if false.
3. **Costs (check phase).** For each entry in `costs`:
   - Evaluate formula → `value`.
   - If `caster.<attr> < value`, refuse activation (no partial deduct).
4. **Cancel pass.** Walk caster's `entity_abilities` and `entity_effects`;
   for each record, evaluate `cancel` query against record (subject = record's
   `tags ∪ owned_tags`). Cancel matching records.
5. **Costs (deduct phase).** Atomically deduct: `caster.<attr> -= value`.
6. **Cooldowns.** For each `cooldowns { }` block on the ability, evaluate
   its `duration` formula and `apply` the synthetic cooldown effect to
   `caster`. Cooldowns start now and persist whether or not the body
   completes — body cancellation does NOT cancel cooldowns.
7. **Owned tags.** Add ability's `owned_tags` to caster's `entity_tags`.
   (Cooldown tags arrive via the cooldown effects' own `owned_tags`.)
8. **Push record.** `ability_record_t { ability_tag, state=running, pc=0,
   resume_tick=now }` → caster's `entity_abilities`.
9. **Run.** Resume bytecode of `on activate`; runs until `WAIT_*` or `RETURN`.

### 13.2 Effect application

`apply(world, target, effect_tag, source)` runs synchronously. ctx for all
queries/formulas: `{ self = target, source = source }`.

1. Look up `effect_def_t* ed`.
2. **Requirements.** Evaluate this effect's `requirements` query on target.
   Refuse if false. (Ward-style immunities are expressed here via
   `none { target.immunity.<bucket> }` — see § 3.4.)
3. **Stack rule.** If existing `entity_effects` record with same `effect_tag`
   + same `source` exists, increment stacks (or refresh duration —
   compile-time per-effect choice).
4. **Owned tags.** Add `owned_tags` to target's `entity_tags`.
5. **Duration / period.** Evaluate `duration` and `period` formulas.
6. **Snapshot.** Read each attr in the effect's `source { }` block from the
   source entity → `src_caps[]`. Read each attr in the `target { }` block
   from the target entity → `tgt_caps[]`. Stored inline in the record;
   immutable after this point.
7. **Push record.** Allocate `sizeof(effect_hdr_t) + 6*(n_src_caps + n_tgt_caps)`
   bytes in `entity_effects`; write `effect_hdr_t { effect_tag, asset_idx,
   end_tick = now+duration, next_period_tick = now+period, source,
   enabled = ongoing_initial }` + the snapshots.
   (`ongoing_initial` = result of evaluating `ongoing` query now; 1 if no block.)
7. **Modifiers.** Push the effect's static modifier contributions to
   `attr_mods` with `active = enabled` and back-ref to this record.

### 13.3 Per-tick systems (added to `ecs_pipeline_run`)

**Tick-begin phase** (runs before any script this tick):

`entity_tags` still holds last tick's finalized value when this phase starts.
That previous state is the stable input for step 1; step 2 overwrites it.
`enabled` is never trusted from a prior tick — it is always clobbered here.

1. **Ongoing eval** — stride-walk `entity_effects`; for each record:
   - No `ongoing` block → write `enabled = 1`.
   - Has `ongoing` block → evaluate query against current (= last tick's)
     `entity_tags` and attribute values. Write result to `record->enabled`.
   Single pass. Does NOT read other records' `enabled` written in this same
   pass — those are not visible yet. 1-tick cascade lag is accepted.
2. **Rebuild `entity_tags`** — clear and repopulate from scratch:
   innate tags (entity's own `tags` block) + owned_tags of every record
   where `enabled == 1`. Sort result (bsearch requires sorted u16 array).
   After this step `entity_tags` holds the **current tick's value**.
   All scripts and queries running during this tick read this rebuilt set.
3. **Attribute recompute** — for each attribute:
   `final = base_value + sum(attr_mods of records where enabled == 1)`.
   Write final to hot POD tree / `attr_misc`. No clamp — scripts clamp
   explicitly when they write.

**Tick-run phase** (script execution):

4. **Effect `every` bodies** — stride-walk `entity_effects`; per record:
   skip if `enabled == 0`; run `every` body if `now >= next_period_tick`,
   advance `next_period_tick`; expire if `end_tick <= now` (mark pending removal).
5. **Ability tick** — iterate `entity_abilities`; per record: if
   `state == running` and `resume_tick <= now`, resume bytecode.
6. **Event dispatch** — iterate `events_in`; for each recipient, walk
   handlers + enabled effects' `on TAG` blocks; wake abilities on
   `WAIT_EVENT` matching tag.

   Attribute base writes from scripts (`STORE_ATTR`) take effect immediately
   and are visible to subsequent scripts in the same tick. `apply` / `remove`
   statements enqueue to a **pending queue** — they do NOT modify
   `entity_effects` or `entity_tags` mid-tick.

**Tick-end phase**:

7. **Flush pending queue** — process all queued `apply`/`remove` operations:
   create or destroy `effect_hdr_t` records. Expiry removals (from step 4)
   also processed here.
8. **Clear temporaries** — `ecs_world_end_tick` clears `events_in`
   (TEMPORARY) and reaps despawned entities.

Next tick-begin sees the flushed effect set and rebuilds from it.

All steps registered through `ecs_pipeline_t`. No runtime allocation per tick.

---

## 14. Schema versioning + saves

- Save header includes `schema_crc` and the full path string for every tag
  ID present in the save (read from the blob's string table at save time).
- Load:
  - If `schema_crc == current_schema_crc` → fast path, ship raw IDs.
  - Else build `path → new_id` by walking the current `tag_defs[]` (each
    entry's name reachable via `tag_def_name(blob, &def, &len)`); walk every
    BUFFER tree (`entity_tags`, `attr_mods`, `entity_effects`, `entity_handlers`,
    `entity_abilities`) and remap u16s.
- Network: peers exchange `schema_crc` in handshake; mismatch = hard fail
  (rebuild both sides).

---

## 15. Reserved identifiers

Reserved subject names (cannot be tag names — compiler rejects):

- `self` — current entity (the owner of an ability/effect, or the recipient
  of an event handler).
- `target` — ability/effect target; `null` outside ability/effect activation.
- `source` — applier of an effect; `null` outside effect contexts.
- `record` — candidate ability/effect record inside a `cancel` query.
- `event` — current event payload; `null` outside `on TAG` body.
- `stacks` — current stack count of the calling effect record.
- `now` — current `predicted_tick`.
- `timed_out` — set to 1 by `wait_event TAG timeout T` when the timer
  expires before a matching event arrives; 0 otherwise.
- `disabled` — set to 1 when a `wait`/`wait_event` returns because
  `ongoing` flipped to false during the wait; 0 otherwise.
- `locals[0..N-1]` — inline coroutine frame on the calling ability/effect
  record. N = 4 inline; spill to `ability_locals_ext` BUFFER if more.

Reserved lifecycle hook names (used after `on `):

- `activate`, `end`        (ability)
- `init`, `despawn`        (prefab)

Reserved DSL keywords:

- `all`, `any`, `none`, `exact` — query combinators / clause modifiers.
- `tags`, `owned_tags` — owning tag containers.
- `requirements`, `ongoing`, `cancel` — query containers.
- `costs`, `cooldowns`, `duration`, `period`, `every`, `attributes`, `effects`, `abilities` — body blocks.
- `prefab`, `ability`, `effect` — top-level decls.
- `commands` — top-level command-tag list (§ 2.6).
- `command` — optional documentation prefix inside `commands { }`.
- `input` — per-prefab input layout block (§ 2.6).
- `button`, `stick` — input slot kinds.
- `wait`, `wait_event`, `timeout`, `continue`, `break`, `emit`, `apply`, `remove`, `cancel`, `despawn`,
  `spawn`, `if`, `else`, `while`, `return` — statements.
- `and`, `or`, `not` — boolean operators.
- `match`, `has_tag`, `clamp`, `min`, `max`, `abs`, `random_range`,
  `distance`, `is_behind`, `is_in_front`, `angle_to`,
  `cooldown_remaining`, `for_each_in_radius` — builtins.

None of these may be used as tag names anywhere in the script source.

---

## 16. Source file layout

### 16.1 Compiler (linked into game — runs at startup / hot-reload)

```
src/compiler/
  ecs_lexer.h / ecs_lexer.c
      Tokenizer. Two modes:
      - Lightweight (pass 1): yields only tag-position tokens; skips bodies.
      - Full (pass 4): yields every token for the recursive-descent parser.
      Single file handles both modes via a flag; keeps the token type enum shared.

  ecs_parser.h / ecs_parser.c
      Recursive-descent parser. Consumes the full token stream from ecs_lexer
      and produces an in-memory AST (per-file, no global state).

  ecs_ast.h
      AST node type definitions (DeclNode, BlockNode, QueryNode, FormulaNode,
      OnHookNode, …). No .c — header-only type defs.

  ecs_compiler.h / ecs_compiler.c
      Orchestrates all six passes. Owns the tag set, def tables, formula pool,
      clause pool. Calls into ecs_lexer → ecs_parser → ecs_codegen.
      Entry point: compiler_build(opts, db) → populates script_db_t.

  ecs_codegen.h / ecs_codegen.c
      Pass 5: walks AST bodies, runs linear-scan register allocator, emits
      uint32_t instructions into script_pool[]. Handles formula deduplication.

  (no ecs_emit — pass 6 writes directly into script_db_t malloc'd arrays)
```

### 16.2 Runtime (linked into game binary)

```
src/
  ecs_script.h / ecs_script.c
      Public API surface. Functions: apply_effect, remove_effect, grant_ability,
      cancel_ability, spawn_prefab, despawn. Tick-phase entry points registered
      into ecs_pipeline_t (tick-begin, tick-run, tick-end).

  ecs_vm.h / ecs_vm.c
      Register-based bytecode VM (§ 10). Computed-goto dispatch loop.
      vm_resume(vm_frame_t*) — runs until WAIT_*, RETURN, or end of script.
      No allocation per invocation; register file is inline in vm_frame_t.

  ecs_formula.h / ecs_formula.c
      formula_eval(uint16_t formula_id, const ctx_t*) → fixed_t.
      Runs the pure subset of the VM over formula_pool[]. No coroutine state.

  ecs_effect.h / ecs_effect.c
      effect_hdr_t stride iteration helpers. Tick-begin: ongoing eval, tag
      rebuild, attribute recompute (§ 13.3 steps 1-3). Tick-end: flush pending
      apply/remove queue, expire records.

  ecs_script_query.h / ecs_script_query.c
      tag_query_eval(const ctx_t*, const tag_query_t*) → int (§ 7.9).
      eval_clause for tag (hierarchical + exact) and predicate clauses.
      Used by requirements, ongoing, cancel, and match() builtin.
```

### 16.3 No generated files

No `gen/` directory. Compiler is part of the game binary. At startup:

```c
script_db_t db;
const char* scripts[] = { "data/player.script", "data/abilities.script", ... };
compiler_build(&(compiler_opts_t){ .files = scripts, .file_count = 3 }, &db);
// db now holds all tables; pass to runtime functions
script_tick_begin(&db, world);
```

Game code includes only `ecs_script.h`. All runtime functions take
`const script_db_t*` as first argument.

---

## 17. Open issues

- **Hot/cold attribute selection** — every attribute tag mentioned in any
  entity's `attributes` block becomes a candidate for hot allocation.
  Compiler picks the top N most-referenced attribute tags (across all
  scripts and all entity inits) and assigns them to dedicated POD trees;
  the rest live in `attr_misc`. Stable-across-builds because reference
  counts are a pure function of source.
- **Reserved gaps for hot-reload** — adding a tag shifts every later ID. For
  dev iteration we accept the rebuild cost (bytecode re-emitted, live world
  remapped via tag-path lookup). Reserved-gap padding is *not* part of the
  language — keep determinism simple.
- **Multi-target abilities** — `target` is a single entity in v1. Multi-target
  (cone, AOE) → loop with `for_each_in_radius(...)` builtin; each iteration
  rebinds `target`.
