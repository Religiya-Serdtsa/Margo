MARGO ?= ../../build/margo

all: slice-owned

slice-owned: slice_owned.margo $(MARGO)
	$(MARGO) build $< -o $@

clean:
	rm -f slice-owned

.PHONY: all clean
