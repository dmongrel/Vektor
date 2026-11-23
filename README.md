# Vektor

A Windows screen saver in glowing vector graphics, in the spirit of the arcade vector games
(Tempest, Space War, Battlezone) but with scenes of its own. It is plain C, Win32 and GDI: no GPU
API, no runtime to install.

## What it shows

Five scenes play in a continuous loop, each about 45 to 60 seconds. Every scene hands over to the
next on screen, with no cut or fade to black. Each pass is re-randomized (fault lines, tunnel
branches, tank placement, which station fails first, the debris), so the loop never plays the
same twice.

1. **Tectonic planet.** A rotating wireframe planet tears itself apart along glowing fault lines.
   Magma vents erupt, and the planet splits into two halves that drift apart.
2. **Wormhole transit.** The camera dives into the gap between the halves, which becomes a
   branching tunnel. A ship flies it, seen from behind. Branches it doesn't take implode, and its
   shots set off chain reactions in the tunnel ahead.
3. **Orbital skirmish.** The last tunnel ring tilts into the equator of a sphere. Six to eight
   wireframe tanks fight on its surface, firing arcing missiles whose shockwaves stun their
   neighbours.
4. **Station cascade.** The winner's final salvo goes into orbit and unfolds into a ring of
   stations. One reactor fails and the explosions run round the ring. The planet's gravity pulls
   the debris into a ring of fire.
5. **Debris harvester.** A harvester ship clears the fragments that escaped, pulling them in with a
   tractor beam. Its waste pellets then gather into the next planet.

### How GDI is made to glow

GDI has no alpha blending, antialiasing or additive light, so the look is built around that
([PLAN.md](PLAN.md) has the full reasoning):

- **Glow:** a bright core line with wider, darker strokes of the same hue around it.
- **Fading trails:** colours fade toward black rather than toward transparent, which reads the
  same on a black background.
- **Overlaps and flashes:** lines are drawn with `R2_MERGEPEN`, so crossings OR together and come
  out brighter. Flashes OR a colour over the whole screen.
- **Hidden lines:** wireframe solids hide their back lines, as Battlezone did.
- **Speed:** trails are queued per colour and drawn with one `PolyPolyline` each, and no GDI
  object is created per frame, so the saver can run for hours.

Animation is driven by measured time on a fixed 1/120 s physics step, and line widths scale with
the screen height so 4K doesn't turn to hairlines. With several monitors, one scene spans all of
them, centred on the whole span.

## Installing

Download `vektor.scr` from the [latest release](https://github.com/dmongrel/Vektor/releases/latest).
Right-click it and choose **Install**, or copy it to `C:\Windows\System32` and pick **Vektor** in
Screen Saver Settings.

## Building

### Prerequisites (MSYS2 UCRT64)

- `gcc`, `windres` and `make` (`mingw-w64-ucrt-x86_64-toolchain`)

### Commands

```sh
make            # build vektor.scr
make debug      # build vektor-debug.exe, with the development switches below
make install    # copy vektor.scr to System32 (needs an elevated shell)
make clean
```

`vektor.scr` is linked with `-static`. Without it, the saver runs from the project folder but fails
to start from System32.

## Running

| Argument | Behaviour |
|----------|-----------|
| (none) or `/s` | Full screen across every monitor. Any key, click, wheel or mouse movement ends it, as does another application taking focus |
| `/p <hwnd>` | Live preview in the Screen Saver Settings dialog, at 30 fps (`/p:<hwnd>` also works) |
| `/c` | There is nothing to configure: a message box with the name and version |

### Development switches (`vektor-debug.exe` only)

| Switch | Effect |
|--------|--------|
| `/scene N` | Start at scene N (1–5) |
| `/seed N` | Fix the random seed, so a run can be repeated |
| `/fakemonitors` | Split the screen into two pretend monitors of different heights, with a bezel between, to exercise the multi-monitor code on one screen |
| `/shot <scene> <times> <W> <H> <outdir>` | Render frames of a scene at the given times to BMP files, without a window |
| `/bench <scene> <seconds> <W> <H> <outfile>` | Render every 1/60 s frame of a scene and log the drawing cost |

## Layout

```
src/main.c              modes, window, exit-on-input rules, frame loop, back buffer
src/director.c          the scene order and the handovers between scenes
src/scene_*.c           planet, wormhole, skirmish, stations, harvester
src/gfx.c, src/fx.c     drawing (pens, batched lines, glow, flashes) and particles / explosions
src/debug.c             /shot and /bench (debug builds)
src/vektor.h            shared declarations
vektor.rc, .manifest    the name in the Screen Saver list; Per-Monitor-V2 DPI awareness
PLAN.md                 the design: scenes, transitions, technical approach
```

## License

Vektor is released under the [MIT License](LICENSE).
