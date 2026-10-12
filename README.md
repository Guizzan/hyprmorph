# hyprmorph

Hyprland 0.56 plugin: a workspace switch plays the arctis-hub OLED page morph.
The old picture breaks into tiles that fly (staggered, bowed, ease-in-out cubic,
0.6 s) and reassemble into the new workspace. Only tiles whose content changed
move; they are paired by brightness band, then Hilbert order, so text flies to
text and dark to dark. The destination is captured live every frame.

Needs Hyprland's own workspace animation disabled:
`hl.animation({ leaf = "workspaces", enabled = false })`.

Build: `make` (writes hyprmorph-dev.so), `make install` (swaps hyprmorph.so with a
new inode). The .so can't be truly unloaded (GNU-unique symbols from Hyprland
headers), so to upgrade a running session load a copy under a NEW filename.
Test in a nested session first: `env -u AQ_DRM_DEVICES Hyprland -c test/hyprland.lua`
(set HYPRMORPH_DEBUG=1 to dump the tile pictures to /tmp/hyprmorph-{old,new}.ppm).
Rebuild after every Hyprland update.

NEVER hot-swap the plugin in a running session (`hyprctl plugin unload` + load of a
new copy): on 2026-10-12 that crashed the real session (render-stage hook called into
unmapped code). Install with `make install`, then log out and back in.
