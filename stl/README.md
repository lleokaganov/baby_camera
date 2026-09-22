# Case

These three files are **not ours**. They are part of
**[ESP32 Cam Case (Ball Joint mount)](https://www.thingiverse.com/thing:6106534)**
by **CasperJohansen**, published on Thingiverse under
**[CC BY 4.0](https://creativecommons.org/licenses/by/4.0/)**.

Copied here unmodified, because a link is a single point of failure and the
files are small. The full archive has more variants than the three below — a
sealed shell for outdoor use, plates for larger lenses, an arm that stands the
camera off the wall. Go to the original if none of these fit.

Note that the root `LICENSE` of this repository (MIT) covers the firmware and
the tools. It does not cover this directory.

| file | what |
|---|---|
| `ESP32Cam-Caseantennahole+3vents.stl` | the shell, both halves on one plate |
| `ESP32Cam-LensAdapter8x8module+smiley.stl` | front plate for the stock 8×8 mm camera |
| `ESP32Cam-MountingKit.stl` | wall plate, ball and knurled nut, all three on one plate |

Assembled, the shell is about 49 × 36 × 26 mm.

## Printing

No supports, nothing to rotate — print as they lie. The author used 0.12 mm
layers at 40% infill; ours came out fine at 0.2 mm with three perimeters, which
matters for the ball joint because it grips by friction.

Screws: 4× M2.5×4 for the lens plate, 4× M2.5×5 to close the shell, 2× M2×5 to
attach the ball. Find them before you start a two-hour print.

**Material.** Something that shrugs off warmth: the board is not cold when it
streams continuously. HIPS and ABS both work; PLA sits far too close to the
edge for a thing that lives over a cot. If your bed sticks well, print the ball
joint in a material with some give — it holds its angle by friction, and a
brittle one splits at the nut.

The vents double as the sound port, so do not fill them in.
