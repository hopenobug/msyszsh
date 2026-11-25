CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS  := -lshell32 -ladvapi32

TARGET  := msyszsh.exe
SRC     := msyszsh.c

all: $(TARGET) fix-integrity

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# Strip the inherited low integrity label, otherwise the MSYS2 runtime fails to
# initialise (see README). A freshly built file inherits the label again, hence
# this runs on every `make`.
# The first command is for MSYS (//switch), the second for cmd.exe (/switch);
# whichever applies wins.
fix-integrity: $(TARGET)
	@echo "icacls $(TARGET) /setintegritylevel Medium"
	@icacls "$(TARGET)" //setintegritylevel Medium >/dev/null 2>&1 \
		|| icacls "$(TARGET)" /setintegritylevel Medium >/dev/null 2>&1 \
		|| echo "warning: could not set integrity level, see README"

# requires MSYS2_ROOT to be set
run:
	./$(TARGET)

clean:
	rm -f $(TARGET) *.o

.PHONY: all run fix-integrity clean
