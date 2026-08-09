MARGO ?= ../../build/margo

all: margo-grep

margo-grep: margo_grep.margo $(MARGO)
	$(MARGO) build $< -o $@

clean:
	rm -f margo-grep

.PHONY: all clean
