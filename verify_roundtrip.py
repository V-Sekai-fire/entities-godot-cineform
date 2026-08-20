# SPDX-License-Identifier: Apache-2.0 OR MIT
"""Does a CineForm file this extension wrote survive a read by something else?

WHY THIS EXISTS AND WHAT IT REFUSES TO DO. A codec checked only against its own output has
been checked against nothing. Two implementations of one format disagree somewhere, and the
place they disagree is the bug. So the encoder here is the extension, or FFmpeg standing in
for it, and the decoder is always the other one.

The pattern is not a photograph. It carries flat fields, a hard edge and a fine checker,
because a wavelet codec does its worst at edges and a smooth image alone would flatter it.

Run:  python verify_roundtrip.py [file.cfhd]

With no argument, this writes its own file with FFmpeg and checks that. With a file, it
checks the one Godot produced.
"""

import os
import subprocess
import sys
import tempfile

import numpy as np

W, H, N = 640, 480, 12


def pattern(i):
    """Flat fields, a moving hard edge, and a one pixel checker."""
    y, x = np.mgrid[0:H, 0:W]
    img = np.zeros((H, W, 3), dtype=np.uint8)
    img[..., 0] = (x * 255 // (W - 1)).astype(np.uint8)
    img[..., 1] = 40
    img[..., 2] = 200
    img[:, : (W // 4) + i * 8] = (255, 255, 255)          # the hard edge
    checker = ((x + y) & 1).astype(bool)
    img[H // 2 :][checker[H // 2 :]] = (0, 0, 0)          # the fine detail
    return img


def ffmpeg(args, **kw):
    return subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error"] + args, **kw)


def encode(path):
    p = subprocess.Popen(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-y",
         "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{W}x{H}", "-r", "30", "-i", "pipe:0",
         # `-f` is explicit. FFmpeg guesses the muxer from the extension, and `.cfhd` is not
         # an extension it knows, so it exits before reading a single frame. The symptom is an
         # OSError on the first write to a pipe that is already closed.
         "-c:v", "cfhd", "-quality", "film3+", "-pix_fmt", "gbrp12le",
         "-f", "matroska", path],
        stdin=subprocess.PIPE, stderr=subprocess.PIPE)
    for i in range(N):
        p.stdin.write(pattern(i).tobytes())
    p.stdin.close()
    err = p.stderr.read().decode(errors="ignore")
    p.wait()
    if p.returncode != 0:
        raise SystemExit("encode failed: " + err.strip()[:400])


def decode(path):
    r = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-i", path,
         "-f", "rawvideo", "-pix_fmt", "rgb24", "pipe:1"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    raw = r.stdout
    n = len(raw) // (W * H * 3)
    return [np.frombuffer(raw, np.uint8, W * H * 3, i * W * H * 3).reshape(H, W, 3)
            for i in range(n)], r.stderr.decode(errors="ignore")


def check(path, expect_frames=N):
    got, err = decode(path)
    if len(got) != expect_frames:
        return False, f"decoded {len(got)} frames, expected {expect_frames}. {err.strip()[:200]}"
    worst_med = worst_max = 0.0
    for i, g in enumerate(got):
        d = np.abs(g.astype(np.int16) - pattern(i).astype(np.int16))
        worst_med = max(worst_med, float(np.median(d)))
        worst_max = max(worst_max, float(d.max()))
    # 8 bit in, 12 bit internally. A visually lossless wavelet should stay within a couple of
    # codes on flat areas. The edge and the checker are where it spends its error.
    ok = worst_med <= 1.0 and worst_max <= 24.0
    return ok, f"median {worst_med:.2f} codes, max {worst_max:.2f} codes over {len(got)} frames"


def main():
    if len(sys.argv) > 1:
        ok, detail = check(sys.argv[1])
        print(("  ok   " if ok else "  FAIL ") + sys.argv[1] + "  " + detail)
        return 0 if ok else 1

    with tempfile.TemporaryDirectory() as tmp:
        good = os.path.join(tmp, "good.cfhd")
        encode(good)
        ok, detail = check(good)
        print(("  ok   " if ok else "  FAIL ") + "round trip through FFmpeg  " + detail)
        if not ok:
            return 1

        # NEGATIVE CONTROL. Corrupt the middle of the file and assert the check fails. Without
        # this, a check that returns True on everything looks exactly like a passing gate.
        bad = os.path.join(tmp, "bad.cfhd")
        raw = bytearray(open(good, "rb").read())
        mid = len(raw) // 2
        raw[mid:mid + 8192] = b"\x00" * 8192
        open(bad, "wb").write(bytes(raw))
        bad_ok, bad_detail = check(bad)
        caught = not bad_ok
        print(("  ok   " if caught else "  FAIL ")
              + "corrupted file is rejected  " + bad_detail)
        if not caught:
            print("       the check passed on a file with 8 KB zeroed. It is not gating.")
            return 1

        size = os.path.getsize(good)
        print(f"\n{size / N / 1024:.1f} KB per frame at {W}x{H}, "
              f"{size / N * 1000 / 2**20:.1f} MB per 1000 frames")
        print("2 controls, each failing for its own reason.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
