"""Every parameter must actually change the picture.

A uniform name that does not match between the C++ and the GLSL is silently
ignored: glGetUniformLocation returns -1, glUniform on -1 is a documented no-op,
and nothing in the build says a word. A control can therefore be completely dead
while everything compiles, links, loads and renders. Nothing else in this repo
catches that.

So: render each parameter at two positions against the two input cards, and
report any that made no difference at all.

    python3 tools/sweep.py [--size WxH] [--jobs N] [--binary PATH]

Exit code 1 means something is dead.

------------------------------------------------------------------ the traps

**This is a MIXER, so every render needs two inputs.** The harness feeds them
itself -- `--input-a` is Dest, the incoming video, and `--input-b` is Src, the
computer's picture -- so the sweep does not have to say, but a render that
somehow reached the plugin with one input would return FF_FAIL and the sweep
would report the render as failed rather than the control as dead.

**Nothing crawls on frame zero.** The crawl and the roll are both functions of
elapsed time, and at t = 0 every phase is 0 whatever the rate is. Clock Error,
Crawl Rate and Roll Rate are all swept thirty frames in.

**The roll only exists below the lock threshold.** Roll Rate is swept with Sync
Quality at 0.2; at the default 1.0 it provably does nothing, because there is no
roll to have a rate.

**The fringe needs a disagreement to colour.** Fringe and the three Edge Tint
channels are swept with a four-Amiga-pixel Key Delay, which at any sweep raster
is a band several output pixels wide rather than the one-pixel default.

**The key colour needs a tolerance wide enough to move inside.** At the
default 0.12 the card's colour 0 is the only thing within reach of the key, so
Key Colour Green and Key Colour Blue swept from end to end key nothing at
either position and read as dead. They are swept with Tolerance at 0.5.

**Amiga Mode is swept at a four-pixel Key Delay.** At the default one-pixel delay
lores against superhires is one output texel against a quarter of one at
320x180. That does change the picture here, but only through the partial
coverage of a quarter texel, which is exactly the rasteriser-dependent kind of
difference a GPU-less runner may round differently. At four pixels the fringe
is four texels against one, and the difference does not rest on the filter.

**Crawl Wrap needs a crawl that reaches the wrap.** At the default Clock Error the
crawl walks about half a pixel in thirty frames, never reaching even the
one-pixel wrap, so a 1-pixel and a 16-pixel wrap render the same frame. It is
swept at Clock Error 0.6 (about 12 Amiga pixels a second), thirty frames in.

**Dissolve only exists at the Dissolve fader position.** At Video and at Overlay
the pot is not in the signal path, which is what a three-position fader means.

**Every name must be unique.** `--set` finds a parameter by name and takes the
first match.

**A dropdown holds its element VALUE.** `gltest --list` prints an option's real
range for exactly this reason.

**Never sweep the About block.** Those are buttons that open a web browser.
"""
import argparse
import concurrent.futures
import os
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = str(ROOT / "build" / "gltest")
SCRATCH = tempfile.mkdtemp(prefix="glsweep")

WIDTH, HEIGHT = 480, 270
FRAMES = 1

# Parameters that cannot or must not be swept, with the reason.
SKIP = {}

# A key delay wide enough that the fringe is several output pixels at any
# sweep raster: -4 Amiga pixels, i.e. 0.5 - 4/16.
WIDE_FRINGE = {"Key Delay": 0.25, "Fringe": 1.0, "_frames": 1}

# The key colour only means anything when the tolerance is wide enough for
# moving it to change which colours fall inside. At the default 0.12 the
# card's colour 0 is the ONLY thing within reach, so green and blue swept
# from end to end key nothing at either position and read as dead -- which
# is what they did, until this was added.
KEY_COLOUR = {"Tolerance": 0.5, "Softness": 0.3}

CONTEXT = {
    "Key Colour Red": KEY_COLOUR,
    "Key Colour Green": KEY_COLOUR,
    "Key Colour Blue": KEY_COLOUR,
    "Amiga Mode": {"Key Delay": 0.25},
    "Clock Error": {"_frames": 30},
    "Crawl Wrap": {"Clock Error": 0.6, "_frames": 30},
    "Crawl Rate": {"_frames": 30},
    "Roll Rate": {"Sync Quality": 0.2, "_frames": 30},
    "Sync Quality": {"_frames": 30},
    "Dissolve": {"Fader": 2},
    "Fringe": {"Key Delay": 0.25, "_frames": 1},
    "Edge Tint Red": WIDE_FRINGE,
    "Edge Tint Green": WIDE_FRINGE,
    "Edge Tint Blue": WIDE_FRINGE,
}


def parameters():
    """id, name, kind, low, high from the harness's own declaration."""
    out = subprocess.run([BIN, "--list"], capture_output=True, text=True)
    if out.returncode != 0:
        print("could not list parameters:", out.stdout, out.stderr)
        sys.exit(1)

    found = []
    for line in out.stdout.splitlines():
        m = re.match(
            r"\s*(\d+)\s+(.+?)\s{2,}(\S+)\s+([\d.eE+-]+)\s+\[\s*([\d.eE+-]+)\s*\.\.\s*([\d.eE+-]+)\s*\]",
            line,
        )
        if m:
            found.append(
                (int(m.group(1)), m.group(2).strip(), m.group(3),
                 float(m.group(5)), float(m.group(6)))
            )
        else:
            m = re.match(r"\s*(\d+)\s+(.+?)\s{2,}(about|text|buffer)\s", line)
            if m:
                found.append((int(m.group(1)), m.group(2).strip(), m.group(3), 0.0, 0.0))
    return found


def render(path, overrides, frames):
    args = [BIN, "--out", path, "--size", f"{WIDTH}x{HEIGHT}", "--frames", str(frames)]
    merged = {k: v for k, v in overrides.items() if not k.startswith("_")}
    for name, value in merged.items():
        args += ["--set", f"{name}={value}"]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        print("render failed:", " ".join(args), r.stdout, r.stderr)
        sys.exit(1)
    return pathlib.Path(path).read_bytes()


def pixels(png):
    """Raw RGBA out of the harness's own PNG (filter 0 rows), so nothing else
    is a dependency."""
    i = 8
    idat = b""
    width = height = 0
    while i < len(png):
        length = int.from_bytes(png[i:i + 4], "big")
        kind = png[i + 4:i + 8]
        data = png[i + 8:i + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(data[0:4], "big")
            height = int.from_bytes(data[4:8], "big")
        elif kind == b"IDAT":
            idat += data
        i += 12 + length
    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for row in range(height):
        out += raw[row * (stride + 1) + 1:(row + 1) * (stride + 1)]
    return out


def difference(a, b):
    pa, pb = pixels(a), pixels(b)
    if len(pa) != len(pb):
        return 1.0, len(pa)
    changed = sum(1 for x, y in zip(pa, pb) if x != y)
    return changed / max(len(pa), 1), changed


def sweep_one(job):
    pid, name, low, high, context = job
    frames = context.get("_frames", FRAMES)

    lo = dict(context)
    hi = dict(context)
    lo[name] = context.get("_low", low)
    hi[name] = context.get("_high", high)

    a = render(f"{SCRATCH}/{pid}_lo.png", lo, frames)
    b = render(f"{SCRATCH}/{pid}_hi.png", hi, frames)
    fraction, count = difference(a, b)
    # Progress as it happens, on stderr, so a run that is cut off by a CI
    # timeout still says how far it got.
    print(f"  swept {pid:3d} {name}", file=sys.stderr, flush=True)
    return pid, name, fraction, count


def main():
    global WIDTH, HEIGHT, BIN

    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--size", default="%dx%d" % (WIDTH, HEIGHT))
    ap.add_argument("--jobs", type=int, default=0)
    ap.add_argument("--binary", default=BIN)
    args = ap.parse_args()
    BIN = args.binary
    if "x" in args.size:
        WIDTH, HEIGHT = (int(v) for v in args.size.split("x", 1))
    jobs = args.jobs or min(8, os.cpu_count() or 1)

    if not pathlib.Path(BIN).exists():
        print(f"{BIN} is not built")
        return 1

    skipped = []
    work = []
    for pid, name, kind, low, high in parameters():
        if kind == "about":
            skipped.append((name, "a button that opens a web browser"))
            continue
        if kind in ("text", "buffer") or name in SKIP:
            skipped.append((name, SKIP.get(name, kind)))
            continue
        context = CONTEXT.get(name, {})
        work.append((pid, name, low, high, context))

    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for r in pool.map(sweep_one, work):
            results.append(r)

    dead = []
    for pid, name, fraction, count in sorted(results):
        if count == 0:
            dead.append(name)
            print(f"DEAD  {pid:4d}  {name}")
        else:
            print(f"ok    {pid:4d}  {name}  ({count} subpixels, {fraction * 100:.2f}%)")

    print()
    for name, why in skipped:
        print(f"skip  {name}: {why}")

    print(f"\n{len(results)} swept, {len(dead)} dead, {len(skipped)} skipped, {jobs} at a time")
    if dead:
        print("\nDEAD CONTROLS: " + ", ".join(dead))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
