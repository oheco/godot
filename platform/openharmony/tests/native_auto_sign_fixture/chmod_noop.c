/* Test-only LD_PRELOAD fixture for filesystems whose chmod succeeds silently.
 * Build/sign inside the individual test root; never load into the test runner.
 * SPDX-License-Identifier: MIT
 */
#include <sys/stat.h>

int chmod(const char *path, mode_t mode) {
	(void)path;
	(void)mode;
	return 0;
}

int fchmod(int fd, mode_t mode) {
	(void)fd;
	(void)mode;
	return 0;
}
