# subl-alpha

Makes the background of Sublime Text 4 see-through on Linux. The text stays solid and sharp.

Needs a desktop with a compositor (Wayland or X11) and Sublime Text 4 installed at `/usr/bin/subl`.

## Build

You need `gcc`, `make` and the OpenGL development headers (`libglvnd` or `mesa`, depending on your distro).

```sh
make
```

## Use

Start Sublime with `./subl-alpha` instead of `subl`:

```sh
./subl-alpha                  # editor background 50% see-through
./subl-alpha --alpha 0.3      # more see-through (0 = invisible, 1 = solid)
./subl-alpha --chrome         # also make the tabs, sidebar and status bar see-through
./subl-alpha --alpha 0.6 --chrome
./subl-alpha myfile.txt       # anything else is passed to Sublime
```

## License

MIT
