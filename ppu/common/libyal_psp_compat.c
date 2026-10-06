#include <errno.h>
#include <sys/types.h>

/*
 * PSP read-only compatibility shims for libyal/libfsntfs.
 *
 * PPA uses libfsntfs through a BFIO callback handle backed by msstor:.
 * NTFS support is read-only; truncate/write-capable paths must fail closed.
 */

static int ppa_libyal_readonly_unavailable(void)
{
	errno = ENOSYS;
	return -1;
}

int ftruncate(int fd, off_t length)
{
	(void)fd;
	(void)length;

	return ppa_libyal_readonly_unavailable();
}

int _ftruncate(int fd, off_t length)
{
	return ftruncate(fd, length);
}

int truncate(const char *path, off_t length)
{
	(void)path;
	(void)length;

	return ppa_libyal_readonly_unavailable();
}

int _truncate(const char *path, off_t length)
{
	return truncate(path, length);
}

int chsize(int fd, long length)
{
	(void)fd;
	(void)length;

	return ppa_libyal_readonly_unavailable();
}

int _chsize(int fd, long length)
{
	return chsize(fd, length);
}
