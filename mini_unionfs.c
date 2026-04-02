#define FUSE_USE_VERSION 31

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>

struct mini_unionfs_state {
    char *lower_dir;
    char *upper_dir;
};

#define UNIONFS_DATA ((struct mini_unionfs_state *) fuse_get_context()->private_data)

/* Build full path: dir + path */
static void build_full_path(char *buf, size_t size, const char *dir, const char *path) {
    snprintf(buf, size, "%s%s", dir, path);
}

/* Build whiteout path in upper layer.
 * Example:
 *   path = /config.txt
 *   whiteout = upper_dir/.wh.config.txt
 */
static int build_whiteout_path(const char *path, char *whiteout_path, size_t size) {
    struct mini_unionfs_state *state = UNIONFS_DATA;
    const char *filename = strrchr(path, '/');

    if (!filename) {
        return -EINVAL;
    }

    filename++; /* skip '/' */

    if (strcmp(path, "/") == 0 || *filename == '\0') {
        return -EINVAL;
    }

    int written = snprintf(whiteout_path, size, "%s/.wh.%s", state->upper_dir, filename);
    if (written < 0 || (size_t)written >= size) {
        return -ENAMETOOLONG;
    }

    return 0;
}

/* Helper function to resolve paths:
 * 1. Check if upper_dir + "/.wh.filename" exists -> return ENOENT
 * 2. Check if upper_dir + "/filename" exists -> return this path
 * 3. Check if lower_dir + "/filename" exists -> return this path
 * 4. Otherwise, return ENOENT
 */
static int resolve_path(const char *path, char *resolved_path, size_t size) {
    struct mini_unionfs_state *state = UNIONFS_DATA;
    char upper_path[PATH_MAX];
    char lower_path[PATH_MAX];
    char whiteout_path[PATH_MAX];

    if (strcmp(path, "/") == 0) {
        int written = snprintf(resolved_path, size, "/");
        if (written < 0 || (size_t)written >= size) {
            return -ENAMETOOLONG;
        }
        return 0;
    }

    build_full_path(upper_path, sizeof(upper_path), state->upper_dir, path);
    build_full_path(lower_path, sizeof(lower_path), state->lower_dir, path);

    if (build_whiteout_path(path, whiteout_path, sizeof(whiteout_path)) == 0) {
        if (access(whiteout_path, F_OK) == 0) {
            return -ENOENT;
        }
    }

    if (access(upper_path, F_OK) == 0) {
        int written = snprintf(resolved_path, size, "%s", upper_path);
        if (written < 0 || (size_t)written >= size) {
            return -ENAMETOOLONG;
        }
        return 0;
    }

    if (access(lower_path, F_OK) == 0) {
        int written = snprintf(resolved_path, size, "%s", lower_path);
        if (written < 0 || (size_t)written >= size) {
            return -ENAMETOOLONG;
        }
        return 0;
    }

    return -ENOENT;
}

/* Copy lower file to upper file for Copy-on-Write */
static int copy_file_to_upper(const char *path) {
    struct mini_unionfs_state *state = UNIONFS_DATA;
    char lower_path[PATH_MAX];
    char upper_path[PATH_MAX];
    int src_fd, dst_fd;
    char buffer[4096];
    ssize_t bytes_read;
    struct stat st;

    build_full_path(lower_path, sizeof(lower_path), state->lower_dir, path);
    build_full_path(upper_path, sizeof(upper_path), state->upper_dir, path);

    if (access(lower_path, F_OK) != 0) {
        return -ENOENT;
    }

    if (access(upper_path, F_OK) == 0) {
        return 0;
    }

    if (stat(lower_path, &st) == -1) {
        return -errno;
    }

    src_fd = open(lower_path, O_RDONLY);
    if (src_fd == -1) {
        return -errno;
    }

    dst_fd = open(upper_path, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode);
    if (dst_fd == -1) {
        close(src_fd);
        return -errno;
    }

    while ((bytes_read = read(src_fd, buffer, sizeof(buffer))) > 0) {
        if (write(dst_fd, buffer, bytes_read) != bytes_read) {
            close(src_fd);
            close(dst_fd);
            return -EIO;
        }
    }

    close(src_fd);
    close(dst_fd);

    if (bytes_read < 0) {
        return -errno;
    }

    return 0;
}

static int unionfs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    char resolved_path[PATH_MAX];
    (void) fi;

    memset(stbuf, 0, sizeof(struct stat));

    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }

    int res = resolve_path(path, resolved_path, sizeof(resolved_path));
    if (res != 0) {
        return res;
    }

    if (lstat(resolved_path, stbuf) == -1) {
        return -errno;
    }

    return 0;
}

static int unionfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                           off_t offset, struct fuse_file_info *fi,
                           enum fuse_readdir_flags flags) {
    struct mini_unionfs_state *state = UNIONFS_DATA;
    char upper_path[PATH_MAX];
    char lower_path[PATH_MAX];
    DIR *dp;
    struct dirent *de;
    char seen[1024][NAME_MAX + 1];
    int seen_count = 0;
    (void) offset;
    (void) fi;
    (void) flags;

    build_full_path(upper_path, sizeof(upper_path), state->upper_dir, path);
    build_full_path(lower_path, sizeof(lower_path), state->lower_dir, path);

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);

    dp = opendir(upper_path);
    if (dp != NULL) {
        while ((de = readdir(dp)) != NULL) {
            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
                continue;
            }

            if (strncmp(de->d_name, ".wh.", 4) == 0) {
                continue;
            }

            filler(buf, de->d_name, NULL, 0, 0);

            if (seen_count < 1024) {
                snprintf(seen[seen_count], NAME_MAX + 1, "%s", de->d_name);
                seen_count++;
            }
        }
        closedir(dp);
    }

    dp = opendir(lower_path);
    if (dp != NULL) {
        while ((de = readdir(dp)) != NULL) {
            int already_seen = 0;
            char test_path[PATH_MAX];
            char whiteout_path[PATH_MAX];

            if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
                continue;
            }

            for (int i = 0; i < seen_count; i++) {
                if (strcmp(seen[i], de->d_name) == 0) {
                    already_seen = 1;
                    break;
                }
            }

            if (already_seen) {
                continue;
            }

            if (strcmp(path, "/") == 0) {
                snprintf(test_path, sizeof(test_path), "/%s", de->d_name);
            } else {
                snprintf(test_path, sizeof(test_path), "%s/%s", path, de->d_name);
            }

            if (build_whiteout_path(test_path, whiteout_path, sizeof(whiteout_path)) == 0) {
                if (access(whiteout_path, F_OK) == 0) {
                    continue;
                }
            }

            filler(buf, de->d_name, NULL, 0, 0);
        }
        closedir(dp);
    }

    return 0;
}

static int unionfs_open(const char *path, struct fuse_file_info *fi) {
    struct mini_unionfs_state *state = UNIONFS_DATA;
    char resolved_path[PATH_MAX];
    char upper_path[PATH_MAX];

    build_full_path(upper_path, sizeof(upper_path), state->upper_dir, path);

    /* Is the user writing to a lower_dir file? Trigger Copy-on-Write (CoW)! */
    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
        if (access(upper_path, F_OK) != 0) {
            int cow_res = copy_file_to_upper(path);
            if (cow_res != 0 && cow_res != -ENOENT) {
                return cow_res;
            }
        }

        if (access(upper_path, F_OK) == 0) {
            snprintf(resolved_path, sizeof(resolved_path), "%s", upper_path);
        } else {
            int res = resolve_path(path, resolved_path, sizeof(resolved_path));
            if (res != 0) {
                return res;
            }
        }
    } else {
        int res = resolve_path(path, resolved_path, sizeof(resolved_path));
        if (res != 0) {
            return res;
        }
    }

    int fd = open(resolved_path, fi->flags);
    if (fd == -1) {
        return -errno;
    }

    fi->fh = fd;
    return 0;
}

static int unionfs_release(const char *path, struct fuse_file_info *fi) {
    (void) path;
    close((int)fi->fh);
    return 0;
}

static int unionfs_read(const char *path, char *buf, size_t size,
                        off_t offset, struct fuse_file_info *fi) {
    (void) path;
    ssize_t res = pread((int)fi->fh, buf, size, offset);
    if (res == -1) {
        return -errno;
    }
    return (int)res;
}

static int unionfs_write(const char *path, const char *buf, size_t size,
                         off_t offset, struct fuse_file_info *fi) {
    struct mini_unionfs_state *state = UNIONFS_DATA;
    char upper_path[PATH_MAX];
    int fd;
    ssize_t res;

    build_full_path(upper_path, sizeof(upper_path), state->upper_dir, path);

    if (access(upper_path, F_OK) != 0) {
        int cow_res = copy_file_to_upper(path);
        if (cow_res != 0 && cow_res != -ENOENT) {
            return cow_res;
        }
    }

    fd = open(upper_path, O_WRONLY);
    if (fd == -1) {
        return -errno;
    }

    res = pwrite(fd, buf, size, offset);
    close(fd);

    if (res == -1) {
        return -errno;
    }

    return (int)res;
}

static int unionfs_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
    struct mini_unionfs_state *state = UNIONFS_DATA;
    char upper_path[PATH_MAX];

    build_full_path(upper_path, sizeof(upper_path), state->upper_dir, path);

    int fd = open(upper_path, fi->flags | O_CREAT, mode);
    if (fd == -1) {
        return -errno;
    }

    fi->fh = fd;
    return 0;
}

static int unionfs_unlink(const char *path) {
    struct mini_unionfs_state *state = UNIONFS_DATA;
    char upper_path[PATH_MAX];
    char lower_path[PATH_MAX];
    char whiteout_path[PATH_MAX];

    build_full_path(upper_path, sizeof(upper_path), state->upper_dir, path);
    build_full_path(lower_path, sizeof(lower_path), state->lower_dir, path);

    /* If in upper_dir -> physical unlink.
     * If in lower_dir -> create upper_dir/.wh.<filename>
     */
    if (access(upper_path, F_OK) == 0) {
        if (unlink(upper_path) == -1) {
            return -errno;
        }
    }

    if (access(lower_path, F_OK) == 0) {
        int res = build_whiteout_path(path, whiteout_path, sizeof(whiteout_path));
        if (res != 0) {
            return res;
        }

        int fd = open(whiteout_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd == -1) {
            return -errno;
        }
        close(fd);
        return 0;
    }

    if (access(upper_path, F_OK) == 0) {
        return 0;
    }

    return -ENOENT;
}

static int unionfs_mkdir(const char *path, mode_t mode) {
    struct mini_unionfs_state *state = UNIONFS_DATA;
    char upper_path[PATH_MAX];

    build_full_path(upper_path, sizeof(upper_path), state->upper_dir, path);

    if (mkdir(upper_path, mode) == -1) {
        return -errno;
    }

    return 0;
}

static int unionfs_rmdir(const char *path) {
    struct mini_unionfs_state *state = UNIONFS_DATA;
    char upper_path[PATH_MAX];
    char lower_path[PATH_MAX];
    char whiteout_path[PATH_MAX];

    build_full_path(upper_path, sizeof(upper_path), state->upper_dir, path);
    build_full_path(lower_path, sizeof(lower_path), state->lower_dir, path);

    if (access(upper_path, F_OK) == 0) {
        if (rmdir(upper_path) == -1) {
            return -errno;
        }
    }

    if (access(lower_path, F_OK) == 0) {
        int res = build_whiteout_path(path, whiteout_path, sizeof(whiteout_path));
        if (res != 0) {
            return res;
        }

        int fd = open(whiteout_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd == -1) {
            return -errno;
        }
        close(fd);
    }

    return 0;
}

static struct fuse_operations unionfs_oper = {
    .getattr = unionfs_getattr,
    .readdir = unionfs_readdir,
    .open    = unionfs_open,
    .release = unionfs_release,
    .read    = unionfs_read,
    .write   = unionfs_write,
    .create  = unionfs_create,
    .unlink  = unionfs_unlink,
    .mkdir   = unionfs_mkdir,
    .rmdir   = unionfs_rmdir,
};

int main(int argc, char *argv[]) {
    struct mini_unionfs_state *state;
    char *fuse_argv[2];

    if (argc < 4) {
        printf("Usage: %s <lower_dir> <upper_dir> <mount_point>\n", argv[0]);
        return 1;
    }

    state = malloc(sizeof(struct mini_unionfs_state));
    if (state == NULL) {
        return 1;
    }

    state->lower_dir = realpath(argv[1], NULL);
    state->upper_dir = realpath(argv[2], NULL);

    if (state->lower_dir == NULL || state->upper_dir == NULL) {
        printf("Error resolving directory paths\n");
        return 1;
    }

    fuse_argv[0] = argv[0];
    fuse_argv[1] = argv[3];

    return fuse_main(2, fuse_argv, &unionfs_oper, state);
}

