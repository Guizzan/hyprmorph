# hyprmorph — pixel morph for Hyprland

Every window change is a swarm of pixels. A Hyprland plugin that turns window and workspace changes into a pixel morph, inspired by the page morph of a custom SteelSeries Arctis OLED daemon. Pixels break off, each with its own delay and arc, and fly into their new places.

**Demo and details: https://guizzan.github.io/hyprmorph/**

https://github.com/Guizzan/hyprmorph/raw/master/docs/demo.mp4

## What it does

- **Workspace switch** — The content of the old desktop flies, pixel by pixel, into the content of the new one. Pixels that are the same on both stay put; new content streams in from the side you switch toward.
- **Open / close / resize / fullscreen** — Windows that change are rebuilt from their own pixels: each pixel flies from its spot in the old box to the same spot in the new one. Only pixels that actually changed move.
- **Liquid-window drag** — Drag a window (SUPER + mouse) and it becomes a fixed-size blob made of its own pixels, still laid out like the window, with a wobbling outline, a liquid stretch along the movement and a short trail. Drop it and it flies back into place.
- **Live destination** — The target picture is captured every frame, so moving content (video, terminals) keeps updating during the morph.

## Install

Built and tested against **Hyprland 0.56** (Lua config). Plugins are tied to the exact Hyprland version — rebuild after every update.

```sh
make            # builds hyprmorph-dev.so
make install    # copies it to hyprmorph.so with a new inode (safe while Hyprland runs)
```

In your Lua config:

```lua
pcall(hl.plugin.load, os.getenv("HOME") .. "/path/to/hyprmorph/hyprmorph.so")
-- the plugin replaces Hyprland's own window and workspace animations
for _, leaf in ipairs({ "workspaces", "windowsIn", "windowsOut", "windowsMove", "fadeIn", "fadeOut" }) do
    hl.animation({ leaf = leaf, enabled = false })
end
```

Then log out and back in. Never hot-swap the plugin in a running session (unloading it can crash Hyprland), and run only one of these effect plugins at a time.

Try it in a nested session first: `Hyprland -c test/hyprland.lua`.

## See also

[hyprliquid](https://github.com/Guizzan/hyprliquid) (liquid glass) · [hyprhole](https://github.com/Guizzan/hyprhole) (black hole)
