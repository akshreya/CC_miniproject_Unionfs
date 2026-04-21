#include "unionfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <libgen.h>

void build_path(char *out, const char *dir, const char *virtual_path) {
    snprintf(out, PATH_MAX, "%s%s", dir, virtual_path);
}

int is_whiteout(const char *virtual_path) {
    char vpath1[PATH_MAX], vpath2[PATH_MAX];
    strncpy(vpath1, virtual_path, PATH_MAX - 1);
    vpath1[PATH_MAX - 1] = '\0';
    strncpy(vpath2, virtual_path, PATH_MAX - 1);
    vpath2[PATH_MAX - 1] = '\0';

    char *dir  = dirname(vpath1);
    char *base = basename(vpath2);

    char wh_path[PATH_MAX];
    if (strcmp(dir, "/") == 0 || strcmp(dir, ".") == 0) {
        snprintf(wh_path, PATH_MAX, "%s/.wh.%s", UNIONFS_DATA->upper_dir, base);
    } else {
        snprintf(wh_path, PATH_MAX, "%s%s/.wh.%s", UNIONFS_DATA->upper_dir, dir, base);
    }

    struct stat st;
    return (lstat(wh_path, &st) == 0) ? 1 : 0;
}

int resolve_path(const char *virtual_path, char *real_path_out) {
    if (is_whiteout(virtual_path))
        return -ENOENT;

    struct stat st;

    char upper_path[PATH_MAX];
    build_path(upper_path, UNIONFS_DATA->upper_dir, virtual_path);
    if (lstat(upper_path, &st) == 0) {
        strncpy(real_path_out, upper_path, PATH_MAX);
        return 0;
    }

    char lower_path[PATH_MAX];
    build_path(lower_path, UNIONFS_DATA->lower_dir, virtual_path);
    if (lstat(lower_path, &st) == 0) {
        strncpy(real_path_out, lower_path, PATH_MAX);
        return 0;
    }

    return -ENOENT;
}

#ifdef __APPLE__
int unionfs_getattr(const char *path, struct stat *stbuf) {
#else
int unionfs_getattr(const char *path, struct stat *stbuf,
                    struct fuse_file_info *fi) {
    (void) fi;
#endif

    if (strcmp(path, "/") == 0) {
        if (lstat(UNIONFS_DATA->upper_dir, stbuf) == -1)
            return -errno;
        return 0;
    }

    if (is_whiteout(path))
        return -ENOENT;

    char real_path[PATH_MAX];
    int res = resolve_path(path, real_path);
    if (res != 0) return res;

    if (lstat(real_path, stbuf) == -1)
        return -errno;
    return 0;
}

#ifdef __APPLE__
int unionfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                    off_t offset, struct fuse_file_info *fi) {
    (void) offset;
    (void) fi;
#else
int unionfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                    off_t offset, struct fuse_file_info *fi,
                    enum fuse_readdir_flags flags) {
    (void) offset;
    (void) fi;
    (void) flags;
#endif

    FILL_DIR(filler, buf, ".",  NULL, 0);
    FILL_DIR(filler, buf, "..", NULL, 0);

    char upper_path[PATH_MAX];
    build_path(upper_path, UNIONFS_DATA->upper_dir, path);

    DIR *dp = opendir(upper_path);
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;
            if (strncmp(de->d_name, ".wh.", 4) == 0)
                continue;
            FILL_DIR(filler, buf, de->d_name, NULL, 0);
        }
        closedir(dp);
    }

    char lower_path[PATH_MAX];
    build_path(lower_path, UNIONFS_DATA->lower_dir, path);

    dp = opendir(lower_path);
    if (dp) {
        struct dirent *de;
        while ((de = readdir(dp)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
                continue;

            char vpath[PATH_MAX];
            if (strcmp(path, "/") == 0)
                snprintf(vpath, PATH_MAX, "/%s", de->d_name);
            else
                snprintf(vpath, PATH_MAX, "%s/%s", path, de->d_name);

            if (is_whiteout(vpath))
                continue;

            /* skip if already listed from upper */
            char upper_entry[PATH_MAX];
            snprintf(upper_entry, PATH_MAX, "%s/%s", upper_path, de->d_name);
            struct stat st;
            if (lstat(upper_entry, &st) == 0)
                continue;

            FILL_DIR(filler, buf, de->d_name, NULL, 0);
        }
        closedir(dp);
    }

    return 0;
}

/* Copy a file from src_path to dst_path preserving mode. */
static int copy_file(const char *src_path, const char *dst_path) {
    struct stat st;
    if (stat(src_path, &st) != 0) return -errno;

    int src = open(src_path, O_RDONLY);
    if (src < 0) return -errno;

    int dst = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode);
    if (dst < 0) { close(src); return -errno; }

    char buf[65536];
    ssize_t n;
    while ((n = read(src, buf, sizeof(buf))) > 0) {
        if (write(dst, buf, n) != n) {
            close(src); close(dst);
            return -EIO;
        }
    }
    close(src);
    close(dst);
    return (n < 0) ? -errno : 0;
}

int unionfs_open(const char *path, struct fuse_file_info *fi) {
    if (is_whiteout(path))
        return -ENOENT;

    char real_path[PATH_MAX];
    int res = resolve_path(path, real_path);
    if (res != 0) return res;

    /* For any write-mode open, ensure the file is in upper (CoW). */
    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
        char upper_path[PATH_MAX];
        build_path(upper_path, UNIONFS_DATA->upper_dir, path);

        struct stat st;
        if (lstat(upper_path, &st) != 0) {
            /* File only in lower – copy it up. */
            char lower_path[PATH_MAX];
            build_path(lower_path, UNIONFS_DATA->lower_dir, path);

            res = copy_file(lower_path, upper_path);
            if (res != 0) return res;
        }
    }

    return 0;
}

int unionfs_read(const char *path, char *buf, size_t size, off_t offset,
                 struct fuse_file_info *fi) {
    (void) fi;

    char real_path[PATH_MAX];
    int res = resolve_path(path, real_path);
    if (res != 0) return res;

    int fd = open(real_path, O_RDONLY);
    if (fd < 0) return -errno;

    ssize_t n = pread(fd, buf, size, offset);
    close(fd);

    return (n < 0) ? -errno : (int)n;
}

int unionfs_write(const char *path, const char *buf, size_t size,
                  off_t offset, struct fuse_file_info *fi) {
    (void) fi;

    char upper_path[PATH_MAX];
    build_path(upper_path, UNIONFS_DATA->upper_dir, path);

    int fd = open(upper_path, O_WRONLY);
    if (fd < 0) return -errno;

    ssize_t n = pwrite(fd, buf, size, offset);
    close(fd);

    return (n < 0) ? -errno : (int)n;
}

int unionfs_create(const char *path, mode_t mode,
                   struct fuse_file_info *fi) {
    (void) fi;

    char upper_path[PATH_MAX];
    build_path(upper_path, UNIONFS_DATA->upper_dir, path);

    int fd = open(upper_path, O_CREAT | O_WRONLY | O_TRUNC, mode);
    if (fd < 0) return -errno;

    close(fd);
    return 0;
}

int unionfs_unlink(const char *path) {
    char upper_path[PATH_MAX];
    build_path(upper_path, UNIONFS_DATA->upper_dir, path);

    struct stat st;

    /* If the file is in upper, remove it. */
    if (lstat(upper_path, &st) == 0) {
        if (unlink(upper_path) != 0) return -errno;
    }

    /* If the file exists in lower, create a whiteout marker. */
    char lower_path[PATH_MAX];
    build_path(lower_path, UNIONFS_DATA->lower_dir, path);

    if (lstat(lower_path, &st) == 0) {
        char vpath1[PATH_MAX], vpath2[PATH_MAX];
        strncpy(vpath1, path, PATH_MAX - 1); vpath1[PATH_MAX - 1] = '\0';
        strncpy(vpath2, path, PATH_MAX - 1); vpath2[PATH_MAX - 1] = '\0';

        char *dir  = dirname(vpath1);
        char *base = basename(vpath2);

        char wh_path[PATH_MAX];
        if (strcmp(dir, "/") == 0 || strcmp(dir, ".") == 0)
            snprintf(wh_path, PATH_MAX, "%s/.wh.%s", UNIONFS_DATA->upper_dir, base);
        else
            snprintf(wh_path, PATH_MAX, "%s%s/.wh.%s", UNIONFS_DATA->upper_dir, dir, base);

        int fd = open(wh_path, O_CREAT | O_WRONLY, 0644);
        if (fd < 0) return -errno;
        close(fd);
    }

    return 0;
}

int unionfs_mkdir(const char *path, mode_t mode) {
    char upper_path[PATH_MAX];
    build_path(upper_path, UNIONFS_DATA->upper_dir, path);

    if (mkdir(upper_path, mode) != 0) return -errno;
    return 0;
}

int unionfs_rmdir(const char *path) {
    char upper_path[PATH_MAX];
    build_path(upper_path, UNIONFS_DATA->upper_dir, path);

    if (rmdir(upper_path) != 0) return -errno;
    return 0;
}
