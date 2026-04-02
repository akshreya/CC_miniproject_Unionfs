# Mini UnionFS using FUSE

## Overview

This project implements a simplified Union File System using FUSE (Filesystem in Userspace).

It merges two directories:

* **Lower directory** (read-only base layer)
* **Upper directory** (writable layer)

The mounted directory provides a unified view of both.

---

## Features

### 1. Merged View

Files from both lower and upper directories appear in a single mount point.

### 2. Upper Layer Priority

If a file exists in both layers, the upper layer version is used.

### 3. Copy-on-Write (CoW)

When modifying a file that exists only in the lower layer:

* It is first copied to the upper layer
* Then modifications are applied

### 4. Whiteout Mechanism

When deleting a file from the lower layer:

* A special file `.wh.<filename>` is created in the upper layer
* This hides the file from the merged view

---

## Project Structure

```
unionfs_project/
│── mini_unionfs.c
│── Makefile
│── test_env/
│   ├── lower/
│   ├── upper/
│   └── mnt/
```

---

## Build Instructions

```bash
make
```

---

## Run Instructions

```bash
./mini_unionfs test_env/lower test_env/upper test_env/mnt
```

---

## Testing

Open another terminal:

```bash
cd test_env/mnt
```

### Read:

```bash
cat file.txt
```

### Write (CoW):

```bash
echo "new" >> file.txt
```

### Delete (Whiteout):

```bash
rm file.txt
ls ../upper -a
```

Expected:

```
.wh.file.txt
```

---

## Technologies Used

* C
* FUSE (Filesystem in Userspace)
* Linux (WSL)


