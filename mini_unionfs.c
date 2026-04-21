#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <stdlib.h>

struct unionfs_state {
    char *lower_dir;
    char *upper_dir;
};

#define UNIONFS_DATA ((struct unionfs_state *) fuse_get_context()->private_data)

// ---------------- COPY FILE ----------------
int copy_file(const char *src, const char *dest)
{
    int in = open(src, O_RDONLY);
    if (in == -1) return -errno;

    int out = open(dest, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (out == -1) {
        close(in);
        return -errno;
    }

    char buf[4096];
    ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0)
        write(out, buf, n);

    close(in);
    close(out);
    return 0;
}

// ---------------- RESOLVE PATH ----------------
int resolve_path(const char *path, char *resolved)
{
    char upper[512], lower[512], whiteout[512];

    snprintf(upper, sizeof(upper), "%s%s", UNIONFS_DATA->upper_dir, path);
    snprintf(lower, sizeof(lower), "%s%s", UNIONFS_DATA->lower_dir, path);

    snprintf(whiteout, sizeof(whiteout), "%s/.wh.%s",
             UNIONFS_DATA->upper_dir,
             path[0] == '/' ? path + 1 : path);

    if (access(whiteout, F_OK) == 0)
        return -ENOENT;

    if (access(upper, F_OK) == 0) {
        strcpy(resolved, upper);
        return 0;
    }

    if (access(lower, F_OK) == 0) {
        strcpy(resolved, lower);
        return 0;
    }

    return -ENOENT;
}

// ---------------- GETATTR ----------------
static int unionfs_getattr(const char *path, struct stat *stbuf,
                           struct fuse_file_info *fi)
{
    (void) fi;
    char resolved[512];

    if (resolve_path(path, resolved) != 0)
        return -ENOENT;

    if (lstat(resolved, stbuf) == -1)
        return -errno;

    return 0;
}

// ---------------- READDIR ----------------
static int unionfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                            off_t offset, struct fuse_file_info *fi,
                            enum fuse_readdir_flags flags)
{
    (void) offset; (void) fi; (void) flags;

    DIR *dp;
    struct dirent *de;

    char upper[512], lower[512];
    char seen[100][256];
    int count = 0;

    snprintf(upper, sizeof(upper), "%s%s", UNIONFS_DATA->upper_dir, path);
    snprintf(lower, sizeof(lower), "%s%s", UNIONFS_DATA->lower_dir, path);

    dp = opendir(upper);
    if (dp) {
        while ((de = readdir(dp))) {
            if (strncmp(de->d_name, ".wh.", 4) == 0) continue;
            filler(buf, de->d_name, NULL, 0, 0);
            strcpy(seen[count++], de->d_name);
        }
        closedir(dp);
    }

    dp = opendir(lower);
    if (dp) {
        while ((de = readdir(dp))) {
            int found = 0;

            for (int i = 0; i < count; i++)
                if (strcmp(seen[i], de->d_name) == 0)
                    found = 1;

            char whiteout[512];
            snprintf(whiteout, sizeof(whiteout), "%s/.wh.%s",
                     UNIONFS_DATA->upper_dir, de->d_name);

            if (access(whiteout, F_OK) == 0)
                found = 1;

            if (!found)
                filler(buf, de->d_name, NULL, 0, 0);
        }
        closedir(dp);
    }

    return 0;
}

// ---------------- OPEN ----------------
static int unionfs_open(const char *path, struct fuse_file_info *fi)
{
    char upper[512], lower[512], resolved[512];

    snprintf(upper, sizeof(upper), "%s%s", UNIONFS_DATA->upper_dir, path);
    snprintf(lower, sizeof(lower), "%s%s", UNIONFS_DATA->lower_dir, path);

    // Handle CoW
    if ((fi->flags & (O_WRONLY | O_RDWR)) &&
        access(upper, F_OK) != 0 &&
        access(lower, F_OK) == 0) {

        if (copy_file(lower, upper) != 0)
            return -errno;
    }

    // Handle create (safe)
    if ((fi->flags & O_CREAT) && access(upper, F_OK) != 0) {
        int fd = open(upper, O_CREAT | O_WRONLY, 0644);
        if (fd == -1) return -errno;
        close(fd);
    }

    if (resolve_path(path, resolved) != 0)
        return -ENOENT;

    int fd = open(resolved, fi->flags);
    if (fd == -1)
        return -errno;

    close(fd);
    return 0;
}

// ---------------- READ ----------------
static int unionfs_read(const char *path, char *buf, size_t size,
                        off_t offset, struct fuse_file_info *fi)
{
    (void) fi;
    char resolved[512];

    if (resolve_path(path, resolved) != 0)
        return -ENOENT;

    int fd = open(resolved, O_RDONLY);
    if (fd == -1)
        return -errno;

    int res = pread(fd, buf, size, offset);
    close(fd);
    return res;
}

// ---------------- WRITE ----------------
static int unionfs_write(const char *path, const char *buf, size_t size,
                         off_t offset, struct fuse_file_info *fi)
{
    char upper[512], lower[512], resolved[512];

    snprintf(upper, sizeof(upper), "%s%s", UNIONFS_DATA->upper_dir, path);
    snprintf(lower, sizeof(lower), "%s%s", UNIONFS_DATA->lower_dir, path);

    if (access(upper, F_OK) != 0 && access(lower, F_OK) == 0)
        copy_file(lower, upper);

    if (resolve_path(path, resolved) != 0)
        return -ENOENT;

    int fd = open(resolved, O_WRONLY);
    if (fd == -1)
        return -errno;

    int res = pwrite(fd, buf, size, offset);
    close(fd);
    return res;
}

// ---------------- CREATE ----------------
static int unionfs_create(const char *path, mode_t mode,
                          struct fuse_file_info *fi)
{
    char upper[512];

    snprintf(upper, sizeof(upper), "%s%s", UNIONFS_DATA->upper_dir, path);

    int fd = open(upper, O_CREAT | O_WRONLY, mode);
    if (fd == -1)
        return -errno;

    close(fd);
    return 0;
}

// ---------------- UNLINK ----------------
static int unionfs_unlink(const char *path)
{
    char upper[512], lower[512], whiteout[512];

    snprintf(upper, sizeof(upper), "%s%s", UNIONFS_DATA->upper_dir, path);
    snprintf(lower, sizeof(lower), "%s%s", UNIONFS_DATA->lower_dir, path);

    snprintf(whiteout, sizeof(whiteout), "%s/.wh.%s",
             UNIONFS_DATA->upper_dir,
             path[0] == '/' ? path + 1 : path);

    if (access(upper, F_OK) == 0)
        return unlink(upper);

    if (access(lower, F_OK) == 0) {
        int fd = open(whiteout, O_CREAT | O_WRONLY, 0644);
        if (fd == -1)
            return -errno;
        close(fd);
        return 0;
    }

    return -ENOENT;
}

// ---------------- OPERATIONS ----------------
static struct fuse_operations unionfs_oper = {
    .getattr = unionfs_getattr,
    .readdir = unionfs_readdir,
    .open    = unionfs_open,
    .read    = unionfs_read,
    .write   = unionfs_write,
    .create  = unionfs_create,
    .unlink  = unionfs_unlink,
};

// ---------------- MAIN ----------------
int main(int argc, char *argv[])
{
    struct unionfs_state *state = malloc(sizeof(struct unionfs_state));

    state->lower_dir = realpath(argv[1], NULL);
    state->upper_dir = realpath(argv[2], NULL);

    char *fuse_argv[3];
    fuse_argv[0] = argv[0];
    fuse_argv[1] = argv[3];
    fuse_argv[2] = "-f";

    return fuse_main(3, fuse_argv, &unionfs_oper, state);
}
