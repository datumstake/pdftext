#!/usr/bin/env python3
"""Deterministic mutation sweep: corrupt the corpus and check the parser still returns.

This is not a fuzzing campaign and does not pretend to be one. It is a cheap, repeatable
sweep over the mutations a hand-written fixture never thinks of -- truncation mid-stream,
a lying /Length, a flipped byte inside compressed data -- asserting one thing:

    a malformed PDF produces an exit status, never a crash.

Exit 0 (text found), 1 (unreadable) and 2 (no text) are all fine. A negative status on
POSIX means a signal, and on Windows a large status means an unhandled exception; either
is a bug worth a stack trace. Run the binary under ASan/UBSan and this sweep turns a
silent out-of-bounds read into a failure.

    python tools/mutate.py ./pdftext tests/corpus

The RNG is seeded, so a failure reported here reproduces exactly.
"""

import random
import subprocess
import sys
from pathlib import Path

SEED = 20261009
MUTANTS_PER_FILE = 24
TIMEOUT = 20


def mutations(data, rng):
    """Yield (label, bytes) pairs. Deliberately boring, deliberately exhaustive."""
    n = len(data)

    # truncation: the single most common real-world corruption
    for frac in (0.1, 0.25, 0.5, 0.75, 0.9, 0.99):
        cut = int(n * frac)
        yield f"truncate@{frac:g}", data[:cut]

    # the stream keyword present with no terminator
    i = data.find(b"stream")
    if i > 0:
        yield "stream-unterminated", data[: i + 6] + b"\n" + data[i + 6 : i + 40]

    # a /Length that lies in both directions
    for repl in (b"/Length 999999", b"/Length 0", b"/Length -5"):
        j = data.find(b"/Length")
        if j > 0:
            k = data.find(b" ", j + 8)
            yield f"length={repl.decode()}", data[:j] + repl + data[k:]

    # head and tail damage
    yield "no-header", data[9:]
    yield "header-only", data[:9]
    yield "empty", b""
    yield "nul-padded", data + b"\x00" * 64

    # random single-byte flips, seeded so a failure reproduces
    for _ in range(MUTANTS_PER_FILE):
        if n < 2:
            break
        b = bytearray(data)
        pos = rng.randrange(n)
        b[pos] = rng.randrange(256)
        yield f"flip@{pos}", bytes(b)

    # a whole region replaced with noise, which is what a bad transfer looks like
    if n > 100:
        b = bytearray(data)
        start = rng.randrange(n - 50)
        for k in range(start, start + 50):
            b[k] = rng.randrange(256)
        yield f"noise@{start}", bytes(b)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    binary, corpus = sys.argv[1], Path(sys.argv[2])
    scratch = Path("mutants")
    scratch.mkdir(exist_ok=True)

    rng = random.Random(SEED)
    pdfs = sorted(corpus.glob("*.pdf"))
    if not pdfs:
        print(f"no PDFs under {corpus}")
        return 1

    total = 0
    bad = []
    for pdf in pdfs:
        data = pdf.read_bytes()
        for label, mutant in mutations(data, rng):
            total += 1
            target = scratch / "case.pdf"
            target.write_bytes(mutant)
            try:
                p = subprocess.run(
                    [binary, str(target)],
                    capture_output=True,
                    timeout=TIMEOUT,
                )
                rc = p.returncode
            except subprocess.TimeoutExpired:
                bad.append((pdf.name, label, "TIMEOUT"))
                continue
            # 0/1/2 are the documented statuses; anything else is a crash or a signal
            if rc not in (0, 1, 2):
                bad.append((pdf.name, label, f"exit {rc}"))
                err = p.stderr.decode("utf-8", "replace").strip()
                if err:
                    bad[-1] = (pdf.name, label, f"exit {rc}: {err[:300]}")

    print(f"{total} mutants over {len(pdfs)} fixtures, seed {SEED}")
    if bad:
        print(f"\n{len(bad)} failure(s):")
        for name, label, why in bad[:40]:
            print(f"  {name:<16} {label:<24} {why}")
        print(f"\nreproduce: seed {SEED}, same corpus, same ordering")
        return 1
    print("every mutant returned a documented exit status; no crashes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
