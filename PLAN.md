# GDI Vector Screensaver - Project Plan

## Concept
Full-color vector graphics Windows screensaver inspired by classic arcade aesthetics (Tempest, Space War, Battlezone) but with original, cinematic scenarios. Wireframe/geometric style with dynamic explosion effects (not fireworks). The five scenes chain together in a continuous loop. There is no story — each scene is there to look good, and the transitions are visual handoffs, not plot.

Each scene runs about 60 seconds when it has enough action to fill it, or about 45 seconds if it starts to drag, so one full loop is roughly 4-5 minutes. Every pass through a scene is re-randomized (fault lines, tunnel branches, tank placement, which station fails first, debris layout), so the loop never plays the same twice.

---

## Scene 1: Tectonic Planet
A rotating wireframe planet with visible fault lines. The planet isn't being attacked — it's tearing itself apart from internal stress. Cracks glow with increasing intensity, then rupture into cascading explosion sequences as continental plates shift. Magma vents erupt in expanding particle fountains. The planet eventually splits into two fragments that drift apart, trailing debris.

**GDI approach:** Sphere wireframe using parametric equations, rotating via incremental angle updates. Cracks are line segments that grow and branch. Expanding circles for eruptions. Back-facing grid lines are hidden (see Hidden lines below).

**→ Transition to Scene 2:** As the halves drift apart, the camera dives into the glowing gap between them. The fissure's jagged edge closes into a ring, more rings stack up behind it, and the gap becomes the mouth of the wormhole.

---

## Scene 2: Wormhole Transit
A ship travels through a branching 3D wormhole tunnel. Unlike Tempest's static ring structure, this tunnel organically grows and splits. The ship auto-navigates, choosing branches. At junctions, unchosen branches collapse in implosion sequences — shrinking, twisting, then bursting into particle showers. The ship itself occasionally fires forward at tunnel growth nodes, triggering chain-reaction explosions of tunnel segments ahead.

**Camera:** Chase view, slightly behind and above the ship, so the ship's silhouette stays in frame against the tunnel rings and its shots can be seen leaving it.

**GDI approach:** Perspective projection of tube segments. Branching via recursive tunnel generation. Implosion = shrinking circles that invert color then burst.

**→ Transition to Scene 3:** The ship's last shot hits the end of the tunnel. The chain reaction runs backward toward the camera and the rings collapse inward one after another until a single ring is left. That ring tilts into the equator of Scene 3's sphere, and the latitude and longitude lines draw themselves out from it.

---

## Scene 3: Orbital Skirmish
Multiple wireframe tanks (6-8) battle on the outside surface of a rotating spherical platform, viewed from an orbiting camera that circles the battle. Tanks fire arcing missiles at each other; explosions create expanding shockwave rings that temporarily stun nearby tanks. The camera's orbit gradually tightens as the battle intensifies. Tanks that are destroyed explode into cascading debris that falls inward through the sphere's wireframe grid toward its core. The last tank standing fires a final salvo into empty space as the camera pulls back.

**GDI approach:** Spherical coordinate camera system. Tank models from simple boxes and cylinders. Missile arcs via parametric curves. Expanding concentric circles for shockwaves. Tanks on the far side of the sphere are hidden behind it, not drawn through it.

**→ Transition to Scene 4:** The camera keeps pulling back. The final salvo's missiles curve over into orbit around the shrinking sphere, which becomes Scene 4's central planet. Each missile stops at a point on the ring and unfolds into a wireframe station, and the connection cables draw themselves between them.

---

## Scene 4: Station Cascade
A ring of interconnected space stations orbits a central planet. One station's reactor fails — it flashes bright, then explodes, sending a shockwave through the connection cables to adjacent stations. The chain reaction propagates around the ring, each station exploding in sequence. Debris from each explosion strikes the next station, accelerating the cascade. As the ring is destroyed, the planet's gravity pulls most of the debris inward, creating a temporary ring of fire that collapses into the planet's surface. The heavier fragments, thrown outward by the last explosions, escape the pull and are left drifting.

**GDI approach:** Circular orbit calculations for station positions. Connection lines between stations. Sequential explosion timing. Gravity pull via inward velocity vectors on debris particles, with a share of the fragments given escape velocity.

**→ Transition to Scene 5:** The ring of fire sinks into the planet, which dims to a dark outline and drifts off the edge of the frame. The escaped fragments stay put, spread across the screen as the debris field, and the harvester's nose slides in from the opposite edge.

---

## Scene 5: Debris Harvester
A large wireframe harvester ship moves through the debris field left by the previous scene. Instead of breaking up asteroids, it uses a tractor beam (shown as converging lines) to pull smaller debris chunks into its intake. The chunks flash and disappear as they're processed. Occasionally, larger fragments crash into the harvester, causing localized explosions on its hull that it shrugs off. As it harvests, it ejects compressed waste pellets that trail behind it like exhaust. The harvester gradually clears the field, growing slightly larger as it absorbs material, until the screen is clear and it fades out.

**GDI approach:** Tractor beam = converging lines that shorten as objects approach. Harvester as complex wireframe shape. Waste pellets as small circles trailing behind. Growth via scale factor increase.

**→ Transition to Scene 1:** The harvester fades out, but its trail of waste pellets doesn't. The pellets slow, drift together and pull into a single bright point. That point swells into a wireframe sphere, drawn one line at a time, and becomes the new, unbroken planet.

---

## Chain Sequence
1 → 2 → 3 → 4 → 5 → (loop back to 1)

Each transition is part of the outgoing scene's timeline and hands over to the incoming scene without a cut or fade to black.

---

## Technical Approach

The reference for screensaver structure, build and install on this machine is `C:\_C_Projects\_saver-task\GDI-SCREENSAVER.md`. Its checklist of mistakes (section 12) applies here in full.

### Language and build
- Single C program using the Win32 API and GDI. The guide's examples are C++, but nothing in them needs C++; they translate directly.
- Build with gcc from the MSYS2 UCRT64 toolchain: `-municode` with `wWinMain`, `-mwindows`, and **`-static`** (without it the `.scr` runs from the project folder and fails to start from System32).
- Link `-lgdi32 -luser32 -lshell32`.
- Resource script with **STRINGTABLE id 1 = "Vektor"** (the name in the Screen Saver dropdown) and a manifest declaring PerMonitorV2 DPI awareness so the output is not bitmap-stretched on a scaled display.

### Screensaver modes
Windows launches a `.scr` in three modes, and all three are required:
- `/s` (or no argument) — full screen.
- `/p <HWND>` — draw the small preview as a `WS_CHILD` of the given window. Never exit on input in this mode, and keep the frame rate low.
- `/c` — settings. Vektor has no settings, so this shows a message box with the name and version, parented to the given window.
- Accept both `/p 1234` and `/p:1234`, a leading `-` or `/`, and either letter case.

### Window and display
- **No `ChangeDisplaySettings`.** Changing the display mode makes the monitor resync, rearranges windows and leaves other monitors alone. Instead, open one borderless `WS_POPUP`, `WS_EX_TOPMOST` window covering the whole virtual screen (`SM_XVIRTUALSCREEN` ... `SM_CYVIRTUALSCREEN`) at the current resolution.
- **Multiple monitors:** one scene spread across all of them. The camera frames the bounding rectangle of every monitor (`EnumDisplayMonitors`), and each monitor shows its own slice of that one image. Areas of the bounding rectangle that no monitor covers (a shorter monitor beside a taller one) are simply never seen. The composition is centred on the whole span, so with two monitors side by side the centre of the action sits on or near the bezel.
- **`/fakemonitors` (debug builds only):** there is only one monitor here, so `/s /fakemonitors` splits the single screen into two side-by-side fake monitors of different heights, with a black strip between them standing in for the bezel. The scene is spread across both through the same code path real monitors use. That exercises the slicing, the uncovered areas and the gap; real multi-monitor hardware (mixed DPI, negative virtual-screen origins) stays unverified.
- **No registry entry.** Copying the `.scr` into `C:\Windows\System32` is all it takes for Windows to list it; choosing it is the user's job. `make install` does the copy.

### Exit handling
- Exit on key press, any mouse button, mouse wheel, or the mouse moving more than a few pixels.
- Ignore the first `WM_MOUSEMOVE`: it only reports where the pointer already was, and acting on it closes the saver the moment it opens.
- Exit when another application takes focus (`WM_ACTIVATEAPP` with false).
- Swallow `SC_SCREENSAVE` so Windows doesn't start a second copy on top.
- Hide the cursor in full screen; leave it alone in preview.

### Frame timing
- Drive animation from measured elapsed time (`QueryPerformanceCounter`), not from a fixed `Sleep(16)` or `Sleep(33)`. Sleep timing wanders with system load, and the animation speed would wander with it.
- Physics runs on a fixed step (1/120 s) with an accumulator; rendering targets 60 fps and drops to 30 if a frame takes too long. Preview runs at 30.
- Clamp a single frame's elapsed time (e.g. 100 ms) so a stall, such as the machine waking up, doesn't fire one huge simulation step.
- Seed the random generator from `QueryPerformanceCounter`, not `time()`, since the saver can start several times in a second.

### Drawing
- Frame order: Clear → Draw background → Draw scene objects → Draw effects → blit the back buffer to the window.
- **Double buffer:** one off-screen bitmap, created from the *window* DC (created from the memory DC it comes out monochrome), kept between frames and rebuilt only when the size changes. `WM_ERASEBKGND` returns 1 and the window class has no background brush, which stops flicker.
- **Beam off to move.** Reposition with `MoveToEx` and draw with `LineTo`; a `LineTo` used to move leaves a stray line across the screen. Use `Polyline`/`PolyPolyline` for long strokes and trails.
- **No GDI objects created per frame.** A saver runs for hours, and a handle leaked every frame eventually exhausts the GDI quota and drawing stops. Select the stock `DC_PEN`/`DC_BRUSH` once and change colour with `SetDCPenColor`/`SetDCBrushColor`. Wider pens are created once at startup, one per width per palette colour, and kept until exit.
- **Line width scales with resolution.** Base stroke width on screen height (e.g. 1 px at 1080p, 2 px at 1440p, 3 px at 2160p), with thicker strokes for cores and highlights, so 4K doesn't look like hairlines.

### What GDI can't do, and the workaround
GDI has no alpha blending, no antialiasing and no additive light. The look depends on working around that:
- **Fading trails:** fade a colour toward black rather than toward transparent. On a black background that reads the same.
- **Glow:** draw a bright white/yellow core with one or two wider, darker strokes of the same hue around it.
- **Big flashes:** a whole-screen PatBlt that ORs in the flash colour scaled by its strength (ROP `DPo`). Full-screen `AlphaBlend` was tried and cost ~15 ms a frame at 3440x1440.
- **Overlap:** lines are drawn with `R2_MERGEPEN`, so crossings OR together and come out brighter; dots use PatBlt `DPo` the same way. Where a real hot spot is wanted (eruptions, reactor flash), draw it explicitly.
- **Jaggies:** no antialiasing, so lines are left pixel-sharp. That suits the vector-arcade style; it is not worked around.

### Hidden lines
- Wireframe solids — the planet, the skirmish sphere, the tanks, the stations, the harvester — hide their back lines, as Battlezone did. Without it a sphere becomes a tangle.
- Spheres: draw a grid segment only if its midpoint's normal faces the camera.
- Objects on or behind a sphere: hidden if the sphere is between them and the camera (a ray–sphere test against the occluding body).
- Convex models (tanks, stations): back-face culling per face, edges drawn only if they belong to a front-facing face.
- The wormhole is not culled: its rings are meant to be seen through.

### Performance
The open risk is particle count: thousands of particles, each with its own trail, drawn line by line across a full-resolution screen.
- Each scene has a hard particle cap, and old particles are recycled from a fixed pool (no allocation during a scene).
- Trails are stored as short fixed-length ring buffers of points and drawn as 1 px cosmetic lines at every resolution, queued per colour and drawn with one `PolyPolyline` per colour. GDI wide pens cost about 3x as much and per-particle calls dominated the frame; batching took the Scene 1 peak at 3440x1440 from ~50 ms to ~13 ms.
- Measure frame time early, with the heaviest scene (Station Cascade) first.
- Fallback if 60 fps can't be held at native resolution: render to a back buffer at a fraction of the screen size and `StretchBlt` it up. That thickens lines, which suits the style.

### Palette and text
- Neon on black: cyan, magenta, yellow, green, red, white.
- Explosion effects: expanding circles, particle systems with velocity and decay, flash sequences.
- Burning particles: bright dots (white/yellow core) with fading coloured trails (orange → red → dark).
- No text overlays, so no font is embedded.

### Install and test
- `make` builds `vektor.scr`.
- `./vektor.scr /s` runs it full screen directly; `/p` and `/c` are tested from the Screen Saver Settings dialog after `make install` (copies to System32, needs admin).
