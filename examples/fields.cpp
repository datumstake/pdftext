// fields.cpp - pull labelled values out of a document by geometry.
//
// This is the job almost every real extraction task turns out to be: the PDF contains
// "Invoice Number" and it contains "INV-2026-0431", and absolutely nothing in the file
// says those two belong together. The relationship is spatial -- the value sits to the
// right of its label, on the same baseline -- so finding it means working in coordinates,
// which is why extract_text() hands back x and y rather than a blob of text.
//
// Two properties worth copying into your own extractor, because they are what separates
// a demo from something you would put in front of an accounts-payable team:
//
//   1. A field that is not found reports as NOT FOUND. It does not report an empty
//      string, and it does not silently pick the nearest thing on the page. A missing
//      value that looks like a found value is the one failure mode that costs money.
//   2. Every hit carries the coordinates it came from, so a human can be pointed at the
//      spot on the page. An extracted value you cannot trace back is not auditable.
//
// Build and run:
//   c++ -std=c++17 -O2 examples/fields.cpp -o fields
//   ./fields tests/corpus/form.pdf "Invoice Number" "Total Due" "Nonexistent Field"

#include "../pdftext.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

struct Hit {
    bool found = false;
    std::string value;
    double x = 0, y = 0;
    int page = 0;
    double gap = 0;          // how far right of the label the value sat
};

// The value for `label` is the nearest fragment to its right on the same baseline.
// `tol` is how much baseline disagreement to forgive (points); `max_gap` stops a lookup
// from reaching across an entire page and grabbing something unrelated.
static Hit field_after(const std::vector<pdftext::TextItem>& items,
                       const std::string& label,
                       double tol = 2.5, double max_gap = 400.0) {
    Hit hit;
    const pdftext::TextItem* anchor = nullptr;
    for (const auto& it : items)
        if (it.text.find(label) != std::string::npos) { anchor = &it; break; }
    if (!anchor) return hit;                     // no label, no guess

    const pdftext::TextItem* best = nullptr;
    double best_dx = max_gap;
    for (const auto& it : items) {
        if (&it == anchor || it.text.empty()) continue;
        if (it.page != anchor->page) continue;
        if (std::fabs(it.y - anchor->y) > tol) continue;
        double dx = it.x - anchor->x;
        if (dx <= 0 || dx >= best_dx) continue;  // to the right, and nearest wins
        best = &it;
        best_dx = dx;
    }
    if (!best) return hit;                       // label present, value absent -- say so

    hit.found = true;
    hit.value = best->text;
    hit.x = best->x;
    hit.y = best->y;
    hit.page = best->page;
    hit.gap = best_dx;
    return hit;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr,
                "usage: fields <file.pdf> \"Label One\" [\"Label Two\" ...]\n"
                "\n"
                "  Prints the value found to the right of each label, with the page and\n"
                "  coordinates it came from. Labels that are not found say so.\n");
        return 1;
    }

    std::ifstream f(argv[1], std::ios::binary);
    if (!f) { fprintf(stderr, "fields: cannot read %s\n", argv[1]); return 1; }
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(f)),
                             std::istreambuf_iterator<char>());

    auto items = pdftext::extract_text(buf);
    printf("%s  -  %d page(s), %zu fragment(s)\n\n", argv[1],
           pdftext::page_count(items), items.size());

    printf("%-22s %-26s %s\n", "field", "value", "traced to");
    printf("%-22s %-26s %s\n", "----------------------",
           "--------------------------", "---------------------");

    int missing = 0;
    for (int i = 2; i < argc; i++) {
        Hit h = field_after(items, argv[i]);
        if (h.found) {
            char where[64];
            snprintf(where, sizeof where, "p%d at %.0f,%.0f (+%.0fpt)",
                     h.page, h.x, h.y, h.gap);
            printf("%-22s %-26s %s\n", argv[i], h.value.c_str(), where);
        } else {
            printf("%-22s %-26s %s\n", argv[i], "<NOT FOUND>", "-");
            missing++;
        }
    }

    if (missing) {
        printf("\n%d field(s) not found. Reported rather than filled in with a guess:\n"
               "a wrong value that looks right is worse than a gap a human can see.\n",
               missing);
    }
    // non-zero when anything is missing, so this is usable in a pipeline that must not
    // continue on a partial extraction
    return missing ? 3 : 0;
}
