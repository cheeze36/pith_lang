# The `gui` module

Pith ships with an **optional** graphical UI module built on
[raylib](https://www.raylib.com/) (window + render loop) and
[raygui](https://github.com/raysan5/raygui) (immediate-mode widgets). It lets a
Pith program open a window and draw interactive controls.

It is **off by default** so the core interpreter stays dependency-free and the
test suite builds anywhere. You opt in at build time.

## Why immediate mode?

raygui is *immediate-mode*: there are no widget objects to create and no
callbacks to register. Every frame you simply call the widgets you want, and
each call draws the widget **and** returns what the user did with it. That maps
directly onto Pith's native functions — a widget is just a function that
returns a value — so there are no closures to call back from C and nothing extra
for the garbage collector to track.

## Building with the GUI enabled

### Prerequisites (Linux)

raylib itself is downloaded and built automatically — you do not install it. But
its desktop windowing backend needs a few system *development* packages, and
which ones depends on whether you target **X11** or **Wayland**. This is
inherent to native GUI on Linux (every toolkit needs these), not specific to
raylib.

**Option A — X11 backend (default, recommended).** Works everywhere, including
on a Wayland session via XWayland (which almost all Wayland desktops run). On
Debian/Ubuntu:

```sh
sudo apt-get install libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl1-mesa-dev
```

**Option B — native Wayland backend.** No X11 at all; pass `-DPITH_GUI_WAYLAND=ON`
at configure time (below). Needs the Wayland dev packages instead:

```sh
sudo apt-get install libwayland-dev wayland-protocols libxkbcommon-dev libegl1-mesa-dev
```

If you are unsure, use Option A — it runs fine under Wayland through XWayland.

### Configure and build

X11 backend (default):

```sh
cmake -DPITH_ENABLE_GUI=ON -S . -B build
cmake --build build -j
```

Native Wayland backend:

```sh
cmake -DPITH_ENABLE_GUI=ON -DPITH_GUI_WAYLAND=ON -S . -B build
cmake --build build -j
```

The first configure fetches raylib 5.0 over the network. raygui 4.0 is vendored
in `third_party/raygui.h`, so only raylib is downloaded.

### Run the demo

```sh
./build/pith_lang examples/gui_demo.pith
```

If you build **without** `-DPITH_ENABLE_GUI=ON`, `import "gui"` still parses, but
the module is empty, so the first `gui.*` call fails cleanly at runtime.

## Program shape

```pith
import "gui"

gui.init(480, 240, "Hello")        # open the window
gui.set_fps(60)
while (!gui.should_close()):        # loop until the user closes it
    gui.begin()                     # start a frame
    gui.clear(245, 245, 245)        # background colour (r, g, b)
    if (gui.button(20, 20, 120, 32, "Click me")):
        print("clicked!")
    gui.end()                       # present the frame
gui.close()                         # tidy up
```

Widgets must be called between `gui.begin()` and `gui.end()`; the module reports
a clear runtime error if you forget.

## API reference

### Lifecycle

| Function | Returns | Description |
| :-- | :-- | :-- |
| `gui.init(width, height, title)` | `void` | Open the window. Call once, before anything else. |
| `gui.set_fps(fps)` | `void` | Set the target frames per second. |
| `gui.should_close()` | `bool` | True when the user closes the window or presses Escape. |
| `gui.begin()` | `void` | Start a frame. |
| `gui.end()` | `void` | Finish and present the frame. |
| `gui.clear(r, g, b)` | `void` | Fill the background. Call right after `gui.begin()`. |
| `gui.close()` | `void` | Close the window and free resources. |

### Widgets

All widgets are immediate-mode: call them every frame.

| Function | Returns | Description |
| :-- | :-- | :-- |
| `gui.button(x, y, w, h, text)` | `bool` | Draw a button; true on the frame it is clicked. |
| `gui.label(x, y, w, h, text)` | `void` | Draw a non-interactive label. |
| `gui.checkbox(x, y, size, text, checked)` | `bool` | Draw a checkbox; returns the new checked state. |
| `gui.slider(x, y, w, h, value, min, max)` | `float` | Draw a slider; returns the new value. |
| `gui.text(x, y, text, size, r, g, b)` | `void` | Draw free-form text at a pixel position. |

### The "pass current, take back new" idiom

`checkbox` and `slider` hold no state of their own. You keep the state in a Pith
variable, pass it in, and assign the return value back:

```pith
bool enabled = true
float volume  = 50.0

# ...inside the frame...
enabled = gui.checkbox(20, 110, 22, "Enabled", enabled)
volume  = gui.slider(20, 150, 180, 18, volume, 0.0, 100.0)
```

## Colours and coordinates

* Colours are three `int` channels, `r`, `g`, `b`, each `0..255` (alpha is
  always opaque). Out-of-range values are clamped.
* Coordinates and sizes are in pixels. `int` or `float` are both accepted.

## Extending the module

The module lives in `lib/gui.c` / `lib/gui.h`. To add a widget:

1. Write a `static Value native_gui_<name>(int arg_count, Value *args)` function
   following the pattern of the others (validate args with the `require_*` /
   `as_*` helpers, call the raygui function, return a `Value`).
2. Register it in `gui_register_module()` with `register_native_method`.

Browse `third_party/raygui.h` for the full list of available controls.
