MARGO ?= ../../build/margo

all: bench

bench: bench.margo $(MARGO)
	$(MARGO) build $< -o $@

clean:
	rm -f bench

.PHONY: all clean
