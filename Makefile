# pdftext - there is nothing to configure and nothing to fetch.
#
#   make            build the pdftext CLI
#   make test       build and run the test suite
#   make corpus     regenerate tests/corpus from tools/make_corpus.py
#   make demo       run the CLI over every corpus fixture
#   make clean

CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
PYTHON   ?= python3
PREFIX   ?= /usr/local

all: pdftext

pdftext: cli/main.cpp pdftext.h
	$(CXX) $(CXXFLAGS) cli/main.cpp -o $@

test_pdftext: tests/test_pdftext.cpp pdftext.h
	$(CXX) $(CXXFLAGS) tests/test_pdftext.cpp -o $@

test: test_pdftext
	./test_pdftext tests/corpus

corpus:
	$(PYTHON) tools/make_corpus.py

# The corpus is committed, and the generator is deterministic, so regenerating it must
# not change a byte. If this fails either the generator or the committed fixtures moved
# without the other -- which would quietly turn the expectations into a rubber stamp.
check-corpus:
	$(PYTHON) tools/make_corpus.py
	git diff --exit-code -- tests/corpus

demo: pdftext
	@for f in tests/corpus/*.pdf; do \
	  echo "=== $$f"; ./pdftext "$$f" || true; \
	done

install: pdftext
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 pdftext $(DESTDIR)$(PREFIX)/bin/pdftext
	install -d $(DESTDIR)$(PREFIX)/include
	install -m 644 pdftext.h $(DESTDIR)$(PREFIX)/include/pdftext.h

clean:
	rm -f pdftext test_pdftext pdftext.exe test_pdftext.exe *.obj

.PHONY: all test corpus check-corpus demo install clean
