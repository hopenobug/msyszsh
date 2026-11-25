CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
LDLIBS  := -lshell32 -ladvapi32

TARGET  := msyszsh.exe
SRC     := msyszsh.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

# 去掉继承来的低完整性标签，否则 MSYS2 运行时初始化会失败（见 README）。
# 前半句给 MSYS(//switch)，后半句给 cmd.exe(/switch)，任一生效即可。
fix-integrity: $(TARGET)
	@icacls "$(TARGET)" //setintegritylevel Medium >/dev/null 2>&1 \
		|| icacls "$(TARGET)" /setintegritylevel Medium >/dev/null 2>&1 \
		|| echo "warning: could not set integrity level, see README"

# 需要 MSYS2_ROOT 已设置
run: fix-integrity
	./$(TARGET)

clean:
	rm -f $(TARGET) *.o

.PHONY: all run fix-integrity clean
