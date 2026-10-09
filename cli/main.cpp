// pdftext - command-line PDF text extraction.
//
// A thin program over pdftext.h: read a file, print its text. The library does no I/O
// of its own, so all of the file handling, argument parsing and output formatting lives
// here and none of it is forced on a caller who only wants the header.
//
// Build:  cc -std=c++17 -O2 cli/main.cpp -o pdftext

#include "../pdftext.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

static const char* USAGE =
    "pdftext - extract text from a PDF. No dependencies, no network, no temp files.\n"
    "\n"
    "usage:  pdftext [options] <file.pdf> [more.pdf ...]\n"
    "        pdftext --help\n"
    "\n"
    "output modes (default: reflowed lines)\n"
    "  -l, --lines        one reflowed line per output line (default)\n"
    "  -r, --raw          one fragment per line, in content-stream paint order\n"
    "  -p, --positions    fragments with page and x/y, as a table\n"
    "  -j, --json         JSON array of {page, x, y, text} - for piping into jq\n"
    "  -c, --csv          CSV with a header row: page,x,y,text\n"
    "\n"
    "filters\n"
    "      --page N       only sheet N (1-based; 0 selects unattributed fragments)\n"
    "      --tol N        reflow baseline tolerance in points (default 2.0)\n"
    "\n"
    "other\n"
    "  -s, --summary      page count and fragment count only; no text\n"
    "  -q, --quiet        suppress the per-file header when reading several files\n"
    "      --version      print version and exit\n"
    "\n"
    "exit status:  0 text extracted | 1 bad usage or unreadable file\n"
    "              2 the file parsed but contains no extractable text\n"
    "                (most often a scanned page: an image, with no text layer -\n"
    "                 that needs OCR, which this tool deliberately does not do)\n";

static const char* VERSION = "pdftext 1.0.0";

enum class Mode { Lines, Raw, Positions, Json, Csv, Summary };

static bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// Minimal JSON string escaping. The text comes out of a PDF as UTF-8 already, so only
// the structural characters and the C0 controls need handling.
static std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

// RFC 4180: quote always, double any embedded quote. Spreadsheet-safe without guessing
// at the consumer's locale.
static std::string csv_field(const std::string& s) {
    std::string o = "\"";
    for (char c : s) {
        if (c == '"') o += "\"\"";
        else if (c == '\n' || c == '\r') o += ' ';
        else o += c;
    }
    return o + "\"";
}

int main(int argc, char** argv) {
    Mode mode = Mode::Lines;
    int only_page = -1;
    double tol = 2.0;
    bool quiet = false;
    std::vector<std::string> files;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                fprintf(stderr, "pdftext: %s needs a value\n", what);
                exit(1);
            }
            return argv[++i];
        };
        if (a == "-h" || a == "--help")            { fputs(USAGE, stdout); return 0; }
        else if (a == "--version")                 { puts(VERSION); return 0; }
        else if (a == "-l" || a == "--lines")      mode = Mode::Lines;
        else if (a == "-r" || a == "--raw")        mode = Mode::Raw;
        else if (a == "-p" || a == "--positions")  mode = Mode::Positions;
        else if (a == "-j" || a == "--json")       mode = Mode::Json;
        else if (a == "-c" || a == "--csv")        mode = Mode::Csv;
        else if (a == "-s" || a == "--summary")    mode = Mode::Summary;
        else if (a == "-q" || a == "--quiet")      quiet = true;
        else if (a == "--page")                    only_page = atoi(next("--page"));
        else if (a == "--tol")                     tol = atof(next("--tol"));
        else if (!a.empty() && a[0] == '-' && a != "-") {
            fprintf(stderr, "pdftext: unknown option %s (try --help)\n", a.c_str());
            return 1;
        }
        else files.push_back(a);
    }

    if (files.empty()) {
        fputs(USAGE, stderr);
        return 1;
    }

    bool label = files.size() > 1 && !quiet;
    bool any_text = false;
    int status = 0;

    for (size_t fi = 0; fi < files.size(); fi++) {
        const std::string& path = files[fi];
        std::vector<uint8_t> buf;
        if (!read_file(path, buf)) {
            fprintf(stderr, "pdftext: cannot read %s\n", path.c_str());
            status = 1;
            continue;
        }

        auto items = pdftext::extract_text(buf);
        if (only_page >= 0) {
            std::vector<pdftext::TextItem> keep;
            for (const auto& it : items) if (it.page == only_page) keep.push_back(it);
            items.swap(keep);
        }

        size_t nonempty = 0;
        for (const auto& it : items) if (!it.text.empty()) nonempty++;
        if (nonempty) any_text = true;

        if (label) printf("%s==> %s <==\n", fi ? "\n" : "", path.c_str());

        switch (mode) {
            case Mode::Summary: {
                printf("%-40s %4d page(s)  %5zu fragment(s)  %6zu bytes\n",
                       path.c_str(), pdftext::page_count(items), nonempty, buf.size());
                break;
            }
            case Mode::Lines: {
                for (const auto& ln : pdftext::reflow(items, tol))
                    printf("%s\n", ln.text.c_str());
                break;
            }
            case Mode::Raw: {
                for (const auto& it : items)
                    if (!it.text.empty()) printf("%s\n", it.text.c_str());
                break;
            }
            case Mode::Positions: {
                printf("%-5s %10s %10s  %s\n", "page", "x", "y", "text");
                for (const auto& it : items)
                    if (!it.text.empty())
                        printf("%-5d %10.2f %10.2f  %s\n", it.page, it.x, it.y,
                               it.text.c_str());
                break;
            }
            case Mode::Json: {
                printf("[\n");
                bool first = true;
                for (const auto& it : items) {
                    if (it.text.empty()) continue;
                    printf("%s  {\"page\":%d,\"x\":%.2f,\"y\":%.2f,\"text\":\"%s\"}",
                           first ? "" : ",\n", it.page, it.x, it.y,
                           json_escape(it.text).c_str());
                    first = false;
                }
                printf("%s]\n", first ? "" : "\n");
                break;
            }
            case Mode::Csv: {
                printf("page,x,y,text\n");
                for (const auto& it : items) {
                    if (it.text.empty()) continue;
                    printf("%d,%.2f,%.2f,%s\n", it.page, it.x, it.y,
                           csv_field(it.text).c_str());
                }
                break;
            }
        }
    }

    // "parsed fine, found nothing" is a different outcome from "could not read it", and
    // a caller scripting this needs to tell them apart -- an empty result on a scanned
    // page is the single most common surprise with any text extractor.
    if (status == 0 && !any_text) return 2;
    return status;
}
