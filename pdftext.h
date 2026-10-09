// pdftext.h - single-header PDF text extraction. No external libraries.
//
// One file, one include, zero dependencies beyond the C++17 standard library. Hand it
// the bytes of a PDF and get back positioned text fragments with the page each one
// came from.
//
//     #include "pdftext.h"
//     auto items = pdftext::extract_text(bytes);          // positioned fragments
//     for (auto& ln : pdftext::reflow(items))             // grouped into lines
//         printf("p%d  %s\n", ln.page, ln.text.c_str());
//
// Why this exists rather than "just link MuPDF/pdfium/poppler": those are excellent
// and large. If what you need is the text, and the deployment has to be a single
// static binary with no DLLs and no package manager - an offline machine, an air-gapped
// shop, a CI container, a desktop tool a customer installs by copying one file - then a
// 700-line header is a much easier conversation than a vendored C library.
//
// Four pieces, in order:
//   1. inflate()       - raw DEFLATE / zlib decoder (RFC 1950/1951)
//   2. ascii85_decode  - because filters chain: [/ASCII85Decode /FlateDecode]
//   3. ToUnicode CMaps - subset fonts emit glyph indices, not characters
//   4. extract_text()  - text-showing operators out of the content streams
//
// The PDF traversal is deliberately a brute-force stream scan rather than an xref walk.
// Modern PDFs use cross-reference streams and object streams, so a correct xref
// implementation is a large job; scanning the file for every stream/endstream pair,
// inflating whatever inflates, and reading the text operators out of the result
// sidesteps all of it and works on both classic and modern files. The cost of that
// choice is documented in the README under "What it does not do".
//
// MIT licensed. See LICENSE.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pdftext {

// ---------------------------------------------------------------- DEFLATE
struct BitReader {
    const uint8_t* p; size_t n, pos = 0; uint32_t bitbuf = 0; int bitcnt = 0;
    BitReader(const uint8_t* d, size_t len) : p(d), n(len) {}
    int bit() {
        if (bitcnt == 0) { if (pos >= n) return -1; bitbuf = p[pos++]; bitcnt = 8; }
        int b = bitbuf & 1; bitbuf >>= 1; bitcnt--; return b;
    }
    int bits(int c) { int v = 0; for (int i = 0; i < c; i++) { int b = bit(); if (b < 0) return -1; v |= b << i; } return v; }
    void align() { bitcnt = 0; }
};

struct Huff {
    std::vector<int> counts, symbols;
    void build(const uint8_t* lengths, int n) {
        counts.assign(16, 0); symbols.assign(n, 0);
        for (int i = 0; i < n; i++) counts[lengths[i]]++;
        counts[0] = 0;
        std::vector<int> offs(16, 0);
        for (int i = 1; i < 16; i++) offs[i] = offs[i - 1] + counts[i - 1];
        for (int i = 0; i < n; i++) if (lengths[i]) symbols[offs[lengths[i]]++] = i;
    }
    int decode(BitReader& br) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; len++) {
            int b = br.bit(); if (b < 0) return -1;
            code |= b;
            int count = counts[len];
            if (code - count < first) return symbols[index + (code - first)];
            index += count; first += count; first <<= 1; code <<= 1;
        }
        return -1;
    }
};

inline bool inflate_raw(const uint8_t* src, size_t srclen, std::string& out) {
    static const int LBASE[] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const int LEXT[]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const int DBASE[] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const int DEXT[]  = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

    BitReader br(src, srclen);
    Huff fixedL, fixedD; bool fixedReady = false;
    for (;;) {
        int last = br.bit(); if (last < 0) return !out.empty();
        int type = br.bits(2); if (type < 0) return !out.empty();
        if (type == 0) {                                   // stored
            br.align();
            if (br.pos + 4 > br.n) return !out.empty();
            int len = src[br.pos] | (src[br.pos + 1] << 8); br.pos += 4;
            if (br.pos + len > br.n) len = (int)(br.n - br.pos);
            out.append((const char*)src + br.pos, len); br.pos += len;
        } else if (type == 1 || type == 2) {
            Huff L, D; const Huff *pl, *pd;
            if (type == 1) {
                if (!fixedReady) {
                    uint8_t ll[288], dl[30];
                    for (int i = 0; i < 144; i++) ll[i] = 8;
                    for (int i = 144; i < 256; i++) ll[i] = 9;
                    for (int i = 256; i < 280; i++) ll[i] = 7;
                    for (int i = 280; i < 288; i++) ll[i] = 8;
                    for (int i = 0; i < 30; i++) dl[i] = 5;
                    fixedL.build(ll, 288); fixedD.build(dl, 30); fixedReady = true;
                }
                pl = &fixedL; pd = &fixedD;
            } else {
                static const int ORD[] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
                int hlit = br.bits(5) + 257, hdist = br.bits(5) + 1, hclen = br.bits(4) + 4;
                if (hlit < 257) return !out.empty();
                uint8_t clen[19] = {0};
                for (int i = 0; i < hclen; i++) { int v = br.bits(3); if (v < 0) return !out.empty(); clen[ORD[i]] = (uint8_t)v; }
                Huff C; C.build(clen, 19);
                std::vector<uint8_t> lens(hlit + hdist, 0);
                for (int i = 0; i < hlit + hdist;) {
                    int sym = C.decode(br); if (sym < 0) return !out.empty();
                    if (sym < 16) lens[i++] = (uint8_t)sym;
                    else if (sym == 16) { if (!i) return !out.empty(); int r = 3 + br.bits(2); uint8_t prev = lens[i-1]; while (r-- && i < (int)lens.size()) lens[i++] = prev; }
                    else if (sym == 17) { int r = 3 + br.bits(3); while (r-- && i < (int)lens.size()) lens[i++] = 0; }
                    else { int r = 11 + br.bits(7); while (r-- && i < (int)lens.size()) lens[i++] = 0; }
                }
                L.build(lens.data(), hlit); D.build(lens.data() + hlit, hdist);
                pl = &L; pd = &D;
            }
            for (;;) {
                int sym = pl->decode(br); if (sym < 0) return !out.empty();
                if (sym < 256) out.push_back((char)sym);
                else if (sym == 256) break;
                else {
                    sym -= 257; if (sym >= 29) return !out.empty();
                    int len = LBASE[sym] + br.bits(LEXT[sym]);
                    int ds = pd->decode(br); if (ds < 0 || ds >= 30) return !out.empty();
                    int dist = DBASE[ds] + br.bits(DEXT[ds]);
                    if ((size_t)dist > out.size()) return !out.empty();
                    size_t start = out.size() - dist;
                    for (int i = 0; i < len; i++) out.push_back(out[start + i]);
                }
            }
        } else return !out.empty();
        if (last) break;
    }
    return true;
}

inline bool inflate(const uint8_t* src, size_t len, std::string& out) {
    if (len > 2 && (src[0] & 0x0f) == 8 && ((src[0] << 8 | src[1]) % 31) == 0)
        return inflate_raw(src + 2, len - 2, out);      // zlib wrapper
    return inflate_raw(src, len, out);                  // raw deflate
}

// ---------------------------------------------------------------- ASCII85
// Filters chain in PDFs, and ReportLab (among others) writes
// /Filter [/ASCII85Decode /FlateDecode] - so the bytes between stream/endstream are
// ASCII85 text wrapping the deflate data, and inflating them directly returns garbage.
// Detected by the "~>" terminator and the printable character range.
inline bool looks_ascii85(const uint8_t* p, size_t n) {
    if (n < 4) return false;
    size_t tail = n > 6 ? n - 6 : 0;
    bool term = false;
    for (size_t i = tail; i + 1 < n; i++) if (p[i] == '~' && p[i + 1] == '>') term = true;
    if (!term) return false;
    size_t check = n < 64 ? n : 64, ok = 0;
    for (size_t i = 0; i < check; i++) {
        uint8_t c = p[i];
        // '~' and '>' belong to the terminator, which falls inside this window on a
        // short stream - without them a stream under 64 bytes was never recognised.
        if ((c >= '!' && c <= 'u') || c == 'z' || c == '~' || c == '>' || isspace(c)) ok++;
    }
    return ok == check;
}

inline std::string ascii85_decode(const uint8_t* p, size_t n) {
    std::string out;
    uint32_t tuple = 0; int count = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = p[i];
        if (c == '~') break;
        if (isspace(c)) continue;
        if (c == 'z' && count == 0) { out.append(4, '\0'); continue; }
        if (c < '!' || c > 'u') continue;
        tuple = tuple * 85 + (c - '!');
        if (++count == 5) {
            for (int k = 3; k >= 0; k--) out.push_back((char)((tuple >> (k * 8)) & 0xff));
            tuple = 0; count = 0;
        }
    }
    if (count > 0) {                       // partial group
        for (int i = count; i < 5; i++) tuple = tuple * 85 + 84;
        for (int k = 3; k >= 4 - (count - 1); k--) out.push_back((char)((tuple >> (k * 8)) & 0xff));
    }
    return out;
}

// Decode a stream body through whatever filters are actually present.
inline bool decode_stream(const uint8_t* p, size_t n, std::string& out) {
    if (looks_ascii85(p, n)) {
        std::string a85 = ascii85_decode(p, n);
        if (a85.empty()) return false;
        if (inflate((const uint8_t*)a85.data(), a85.size(), out) && out.size() > 8) return true;
        out = a85;                          // ASCII85 only, no deflate
        return out.size() > 8;
    }
    return inflate(p, n, out) && out.size() > 8;
}

// ---------------------------------------------------------------- ToUnicode CMaps
// Real CAD exports subset their fonts: the bytes inside a PDF string are glyph indices
// in a custom order, not characters. Decoding them as WinAnsi yields empty or garbage
// text - which is exactly what happened the first time this parser met a drawing it
// had not generated itself.
//
// The mapping lives in a /ToUnicode CMap stream. Resolving which CMap belongs to which
// font would normally need full xref and object parsing, but there is a shortcut that
// avoids all of it: the CMap declares "/CMapName /R25", and the content stream selects
// that same font with "/R25 9 Tf". Matching on that name links the two without
// resolving a single object reference.
struct CMap {
    std::string name;                                  // e.g. "R25"
    int objnum = -1;                                   // PDF object this stream is
    std::unordered_map<unsigned int, std::string> to;  // code -> UTF-8
};

inline std::string cp_to_utf8(unsigned int cp) {
    std::string o;
    if (cp < 0x80) o.push_back((char)cp);
    else if (cp < 0x800) { o.push_back((char)(0xC0 | (cp >> 6))); o.push_back((char)(0x80 | (cp & 0x3F))); }
    else { o.push_back((char)(0xE0 | (cp >> 12))); o.push_back((char)(0x80 | ((cp >> 6) & 0x3F))); o.push_back((char)(0x80 | (cp & 0x3F))); }
    return o;
}

inline unsigned int hex_at(const std::string& s, size_t& i) {
    while (i < s.size() && s[i] != '<') i++;
    if (i >= s.size()) return 0xFFFFFFFF;
    size_t j = ++i; std::string h;
    while (j < s.size() && s[j] != '>') { if (isxdigit((unsigned char)s[j])) h.push_back(s[j]); j++; }
    i = j + 1;
    return h.empty() ? 0xFFFFFFFF : (unsigned int)strtoul(h.c_str(), nullptr, 16);
}

inline void parse_cmaps(const std::string& s, std::vector<CMap>& out, int objnum = -1) {
    size_t cm = s.find("begincmap");
    if (cm == std::string::npos) return;
    CMap m;
    size_t np = s.find("/CMapName");
    if (np != std::string::npos) {
        size_t sl = s.find('/', np + 9);
        if (sl != std::string::npos) {
            size_t e = sl + 1;
            while (e < s.size() && (isalnum((unsigned char)s[e]) || s[e] == '.' || s[e] == '_' || s[e] == '+')) e++;
            m.name = s.substr(sl + 1, e - sl - 1);
        }
    }
    // bfchar: <src> <dst>
    size_t p = 0;
    while ((p = s.find("beginbfchar", p)) != std::string::npos) {
        size_t end = s.find("endbfchar", p);
        if (end == std::string::npos) break;
        size_t i = p + 11;
        while (i < end) {
            unsigned int src = hex_at(s, i); if (src == 0xFFFFFFFF || i >= end) break;
            unsigned int dst = hex_at(s, i); if (dst == 0xFFFFFFFF) break;
            m.to[src] = cp_to_utf8(dst);
        }
        p = end + 9;
    }
    // bfrange: <lo> <hi> <dstStart>   (array form <lo> <hi> [<d1><d2>...] also handled)
    p = 0;
    while ((p = s.find("beginbfrange", p)) != std::string::npos) {
        size_t end = s.find("endbfrange", p);
        if (end == std::string::npos) break;
        size_t i = p + 12;
        while (i < end) {
            unsigned int lo = hex_at(s, i); if (lo == 0xFFFFFFFF || i >= end) break;
            unsigned int hi = hex_at(s, i); if (hi == 0xFFFFFFFF) break;
            size_t k = i;
            while (k < end && isspace((unsigned char)s[k])) k++;
            if (k < end && s[k] == '[') {
                i = k + 1;
                for (unsigned int c = lo; c <= hi && i < end; c++) {
                    size_t probe = i;
                    while (probe < end && isspace((unsigned char)s[probe])) probe++;
                    if (probe < end && s[probe] == ']') { i = probe + 1; break; }
                    unsigned int d = hex_at(s, i); if (d == 0xFFFFFFFF) break;
                    m.to[c] = cp_to_utf8(d);
                }
                while (i < end && s[i] != ']') i++;
                if (i < end) i++;
            } else {
                unsigned int d0 = hex_at(s, i); if (d0 == 0xFFFFFFFF) break;
                if (hi >= lo && hi - lo < 65536)
                    for (unsigned int c = lo; c <= hi; c++) m.to[c] = cp_to_utf8(d0 + (c - lo));
            }
        }
        p = end + 10;
    }
    m.objnum = objnum;
    if (!m.to.empty()) out.push_back(m);
}

// Resolve which CMap belongs to which font RESOURCE NAME.
//
// The naive shortcut - matching /CMapName to the name used by Tf - is wrong:
// /CMapName is the CMap's own name. The content stream says "/R14 9 Tf", R14 is a
// resource pointing at a font OBJECT, and that object's /ToUnicode points at the CMap.
// Two hops, and there is no way around them.
//
// Full PDF object parsing is not needed for this though: scanning the raw file for the
// two reference patterns resolves the chain directly.
inline std::unordered_map<std::string, const CMap*>
resolve_fonts(const std::vector<uint8_t>& buf, const std::vector<CMap>& cmaps) {
    std::unordered_map<std::string, const CMap*> out;
    std::string raw((const char*)buf.data(), buf.size());

    // object number -> its /ToUnicode target
    std::unordered_map<int, int> objToUni;
    size_t p = 0;
    while ((p = raw.find(" 0 obj", p)) != std::string::npos) {
        size_t ns = p;
        while (ns > 0 && isdigit((unsigned char)raw[ns - 1])) ns--;
        int objn = atoi(raw.c_str() + ns);
        size_t end = raw.find("endobj", p);
        if (end == std::string::npos) end = raw.size();
        size_t tu = raw.find("/ToUnicode", p);
        if (tu != std::string::npos && tu < end) {
            int target = atoi(raw.c_str() + tu + 10);
            if (target > 0) objToUni[objn] = target;
        }
        p = end == raw.size() ? raw.size() : end + 6;
    }

    // resource name -> object number,  e.g.  /R14 14 0 R
    p = 0;
    while ((p = raw.find('/', p)) != std::string::npos) {
        size_t e = p + 1;
        while (e < raw.size() && (isalnum((unsigned char)raw[e]) || raw[e] == '_' || raw[e] == '.')) e++;
        std::string name = raw.substr(p + 1, e - p - 1);
        if (name.size() >= 2 && !isdigit((unsigned char)name[0])) {
            size_t q = e;
            while (q < raw.size() && raw[q] == ' ') q++;
            if (q < raw.size() && isdigit((unsigned char)raw[q])) {
                int objn = atoi(raw.c_str() + q);
                size_t r = q;
                while (r < raw.size() && isdigit((unsigned char)raw[r])) r++;
                if (raw.compare(r, 4, " 0 R") == 0) {
                    auto it = objToUni.find(objn);
                    if (it != objToUni.end())
                        for (auto& cm : cmaps)
                            if (cm.objnum == it->second) { out[name] = &cm; break; }
                }
            }
        }
        p = e;
    }
    return out;
}

// Map each content stream to the PAGE it belongs to.
//
// Without this, a multi-sheet package silently merges every sheet's characteristics
// into one undifferentiated list. On an AS9102 Form 3 the Reference Location is sheet
// AND zone, so merged sheets do not just lose information - they produce a wrong form
// that still looks plausible.
//
// /Kids gives the authoritative page order; file order is the fallback.
inline std::unordered_map<int, int> map_content_to_page(const std::vector<uint8_t>& buf) {
    std::string raw((const char*)buf.data(), buf.size());
    std::unordered_map<int, int> contentToPage;

    // collect Page objects in file order, with their /Contents targets
    std::vector<std::pair<int, std::vector<int>>> pages;   // (pageObj, contentObjs)
    size_t p = 0;
    while ((p = raw.find(" 0 obj", p)) != std::string::npos) {
        size_t ns = p;
        while (ns > 0 && isdigit((unsigned char)raw[ns - 1])) ns--;
        int objn = atoi(raw.c_str() + ns);
        size_t end = raw.find("endobj", p);
        if (end == std::string::npos) end = raw.size();
        std::string body = raw.substr(p, end - p);

        size_t tp = body.find("/Type");
        bool isPage = false;
        while (tp != std::string::npos) {
            size_t q = tp + 5;
            while (q < body.size() && (isspace((unsigned char)body[q]) || body[q] == '/')) q++;
            if (body.compare(q, 4, "Page") == 0 &&
                (q + 4 >= body.size() || body[q + 4] != 's')) { isPage = true; break; }
            tp = body.find("/Type", tp + 5);
        }
        if (isPage) {
            std::vector<int> cs;
            size_t cp = body.find("/Contents");
            if (cp != std::string::npos) {
                size_t q = cp + 9;
                while (q < body.size() && isspace((unsigned char)body[q])) q++;
                bool arr = (q < body.size() && body[q] == '[');
                if (arr) q++;
                for (;;) {
                    while (q < body.size() && isspace((unsigned char)body[q])) q++;
                    if (q >= body.size() || !isdigit((unsigned char)body[q])) break;
                    int cn = atoi(body.c_str() + q);
                    while (q < body.size() && isdigit((unsigned char)body[q])) q++;
                    while (q < body.size() && isspace((unsigned char)body[q])) q++;
                    if (q < body.size() && body[q] == '0') q++;
                    while (q < body.size() && isspace((unsigned char)body[q])) q++;
                    if (q < body.size() && body[q] == 'R') { cs.push_back(cn); q++; }
                    else break;
                    if (!arr) break;
                }
            }
            pages.push_back({objn, cs});
        }
        p = (end == raw.size()) ? raw.size() : end + 6;
    }

    // prefer the /Kids order when it is present and complete
    std::vector<int> order;
    size_t kp = raw.find("/Kids");
    if (kp != std::string::npos) {
        size_t q = raw.find('[', kp);
        size_t e = raw.find(']', q);
        if (q != std::string::npos && e != std::string::npos) {
            std::string kids = raw.substr(q + 1, e - q - 1);
            size_t i = 0;
            while (i < kids.size()) {
                while (i < kids.size() && !isdigit((unsigned char)kids[i])) i++;
                if (i >= kids.size()) break;
                order.push_back(atoi(kids.c_str() + i));
                while (i < kids.size() && isdigit((unsigned char)kids[i])) i++;
                while (i < kids.size() && (isspace((unsigned char)kids[i]) || kids[i]=='0')) i++;
                if (i < kids.size() && kids[i] == 'R') i++;
            }
        }
    }
    std::vector<std::pair<int, std::vector<int>>> ordered;
    if (order.size() == pages.size() && !order.empty()) {
        for (int po : order)
            for (auto& pg : pages) if (pg.first == po) { ordered.push_back(pg); break; }
    }
    if (ordered.size() != pages.size()) ordered = pages;   // fallback: file order

    for (size_t i = 0; i < ordered.size(); i++)
        for (int cn : ordered[i].second) contentToPage[cn] = (int)i + 1;
    return contentToPage;
}

// ---------------------------------------------------------------- PDF text
struct TextItem { std::string text; double x = 0, y = 0; int page = 0; };

// Unescape a PDF literal string: \n \( \) \\ and \ddd octal.
inline std::string pdf_unescape(const std::string& s) {
    std::string o; o.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '\\') { o.push_back(s[i]); continue; }
        if (++i >= s.size()) break;
        char c = s[i];
        switch (c) {
            case 'n': o.push_back('\n'); break;
            case 'r': o.push_back('\r'); break;
            case 't': o.push_back('\t'); break;
            case 'b': o.push_back('\b'); break;
            case 'f': o.push_back('\f'); break;
            case '(': case ')': case '\\': o.push_back(c); break;
            default:
                if (c >= '0' && c <= '7') {
                    int v = 0, k = 0;
                    while (k < 3 && i < s.size() && s[i] >= '0' && s[i] <= '7') { v = v * 8 + (s[i] - '0'); i++; k++; }
                    i--; o.push_back((char)v);
                } else o.push_back(c);
        }
    }
    return o;
}

// Pull text-showing operators out of one decoded content stream.
inline void scan_content(const std::string& c, std::vector<TextItem>& out,
                         const std::unordered_map<std::string, const CMap*>* fonts = nullptr,
                         int page = 0) {
    size_t first_new = out.size();
    const CMap* font = nullptr;
    auto decode = [&](const std::string& raw) -> std::string {
        if (!font) return raw;
        std::string o;
        for (unsigned char b : raw) {
            auto it = font->to.find(b);
            if (it != font->to.end()) o += it->second;
        }
        return o.empty() ? raw : o;
    };
    double tx = 0, ty = 0;
    // Current transformation matrix. Rotated dimension text is emitted as
    // q / translate+rotate cm / text / Q, so the text matrix alone gives coordinates
    // in a rotated local space - which is why rotated callouts first came out at
    // negative positions. Tracking cm and the q/Q stack puts them back on the sheet.
    struct Mat { double a = 1, b = 0, cc = 0, d = 1, e = 0, f = 0; };
    Mat ctm; std::vector<Mat> gs;
    auto apply = [](const Mat& m, double x, double y, double& ox, double& oy) {
        ox = m.a * x + m.cc * y + m.e;
        oy = m.b * x + m.d * y + m.f;
    };
    std::vector<std::string> stack;     // recent numeric operands
    size_t i = 0;
    auto num = [&](int back) -> double {
        int idx = (int)stack.size() - back;
        return (idx >= 0 && idx < (int)stack.size()) ? atof(stack[idx].c_str()) : 0.0;
    };
    std::string pending; double px = 0, py = 0; bool havePending = false;

    while (i < c.size()) {
        char ch = c[i];
        if (ch == '(') {                                   // literal string
            size_t j = i + 1; int depth = 1; std::string lit;
            while (j < c.size() && depth) {
                if (c[j] == '\\' && j + 1 < c.size()) { lit.push_back(c[j]); lit.push_back(c[j+1]); j += 2; continue; }
                if (c[j] == '(') depth++;
                else if (c[j] == ')') { if (--depth == 0) break; }
                lit.push_back(c[j]); j++;
            }
            std::string txt = decode(pdf_unescape(lit));
            if (!txt.empty()) { pending += txt; apply(ctm, tx, ty, px, py); havePending = true; }
            i = j + 1; continue;
        }
        if (ch == '<' && i + 1 < c.size() && c[i+1] != '<') {   // hex string
            size_t j = i + 1; std::string hex;
            while (j < c.size() && c[j] != '>') { if (isxdigit((unsigned char)c[j])) hex.push_back(c[j]); j++; }
            std::string txt;
            for (size_t k = 0; k + 1 < hex.size(); k += 2)
                txt.push_back((char)strtol(hex.substr(k, 2).c_str(), nullptr, 16));
            txt = decode(txt);
            if (!txt.empty()) { pending += txt; apply(ctm, tx, ty, px, py); havePending = true; }
            i = j + 1; continue;
        }
        if (isspace((unsigned char)ch)) { i++; continue; }

        size_t j = i;
        while (j < c.size() && !isspace((unsigned char)c[j]) && c[j] != '(' && c[j] != '<' && c[j] != '[' && c[j] != ']') j++;
        if (j == i) { i++; continue; }
        std::string tok = c.substr(i, j - i);
        i = j;

        if ((tok[0] >= '0' && tok[0] <= '9') || tok[0] == '-' || tok[0] == '.' || tok[0] == '+') {
            stack.push_back(tok); if (stack.size() > 8) stack.erase(stack.begin());
            continue;
        }
        if (tok == "Tf" && fonts) {
            // the operand before the size is the font resource name, e.g. /R25 9 Tf
            size_t fp = c.rfind('/', i - tok.size());
            if (fp != std::string::npos) {
                size_t fe = fp + 1;
                while (fe < c.size() && (isalnum((unsigned char)c[fe]) || c[fe] == '.' || c[fe] == '_' || c[fe] == '+')) fe++;
                std::string fname = c.substr(fp + 1, fe - fp - 1);
                font = nullptr;
                auto fit = fonts->find(fname);
                if (fit != fonts->end()) font = fit->second;
            }
            stack.clear();
        }
        else if (tok == "q") { gs.push_back(ctm); stack.clear(); }
        else if (tok == "Q") { if (!gs.empty()) { ctm = gs.back(); gs.pop_back(); } stack.clear(); }
        else if (tok == "cm") {
            Mat m; m.a = num(6); m.b = num(5); m.cc = num(4); m.d = num(3); m.e = num(2); m.f = num(1);
            Mat r;   // r = m * ctm
            r.a = m.a * ctm.a + m.b * ctm.cc;
            r.b = m.a * ctm.b + m.b * ctm.d;
            r.cc = m.cc * ctm.a + m.d * ctm.cc;
            r.d = m.cc * ctm.b + m.d * ctm.d;
            r.e = m.e * ctm.a + m.f * ctm.cc + ctm.e;
            r.f = m.e * ctm.b + m.f * ctm.d + ctm.f;
            ctm = r; stack.clear();
        }
        else if (tok == "Td" || tok == "TD") { tx += num(2); ty += num(1); stack.clear(); }
        else if (tok == "Tm") { tx = num(2); ty = num(1); stack.clear(); }
        else if (tok == "TJ" || tok == "Tj" || tok == "'" || tok == "\"") {
            if (havePending) { TextItem t; t.text = pending; t.x = px; t.y = py; out.push_back(t); }
            pending.clear(); havePending = false; stack.clear();
        }
        else if (tok == "ET" || tok == "BT") {
            if (havePending) { TextItem t; t.text = pending; t.x = px; t.y = py; out.push_back(t); }
            pending.clear(); havePending = false;
            if (tok == "BT") { tx = ty = 0; }
            stack.clear();
        }
        else stack.clear();
    }
    if (havePending) { TextItem t; t.text = pending; t.x = px; t.y = py; out.push_back(t); }
    for (size_t k = first_new; k < out.size(); k++) out[k].page = page;
}

// Brute-force: find every stream, inflate what inflates, scan for text.
inline std::vector<TextItem> extract_text(const std::vector<uint8_t>& buf) {
    std::vector<TextItem> items;
    std::vector<CMap> cmaps;
    std::vector<std::string> decoded;   // decode once, use twice
    std::vector<int> objnums;           // which PDF object each stream belongs to
    const char* K = "stream";
    for (size_t i = 0; i + 6 < buf.size(); i++) {
        if (memcmp(&buf[i], K, 6) != 0) continue;
        size_t s = i + 6;
        if (s < buf.size() && buf[s] == '\r') s++;
        if (s < buf.size() && buf[s] == '\n') s++;
        size_t e = s;
        const char* E = "endstream";
        bool found = false;
        for (; e + 9 <= buf.size(); e++) { if (memcmp(&buf[e], E, 9) == 0) { found = true; break; } }
        if (!found) break;
        size_t len = e - s;
        if (len > 8) {
            // walk back to the "N 0 obj" this stream lives in, so a CMap can be tied
            // to its object number and resolved from a /ToUnicode reference
            int objn = -1;
            size_t back = i > 400 ? i - 400 : 0;
            for (size_t k = i; k > back; k--) {
                if (k + 6 < buf.size() && memcmp(&buf[k], " 0 obj", 6) == 0) {
                    size_t ns = k;
                    while (ns > 0 && isdigit(buf[ns - 1])) ns--;
                    objn = atoi((const char*)&buf[ns]);
                    break;
                }
            }
            std::string dec;
            if (decode_stream(&buf[s], len, dec)) { decoded.push_back(std::move(dec)); objnums.push_back(objn); }
            else {
                std::string raw((const char*)&buf[s], len);
                if (raw.find("Tj") != std::string::npos || raw.find("TJ") != std::string::npos)
                    { decoded.push_back(std::move(raw)); objnums.push_back(objn); }
            }
        }
        i = e + 8;
    }
    // pass 1: harvest every ToUnicode CMap, tagged with its object number
    std::vector<bool> is_cmap(decoded.size(), false);
    for (size_t k = 0; k < decoded.size(); k++) {
        size_t before = cmaps.size();
        parse_cmaps(decoded[k], cmaps, objnums[k]);
        // A CMap is a font resource, not page content. Scanning one for text harvests
        // its own codespace literals - <00> <ff> and the bfchar pairs - as though they
        // had been drawn on the sheet, and they arrive unattributed to any page.
        is_cmap[k] = cmaps.size() > before ||
                     decoded[k].find("begincmap") != std::string::npos;
    }
    // pass 2: resolve resource name -> font object -> CMap
    auto fonts = resolve_fonts(buf, cmaps);
    auto pagemap = map_content_to_page(buf);
    // pass 3: read the text through the right CMap per font, stamped with its sheet
    for (size_t k = 0; k < decoded.size(); k++) {
        if (is_cmap[k]) continue;
        auto it = pagemap.find(objnums[k]);
        scan_content(decoded[k], items, &fonts, it == pagemap.end() ? 0 : it->second);
    }
    return items;
}

// ---------------------------------------------------------------- layout
// extract_text() returns fragments in content-stream order, which is the order the
// producing application chose to paint them - not reading order. A PDF has no concept
// of a line; "a line" is an inference from coordinates, which is why this is a separate
// step you can skip when you want the raw geometry.
//
// reflow() groups fragments whose baselines agree within `tol` points into one line,
// orders the lines down the page, and orders each line left to right.
struct Line { int page = 0; double y = 0; std::string text; };

inline std::vector<Line> reflow(const std::vector<TextItem>& items, double tol = 2.0) {
    std::vector<int> pages;
    for (const auto& it : items) {
        bool seen = false;
        for (int p : pages) if (p == it.page) { seen = true; break; }
        if (!seen) pages.push_back(it.page);
    }
    std::sort(pages.begin(), pages.end());

    std::vector<Line> out;
    for (int page : pages) {
        std::vector<const TextItem*> on;
        for (const auto& it : items)
            if (it.page == page && !it.text.empty()) on.push_back(&it);
        // down the page first (PDF y grows upward), then left to right
        std::stable_sort(on.begin(), on.end(), [](const TextItem* a, const TextItem* b) {
            if (a->y != b->y) return a->y > b->y;
            return a->x < b->x;
        });
        size_t i = 0;
        while (i < on.size()) {
            double base = on[i]->y;
            std::vector<const TextItem*> row;
            while (i < on.size() && base - on[i]->y <= tol) row.push_back(on[i++]);
            std::stable_sort(row.begin(), row.end(),
                             [](const TextItem* a, const TextItem* b) { return a->x < b->x; });
            Line ln; ln.page = page; ln.y = base;
            for (size_t k = 0; k < row.size(); k++) {
                if (k) ln.text += ' ';
                ln.text += row[k]->text;
            }
            out.push_back(std::move(ln));
        }
    }
    return out;
}

// Highest page number any fragment was attributed to. Zero means nothing was
// attributed - see "page 0" in the README.
inline int page_count(const std::vector<TextItem>& items) {
    int n = 0;
    for (const auto& it : items) if (it.page > n) n = it.page;
    return n;
}

}  // namespace pdftext
