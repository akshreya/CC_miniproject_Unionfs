CC      = gcc
CFLAGS  = -Wall -Wextra -O2 $(shell pkg-config --cflags fuse3)
LDFLAGS = $(shell pkg-config --libs fuse3)
TARGET  = mini_unionfs
SRC     = mini_unionfs.c

.PHONY: all clean test

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test: all
	chmod +x test_unionfs.sh
	./test_unionfs.sh

clean:
	rm -f $(TARGET)
	rm -rf unionfs_test_env
