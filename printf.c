/*********************************************************************
 * bfs                                                               *
 * Copyright (C) 2017 Tavian Barnes <tavianator@tavianator.com>      *
 *                                                                   *
 * This program is free software. It comes without any warranty, to  *
 * the extent permitted by applicable law. You can redistribute it   *
 * and/or modify it under the terms of the Do What The Fuck You Want *
 * To Public License, Version 2, as published by Sam Hocevar. See    *
 * the COPYING file or http://www.wtfpl.net/ for more details.       *
 *********************************************************************/

#include "printf.h"
#include "bfs.h"
#include "color.h"
#include "dstring.h"
#include "mtab.h"
#include "util.h"
#include <assert.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

typedef int bfs_printf_fn(FILE *file, const struct bfs_printf_directive *directive, const struct BFTW *ftwbuf);

/**
 * A single directive in a printf command.
 */
struct bfs_printf_directive {
	/** The printing function to invoke. */
	bfs_printf_fn *fn;
	/** String data associated with this directive. */
	char *str;
	/** The time field to print. */
	enum time_field time_field;
	/** Character data associated with this directive. */
	char c;
	/** The current mount table. */
	const struct bfs_mtab *mtab;
	/** The next printf directive in the chain. */
	struct bfs_printf_directive *next;
};

/** Print some text as-is. */
static int bfs_printf_literal(FILE *file, const struct bfs_printf_directive *directive, const struct BFTW *ftwbuf) {
	return fprintf(file, "%s", directive->str);
}

/** \c: flush */
static int bfs_printf_flush(FILE *file, const struct bfs_printf_directive *directive, const struct BFTW *ftwbuf) {
	return fflush(file);
}

/**
 * Print a value to a temporary buffer before formatting it.
 */
#define BFS_PRINTF_BUF(buf, format, ...)				\
	char buf[256];							\
	int ret = snprintf(buf, sizeof(buf), format, __VA_ARGS__);	\
	assert(ret >= 0 && ret < sizeof(buf));				\
	(void)ret

/**
 * Get a particular time field from a struct stat.
 */
static const struct timespec *get_time_field(const struct stat *statbuf, enum time_field time_field) {
	switch (time_field) {
	case ATIME:
		return &statbuf->st_atim;
	case CTIME:
		return &statbuf->st_ctim;
	case MTIME:
		return &statbuf->st_mtim;
	}

	assert(false);
	return NULL;
}

/** %c, %c, and %t: ctime() */
static int bfs_printf_ctime(FILE *file, const struct bfs_printf_directive *directive, const struct BFTW *ftwbuf) {
	// Not using ctime() itself because GNU find adds nanoseconds
	static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
	static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

	const struct timespec *ts = get_time_field(ftwbuf->statbuf, directive->time_field);
	struct tm tm;
	if (xlocaltime(&ts->tv_sec, &tm) != 0) {
		return -1;
	}

	BFS_PRINTF_BUF(buf, "%s %s %2d %.2d:%.2d:%.2d.%09ld0 %4d",
	               days[tm.tm_wday],
	               months[tm.tm_mon],
	               tm.tm_mday,
	               tm.tm_hour,
	               tm.tm_min,
	               tm.tm_sec,
	               (long)ts->tv_nsec,
	               1900 + tm.tm_year);

	return fprintf(file, directive->str, buf);
}

/** %A, %C, %T: strftime() */
static int bfs_printf_strftime(FILE *file, const struct bfs_printf_directive *directive, const struct BFTW *ftwbuf) {
	const struct timespec *ts = get_time_field(ftwbuf->statbuf, directive->time_field);
	struct tm tm;
	if (xlocaltime(&ts->tv_sec, &tm) != 0) {
		return -1;
	}

	int ret;
	char buf[256];
	char format[] = "% ";
	switch (directive->c) {
	// Non-POSIX strftime() features
	case '@':
		ret = snprintf(buf, sizeof(buf), "%lld.%09ld0", (long long)ts->tv_sec, (long)ts->tv_nsec);
		break;
	case 'k':
		ret = snprintf(buf, sizeof(buf), "%2d", tm.tm_hour);
		break;
	case 'l':
		ret = snprintf(buf, sizeof(buf), "%2d", (tm.tm_hour + 11)%12 + 1);
		break;
	case 'S':
		ret = snprintf(buf, sizeof(buf), "%.2d.%09ld0", tm.tm_sec, (long)ts->tv_nsec);
		break;
	case '+':
		ret = snprintf(buf, sizeof(buf), "%4d-%.2d-%.2d+%.2d:%.2d:%.2d.%09ld0",
		               1900 + tm.tm_year,
		               tm.tm_mon + 1,
		               tm.tm_mday,
		               tm.tm_hour,
		               tm.tm_min,
		               tm.tm_sec,
		               (long)ts->tv_nsec);
		break;

	// POSIX strftime() features
	default:
		format[1] = directive->c;
		ret = strftime(buf, sizeof(buf), format, &tm);
		break;
	}

	assert(ret >= 0 && ret < sizeof(buf));
	(void)ret;

	return fprintf(file, directive->str, buf);
}

/** %b: blocks */
static int bfs_printf_b(FILE *file, const struct bfs_printf_directive *directive, const struct BFTW *ftwbuf) {
	BFS_PRINTF_BUF(buf, "%ju", (uintmax_t)ftwbuf->statbuf->st_blocks);
	return fprintf(file, directive->str, buf);
}

/** %d: depth */
static int bfs_printf_d(FILE *file, const struct bfs_printf_directive *directive, const struct BFTW *ftwbuf) {
	return fprintf(file, directive->str, (intmax_t)ftwbuf->depth);
}

/** %D: device */
static int bfs_printf_D(FILE *file, const struct bfs_printf_directive *directive, const struct BFTW *ftwbuf) {
	BFS_PRINTF_BUF(buf, "%ju", (uintmax_t)ftwbuf->statbuf->st_dev);
	return fprintf(file, directive->str, buf);
}

/** %f: file name */
static int bfs_printf_f(FILE *file, const struct bfs_printf_directive *directive, const struct BFTW *ftwbuf) {
	return fprintf(file, directive->str, ftwbuf->path + ftwbuf->nameoff);
}

/** %F: file system type */
static int bfs_printf_F(FILE *file, const struct bfs_printf_directive *directive, const struct BFTW *ftwbuf) {
	const char *type = bfs_fstype(directive->mtab, ftwbuf->statbuf);
	return fprintf(file, directive->str, type);
}

/** %G: gid */
static int bfs_printf_G(FILE *file, const struct bfs_printf_directive *directive, const struct BFTW *ftwbuf) {
	BFS_PRINTF_BUF(buf, "%ju", (uintmax_t)ftwbuf->statbuf->st_gid);
	return fprintf(file, directive->str, buf);
}

/** %g: group name */
static int bfs_printf_g(FILE *file, const struct bfs_printf_directive *directive, const struct BFTW *ftwbuf) {
	struct group *grp = getgrgid(ftwbuf->statbuf->st_gid);
	if (!grp) {
		return bfs_printf_G(file, directive, ftwbuf);
	}

	return fprintf(file, directive->str, grp->gr_name);
}

/** %h: leading directories */
static int bfs_printf_h(FILE *file, const struct bfs_printf_directive *directive, const struct BFTW *ftwbuf) {
	char *copy = NULL;
	const char *buf;

	if (ftwbuf->nameoff > 0) {
		size_t len = ftwbuf->nameoff;
		if (len > 1) {
			--len;
		}

		buf = copy = strndup(ftwbuf->path, len);
	} else if (ftwbuf->path[0] == '/') {
		buf = "/";
	} else {
		buf = ".";
	}

	if (!buf) {
		return -1;
	}

	int ret = fprintf(file, directive->str, buf);
	free(copy);
	return ret;
}

/** %H: current root */
static int bfs_printf_H(FILE *file, const struct bfs_printf_directive *directive, const struct BFTW *ftwbuf) {
	return fprintf(file, directive->str, ftwbuf->root);
}

/** %i: inode */
static int bfs_printf_i(FILE *file, const struct bfs_printf_directive *directive, const struct BFTW *ftwbuf) {
	BFS_PRINTF_BUF(buf, "%ju", (uintmax_t)ftwbuf->statbuf->st_ino);
	return fprintf(file, directive->str, buf);
}

/** %k: 1K blocks */
static int bfs_printf_k(FILE *file, const struct bfs_printf_directive *directive, const struct BFTW *ftwbuf) {
	BFS_PRINTF_BUF(buf, "%ju", (uintmax_t)(ftwbuf->statbuf->st_blocks + 1)/2);
	return fprintf(file, directive->str, buf);
}

/** %l: link target */
static int bfs_printf_l(FILE *file, const struct bfs_printf_directive *directive, const struct BFTW *ftwbuf) {
	if (ftwbuf->typeflag != BFTW_LNK) {
		return 0;
	}

	char *target = xreadlinkat(ftwbuf->at_fd, ftwbuf->at_path, 0);
	if (!target) {
		return -1;
	}

	int ret = fprintf(file, directive->str, target);
	free(target);
	return ret;
}

/** %m: mode */
static int bfs_printf_m(FILE *file, const struct bfs_printf_directive *directive, const struct BFTW *ftwbuf) {
	return fprintf(file, directive->str, (unsigned int)(ftwbuf->statbuf->st_mode & 07777));
}

/** %M: symbolic mode */
static int bfs_printf_M(FILE *file, const struct bfs_printf_directive *directive, const struct BFTW *ftwbuf) {
	char buf[11];
	format_mode(ftwbuf->statbuf->st_mode, buf);
	return fprintf(file, directive->str, buf);
}

/** %n: link count */
static int bfs_printf_n(FILE *file, const struct bfs_printf_directive *directive, const struct BFTW *ftwbuf) {
	BFS_PRINTF_BUF(buf, "%ju", (uintmax_t)ftwbuf->statbuf->st_nlink);
	return fprintf(file, directive->str, buf);
}

/** %p: full path */
static int bfs_printf_p(FILE *file, const struct bfs_printf_directive *directive, const struct BFTW *ftwbuf) {
	return fprintf(file, directive->str, ftwbuf->path);
}

/** %P: path after root */
static int bfs_printf_P(FILE *file, const struct bfs_printf_directive *directive, const struct BFTW *ftwbuf) {
	const char *path = ftwbuf->path + strlen(ftwbuf->root);
	if (path[0] == '/') {
		++path;
	}
	return fprintf(file, directive->str, path);
}

/** %s: size */
static int bfs_printf_s(FILE *file, const struct bfs_printf_directive *directive, const struct BFTW *ftwbuf) {
	BFS_PRINTF_BUF(buf, "%ju", (uintmax_t)ftwbuf->statbuf->st_size);
	return fprintf(file, directive->str, buf);
}

/** %S: sparseness */
static int bfs_printf_S(FILE *file, const struct bfs_printf_directive *directive, const struct BFTW *ftwbuf) {
	double sparsity = 512.0 * ftwbuf->statbuf->st_blocks / ftwbuf->statbuf->st_size;
	return fprintf(file, directive->str, sparsity);
}

/** %U: uid */
static int bfs_printf_U(FILE *file, const struct bfs_printf_directive *directive, const struct BFTW *ftwbuf) {
	BFS_PRINTF_BUF(buf, "%ju", (uintmax_t)ftwbuf->statbuf->st_uid);
	return fprintf(file, directive->str, buf);
}

/** %u: user name */
static int bfs_printf_u(FILE *file, const struct bfs_printf_directive *directive, const struct BFTW *ftwbuf) {
	struct passwd *pwd = getpwuid(ftwbuf->statbuf->st_uid);
	if (!pwd) {
		return bfs_printf_U(file, directive, ftwbuf);
	}

	return fprintf(file, directive->str, pwd->pw_name);
}

static const char *bfs_printf_type(enum bftw_typeflag typeflag) {
	switch (typeflag) {
	case BFTW_BLK:
		return "b";
	case BFTW_CHR:
		return "c";
	case BFTW_DIR:
		return "d";
	case BFTW_DOOR:
		return "D";
	case BFTW_FIFO:
		return "p";
	case BFTW_LNK:
		return "l";
	case BFTW_REG:
		return "f";
	case BFTW_SOCK:
		return "s";
	default:
		return "U";
	}
}

/** %y: type */
static int bfs_printf_y(FILE *file, const struct bfs_printf_directive *directive, const struct BFTW *ftwbuf) {
	const char *type = bfs_printf_type(ftwbuf->typeflag);
	return fprintf(file, directive->str, type);
}

/** %Y: target type */
static int bfs_printf_Y(FILE *file, const struct bfs_printf_directive *directive, const struct BFTW *ftwbuf) {
	if (ftwbuf->typeflag != BFTW_LNK) {
		return bfs_printf_y(file, directive, ftwbuf);
	}

	const char *type = "U";

	struct stat sb;
	if (fstatat(ftwbuf->at_fd, ftwbuf->at_path, &sb, 0) == 0) {
		type = bfs_printf_type(mode_to_typeflag(sb.st_mode));
	} else {
		switch (errno) {
		case ELOOP:
			type = "L";
			break;
		case ENOENT:
			type = "N";
			break;
		}
	}

	return fprintf(file, directive->str, type);
}

/**
 * Free a printf directive.
 */
static void free_directive(struct bfs_printf_directive *directive) {
	if (directive) {
		dstrfree(directive->str);
		free(directive);
	}
}

/**
 * Create a new printf directive.
 */
static struct bfs_printf_directive *new_directive() {
	struct bfs_printf_directive *directive = malloc(sizeof(*directive));
	if (!directive) {
		perror("malloc()");
		goto error;
	}

	directive->fn = NULL;
	directive->str = dstralloc(2);
	if (!directive->str) {
		perror("dstralloc()");
		goto error;
	}
	directive->time_field = 0;
	directive->c = 0;
	directive->mtab = NULL;
	directive->next = NULL;
	return directive;

error:
	free_directive(directive);
	return NULL;
}

/**
 * Append a printf directive to the chain.
 */
static void append_directive(struct bfs_printf_directive ***tail, struct bfs_printf_directive *directive) {
	**tail = directive;
	*tail = &directive->next;
}

/**
 * Append a literal string to the chain.
 */
static int append_literal(struct bfs_printf_directive ***tail, struct bfs_printf_directive **literal, bool last) {
	struct bfs_printf_directive *directive = *literal;
	if (!directive || dstrlen(directive->str) == 0) {
		return 0;
	}

	directive->fn = bfs_printf_literal;
	append_directive(tail, directive);

	if (last) {
		*literal = NULL;
	} else {
		*literal = new_directive();
		if (!*literal) {
			return -1;
		}
	}

	return 0;
}

struct bfs_printf *parse_bfs_printf(const char *format, const struct cmdline *cmdline) {
	CFILE *cerr = cmdline->cerr;

	struct bfs_printf *command = malloc(sizeof(*command));
	if (!command) {
		perror("malloc()");
		return NULL;
	}

	command->directives = NULL;
	command->needs_stat = false;
	struct bfs_printf_directive **tail = &command->directives;

	struct bfs_printf_directive *literal = new_directive();
	if (!literal) {
		goto error;
	}

	for (const char *i = format; *i; ++i) {
		char c = *i;

		if (c == '\\') {
			c = *++i;

			if (c >= '0' && c < '8') {
				c = 0;
				for (int j = 0; j < 3 && *i >= '0' && *i < '8'; ++i, ++j) {
					c *= 8;
					c += *i - '0';
				}
				--i;
				goto one_char;
			}

			switch (c) {
			case 'a':  c = '\a'; break;
			case 'b':  c = '\b'; break;
			case 'f':  c = '\f'; break;
			case 'n':  c = '\n'; break;
			case 'r':  c = '\r'; break;
			case 't':  c = '\t'; break;
			case 'v':  c = '\v'; break;
			case '\\': c = '\\'; break;

			case 'c':
				if (append_literal(&tail, &literal, true) != 0) {
					goto error;
				}
				struct bfs_printf_directive *directive = new_directive();
				if (!directive) {
					goto error;
				}
				directive->fn = bfs_printf_flush;
				append_directive(&tail, directive);
				goto done;

			case '\0':
				cfprintf(cerr, "%{er}error: '%s': Incomplete escape sequence '\\'.%{rs}\n", format);
				goto error;

			default:
				cfprintf(cerr, "%{er}error: '%s': Unrecognized escape sequence '\\%c'.%{rs}\n", format, c);
				goto error;
			}
		} else if (c == '%') {
			if (i[1] == '%') {
				c = *++i;
				goto one_char;
			}

			struct bfs_printf_directive *directive = new_directive();
			if (!directive) {
				goto directive_error;
			}
			if (dstrncat(&directive->str, &c, 1) != 0) {
				perror("dstralloc()");
				goto directive_error;
			}

			const char *specifier = "s";

			// Parse any flags
			bool must_be_numeric = false;
			while (true) {
				c = *++i;

				switch (c) {
				case '#':
				case '0':
				case '+':
					must_be_numeric = true;
				case ' ':
				case '-':
					if (strchr(directive->str, c)) {
						cfprintf(cerr, "%{er}error: '%s': Duplicate flag '%c'.%{rs}\n", format, c);
						goto directive_error;
					}
					if (dstrncat(&directive->str, &c, 1) != 0) {
						perror("dstrncat()");
						goto directive_error;
					}
					continue;
				}

				break;
			}

			// Parse the field width
			while (c >= '0' && c <= '9') {
				if (dstrncat(&directive->str, &c, 1) != 0) {
					perror("dstrncat()");
					goto directive_error;
				}
				c = *++i;
			}

			// Parse the precision
			if (c == '.') {
				do {
					if (dstrncat(&directive->str, &c, 1) != 0) {
						perror("dstrncat()");
						goto directive_error;
					}
					c = *++i;
				} while (c >= '0' && c <= '9');
			}

			switch (c) {
			case 'a':
				directive->fn = bfs_printf_ctime;
				directive->time_field = ATIME;
				command->needs_stat = true;
				break;
			case 'b':
				directive->fn = bfs_printf_b;
				command->needs_stat = true;
				break;
			case 'c':
				directive->fn = bfs_printf_ctime;
				directive->time_field = CTIME;
				command->needs_stat = true;
				break;
			case 'd':
				directive->fn = bfs_printf_d;
				specifier = "jd";
				break;
			case 'D':
				directive->fn = bfs_printf_D;
				command->needs_stat = true;
				break;
			case 'f':
				directive->fn = bfs_printf_f;
				break;
			case 'F':
				if (!cmdline->mtab) {
					cfprintf(cerr, "%{er}error: '%s': Couldn't parse the mount table.%{rs}\n", format);
					goto directive_error;
				}
				directive->fn = bfs_printf_F;
				directive->mtab = cmdline->mtab;
				command->needs_stat = true;
				break;
			case 'g':
				directive->fn = bfs_printf_g;
				command->needs_stat = true;
				break;
			case 'G':
				directive->fn = bfs_printf_G;
				command->needs_stat = true;
				break;
			case 'h':
				directive->fn = bfs_printf_h;
				break;
			case 'H':
				directive->fn = bfs_printf_H;
				break;
			case 'i':
				directive->fn = bfs_printf_i;
				command->needs_stat = true;
				break;
			case 'k':
				directive->fn = bfs_printf_k;
				command->needs_stat = true;
				break;
			case 'l':
				directive->fn = bfs_printf_l;
				break;
			case 'm':
				directive->fn = bfs_printf_m;
				specifier = "o";
				command->needs_stat = true;
				break;
			case 'M':
				directive->fn = bfs_printf_M;
				command->needs_stat = true;
				break;
			case 'n':
				directive->fn = bfs_printf_n;
				command->needs_stat = true;
				break;
			case 'p':
				directive->fn = bfs_printf_p;
				break;
			case 'P':
				directive->fn = bfs_printf_P;
				break;
			case 's':
				directive->fn = bfs_printf_s;
				command->needs_stat = true;
				break;
			case 'S':
				directive->fn = bfs_printf_S;
				specifier = "g";
				command->needs_stat = true;
				break;
			case 't':
				directive->fn = bfs_printf_ctime;
				directive->time_field = MTIME;
				command->needs_stat = true;
				break;
			case 'u':
				directive->fn = bfs_printf_u;
				command->needs_stat = true;
				break;
			case 'U':
				directive->fn = bfs_printf_U;
				command->needs_stat = true;
				break;
			case 'y':
				directive->fn = bfs_printf_y;
				break;
			case 'Y':
				directive->fn = bfs_printf_Y;
				break;

			case 'A':
				directive->time_field = ATIME;
				goto directive_strftime;
			case 'C':
				directive->time_field = CTIME;
				goto directive_strftime;
			case 'T':
				directive->time_field = MTIME;
				goto directive_strftime;

			directive_strftime:
				directive->fn = bfs_printf_strftime;
				command->needs_stat = true;
				c = *++i;
				switch (c) {
				case '@':
				case 'H':
				case 'I':
				case 'k':
				case 'l':
				case 'M':
				case 'p':
				case 'r':
				case 'S':
				case 'T':
				case '+':
				case 'X':
				case 'Z':
				case 'a':
				case 'A':
				case 'b':
				case 'B':
				case 'c':
				case 'd':
				case 'D':
				case 'h':
				case 'j':
				case 'm':
				case 'U':
				case 'w':
				case 'W':
				case 'x':
				case 'y':
				case 'Y':
					directive->c = c;
					break;

				case '\0':
					cfprintf(cerr, "%{er}error: '%s': Incomplete time specifier '%s%c'.%{rs}\n",
					         format, directive->str, i[-1]);
					goto directive_error;

				default:
					cfprintf(cerr, "%{er}error: '%s': Unrecognized time specifier '%%%c%c'.%{rs}\n",
					         format, i[-1], c);
					goto directive_error;
				}
				break;

			case '\0':
				cfprintf(cerr, "%{er}error: '%s': Incomplete format specifier '%s'.%{rs}\n",
				         format, directive->str);
				goto directive_error;

			default:
				cfprintf(cerr, "%{er}error: '%s': Unrecognized format specifier '%%%c'.%{rs}\n",
				         format, c);
				goto directive_error;
			}

			if (must_be_numeric && strcmp(specifier, "s") == 0) {
				cfprintf(cerr, "%{er}error: '%s': Invalid flags '%s' for string format '%%%c'.%{rs}\n",
				         format, directive->str + 1, c);
				goto directive_error;
			}

			if (dstrcat(&directive->str, specifier) != 0) {
				perror("dstrcat()");
				goto directive_error;
			}

			if (append_literal(&tail, &literal, false) != 0) {
				goto directive_error;
			}
			append_directive(&tail, directive);
			continue;

		directive_error:
			free_directive(directive);
			goto error;
		}

	one_char:
		if (dstrncat(&literal->str, &c, 1) != 0) {
			perror("dstrncat()");
			goto error;
		}
	}

done:
	if (append_literal(&tail, &literal, true) != 0) {
		goto error;
	}

	free_directive(literal);
	return command;

error:
	free_directive(literal);
	free_bfs_printf(command);
	return NULL;
}

int bfs_printf(FILE *file, const struct bfs_printf *command, const struct BFTW *ftwbuf) {
	int ret = -1;

	for (struct bfs_printf_directive *directive = command->directives; directive; directive = directive->next) {
		if (directive->fn(file, directive, ftwbuf) < 0) {
			goto done;
		}
	}

	ret = 0;
done:
	return ret;
}

void free_bfs_printf(struct bfs_printf *command) {
	if (command) {
		struct bfs_printf_directive *directive = command->directives;
		while (directive) {
			struct bfs_printf_directive *next = directive->next;
			free_directive(directive);
			directive = next;
		}

		free(command);
	}
}
