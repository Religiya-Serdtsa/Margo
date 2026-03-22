MARGO ?= ../../build/margo
TARGET ?= example

all: $(TARGET)

$(TARGET): example.margo
	$(MARGO) build $< -o $@

clean:
	rm -f $(TARGET)

.PHONY: all clean
