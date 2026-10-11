# pdftext

[![ci](https://github.com/datumstake/pdftext/actions/workflows/ci.yml/badge.svg)](https://github.com/datumstake/pdftext/actions/workflows/ci.yml)
[![tests](https://img.shields.io/badge/tests-132%20checks-success)](tests/test_pdftext.cpp)
[![platforms](https://img.shields.io/badge/platforms-linux%20%7C%20macos%20%7C%20windows-blue)](.github/workflows/ci.yml)
[![dependencies](https://img.shields.io/badge/dependencies-0-success)](pdftext.h)
[![header only](https://img.shields.io/badge/header--only-1%20file-success)](pdftext.h)
[![license](https://img.shields.io/badge/license-MIT-blue)](LICENSE)

**PDF text extraction in one header file. No dependencies, no build system, no network.**

Copy `pdftext.h` into your project, `#include` it, and you have positioned text out of a
PDF — with the page each fragment came from. That is the whole installation procedure.

```cpp
#include "pdftext.h"

std::vector<uint8_t> bytes = /* read the file however you like */;

for (const auto& line : pdftext::reflow(pdftext::extract_text(bytes)))
    printf("p%d  %s\n", line.page, line.text.c_str());
```

```
$ pdftext invoice.pdf
REMITTANCE ADVICE
Invoice Number INV-2026-0431
Invoice Date 2026-10-09
Total Due 1,284.00
```

Nothing is fetched, nothing is linked, nothing is written to disk. The library does no
I/O at all — you hand it bytes.

---

## Why this exists

MuPDF, pdfium and poppler are excellent and you should probably use one of them.

Use this instead when the *deployment* is the hard part:

- the machine is offline, or air-gapped, and "install a package" is not available
- the deliverable has to be **one static binary** with no DLLs and no redistributable
- it is a CI container and you would rather not add 40 MB and a transitive CVE surface
  to extract a line of text
- it is a desktop tool a customer installs by copying a single file
- the license has to be permissive and the dependency list has to be empty, in writing

A 700-line header you can read in one sitting is a different kind of thing from a
vendored C library. Sometimes that difference is the whole requirement.

## What it handles

Each of these exists because a real file broke on it, and each has a named fixture in
[`tests/corpus/`](tests/corpus) that fails if it regresses.

| | why it matters | fixture |
|---|---|---|
| **DEFLATE / zlib**, written from scratch | no zlib to link; `/FlateDecode` is what almost every PDF uses | `flate.pdf` |
| **Chained filters** — `[/ASCII85Decode /FlateDecode]` | inflating the raw stream bytes returns garbage; the ASCII85 layer has to come off first. ReportLab writes these | `ascii85.pdf` |
| **Subset fonts via `/ToUnicode` CMaps** | the bytes in the string are *glyph indices*, not characters. This is the single most common reason a PDF "has no text". Both `bfchar` and `bfrange` | `cmap.pdf` |
| **Per-page attribution** through `/Kids` | a merged multi-sheet package otherwise flattens into one list — not noisily, but as a plausible wrong answer | `multipage.pdf` |
| **The CTM and the `q`/`Q` stack** | rotated dimension text is painted inside a rotated transform; ignore the matrix and it comes back at the origin or negative | `rotated.pdf` |
| **String syntax** — escapes, nested parens, octal, hex | get it wrong and text truncates at the first stray bracket | `escapes.pdf` |
| **Uncompressed streams** | the fallback path, taken when nothing inflates | `plain.pdf` |
| **Label/value geometry** | the relationship between a label and its value is spatial and nothing else | `form.pdf` |

## What it does **not** do

Stated up front, because a feature list without its limits is marketing.

- **No OCR.** A scanned page is an image. There is no text to extract and this library
  will correctly find nothing — the CLI exits `2` to say so rather than printing an empty
  success. If your input is scans, you need an OCR engine; this is not one.
- **No xref or object-stream walk.** Traversal is a brute-force scan for every
  `stream`/`endstream` pair, inflating whatever inflates. That is *why* it works on both
  classic and modern files without implementing cross-reference streams — but it means a
  PDF's own object graph is not consulted, so a stream that is present in the file but
  unreferenced by any page is still read.
- **No encryption.** A password-protected or RC4/AES-encrypted PDF will not decode.
- **No layout analysis beyond baselines.** `reflow()` groups fragments by y and sorts by
  x. It does not detect tables, columns, or reading order in a multi-column document.
  Column structure is recoverable from the x coordinates you get back, but you write that
  part — it is domain-specific and guessing it in a library does more harm than good.
- **No glyph-level metrics.** Word spacing is inferred from fragment boundaries, not from
  font advance widths, so inter-word spacing inside a single show operation is whatever
  the producer emitted.
- **Not a renderer.** No images, no vectors, no form-field values, no annotations.
- **Not hardened against hostile input to a security boundary.** It is fuzz-smoked under
  ASan/UBSan in CI against 300+ mutated files and must not crash — but if you are parsing
  untrusted uploads, sandbox the process. That advice applies to every PDF parser.

## Install

Copy one file.

```sh
curl -O https://raw.githubusercontent.com/datumstake/pdftext/main/pdftext.h
```

Requires a C++17 compiler. Nothing else. Builds clean with `-Wall -Wextra -Wpedantic
-Werror` on g++ and clang, and `/W4 /WX` on MSVC — [CI proves it on all three
platforms](.github/workflows/ci.yml).

## The CLI

```sh
make                      # builds ./pdftext
./pdftext file.pdf        # reflowed lines (default)
```

| flag | output |
|---|---|
| `-l, --lines` | one reflowed line per output line *(default)* |
| `-r, --raw` | one fragment per line, in paint order |
| `-p, --positions` | a table of page, x, y, text |
| `-j, --json` | `[{"page":1,"x":72.00,"y":700.00,"text":"..."}]` — pipe into `jq` |
| `-c, --csv` | RFC 4180 CSV with a header row |
| `-s, --summary` | page and fragment counts only |
| `--page N` | one sheet only |
| `--tol N` | reflow baseline tolerance, in points (default `2.0`) |

Exit status is meaningful, because a script needs to tell these apart:

| | |
|---|---|
| `0` | text extracted |
| `1` | bad usage, or the file could not be read |
| `2` | the file parsed, and contains no extractable text — **usually a scan** |

## API

The whole public surface, in four declarations:

```cpp
namespace pdftext {

struct TextItem { std::string text; double x, y; int page; };
struct Line     { int page; double y; std::string text; };

// Positioned fragments, in content-stream paint order.
std::vector<TextItem> extract_text(const std::vector<uint8_t>& pdf_bytes);

// Group fragments into lines by baseline, then order down the page and left to right.
std::vector<Line> reflow(const std::vector<TextItem>& items, double tol = 2.0);

// Highest page number any fragment carries. 0 means nothing was attributed.
int page_count(const std::vector<TextItem>& items);

}
```

`extract_text` returns fragments in the order the producing application painted them,
which is *not* reading order — a PDF has no concept of a line. `reflow` is the separate
step that infers one, so you can skip it when you want the raw geometry.

The lower layers are public too, and occasionally that is what you came for:
`inflate()`, `ascii85_decode()`, `pdf_unescape()`, `parse_cmaps()`.

### Working in coordinates

The coordinates are the point. Most extraction work is "find the value to the right of
this label", and the relationship between a label and its value is spatial — nothing in
the file records it.

[`examples/fields.cpp`](examples/fields.cpp) is that lookup, in 60 lines:

```sh
$ c++ -std=c++17 -O2 examples/fields.cpp -o fields
$ ./fields tests/corpus/form.pdf "Invoice Number" "Total Due" "Shipping Method"

field                  value                      traced to
---------------------- -------------------------- ---------------------
Invoice Number         INV-2026-0431              p1 at 300,680 (+228pt)
Total Due              1,284.00                   p1 at 300,512 (+228pt)
Shipping Method        <NOT FOUND>                -

1 field(s) not found. Reported rather than filled in with a guess:
a wrong value that looks right is worse than a gap a human can see.
```

Two things in that output are deliberate, and they are the difference between a demo and
something you would put in front of an accounts-payable team:

1. **The missing field says `<NOT FOUND>`** and the process exits non-zero. It does not
   return an empty string and it does not grab the nearest plausible thing on the page. A
   wrong value that looks right is the failure mode that costs money.
2. **Every hit carries the page and coordinates it came from.** An extracted value you
   cannot trace back to a spot on the page is not auditable, and a human reviewing a
   hundred of them needs to be pointed somewhere.

## Tests

```sh
make test
```

```
plain:      uncompressed stream; two fragments on one baseline, painted out of order
form:       label/value rows sharing a baseline -- geometry is the only link
flate:      /FlateDecode -- the common case
ascii85:    /Filter [/ASCII85Decode /FlateDecode] -- filters chain
cmap:       subset font: glyph indices decoded through a /ToUnicode CMap
multipage:  three pages via /Kids; every fragment attributed to its sheet
rotated:    rotated text inside q/cm/Q -- position must land on the sheet
escapes:    literal string escapes, nested parens, octal bytes, and hex strings

132 checks, 0 failure(s)
```

**The parser does not grade its own homework.** The corpus is assembled byte by byte by
[`tools/make_corpus.py`](tools/make_corpus.py) — standard library only, no PDF toolkit —
and that generator writes the `.expected.txt` beside each fixture, because it knows what
text it drew and where. The extractor is measured against the producer's declaration.
CI regenerates the corpus and fails on any diff, so the expectations can never quietly
drift into a rubber stamp.

Hand-written fixtures rather than library-generated ones for the same reason: a library
emits whatever it prefers, so the fixture ends up testing the library's taste. Here
`cmap.pdf` exists *because* subset fonts emit glyph indices. When a test fails you know
which PDF construct broke.

CI also runs the suite under **ASan + UBSan**, and a seeded [mutation
sweep](tools/mutate.py) — 300+ truncated, byte-flipped and length-corrupted files —
asserting a documented exit status every time. A malformed PDF is a result, not a crash.

## Two defects this test suite found

Both were in code that built clean and ran successfully, which is the only kind worth
writing a test for.

**Short ASCII85 streams were never detected.** The detector samples the first 64 bytes
and required every one to be in the ASCII85 alphabet — but on a stream shorter than that,
the `~>` terminator falls *inside* the sample window and fails its own check. Every
real-world stream is long enough to hide this. The fixture is not.

**CMap streams were being read as page content.** A `/ToUnicode` CMap is a font resource,
not something painted on a sheet, but the brute-force scan does not know that: it found
the CMap's own `<00> <ff>` codespace literals and emitted them as text — unattributed to
any page, sitting at the top of the output. Found by having a fixture that asserts a line
*count*, not just that the expected lines are present somewhere.

## Provenance

This started as the PDF layer inside a desktop tool that had to read mechanical
engineering drawings on a machine with no network and no installed runtime — which is why
there is a from-scratch DEFLATE decoder in here rather than a `-lz`. The CMap handling,
the CTM tracking and the page attribution were each added after a real CAD export came
back wrong in a way that still looked plausible.

Extracted here unchanged in behaviour: the port is verified byte-for-byte against the
original on the drawings it was developed on — 245 fragments across a 3-sheet package,
every fragment on the same sheet it started on.

## License

MIT. See [LICENSE](LICENSE).

---

Built by **[datumstake](https://github.com/datumstake)** — available for contract work on
document extraction, developer tooling, and systems that have to work offline.
The rest of the set:

[gapsmith](https://github.com/datumstake/gapsmith) — resolve a whole class of porting gaps from rules that carry their own proof ·
[ratchet](https://github.com/datumstake/ratchet) — automation that cannot grade its own work ·
[handle](https://github.com/datumstake/handle) — a logged-in Chrome, nine verbs ·
[focus-three](https://github.com/datumstake/focus-three) — three tasks, one timer, nothing else — a 153 KB native Windows app
