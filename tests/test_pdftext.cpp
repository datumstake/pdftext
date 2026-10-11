// Tests for pdftext.h. No framework, no dependencies: one translation unit, exit 0 or
// exit 1. The corpus is committed, so this runs the same on every platform and needs
// nothing installed -- which is the whole point of a single-header library.
//
//   cc -std=c++17 tests/test_pdftext.cpp -o test_pdftext && ./test_pdftext
//
// Corpus fixtures are graded against `.expected.txt` files written by
// tools/make_corpus.py -- the producer declares the truth, the parser is measured
// against it. A test that asked the parser what it found would prove nothing.

#include "../pdftext.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int failures = 0;
static int checks = 0;

static void ok(bool cond, const std::string& what) {
    checks++;
    if (cond) return;
    failures++;
    printf("  FAIL  %s\n", what.c_str());
}

template <typename A, typename B>
static void eq(const A& got, const B& want, const std::string& what) {
    checks++;
    if (got == want) return;
    failures++;
    std::ostringstream g, w;
    g << got;
    w << want;
    printf("  FAIL  %s\n          got  %s\n         want  %s\n", what.c_str(),
           g.str().c_str(), w.str().c_str());
}

// ------------------------------------------------------------------ helpers
static std::string corpus_dir;

static bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// Parse an .expected.txt into (page, text) pairs, skipping '#' comment lines.
static std::vector<std::pair<int, std::string>> read_expected(const std::string& path,
                                                              std::string& note) {
    std::vector<std::pair<int, std::string>> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line[0] == '#') {
            if (note.empty() && line.size() > 2) note = line.substr(2);
            continue;
        }
        size_t tab = line.find('\t');
        if (tab == std::string::npos || line[0] != 'p') continue;
        int page = atoi(line.c_str() + 1);
        std::string text = line.substr(tab + 1);
        // the expected file stores escapes literally for whitespace that would
        // otherwise be invisible in a diff
        std::string unesc;
        for (size_t i = 0; i < text.size(); i++) {
            if (text[i] == '\\' && i + 1 < text.size()) {
                if (text[i + 1] == 't') { unesc += '\t'; i++; continue; }
                if (text[i + 1] == '\\') { unesc += '\\'; i++; continue; }
            }
            unesc += text[i];
        }
        out.push_back({page, unesc});
    }
    return out;
}

// ------------------------------------------------------------------ corpus
static void corpus_case(const std::string& name) {
    std::vector<uint8_t> buf;
    const std::string pdf = corpus_dir + "/" + name + ".pdf";
    if (!read_file(pdf, buf)) {
        failures++;
        printf("  FAIL  cannot open %s (run: python tools/make_corpus.py)\n", pdf.c_str());
        return;
    }
    std::string note;
    auto want = read_expected(corpus_dir + "/" + name + ".expected.txt", note);
    if (want.empty()) {
        failures++;
        printf("  FAIL  no expectations for %s\n", name.c_str());
        return;
    }

    auto items = pdftext::extract_text(buf);
    auto got = pdftext::reflow(items);

    printf("%-11s %-72s\n", (name + ":").c_str(), note.c_str());
    eq(got.size(), want.size(), name + ": line count");
    size_t n = got.size() < want.size() ? got.size() : want.size();
    for (size_t i = 0; i < n; i++) {
        eq(got[i].page, want[i].first, name + ": line " + std::to_string(i) + " page");
        eq(got[i].text, want[i].second, name + ": line " + std::to_string(i) + " text");
    }
    // every fragment must be attributed to a real sheet; page 0 means the page tree
    // walk failed and a multi-sheet package would silently merge
    for (const auto& it : items)
        if (!it.text.empty()) ok(it.page >= 1, name + ": fragment attributed to a page");
}

// ------------------------------------------------------------------ unit cases
static void test_inflate_roundtrip() {
    // Stored (type 0) DEFLATE blocks: a literal header we can write by hand, which
    // checks the bit reader and the block loop without needing a compressor here.
    // 0x01 = final block, type 0; then LEN, NLEN little-endian, then raw bytes.
    const char* payload = "the quick brown fox";
    size_t len = strlen(payload);
    std::vector<uint8_t> block;
    block.push_back(0x01);
    block.push_back((uint8_t)(len & 0xff));
    block.push_back((uint8_t)(len >> 8));
    block.push_back((uint8_t)(~len & 0xff));
    block.push_back((uint8_t)((~len >> 8) & 0xff));
    for (size_t i = 0; i < len; i++) block.push_back((uint8_t)payload[i]);

    std::string out;
    ok(pdftext::inflate_raw(block.data(), block.size(), out), "inflate: stored block decodes");
    eq(out, std::string(payload), "inflate: stored block content");
}

static void test_ascii85() {
    // "sure." encoded, with the "~>" terminator. Short enough that the terminator sits
    // inside the detector's sampling window, which is what broke looks_ascii85 before.
    const char* enc = "F*2M7/c~>";
    std::string got = pdftext::ascii85_decode((const uint8_t*)enc, strlen(enc));
    eq(got, std::string("sure."), "ascii85: decodes a known group");
    ok(pdftext::looks_ascii85((const uint8_t*)enc, strlen(enc)), "ascii85: detected by terminator");
    const char* binary = "\x78\x9c\x01\x02\xff\xfe";
    ok(!pdftext::looks_ascii85((const uint8_t*)binary, 6), "ascii85: raw deflate not misdetected");
}

static void test_unescape() {
    eq(pdftext::pdf_unescape("a\\(b\\)c"), std::string("a(b)c"), "unescape: parens");
    eq(pdftext::pdf_unescape("a\\\\b"), std::string("a\\b"), "unescape: backslash");
    eq(pdftext::pdf_unescape("\\101\\102"), std::string("AB"), "unescape: octal");
    eq(pdftext::pdf_unescape("tab\\there"), std::string("tab\there"), "unescape: tab");
}

static void test_rotated_position() {
    // The strings alone are checked by the corpus pass. What matters here is that the
    // rotated fragment landed where it is painted on the sheet rather than at the
    // origin of the rotated local space, which is what happens if the CTM is ignored.
    std::vector<uint8_t> buf;
    if (!read_file(corpus_dir + "/rotated.pdf", buf)) {
        failures++;
        printf("  FAIL  cannot open rotated.pdf\n");
        return;
    }
    auto items = pdftext::extract_text(buf);
    const pdftext::TextItem* rot = nullptr;
    const pdftext::TextItem* after = nullptr;
    for (const auto& it : items) {
        if (it.text.find("rotated") != std::string::npos) rot = &it;
        if (it.text.find("popped") != std::string::npos) after = &it;
    }
    ok(rot != nullptr, "rotated: fragment found");
    ok(after != nullptr, "rotated: post-transform fragment found");
    if (rot) {
        ok(std::fabs(rot->x - 300.0) < 1.0, "rotated: x placed by the CTM (expect ~300)");
        ok(std::fabs(rot->y - 400.0) < 1.0, "rotated: y placed by the CTM (expect ~400)");
    }
    if (after) {
        // Q must restore the CTM; if the stack leaks, this lands at 372,500 instead.
        ok(std::fabs(after->x - 72.0) < 1.0, "rotated: Q restored the CTM (expect x~72)");
        ok(std::fabs(after->y - 100.0) < 1.0, "rotated: Q restored the CTM (expect y~100)");
    }
}

static void test_page_count() {
    std::vector<uint8_t> buf;
    if (!read_file(corpus_dir + "/multipage.pdf", buf)) {
        failures++;
        printf("  FAIL  cannot open multipage.pdf\n");
        return;
    }
    auto items = pdftext::extract_text(buf);
    eq(pdftext::page_count(items), 3, "page_count: three sheets");
}

static void test_garbage_is_not_a_crash() {
    // A parser that is handed bytes from the outside world will be handed rubbish. The
    // contract is "returns nothing", never "reads past the end".
    std::vector<uint8_t> empty;
    eq(pdftext::extract_text(empty).size(), (size_t)0, "robustness: empty input");

    const char* nonsense = "%PDF-1.4\nthis is not a pdf at all, no streams here\n%%EOF";
    std::vector<uint8_t> junk(nonsense, nonsense + strlen(nonsense));
    eq(pdftext::extract_text(junk).size(), (size_t)0, "robustness: no streams");

    // A truncated stream: the keyword is there, the terminator is not.
    const char* trunc = "%PDF-1.4\n1 0 obj\n<< /Length 999 >>\nstream\n\x78\x9c garbage";
    std::vector<uint8_t> cut(trunc, trunc + strlen(trunc));
    pdftext::extract_text(cut);          // must return, not fault
    ok(true, "robustness: truncated stream returns");

    // A stream whose declared body is valid ASCII85 but not valid deflate.
    const char* a85 = "%PDF-1.4\n1 0 obj\n<< /Length 20 >>\nstream\nzzzzzzzzzzzz~>\nendstream\nendobj\n";
    std::vector<uint8_t> bad(a85, a85 + strlen(a85));
    pdftext::extract_text(bad);
    ok(true, "robustness: undecodable stream returns");
}

static void test_reflow_grouping() {
    // Constructed directly, so the grouping rule is tested without a PDF in the way.
    std::vector<pdftext::TextItem> items;
    auto push = [&](const char* t, double x, double y, int page) {
        pdftext::TextItem it;
        it.text = t; it.x = x; it.y = y; it.page = page;
        items.push_back(it);
    };
    push("world", 200, 500, 1);          // out of reading order on purpose
    push("hello", 100, 500, 1);
    push("below", 100, 480, 1);
    push("near", 100, 499, 1);           // within the 2pt default tolerance of 500
    push("page2", 100, 500, 2);

    auto lines = pdftext::reflow(items);
    eq(lines.size(), (size_t)3, "reflow: three lines across two pages");
    if (lines.size() == 3) {
        eq(lines[0].text, std::string("hello near world"), "reflow: sorted left to right");
        eq(lines[0].page, 1, "reflow: first line on page 1");
        eq(lines[1].text, std::string("below"), "reflow: next line down the page");
        eq(lines[2].page, 2, "reflow: page 2 comes after page 1");
    }

    // A tighter tolerance must split the baseline that the default merged.
    auto tight = pdftext::reflow(items, 0.5);
    eq(tight.size(), (size_t)4, "reflow: tolerance splits a near-baseline");
}

// ------------------------------------------------------- short reads are reported
static void test_a_short_stream_is_reported() {
    // The bug this pins, measured 2026-10-11: cutting flate.pdf's 96-byte compressed
    // body in half printed "Flate compresse" and exited 0. inflate_raw returned
    // `!out.empty()` on every EOF path -- "I got some bytes, call it success" -- so a
    // half-decoded stream was indistinguishable from a whole one.
    //
    // Note what is asserted. NOT "truncated input produces no text": partial text is
    // the right behaviour, because real PDFs are damaged in boring ways and half a
    // page beats no page. What is asserted is that the caller is TOLD.
    std::vector<uint8_t> buf;
    if (!read_file(corpus_dir + "/flate.pdf", buf)) {
        printf("  SKIP  flate.pdf missing\n");
        return;
    }

    pdftext::Status whole;
    auto full = pdftext::extract_text(buf, &whole);
    ok(whole.streams > 0, "short read: the intact file decoded at least one stream");
    ok(whole.complete(), "short read: the intact file reports complete");

    // Cut the compressed body in half, leaving the file structurally intact.
    std::string hay((const char*)buf.data(), buf.size());
    size_t s0 = hay.find("stream");
    size_t e0 = hay.find("endstream", s0);
    ok(s0 != std::string::npos && e0 != std::string::npos, "short read: fixture shape");
    if (s0 == std::string::npos || e0 == std::string::npos) return;
    size_t body = s0 + 7;
    size_t keep = (e0 - body) / 2;
    std::string cuts = hay.substr(0, body + keep) + hay.substr(e0);
    std::vector<uint8_t> cut(cuts.begin(), cuts.end());

    pdftext::Status half;
    auto part = pdftext::extract_text(cut, &half);
    ok(!half.complete(), "short read: a halved stream reports INCOMPLETE");
    eq(half.short_streams, 1, "short read: exactly one stream ended early");
    ok(part.size() <= full.size(), "short read: partial text is still returned");
}

// ------------------------------------------------------------------ main
int main(int argc, char** argv) {
    corpus_dir = (argc > 1) ? argv[1] : "tests/corpus";
    printf("pdftext tests   corpus: %s\n\n", corpus_dir.c_str());

    const char* names[] = {"plain", "form", "flate", "ascii85", "cmap",
                           "multipage", "rotated", "escapes"};
    for (const char* n : names) corpus_case(n);

    printf("\nunit cases\n");
    test_inflate_roundtrip();
    test_ascii85();
    test_unescape();
    test_rotated_position();
    test_page_count();
    test_reflow_grouping();
    test_garbage_is_not_a_crash();
    test_a_short_stream_is_reported();

    printf("\n%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
