MARGO ?= ../../build/margo

all: matrix_astar

matrix_astar: matrix_astar.margo $(MARGO)
	$(MARGO) build $< -o $@

clean:
	rm -f matrix_astar

.PHONY: all clean
