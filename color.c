/*********************************************************************
 * bfs                                                               *
 * Copyright (C) 2015 Tavian Barnes <tavianator@tavianator.com>      *
 *                                                                   *
 * This program is free software. It comes without any warranty, to  *
 * the extent permitted by applicable law. You can redistribute it   *
 * and/or modify it under the terms of the Do What The Fuck You Want *
 * To Public License, Version 2, as published by Sam Hocevar. See    *
 * the COPYING file or http://www.wtfpl.net/ for more details.       *
 *********************************************************************/

#include "color.h"
#include "bftw.h"
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

struct ext_color {
	const char *ext;
	size_t len;

	const char *color;

	struct ext_color *next;
};

struct colors {
	const char *reset;
	const char *normal;
	const char *file;
	const char *dir;
	const char *link;
	const char *multi_hard;
	const char *pipe;
	const char *door;
	const char *block;
	const char *chardev;
	const char *orphan;
	const char *missing;
	const char *socket;
	const char *setuid;
	const char *setgid;
	const char *capable;
	const char *sticky_ow;
	const char *ow;
	const char *sticky;
	const char *exec;

	const char *warning;
	const char *error;

	struct ext_color *ext_list;

	char *data;
};

struct colors *parse_colors(const char *ls_colors) {
	struct colors *colors = malloc(sizeof(struct colors));
	if (!colors) {
		goto done;
	}

	// Defaults generated by dircolors --print-database
	colors->reset      = "0";
	colors->normal     = NULL;
	colors->file       = NULL;
	colors->dir        = "01;34";
	colors->link       = "01;36";
	colors->multi_hard = NULL;
	colors->pipe       = "40;33";
	colors->socket     = "01;35";
	colors->door       = "01;35";
	colors->block      = "40;33;01";
	colors->chardev    = "40;33;01";
	colors->orphan     = "40;31;01";
	colors->setuid     = "37;41";
	colors->setgid     = "30;43";
	colors->capable    = "30;41";
	colors->sticky_ow  = "30;42";
	colors->ow         = "34;42";
	colors->sticky     = "37;44";
	colors->exec       = "01;32";
	colors->warning    = "40;33;01";
	colors->error      = "40;31;01";
	colors->ext_list   = NULL;
	colors->data       = NULL;

	if (ls_colors) {
		colors->data = strdup(ls_colors);
	}

	if (!colors->data) {
		goto done;
	}

	char *start = colors->data;
	char *end;
	struct ext_color *ext;
	for (end = strchr(start, ':'); *start && end; start = end + 1, end = strchr(start, ':')) {
		char *equals = strchr(start, '=');
		if (!equals) {
			continue;
		}

		*equals = '\0';
		*end = '\0';

		const char *key = start;
		const char *value = equals + 1;

		// Ignore all-zero values
		if (strspn(value, "0") == strlen(value)) {
			continue;
		}

		switch (key[0]) {
		case 'b':
			if (strcmp(key, "bd") == 0) {
				colors->block = value;
			}
			break;

		case 'c':
			if (strcmp(key, "ca") == 0) {
				colors->capable = value;
			} else if (strcmp(key, "cd") == 0) {
				colors->chardev = value;
			}
			break;

		case 'd':
			if (strcmp(key, "di") == 0) {
				colors->dir = value;
			} else if (strcmp(key, "do") == 0) {
				colors->door = value;
			}
			break;

		case 'e':
			if (strcmp(key, "ex") == 0) {
				colors->exec = value;
			}
			break;

		case 'f':
			if (strcmp(key, "fi") == 0) {
				colors->file = value;
			}
			break;

		case 'l':
			if (strcmp(key, "ln") == 0) {
				colors->link = value;
			}
			break;

		case 'm':
			if (strcmp(key, "mh") == 0) {
				colors->multi_hard = value;
			} else if (strcmp(key, "mi") == 0) {
				colors->missing = value;
			}
			break;

		case 'n':
			if (strcmp(key, "no") == 0) {
				colors->normal = value;
			}
			break;

		case 'o':
			if (strcmp(key, "or") == 0) {
				colors->orphan = value;
			} else if (strcmp(key, "ow") == 0) {
				colors->ow = value;
			}
			break;

		case 'p':
			if (strcmp(key, "pi") == 0) {
				colors->pipe = value;
			}
			break;

		case 'r':
			if (strcmp(key, "rs") == 0) {
				colors->reset = value;
			}
			break;

		case 's':
			if (strcmp(key, "sg") == 0) {
				colors->setgid = value;
			} else if (strcmp(key, "so") == 0) {
				colors->socket = value;
			} else if (strcmp(key, "st") == 0) {
				colors->sticky = value;
			} else if (strcmp(key, "su") == 0) {
				colors->setuid = value;
			}
			break;

		case 't':
			if (strcmp(key, "tw") == 0) {
				colors->sticky_ow = value;
			}
			break;

		case '*':
			ext = malloc(sizeof(struct ext_color));
			if (ext) {
				ext->ext = key + 1;
				ext->len = strlen(ext->ext);
				ext->color = value;
				ext->next = colors->ext_list;
				colors->ext_list = ext;
			}
		}
	}

done:
	return colors;
}

static const char *file_color(const struct colors *colors, const char *filename, const struct BFTW *ftwbuf) {
	const struct stat *sb = ftwbuf->statbuf;
	if (!sb) {
		return colors->orphan;
	}

	const char *color = NULL;

	switch (sb->st_mode & S_IFMT) {
	case S_IFREG:
		if (sb->st_mode & S_ISUID) {
			color = colors->setuid;
		} else if (sb->st_mode & S_ISGID) {
			color = colors->setgid;
		} else if (sb->st_mode & 0111) {
			color = colors->exec;
		}

		if (!color && sb->st_nlink > 1) {
			color = colors->multi_hard;
		}

		if (!color) {
			size_t namelen = strlen(filename);

			for (struct ext_color *ext = colors->ext_list; ext; ext = ext->next) {
				if (namelen >= ext->len && memcmp(filename + namelen - ext->len, ext->ext, ext->len) == 0) {
					color = ext->color;
					break;
				}
			}
		}

		if (!color) {
			color = colors->file;
		}

		break;

	case S_IFDIR:
		if (sb->st_mode & S_ISVTX) {
			if (sb->st_mode & S_IWOTH) {
				color = colors->sticky_ow;
			} else {
				color = colors->sticky;
			}
		} else if (sb->st_mode & S_IWOTH) {
			color = colors->ow;
		}

		if (!color) {
			color = colors->dir;
		}

		break;

	case S_IFLNK:
		if (faccessat(ftwbuf->at_fd, ftwbuf->at_path, F_OK, 0) == 0) {
			color = colors->link;
		} else {
			color = colors->orphan;
		}
		break;

	case S_IFBLK:
		color = colors->block;
		break;
	case S_IFCHR:
		color = colors->chardev;
		break;
	case S_IFIFO:
		color = colors->pipe;
		break;
	case S_IFSOCK:
		color = colors->socket;
		break;

#ifdef S_IFDOOR
	case S_IFDOOR:
		color = colors->door;
		break;
#endif
	}

	if (!color) {
		color = colors->normal;
	}

	return color;
}

static void print_esc(const char *esc, FILE *file) {
	fputs("\033[", file);
	fputs(esc, file);
	fputs("m", file);
}

void pretty_print(const struct colors *colors, const struct BFTW *ftwbuf) {
	const char *path = ftwbuf->path;

	if (!colors) {
		puts(path);
		return;
	}

	const char *filename = path + ftwbuf->nameoff;

	if (colors->dir) {
		print_esc(colors->dir, stdout);
	}
	fwrite(path, 1, ftwbuf->nameoff, stdout);
	if (colors->dir) {
		print_esc(colors->reset, stdout);
	}

	const char *color = file_color(colors, filename, ftwbuf);
	if (color) {
		print_esc(color, stdout);
	}
	fputs(filename, stdout);
	if (color) {
		print_esc(colors->reset, stdout);
	}
	fputs("\n", stdout);
}

static void pretty_format(const struct colors *colors, const char *color, const char *format, va_list args) {
	if (color) {
		print_esc(color, stderr);
	}

	vfprintf(stderr, format, args);

	if (color) {
		print_esc(colors->reset, stderr);
	}
}

void pretty_warning(const struct colors *colors, const char *format, ...) {
	va_list args;
	va_start(args, format);

	pretty_format(colors, colors ? colors->warning : NULL, format, args);

	va_end(args);
}

void pretty_error(const struct colors *colors, const char *format, ...) {
	va_list args;
	va_start(args, format);

	pretty_format(colors, colors ? colors->error : NULL, format, args);

	va_end(args);
}

void free_colors(struct colors *colors) {
	if (colors) {
		struct ext_color *ext = colors->ext_list;
		while (ext) {
			struct ext_color *saved = ext;
			ext = ext->next;
			free(saved);
		}

		free(colors->data);
		free(colors);
	}
}
