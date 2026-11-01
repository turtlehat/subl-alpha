# subl-alpha

Makes the background of Sublime Text 4 see-through on Linux and Windows. The text stays solid and sharp.

On Linux it needs a desktop with a compositor (Wayland or X11) and Sublime Text 4 installed at `/usr/bin/subl`. On Windows it needs Sublime Text 4 installed at `C:\Program Files\Sublime Text`; the see-through background is frosted with Windows acrylic.

## Build

### Linux

You need `gcc`, `make` and the OpenGL development headers (`libglvnd` or `mesa`, depending on your distro).

```sh
make
```

### Windows

You need [Zig](https://ziglang.org/download/) (`winget install --id zig.zig --exact --source winget`).

```powershell
zig cc -O2 -shared -o shim.dll shim.c shim-windows.c -lopengl32 -lgdi32 -luser32
zig cc -O2 -municode -o subl-alpha-windows.exe subl-alpha-windows.c
```

## Use

Close Sublime, then start it with `./subl-alpha-linux` (Linux) or `.\subl-alpha-windows.exe` (Windows) instead of `subl`:

```sh
./subl-alpha-linux                  # editor background 50% see-through
./subl-alpha-linux --editor 0.3     # more see-through (0 = invisible, 1 = solid)
./subl-alpha-linux --chrome         # also make the tabs, sidebar and status bar see-through (same opacity as the editor)
./subl-alpha-linux --editor 0.3 --chrome 0.8   # chrome with its own opacity
./subl-alpha-linux myfile.txt       # anything else is passed to Sublime
```

On Windows the options are the same, and the see-through parts show an acrylic backdrop (opacity never goes fully to 0):

```bat
.\subl-alpha-windows.exe --editor 0.3 --chrome 0.8
```

## License

MIT