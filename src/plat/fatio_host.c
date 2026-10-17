/* Host: newlib's device table for libfat, and the stdio bridge that sends the core's file calls on "<dev>:/..." paths to
 * libfat, so the console's storage path (core -> stdio -> libfat -> disc guard -> device) runs unchanged on the PC.
 *
 * The host links with -Wl,--wrap=fopen,--wrap=remove,--wrap=mkdir,--wrap=stat,--wrap=fsync,--wrap=fileno (Makefile
 * HOST_WRAP): every reference in XTS objects goes to __wrap_X here, which forwards to the real libc function unless the
 * path names a registered libfat device. Files are glibc FILE objects made with fopencookie(), so fwrite/fread/fgets/
 * fflush/fclose/clearerr work as usual; fileno() on them returns a private descriptor that only __wrap_fsync knows.
 *
 * libfat (like newlib) passes the open file as an int holding the FILE_STRUCT pointer. FILE_STRUCTs therefore come from a
 * pool mapped below 2 GB (MAP_32BIT), which keeps the libfat sources identical to the console build. */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iosupport.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <plat/disc_host.h>

/* ---- newlib device table (what libfat's fatMount/fatUnmount/GetDeviceOpTab use) ---- */
const devoptab_t *devoptab_list[STD_MAX];

int FindDevice(const char *name)
{
  if (!name) return -1;
  if (!strchr(name, ':')) {          /* newlib returns its default device here; the host has none: exact name */
    for (int i = 0; i < STD_MAX; i++)
      if (devoptab_list[i] && !strcmp(devoptab_list[i]->name, name)) return i;
    return -1;
  }
  for (int i = 0; i < STD_MAX; i++) {
    if (!devoptab_list[i]) continue;
    size_t n = strlen(devoptab_list[i]->name);
    if (!strncmp(devoptab_list[i]->name, name, n) &&
        (name[n] == ':' || (isdigit((unsigned char)name[n]) && name[n + 1] == ':')))
      return i;
  }
  return -1;
}

int RemoveDevice(const char *name)
{
  int d = FindDevice(name);
  if (d < 0) return -1;
  devoptab_list[d] = NULL;
  return 0;
}

int AddDevice(const devoptab_t *device)
{
  int free_slot = -1;
  for (int i = 3; i < STD_MAX; i++) {
    if (devoptab_list[i] && !strcmp(devoptab_list[i]->name, device->name)) { devoptab_list[i] = device; return i; }
    if (!devoptab_list[i] && free_slot < 0) free_slot = i;
  }
  if (free_slot >= 0) devoptab_list[free_slot] = device;
  return free_slot;
}

const devoptab_t *GetDeviceOpTab(const char *name)
{
  int d = FindDevice(name);
  return d >= 0 ? devoptab_list[d] : NULL;
}

/* libfat's disc.c (built with -DLIBXENON, as on the console) lists libxenon's interfaces for fatInit(); on the host
 * they exist but have no medium. */
static bool no_medium(void) { return false; }
static bool no_rw(sec_t s, sec_t n, void *b) { (void)s; (void)n; (void)b; return false; }
static bool no_w(sec_t s, sec_t n, const void *b) { (void)s; (void)n; (void)b; return false; }
#define NO_DISC { 0, 0, no_medium, no_medium, no_rw, no_w, no_medium, no_medium, NULL }
DISC_INTERFACE xenon_ata_ops = NO_DISC, usb2mass_ops_0 = NO_DISC, usb2mass_ops_1 = NO_DISC, usb2mass_ops_2 = NO_DISC;

int xts_fatio_active(void)
{
  for (int i = 3; i < STD_MAX; i++)
    if (devoptab_list[i]) return 1;
  return 0;
}

/* ---- open files ---- */
#define NFILES 64
#define FD_BASE 0x4000
#define SLOT_BYTES 4096
static struct ofile { FILE *fp; const devoptab_t *dev; int fd; uint8_t *fs; } files[NFILES];
static uint8_t *pool;

static int is_fat_path(const char *path)
{
  return path && strchr(path, ':') && GetDeviceOpTab(path) != NULL;
}

static ssize_t ck_read(void *c, char *buf, size_t n)
{
  struct ofile *o = c;
  struct _reent r = { 0 };
  ssize_t k = o->dev->read_r(&r, o->fd, buf, n);
  if (k < 0) errno = r._errno ? r._errno : EIO;
  return k;
}

static ssize_t ck_write(void *c, const char *buf, size_t n)
{
  struct ofile *o = c;
  struct _reent r = { 0 };
  ssize_t k = o->dev->write_r(&r, o->fd, buf, n);
  if (k < 0) { errno = r._errno ? r._errno : EIO; return -1; }
  if (k == 0 && n) { errno = EIO; return -1; }    /* glibc would retry forever on 0 */
  return k;
}

static int ck_seek(void *c, off64_t *pos, int whence)
{
  struct ofile *o = c;
  struct _reent r = { 0 };
  off_t p = o->dev->seek_r(&r, o->fd, (off_t)*pos, whence);
  if (p < 0) { errno = r._errno ? r._errno : EINVAL; return -1; }
  *pos = p;
  return 0;
}

static int ck_close(void *c)
{
  struct ofile *o = c;
  struct _reent r = { 0 };
  int k = o->dev->close_r(&r, o->fd);
  o->fp = NULL;
  o->dev = NULL;
  if (k < 0) { errno = r._errno ? r._errno : EIO; return -1; }
  return 0;
}

static FILE *fat_fopen(const char *path, const char *mode)
{
  const devoptab_t *dev = GetDeviceOpTab(path);
  if (!pool) {
    pool = mmap(NULL, (size_t)NFILES * SLOT_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (pool == MAP_FAILED) { pool = NULL; errno = ENOMEM; return NULL; }
  }
  if (dev->structSize > SLOT_BYTES) { errno = ENOMEM; return NULL; }
  int i = 0;
  while (i < NFILES && files[i].dev) i++;
  if (i == NFILES) { errno = EMFILE; return NULL; }
  int plus = strchr(mode, '+') != NULL, flags;
  switch (mode[0]) {
  case 'r': flags = plus ? O_RDWR : O_RDONLY; break;
  case 'w': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; break;
  case 'a': flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND; break;
  default: errno = EINVAL; return NULL;
  }
  struct ofile *o = &files[i];
  o->fs = pool + (size_t)i * SLOT_BYTES;
  memset(o->fs, 0, SLOT_BYTES);
  struct _reent r = { 0 };
  int fd = dev->open_r(&r, o->fs, path, flags, 0666);
  if (fd == -1) { errno = r._errno ? r._errno : ENOENT; return NULL; }
  o->dev = dev;
  o->fd = fd;
  cookie_io_functions_t io = { ck_read, ck_write, ck_seek, ck_close };
  o->fp = fopencookie(o, plus ? "r+" : (mode[0] == 'r' ? "r" : "w"), io);
  if (!o->fp) { dev->close_r(&r, fd); o->dev = NULL; return NULL; }
  return o->fp;
}

static struct ofile *find_fp(FILE *fp)
{
  for (int i = 0; i < NFILES; i++)
    if (files[i].dev && files[i].fp == fp) return &files[i];
  return NULL;
}

int xts_fatio_statvfs(const char *path, struct statvfs *sv)
{
  const devoptab_t *dev = GetDeviceOpTab(path);
  if (!dev || !dev->statvfs_r) { errno = ENODEV; return -1; }
  struct _reent r = { 0 };
  int k = dev->statvfs_r(&r, path, sv);
  if (k < 0) errno = r._errno;
  return k;
}

/* ---- the wrapped libc entry points ---- */
FILE *__real_fopen(const char *path, const char *mode);
int __real_remove(const char *path);
int __real_mkdir(const char *path, mode_t mode);
int __real_stat(const char *path, struct stat *st);
int __real_fsync(int fd);
int __real_fileno(FILE *fp);

FILE *__wrap_fopen(const char *path, const char *mode)
{
  return is_fat_path(path) ? fat_fopen(path, mode) : __real_fopen(path, mode);
}

int __wrap_remove(const char *path)
{
  if (!is_fat_path(path)) return __real_remove(path);
  const devoptab_t *dev = GetDeviceOpTab(path);
  struct _reent r = { 0 };
  int k = dev->unlink_r(&r, path);
  if (k < 0) errno = r._errno;
  return k;
}

int __wrap_mkdir(const char *path, mode_t mode)
{
  if (!is_fat_path(path)) return __real_mkdir(path, mode);
  const devoptab_t *dev = GetDeviceOpTab(path);
  struct _reent r = { 0 };
  int k = dev->mkdir_r(&r, path, (int)mode);
  if (k < 0) errno = r._errno;
  return k;
}

int __wrap_stat(const char *path, struct stat *st)
{
  if (!is_fat_path(path)) return __real_stat(path, st);
  const devoptab_t *dev = GetDeviceOpTab(path);
  struct _reent r = { 0 };
  memset(st, 0, sizeof *st);
  int k = dev->stat_r(&r, path, st);
  if (k < 0) errno = r._errno;
  return k;
}

int __wrap_fileno(FILE *fp)
{
  struct ofile *o = find_fp(fp);
  return o ? FD_BASE + (int)(o - files) : __real_fileno(fp);
}

int __wrap_fsync(int fd)
{
  if (fd < FD_BASE || fd >= FD_BASE + NFILES) return __real_fsync(fd);
  struct ofile *o = &files[fd - FD_BASE];
  if (!o->dev) { errno = EBADF; return -1; }
  struct _reent r = { 0 };
  int k = o->dev->fsync_r(&r, o->fd);
  if (k < 0) errno = r._errno ? r._errno : EIO;
  return k;
}
