MARGO ?= ../../build/margo

all: number_baseball

number_baseball: number_baseball.margo $(MARGO)
	$(MARGO) build $< -o $@

clean:
	rm -f number_baseball

.PHONY: all clean
