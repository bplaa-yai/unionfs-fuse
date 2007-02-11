/*-
 * Copyright (c) 1991, 1993, 1994
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Bernd Schubert <bernd-schubert@gmx.de>:
 *	This file was taken from OpenBSD and modified to fit the unionfs requirements.
 */


#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <utime.h>
#include <syslog.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include "unionfs.h"
#include "cow.h"

// BSD seems to know S_ISTXT itself
#ifndef S_ISTXT
#define S_ISTXT S_ISVTX
#endif

/**
 * set the stat() data of a file
 **/
static int setfile(const char *path, struct stat *fs)
{
	struct utimbuf ut;
	int rval;

	rval = 0;
	fs->st_mode &= S_ISUID | S_ISGID | S_IRWXU | S_IRWXG | S_IRWXO;

	ut.actime  = fs->st_atime;
	ut.modtime = fs->st_mtime;
	if (utime(path, &ut)) {
		syslog(LOG_WARNING,   "utimes: %s", path);
		rval = 1;
	}
	/*
	* Changing the ownership probably won't succeed, unless we're root
	* or POSIX_CHOWN_RESTRICTED is not set.  Set uid/gid before setting
	* the mode; current BSD behavior is to remove all setuid bits on
	* chown.  If chown fails, lose setuid/setgid bits.
	*/
	if (chown(path, fs->st_uid, fs->st_gid)) {
		if (errno != EPERM) {
			syslog(LOG_WARNING,   "chown: %s", path);
			rval = 1;
		}
		fs->st_mode &= ~(S_ISTXT | S_ISUID | S_ISGID);
	}
	
	if (chmod(path, fs->st_mode)) {
		syslog(LOG_WARNING,   "chown: %s", path);
		rval = 1;
	}

#ifdef HAVE_CHFLAGS
		/*
		 * XXX
		 * NFS doesn't support chflags; ignore errors unless there's reason
		 * to believe we're losing bits.  (Note, this still won't be right
		 * if the server supports flags and we were trying to *remove* flags
		 * on a file that we copied, i.e., that we didn't create.)
		 */
		errno = 0;
		if (chflags(path, fs->st_flags)) {
			if (errno != EOPNOTSUPP || fs->st_flags != 0) {
				syslog(LOG_WARNING,   "chflags: %s", path);
				rval = 1;
			}
			return (rval);
		}
#endif
	return 0;
}

/**
 * set the stat() data of a link
 **/
static int setlink(const char *path, struct stat *fs)
{

	if (lchown(path, fs->st_uid, fs->st_gid)) {
		if (errno != EPERM) {
			syslog(LOG_WARNING,   "lchown: %s", path);
			return (1);
		}
	}
	return (0);
}


/**
 * copy an ordinary file with all of its stat() data
 **/
int copy_file(struct cow *cow)
{
	static char buf[MAXBSIZE];
	struct stat to_stat, *fs;
	int from_fd, rcount, rval, to_fd, wcount;
#ifdef VM_AND_BUFFER_CACHE_SYNCHRONIZED
	char *p;
#endif

	if ((from_fd = open(cow->from_path, O_RDONLY, 0)) == -1) {
		syslog(LOG_WARNING, "%s", cow->from_path);
		return (1);
	}

	fs = cow->stat;

	to_fd = open(cow->to_path, O_WRONLY | O_TRUNC | O_CREAT,
	             fs->st_mode & ~(S_ISTXT | S_ISUID | S_ISGID));

	if (to_fd == -1) {
		syslog(LOG_WARNING, "%s", cow->to_path);
		(void)close(from_fd);
		return (1);
	}

	/*
	 * Mmap and write if less than 8M (the limit is so we don't totally
	 * trash memory on big files.  This is really a minor hack, but it
	 * wins some CPU back.
	 */
#ifdef VM_AND_BUFFER_CACHE_SYNCHRONIZED
	if (fs->st_size <= 8 * 1048576) {
		if ((p = mmap(NULL, (size_t)fs->st_size, PROT_READ,
		    MAP_FILE|MAP_SHARED, from_fd, (off_t)0)) == MAP_FAILED) {
			syslog(LOG_WARNING,   "mmap: %s", cow->from_path);
			rval = 1;
		} else {
			madvise(p, fs->st_size, MADV_SEQUENTIAL);
			if (write(to_fd, p, fs->st_size) != fs->st_size) {
				syslog(LOG_WARNING,   "%s", cow->to_path);
				rval = 1;
			}
			/* Some systems don't unmap on close(2). */
			if (munmap(p, fs->st_size) < 0) {
				syslog(LOG_WARNING,   "%s", cow->from_path);
				rval = 1;
			}
		}
	} else
#endif
	{
		while ((rcount = read(from_fd, buf, MAXBSIZE)) > 0) {
			wcount = write(to_fd, buf, rcount);
			if (rcount != wcount || wcount == -1) {
				syslog(LOG_WARNING,   "%s", cow->to_path);
				rval = 1;
				break;
			}
		}
		if (rcount < 0) {
			syslog(LOG_WARNING,   "copy failed: %s", cow->from_path);
			return 1;
		}
	}

	if (rval == 1) {
		(void)close(from_fd);
		(void)close(to_fd);
		return (1);
	}

	if (setfile(cow->to_path, cow->stat))
		rval = 1;
	/*
	 * If the source was setuid or setgid, lose the bits unless the
	 * copy is owned by the same user and group.
	 */
#define	RETAINBITS \
	(S_ISUID | S_ISGID | S_ISVTX | S_IRWXU | S_IRWXG | S_IRWXO)
	else if (fs->st_mode & (S_ISUID | S_ISGID) && fs->st_uid == cow->uid) {
		if (fstat(to_fd, &to_stat)) {
			syslog(LOG_WARNING,   "%s", cow->to_path);
			rval = 1;
		} else if (fs->st_gid == to_stat.st_gid &&
		    fchmod(to_fd, fs->st_mode & RETAINBITS & ~cow->umask)) {
			syslog(LOG_WARNING,   "%s", cow->to_path);
			rval = 1;
		}
	}
	(void)close(from_fd);
	if (close(to_fd)) {
		syslog(LOG_WARNING,   "%s", cow->to_path);
		rval = 1;
	}
	
	return (rval);
}

/**
 * copy a link, actually we recreate the link and only copy its stat() data.
 */
int copy_link(struct cow *cow)
{
	int len;
	char link[PATHLEN_MAX];

	if ((len = readlink(cow->from_path, link, sizeof(link)-1)) == -1) {
		syslog(LOG_WARNING,   "readlink: %s", cow->from_path);
		return (1);
	}
	
	if (symlink(cow->from_path, cow->to_path)) {
		syslog(LOG_WARNING,   "symlink: %s", link);
		return (1);
	}
	
	return setlink(cow->to_path, cow->stat);
}

/**
 * copy a fifo, actually we recreate the fifo and only copy
 * its stat() data
 **/
int copy_fifo(struct cow *cow)
{
	if (mkfifo(cow->to_path, cow->stat->st_mode)) {
		syslog(LOG_WARNING,   "mkfifo: %s", cow->to_path);
		return (1);
	}
	return setfile(cow->to_path, cow->stat);
}

/**
 * copy a special file, actually we recreate this file and only copy
 * its stat() data
 */
int copy_special(struct cow *cow)
{
	if (mknod(cow->to_path, cow->stat->st_mode, cow->stat->st_rdev)) {
		syslog(LOG_WARNING,   "mknod: %s", cow->to_path);
		return (1);
	}
	return setfile(cow->to_path, cow->stat);
}