/*********************************************************************
 * bfs                                                               *
 * Copyright (C) 2016-2017 Tavian Barnes <tavianator@tavianator.com> *
 *                                                                   *
 * This program is free software. It comes without any warranty, to  *
 * the extent permitted by applicable law. You can redistribute it   *
 * and/or modify it under the terms of the Do What The Fuck You Want *
 * To Public License, Version 2, as published by Sam Hocevar. See    *
 * the COPYING file or http://www.wtfpl.net/ for more details.       *
 *********************************************************************/

#ifndef BFS_UTIL_H
#define BFS_UTIL_H

#include "bftw.h"
#include <dirent.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <regex.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <time.h>

// Some portability concerns

#if __APPLE__
#	define st_atim st_atimespec
#	define st_ctim st_ctimespec
#	define st_mtim st_mtimespec
#endif

#if !defined(FNM_CASEFOLD) && defined(FNM_IGNORECASE)
#	define FNM_CASEFOLD FNM_IGNORECASE
#endif

#ifndef S_ISDOOR
#	define S_ISDOOR(mode) false
#endif

#ifndef S_ISPORT
#	define S_ISPORT(mode) false
#endif

#ifndef S_ISWHT
#	define S_ISWHT(mode) false
#endif

/**
 * readdir() wrapper that makes error handling cleaner.
 */
int xreaddir(DIR *dir, struct dirent **de);

/**
 * readlinkat() wrapper that dynamically allocates the result.
 *
 * @param fd
 *         The base directory descriptor.
 * @param path
 *         The path to the link, relative to fd.
 * @param size
 *         An estimate for the size of the link name (pass 0 if unknown).
 * @return The target of the link, allocated with malloc(), or NULL on failure.
 */
char *xreadlinkat(int fd, const char *path, size_t size);

/**
 * Check if a file descriptor is open.
 */
bool isopen(int fd);

/**
 * Open a file and redirect it to a particular descriptor.
 *
 * @param fd
 *         The file descriptor to redirect.
 * @param path
 *         The path to open.
 * @param flags
 *         The flags passed to open().
 * @param mode
 *         The mode passed to open() (optional).
 * @return fd on success, -1 on failure.
 */
int redirect(int fd, const char *path, int flags, ...);

/**
 * Like dup(), but set the FD_CLOEXEC flag.
 *
 * @param fd
 *         The file descriptor to duplicate.
 * @return A duplicated file descriptor, or -1 on failure.
 */
int dup_cloexec(int fd);

/**
 * Dynamically allocate a regex error message.
 *
 * @param err
 *         The error code to stringify.
 * @param regex
 *         The (partially) compiled regex.
 * @return A human-readable description of the error, allocated with malloc().
 */
char *xregerror(int err, const regex_t *regex);

/**
 * localtime_r() wrapper that calls tzset() first.
 *
 * @param timep
 *         The time_t to convert.
 * @param result
 *         Buffer to hold the result.
 * @return 0 on success, -1 on failure.
 */
int xlocaltime(const time_t *timep, struct tm *result);

/**
 * Format a mode like ls -l (e.g. -rw-r--r--).
 *
 * @param mode
 *         The mode to format.
 * @param str
 *         The string to hold the formatted mode.
 */
void format_mode(mode_t mode, char str[11]);

/**
 * basename() variant that doesn't modify the input.
 *
 * @param path
 *         The path in question.
 * @return A pointer into path at the base name offset.
 */
const char *xbasename(const char *path);

/**
 * Convert a stat() st_mode to a bftw() typeflag.
 */
enum bftw_typeflag mode_to_typeflag(mode_t mode);

/**
 * Convert a directory entry to a bftw() typeflag.
 */
enum bftw_typeflag dirent_to_typeflag(const struct dirent *de);

#endif // BFS_UTIL_H
