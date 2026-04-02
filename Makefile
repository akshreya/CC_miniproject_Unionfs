all:
	gcc mini_unionfs.c -o mini_unionfs `pkg-config fuse3 --cflags --libs`
