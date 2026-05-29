# HOTD: Scarlet Dawn — Gore Restoration Patch

> created by **Michel Denizob**

> **Disclaimer.** Unofficial fan-made tool — **not affiliated with, authorized, or
> endorsed by SEGA**. "House of the Dead", "Scarlet Dawn" and "SEGA" are trademarks of
> their respective owners, used here only to describe the game this tool operates on.
> This repository ships **no game assets or code** from the game: the tool only edits a
> copy you **already own and obtained lawfully** (it reads your file, computes the
> changes, writes them back). For **educational / personal use**, **no warranty, use at
> your own risk** — and subject to the game's EULA and your local law. Licensed under
> [`LICENSE`](LICENSE) (MIT for the code, plus the notice there).

Re-enables the cut gore on **House of the Dead: Scarlet Dawn** (UE4.18 "Hodzero").
`hotd_gore_patch.c` turns a **vanilla** `Hodzero-WindowsNoEditor.pak` into the
gore-enabled pak **by logic** — every edit is reconstructed and documented from the
vanilla bytes (no prebuilt diff, no asset blob). Single self-contained C file; the only
dependency is zlib.

## What this restores

SEGA developed three gore effects for this game but **disabled all of them before
release due to censorship** — documented on the HOTD wiki, *[List of unused content in
House of the Dead: Scarlet Dawn](https://thehouseofthedead.fandom.com/wiki/List_of_unused_content_in_House_of_the_Dead:_Scarlet_Dawn)*:

> "Three increasingly violent gore effects were developed. Due to censorship, none of
> them were used." — (1) burn marks; (2) a creature's flesh burns away to reveal the
> bones, which then collapse; (3) flesh turns to ash and scatters.

The game data still ships **four unused "burned" models** (`Burned_Small/Middle/Large/Fat`).
This patch re-enables **variant (2)**: it binds the orphaned burned-flesh body, reveals it
through bullet damage, and drops the skeleton at death — the effect SEGA originally intended.

## Build

```bash
make                                  # -> ./hotd_gore_patch
# or directly:
cc -O2 hotd_gore_patch.c -o hotd_gore_patch -lz
# Windows (MinGW):
x86_64-w64-mingw32-gcc -O2 hotd_gore_patch.c -o hotd_gore_patch.exe -lz
```

## Run

```bash
./hotd_gore_patch Hodzero-WindowsNoEditor.pak check    # dry-run, lists changes
./hotd_gore_patch Hodzero-WindowsNoEditor.pak apply    # patch in place
./hotd_gore_patch Hodzero-WindowsNoEditor.pak revert   # undo -> original pak
```

**Safety / revert.** The original bytes `[0..oldsize)` are never modified — `apply` only
*appends* 5 stored (uncompressed) asset copies + a fresh index + footer at end-of-file
and repoints the index. The `revert` command restores the original pak exactly: it
locates the appended region and truncates back to it (and refuses if that signature
isn't present, so it can't corrupt an unrelated file).

## 1. What is modified, and why

All offsets below are in the **decompressed** asset. The gore is gated by the boolean
`bGoreFlag`, hard-set to `false` at init in three damage components; the value lives in
the Kismet bytecode as `EX_False (0x28)` and we flip it to `EX_True (0x27)`.

| # | Asset (in the pak) | Edit | Why | Effect |
|---|---|---|---|---|
| 1 | `BPC_ClothDamage.uexp` | `@5582` `0x28→0x27` | ClothDamage runs the per-zone damage state machine on the **live skin** (its mesh list is populated via `BP_EnemyNormal.UCS → Add Control Mesh`). With the flag on, hits clip the `BLEND_Masked` skin → holes. | bullet holes that reveal flesh (with #4) |
| 2 | `BP_CrashBones.uexp` | `@2103` `0x28→0x27` | ungates the death bone-shards (7 physics bone props at body sockets + `P_ZombieDead_01`). | the "skeleton" at death |
| 3 | `BPC_BurnBodyDamage.uexp` | `@2195` `0x28→0x27` | **NO-OP — same `bGoreFlag` pattern, but inert (see §2).** | none |
| 4 | `BP_EnemyNormal.uasset` + `.uexp` | mesh-bind (3 sub-ops) | the burned-flesh body ships orphaned; we re-wire it. | flesh through holes (alive), clean skeleton (death) |

### Change 4 in detail (the mesh-bind), fully decomposed

The art for a burned-flesh body (`SK_Burned_Middle` + `MI_Burned_*` + `T_Burned_*`)
ships, but the `BurnedBody` `SkeletalMeshComponent` has **no mesh bound** and is hidden,
so it never renders. Three procedural sub-operations fix it (see `meshbind()`,
`set_burnedbody_visible()`, `hide_at_death()` in the source — every magic number is
asserted against the stock asset, so a wrong input fails loudly):

- **4a — Bind the mesh.** Add 3 names (`SkeletalMesh`, `SK_Burned_Middle`, the package
  path), 2 imports (a `Package` outer `-307` + the `SkeletalMesh` object `-308`), one
  `SkeletalMesh` `ObjectProperty` tag (29 B) spliced into `BurnedBody`'s serialized
  template at its `None` terminator, and one Event-Driven-Loader
  `CreateBeforeSerialization` preload entry. Then recompute the package summary offsets,
  every export `SerialOffset`, and the shifted `FirstExportDependency` values.
  *(uasset `90908 → 91101`, uexp `34191 → 34220`.)*
- **4b — Always-visible.** Flip `BP_EnemyNormal.uexp @10689 0x28→0x27`, turning the
  `UserConstructionScript` call `BurnedBody.SetVisibility(False,False)` into
  `SetVisibility(True,False)`. The flesh body becomes visible but is **occluded** by the
  opaque live skin — so it only shows through the cloth holes (#1). *(import `-234` ==
  `SetVisibility`, verified by disassembly.)*
- **4c — Hide at death.** Insert `BurnedBody.SetVisibility(False,False)` (22 B) before
  the `Return` in `StartDieMotion`, so at death the flesh body disappears and the corpse
  goes skin → skeleton cleanly. *(uexp `34220 → 34242`.)*

**Net behaviour (validated in-game):** living enemies show flesh through bullet holes;
on death the flesh body hides and the bone shards (#2) eject.

## 2. Why change #3 (burn) does nothing — proven from bytecode

`BPC_BurnBodyDamage`'s visible effect would come from ramping `BurnWeight` over its
`ClothMaterials` / `MeatMaterials` arrays. But:

- `Initialize` only `Array_Clear`s those two arrays — it never fills them.
- The only fillers, `Init ClothMaterial` / `Init MeatMaterial`, have **zero call sites**
  anywhere (the function names don't even exist in `BP_EnemyNormal` / `BP_EnemyZako` /
  `BP_EnemyBase`).
- So both gated paths (`StartDamageEffect` if `bGoreFlag==true`, `StartDeadEffect` if
  `bGoreFlag==false`) ramp over **empty arrays** → nothing is driven → no pixels change.

Flipping `@2195` therefore has no in-game effect. It is included for completeness (it is
the same `bGoreFlag` pattern as #1 and #2) and can be dropped with no visible difference.

## 3. Verification

The patcher is fully deterministic. Run on a vanilla pak it reconstructs the 5 patched
payloads with these decompressed SHA-1s:

```
BPC_ClothDamage.uexp     0d3e9529…   BP_CrashBones.uexp      15c94df0…
BPC_BurnBodyDamage.uexp  bb6b9658…   BP_EnemyNormal.uasset   f6838779…
BP_EnemyNormal.uexp      c64eca11…
```

The mesh-bind (#4) is reconstructed **procedurally** in code — the new names, imports,
`SkeletalMesh` property and EDL entry are computed from the vanilla asset, not read from
a prebuilt file. Verified end-to-end on a vanilla clone: the output re-parses, its footer
is self-consistent (`footer hash == SHA-1(index)`, so the engine mounts it), every
untouched entry still decompresses, and `revert` truncates it back to the exact original.

## 4. Limitations

- **Version-specific.** Offsets, FName-hash convention and the BurnedBody/export layout
  are for this exact cook (UE4.18 PakFile v4). A different build invalidates them; the
  asserts will fail rather than corrupt anything.
- **One body size.** All common enemies get `SK_Burned_Middle` (single bind on the shared
  `BP_EnemyNormal`). Per-size proportions (Small/Large/Fat) would need a per-subclass
  `BurnedBody.SkeletalMesh` override.
