# applications/pocketjs (Edgi overlay)

Product bind for PocketJS on Edgi-Talk. Shared host loop / ESP shims / SCons
fragment live in the monorepo fork:

https://github.com/1024971823/pocketjs/tree/host/rt-thread-edgitalk/hosts/rt-thread-edgitalk/native

## Prefer monorepo host (`POCKETJS_ROOT`)

```sh
export POCKETJS_ROOT=/path/to/pocketjs   # clone of 1024971823/pocketjs
# defaults set by SConscript when POCKETJS_ROOT is valid:
#   POCKETJS_NATIVE_HOST_LOOP=0   # still use this tree's pocketjs_app.c
#   POCKETJS_NATIVE_PACKAGE_STUB=0
export POCKETJS_QUICKJS_ROOT=/path/to/quickjs-ng   # optional
```

Then the usual firmware build includes
`hosts/rt-thread-edgitalk/native/SConscript` for shared components; this
directory still supplies wifi/bt/game/synth/music/dashboard, generated embed,
and (for now) `pocketjs_app.c`.

Unset `POCKETJS_ROOT` to use the **fallback** full in-tree source list (builds
alone, same as before P1).

## Next thinning

Implement `pocketjs_host_board.h` hooks here, set `POCKETJS_NATIVE_HOST_LOOP=1`,
and drop the duplicate loop from `pocketjs_app.c`.
