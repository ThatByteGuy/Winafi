#define _GNU_SOURCE
#include "mount_ops.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>

extern char **environ;

static int run_mount(char *const argv[]) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execve("/usr/bin/mount", argv, environ);
        execve("/bin/mount", argv, environ);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    return 0;
}

int mount_create_temp_dirs(mount_context_t *ctx) {
    if (!ctx) return -1;

    // Create base temp directory
    char template[] = "/tmp/winafi-mount-XXXXXX";
    char *result = mkdtemp(template);
    if (!result) {
        return -1;  // E-50-B
    }

    strncpy(ctx->temp_dir, template, 255);
    ctx->temp_dir[255] = '\0';

    // Create subdirectories
    if (snprintf(ctx->fat_mount, sizeof(ctx->fat_mount), "%s/fat", ctx->temp_dir) < 0 ||
        strlen(ctx->temp_dir) + strlen("/fat") >= sizeof(ctx->fat_mount)) {
        rmdir(ctx->temp_dir);
        return -1;
    }
    if (snprintf(ctx->ntfs_mount, sizeof(ctx->ntfs_mount), "%s/ntfs", ctx->temp_dir) < 0 ||
        strlen(ctx->temp_dir) + strlen("/ntfs") >= sizeof(ctx->ntfs_mount)) {
        rmdir(ctx->temp_dir);
        return -1;
    }

    if (mkdir(ctx->fat_mount, 0700) != 0) {
        rmdir(ctx->temp_dir);
        return -1;  // E-50-B
    }
    if (mkdir(ctx->ntfs_mount, 0700) != 0) {
        rmdir(ctx->fat_mount);
        rmdir(ctx->temp_dir);
        return -1;  // E-50-B
    }

    return 0;
}

int mount_fat32(const char *device, const char *mount_point) {
    if (!device || !mount_point) return -1;

    char *argv[] = {
        (char *)"mount", (char *)"-t", (char *)"vfat", (char *)"-o",
        (char *)"uid=0,gid=0,umask=0,fmask=0111,flush",
        (char *)device, (char *)mount_point, NULL
    };
    if (run_mount(argv) != 0) {
        return -1;  // E-22-A
    }

    return 0;
}

int mount_ntfs(const char *device, const char *mount_point) {
    if (!device || !mount_point) return -1;

    // Try ntfs-3g (FUSE) first — supports flush and broad compatibility
    char *fuse_argv[] = {
        (char *)"mount", (char *)"-t", (char *)"ntfs-3g", (char *)"-o",
        (char *)"uid=0,gid=0,umask=0,fmask=0111,flush",
        (char *)device, (char *)mount_point, NULL
    };
    if (run_mount(fuse_argv) == 0) {
        return 0;
    }

    // Fallback to ntfs3 (kernel driver) — no flush support
    char *ntfs3_argv[] = {
        (char *)"mount", (char *)"-t", (char *)"ntfs3", (char *)"-o",
        (char *)"uid=0,gid=0,umask=0,fmask=0111",
        (char *)device, (char *)mount_point, NULL
    };
    if (run_mount(ntfs3_argv) == 0) {
        return 0;
    }

    // Final fallback: let mount auto-detect
    char *fallback_argv[] = {
        (char *)"mount", (char *)"-t", (char *)"ntfs", (char *)"-o",
        (char *)"uid=0,gid=0,umask=0",
        (char *)device, (char *)mount_point, NULL
    };
    if (run_mount(fallback_argv) != 0) {
        return -1;  // E-22-A
    }

    return 0;
}

int unmount_and_cleanup(mount_context_t *ctx) {
    if (!ctx) return -1;

    int ret = 0;

    // Unmount NTFS first (use MNT_DETACH to avoid blocking on FUSE flush)
    if (strlen(ctx->ntfs_mount) > 0) {
        if (umount2(ctx->ntfs_mount, MNT_DETACH) != 0) {
            ret = -1;  // E-22-B (but continue cleanup)
        }
        rmdir(ctx->ntfs_mount);
    }

    // Unmount FAT32
    if (strlen(ctx->fat_mount) > 0) {
        if (umount2(ctx->fat_mount, MNT_DETACH) != 0) {
            ret = -1;
        }
        rmdir(ctx->fat_mount);
    }

    // Remove temp directory
    if (strlen(ctx->temp_dir) > 0) {
        if (rmdir(ctx->temp_dir) != 0) {
            ret = -1;  // E-50-F (non-fatal)
        }
    }

    return ret;
}

int mount_sync(void) {
    // Sync all filesystems — can be slow if other I/O is in flight
    sync();
    return 0;
}

int mount_sync_path(const char *path) {
    if (!path) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    int ret = syncfs(fd);
    close(fd);
    return ret;
}

int mount_sync_path_timeout(const char *path, int timeout_sec) {
    if (!path) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) _exit(1);
        syncfs(fd);
        close(fd);
        _exit(0);
    }
    if (pid < 0) return -1;
    int status;
    while (timeout_sec > 0) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return 0;
            return -1;
        }
        if (r < 0) return -1;
        sleep(1);
        timeout_sec--;
    }
    // syncfs is uninterruptible (D state); SIGTERM won't stop it.
    // Detach child — it will finish and exit harmlessly in background.
    // All the data will still flush to disk.
    return 0;
}
