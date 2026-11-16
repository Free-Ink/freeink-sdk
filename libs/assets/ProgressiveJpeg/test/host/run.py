#!/usr/bin/env python3
"""Progressive JPEG host test: decodes generated progressive JPEGs (4:2:0,
4:2:2, 4:4:4, grayscale, restart intervals, odd sizes, truncation) and checks
the luma against djpeg's grayscale decode. Needs Pillow, cjpeg and djpeg.
Set PJ_EXTRA_JPEGS=a.jpg:b.jpg to also check real-world files."""
import os
import random
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.normpath(os.path.join(HERE, "..", ".."))
tmp = tempfile.mkdtemp(prefix="pj-")
tool = os.path.join(tmp, "pjdecode")
cxx = os.environ.get("CXX", "c++")
subprocess.check_call([cxx, "-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-I", os.path.join(LIB, "include"),
                       os.path.join(LIB, "src", "ProgressiveJpeg.cpp"), os.path.join(HERE, "pjdecode.cpp"), "-o", tool])


def scene(w, h, seed):
    rnd = random.Random(seed)
    im = Image.new("RGB", (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = ((x * 255) // w, (y * 255) // h, ((x ^ y) * 3) & 255)
    d = ImageDraw.Draw(im)
    for _ in range(40):
        x0, y0 = rnd.randrange(w), rnd.randrange(h)
        d.rectangle([x0, y0, x0 + rnd.randrange(60), y0 + rnd.randrange(60)],
                    fill=tuple(rnd.randrange(256) for _ in range(3)))
        d.line([rnd.randrange(w), rnd.randrange(h), rnd.randrange(w), rnd.randrange(h)], fill=(0, 0, 0), width=1)
    d.text((10, 10), "FreeInk progressive", fill=(255, 255, 255))
    return im


def read_pgm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(maxsplit=4)
    assert parts[0] == b"P5", path
    w, h = int(parts[1]), int(parts[2])
    return w, h, parts[4][-w * h:]


def check(name, jpg, expect_ok=True, max_mean=1.0, max_peak=24):
    out = os.path.join(tmp, name + ".pgm")
    r = subprocess.run([tool, jpg, out], capture_output=True, text=True)
    if not expect_ok:
        assert r.returncode == 2, f"{name}: expected begin() to refuse, got {r.returncode} {r.stdout}"
        print(f"  {name}: refused as expected ({r.stdout.strip()})")
        return
    if r.returncode == 2 and name.startswith("extra") and r.stdout.strip() == "begin 3":
        print(f"  {name}: not progressive, skipped")
        return
    assert r.returncode == 0, f"{name}: rc {r.returncode} {r.stdout}"
    ref = os.path.join(tmp, name + ".ref.pgm")
    with open(ref, "wb") as f:
        subprocess.check_call(["djpeg", "-grayscale", "-dct", "float", "-pnm", jpg], stdout=f)
    w, h, got = read_pgm(out)
    rw, rh, want = read_pgm(ref)
    assert (w, h) == (rw, rh), f"{name}: size {w}x{h} vs {rw}x{rh}"
    diffs = [abs(a - b) for a, b in zip(got, want)]
    mean, peak = sum(diffs) / len(diffs), max(diffs)
    print(f"  {name}: {w}x{h} mean |diff| {mean:.3f}, max {peak}")
    assert mean <= max_mean and peak <= max_peak, f"{name}: luma differs from djpeg"


def cjpeg(name, src_ppm, *args):
    path = os.path.join(tmp, name + ".jpg")
    with open(path, "wb") as f:
        subprocess.check_call(["cjpeg", *args, src_ppm], stdout=f)
    return path


im = scene(467, 701, 1)
ppm = os.path.join(tmp, "scene.ppm")
im.save(ppm)
pgm = os.path.join(tmp, "scene.pgm")
im.convert("L").save(pgm)

pil = os.path.join(tmp, "pil420.jpg")
im.save(pil, "JPEG", quality=85, progressive=True, subsampling=2)
check("pillow-420", pil)
pil_opt = os.path.join(tmp, "pil-opt.jpg")
im.save(pil_opt, "JPEG", quality=95, progressive=True, optimize=True, subsampling=0)
check("pillow-444-optimized", pil_opt)
check("cjpeg-422-restart-blocks", cjpeg("c422", ppm, "-progressive", "-sample", "2x1", "-restart", "3B"))
check("cjpeg-420-restart-rows", cjpeg("c420r", ppm, "-progressive", "-sample", "2x2", "-restart", "1"))
check("cjpeg-gray", cjpeg("cgray", pgm, "-progressive", "-quality", "70"))
check("cjpeg-gray-restart", cjpeg("cgrayr", pgm, "-progressive", "-restart", "5B"))
tiny = os.path.join(tmp, "tiny.jpg")
scene(9, 5, 2).save(tiny, "JPEG", progressive=True)
check("tiny-9x5", tiny)

base = os.path.join(tmp, "base.jpg")
im.save(base, "JPEG", quality=85)
check("baseline", base, expect_ok=False)

# Truncated stream: must not crash, decodes what arrived (no reference check).
with open(pil, "rb") as f:
    data = f.read()
cut = os.path.join(tmp, "cut.jpg")
with open(cut, "wb") as f:
    f.write(data[: len(data) // 2])
r = subprocess.run([tool, cut, os.path.join(tmp, "cut.pgm")], capture_output=True, text=True)
assert r.returncode == 0, f"truncated: rc {r.returncode} {r.stdout}"
print("  truncated: decoded partial stream")

for i, extra in enumerate(p for p in os.environ.get("PJ_EXTRA_JPEGS", "").split(":") if p):
    check(f"extra-{i}-{os.path.basename(extra)}", extra)

print("ProgressiveJpeg: decodes match djpeg luma")
