/*********************************************************************
 * bfs                                                               *
 * Copyright (C) 2015-2017 Tavian Barnes <tavianator@tavianator.com> *
 *                                                                   *
 * This program is free software. It comes without any warranty, to  *
 * the extent permitted by applicable law. You can redistribute it   *
 * and/or modify it under the terms of the Do What The Fuck You Want *
 * To Public License, Version 2, as published by Sam Hocevar. See    *
 * the COPYING file or http://www.wtfpl.net/ for more details.       *
 *********************************************************************/

#include "bfs.h"
#include "exec.h"
#include "printf.h"
#include "typo.h"
#include "util.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <regex.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Strings printed by -D tree for "fake" expressions
static char *fake_and_arg = "-a";
static char *fake_false_arg = "-false";
static char *fake_or_arg = "-o";
static char *fake_print_arg = "-print";
static char *fake_true_arg = "-true";

/**
 * Singleton true expression instance.
 */
static struct expr expr_true = {
	.eval = eval_true,
	.lhs = NULL,
	.rhs = NULL,
	.pure = true,
	.always_true = true,
	.argc = 1,
	.argv = &fake_true_arg,
};

/**
 * Singleton false expression instance.
 */
static struct expr expr_false = {
	.eval = eval_false,
	.lhs = NULL,
	.rhs = NULL,
	.pure = true,
	.always_false = true,
	.argc = 1,
	.argv = &fake_false_arg,
};

/**
 * Free an expression.
 */
static void free_expr(struct expr *expr) {
	if (expr && expr != &expr_true && expr != &expr_false) {
		if (expr->cfile && expr->cfile->close) {
			if (cfclose(expr->cfile) != 0) {
				perror("cfclose()");
			}
		}

		if (expr->regex) {
			regfree(expr->regex);
			free(expr->regex);
		}

		free_bfs_printf(expr->printf);
		free_bfs_exec(expr->execbuf);

		free_expr(expr->lhs);
		free_expr(expr->rhs);
		free(expr);
	}
}

/**
 * Create a new expression.
 */
static struct expr *new_expr(eval_fn *eval, bool pure, size_t argc, char **argv) {
	struct expr *expr = malloc(sizeof(struct expr));
	if (!expr) {
		perror("malloc()");
		return NULL;
	}

	expr->eval = eval;
	expr->lhs = NULL;
	expr->rhs = NULL;
	expr->pure = pure;
	expr->always_true = false;
	expr->always_false = false;
	expr->evaluations = 0;
	expr->successes = 0;
	expr->elapsed.tv_sec = 0;
	expr->elapsed.tv_nsec = 0;
	expr->argc = argc;
	expr->argv = argv;
	expr->cfile = NULL;
	expr->regex = NULL;
	expr->execbuf = NULL;
	expr->printf = NULL;
	return expr;
}

/**
 * Create a new unary expression.
 */
static struct expr *new_unary_expr(eval_fn *eval, struct expr *rhs, char **argv) {
	struct expr *expr = new_expr(eval, rhs->pure, 1, argv);
	if (!expr) {
		free_expr(rhs);
		return NULL;
	}

	expr->rhs = rhs;
	return expr;
}

/**
 * Create a new binary expression.
 */
static struct expr *new_binary_expr(eval_fn *eval, struct expr *lhs, struct expr *rhs, char **argv) {
	struct expr *expr = new_expr(eval, lhs->pure && rhs->pure, 1, argv);
	if (!expr) {
		free_expr(rhs);
		free_expr(lhs);
		return NULL;
	}

	expr->lhs = lhs;
	expr->rhs = rhs;
	return expr;
}

/**
 * Dump the parsed expression tree, for debugging.
 */
static void dump_expr(const struct expr *expr, bool verbose) {
	fputs("(", stderr);

	for (size_t i = 0; i < expr->argc; ++i) {
		if (i > 0) {
			fputs(" ", stderr);
		}
		fputs(expr->argv[i], stderr);
	}

	if (verbose) {
		double rate = 0.0, time = 0.0;
		if (expr->evaluations) {
			rate = 100.0*expr->successes/expr->evaluations;
			time = (1.0e9*expr->elapsed.tv_sec + expr->elapsed.tv_nsec)/expr->evaluations;
		}
		fprintf(stderr, " [%zu/%zu=%g%%; %gns]", expr->successes, expr->evaluations, rate, time);
	}

	if (expr->lhs) {
		fputs(" ", stderr);
		dump_expr(expr->lhs, verbose);
	}

	if (expr->rhs) {
		fputs(" ", stderr);
		dump_expr(expr->rhs, verbose);
	}

	fputs(")", stderr);
}

/**
 * Free the parsed command line.
 */
void free_cmdline(struct cmdline *cmdline) {
	if (cmdline) {
		free_expr(cmdline->expr);

		free_bfs_mtab(cmdline->mtab);

		cfclose(cmdline->cerr);
		cfclose(cmdline->cout);
		free_colors(cmdline->colors);

		struct root *root = cmdline->roots;
		while (root) {
			struct root *next = root->next;
			free(root);
			root = next;
		}

		free(cmdline);
	}
}

/**
 * Color use flags.
 */
enum use_color {
	COLOR_NEVER,
	COLOR_AUTO,
	COLOR_ALWAYS,
};

/**
 * Ephemeral state for parsing the command line.
 */
struct parser_state {
	/** The command line being constructed. */
	struct cmdline *cmdline;
	/** The command line arguments being parsed. */
	char **argv;
	/** The name of this program. */
	const char *command;
	/** The current tail of the root path list. */
	struct root **roots_tail;

	/** The current regex flags to use. */
	int regex_flags;

	/** Whether -color or -nocolor has been passed. */
	enum use_color use_color;
	/** Whether a -print action is implied. */
	bool implicit_print;
	/** Whether warnings are enabled (see -warn, -nowarn). */
	bool warn;
	/** Whether the expression has started. */
	bool expr_started;
	/** Whether any non-option arguments have been encountered. */
	bool non_option_seen;
	/** Whether an information option like -help or -version was passed. */
	bool just_info;

	/** The current time. */
	struct timespec now;
};

/**
 * Possible token types.
 */
enum token_type {
	/** A flag. */
	T_FLAG,
	/** A root path. */
	T_PATH,
	/** An option. */
	T_OPTION,
	/** A test. */
	T_TEST,
	/** An action. */
	T_ACTION,
	/** An operator. */
	T_OPERATOR,
};

/**
 * Log an optimization.
 */
static void debug_opt(const struct parser_state *state, const char *format, ...) {
	if (!(state->cmdline->debug & DEBUG_OPT)) {
		return;
	}

	va_list args;
	va_start(args, format);

	for (const char *i = format; *i != '\0'; ++i) {
		if (*i == '%') {
			switch (*++i) {
			case '%':
				fputc('%', stderr);
				break;

			case 's':
				fputs(va_arg(args, const char *), stderr);
				break;

			case 'e':
				dump_expr(va_arg(args, const struct expr *), false);
				break;
			}
		} else {
			fputc(*i, stderr);
		}
	}

	va_end(args);
}

/**
 * Invoke stat() on an argument.
 */
static int stat_arg(const struct parser_state *state, struct expr *expr, struct stat *sb) {
	const struct cmdline *cmdline = state->cmdline;

	bool follow = cmdline->flags & (BFTW_COMFOLLOW | BFTW_LOGICAL);
	int flags = follow ? 0 : AT_SYMLINK_NOFOLLOW;

	int ret = fstatat(AT_FDCWD, expr->sdata, sb, flags);
	if (ret != 0) {
		cfprintf(cmdline->cerr, "%{er}error: '%s': %s%{rs}\n", expr->sdata, strerror(errno));
	}
	return ret;
}

/**
 * Parse the expression specified on the command line.
 */
static struct expr *parse_expr(struct parser_state *state);

/**
 * Advance by a single token.
 */
static char **parser_advance(struct parser_state *state, enum token_type type, size_t argc) {
	if (type != T_FLAG && type != T_PATH) {
		state->expr_started = true;

		if (type != T_OPTION) {
			state->non_option_seen = true;
		}
	}

	char **argv = state->argv;
	state->argv += argc;
	return argv;
}

/**
 * Parse a root path.
 */
static int parse_root(struct parser_state *state, const char *path) {
	struct root *root = malloc(sizeof(struct root));
	if (!root) {
		perror("malloc()");
		return -1;
	}

	root->path = path;
	root->next = NULL;
	*state->roots_tail = root;
	state->roots_tail = &root->next;
	return 0;
}

/**
 * While parsing an expression, skip any paths and add them to the cmdline.
 */
static int skip_paths(struct parser_state *state) {
	while (true) {
		const char *arg = state->argv[0];
		if (!arg) {
			return 0;
		}

		if (arg[0] == '-') {
			if (strcmp(arg, "--") == 0) {
				// find uses -- to separate flags from the rest
				// of the command line.  We allow mixing flags
				// and paths/predicates, so we just ignore --.
				parser_advance(state, T_FLAG, 1);
				continue;
			}
			if (strcmp(arg, "-") != 0) {
				// - by itself is a file name.  Anything else
				// starting with - is a flag/predicate.
				return 0;
			}
		}

		// By POSIX, these are always options
		if (strcmp(arg, "(") == 0 || strcmp(arg, "!") == 0) {
			return 0;
		}

		if (state->expr_started) {
			// By POSIX, these can be paths.  We only treat them as
			// such at the beginning of the command line.
			if (strcmp(arg, ")") == 0 || strcmp(arg, ",") == 0) {
				return 0;
			}
		}

		if (parse_root(state, arg) != 0) {
			return -1;
		}

		parser_advance(state, T_PATH, 1);
	}
}

/** Integer parsing flags. */
enum int_flags {
	IF_BASE_MASK   = 0x03F,
	IF_INT         = 0x040,
	IF_LONG        = 0x080,
	IF_LONG_LONG   = 0x0C0,
	IF_SIZE_MASK   = 0x0C0,
	IF_UNSIGNED    = 0x100,
	IF_PARTIAL_OK  = 0x200,
	IF_QUIET       = 0x400,
};

/**
 * Parse an integer.
 */
static const char *parse_int(const struct parser_state *state, const char *str, void *result, enum int_flags flags) {
	char *endptr;

	int base = flags & IF_BASE_MASK;
	if (base == 0) {
		base = 10;
	}

	errno = 0;
	long long value = strtoll(str, &endptr, base);
	if (errno != 0) {
		goto bad;
	}

	if (endptr == str) {
		goto bad;
	}

	if (!(flags & IF_PARTIAL_OK) && *endptr != '\0') {
		goto bad;
	}

	if ((flags & IF_UNSIGNED) && value < 0) {
		goto bad;
	}

	switch (flags & IF_SIZE_MASK) {
	case IF_INT:
		if (value < INT_MIN || value > INT_MAX) {
			goto bad;
		}
		*(int *)result = value;
		break;

	case IF_LONG:
		if (value < LONG_MIN || value > LONG_MAX) {
			goto bad;
		}
		*(long *)result = value;
		break;

	case IF_LONG_LONG:
		*(long long *)result = value;
		break;
	}

	return endptr;

bad:
	if (!(flags & IF_QUIET)) {
		cfprintf(state->cmdline->cerr, "%{er}error: '%s' is not a valid integer.%{rs}\n", str);
	}
	return NULL;
}

/**
 * Parse an integer and a comparison flag.
 */
static const char *parse_icmp(const struct parser_state *state, const char *str, struct expr *expr, enum int_flags flags) {
	switch (str[0]) {
	case '-':
		expr->cmp_flag = CMP_LESS;
		++str;
		break;
	case '+':
		expr->cmp_flag = CMP_GREATER;
		++str;
		break;
	default:
		expr->cmp_flag = CMP_EXACT;
		break;
	}

	return parse_int(state, str, &expr->idata, flags | IF_LONG_LONG | IF_UNSIGNED);
}

/**
 * Check if a string could be an integer comparison.
 */
static bool looks_like_icmp(const char *str) {
	if (str[0] == '-' || str[0] == '+') {
		++str;
	}
	return str[0] >= '0' && str[0] <= '9';
}

/**
 * Parse a single flag.
 */
static struct expr *parse_flag(struct parser_state *state, size_t argc) {
	parser_advance(state, T_FLAG, argc);
	return &expr_true;
}

/**
 * Parse a flag that doesn't take a value.
 */
static struct expr *parse_nullary_flag(struct parser_state *state) {
	return parse_flag(state, 1);
}

/**
 * Parse a flag that takes a single value.
 */
static struct expr *parse_unary_flag(struct parser_state *state) {
	return parse_flag(state, 2);
}

/**
 * Parse a single option.
 */
static struct expr *parse_option(struct parser_state *state, size_t argc) {
	const char *arg = *parser_advance(state, T_OPTION, argc);

	if (state->warn && state->non_option_seen) {
		cfprintf(state->cmdline->cerr,
		         "%{wr}warning: The '%s' option applies to the entire command line.  For clarity, place\n"
		         "it before any non-option arguments.%{rs}\n\n",
		         arg);
	}


	return &expr_true;
}

/**
 * Parse an option that doesn't take a value.
 */
static struct expr *parse_nullary_option(struct parser_state *state) {
	return parse_option(state, 1);
}

/**
 * Parse an option that takes a value.
 */
static struct expr *parse_unary_option(struct parser_state *state) {
	return parse_option(state, 2);
}

/**
 * Parse a single positional option.
 */
static struct expr *parse_positional_option(struct parser_state *state, size_t argc) {
	parser_advance(state, T_OPTION, argc);
	return &expr_true;
}

/**
 * Parse a positional option that doesn't take a value.
 */
static struct expr *parse_nullary_positional_option(struct parser_state *state) {
	return parse_positional_option(state, 1);
}

/**
 * Parse a positional option that takes a single value.
 */
static struct expr *parse_unary_positional_option(struct parser_state *state, const char **value) {
	const char *arg = state->argv[0];
	*value = state->argv[1];
	if (!*value) {
		cfprintf(state->cmdline->cerr, "%{er}error: %s needs a value.%{rs}\n", arg);
		return NULL;
	}

	return parse_positional_option(state, 2);
}

/**
 * Parse a single test.
 */
static struct expr *parse_test(struct parser_state *state, eval_fn *eval, size_t argc) {
	char **argv = parser_advance(state, T_TEST, argc);
	return new_expr(eval, true, argc, argv);
}

/**
 * Parse a test that doesn't take a value.
 */
static struct expr *parse_nullary_test(struct parser_state *state, eval_fn *eval) {
	return parse_test(state, eval, 1);
}

/**
 * Parse a test that takes a value.
 */
static struct expr *parse_unary_test(struct parser_state *state, eval_fn *eval) {
	const char *arg = state->argv[0];
	const char *value = state->argv[1];
	if (!value) {
		cfprintf(state->cmdline->cerr, "%{er}error: %s needs a value.%{rs}\n", arg);
		return NULL;
	}

	struct expr *expr = parse_test(state, eval, 2);
	if (expr) {
		expr->sdata = value;
	}
	return expr;
}

/**
 * Parse a single action.
 */
static struct expr *parse_action(struct parser_state *state, eval_fn *eval, size_t argc) {
	if (eval != eval_nohidden && eval != eval_prune) {
		state->implicit_print = false;
	}

	char **argv = parser_advance(state, T_ACTION, argc);
	return new_expr(eval, false, argc, argv);
}

/**
 * Parse an action that takes no arguments.
 */
static struct expr *parse_nullary_action(struct parser_state *state, eval_fn *eval) {
	return parse_action(state, eval, 1);
}

/**
 * Parse an action that takes one argument.
 */
static struct expr *parse_unary_action(struct parser_state *state, eval_fn *eval) {
	const char *arg = state->argv[0];
	const char *value = state->argv[1];
	if (!value) {
		cfprintf(state->cmdline->cerr, "%{er}error: %s needs a value.%{rs}\n", arg);
		return NULL;
	}

	struct expr *expr = parse_action(state, eval, 2);
	if (expr) {
		expr->sdata = value;
	}
	return expr;
}

/**
 * Parse a test expression with integer data and a comparison flag.
 */
static struct expr *parse_test_icmp(struct parser_state *state, eval_fn *eval) {
	struct expr *expr = parse_unary_test(state, eval);
	if (!expr) {
		return NULL;
	}

	if (!parse_icmp(state, expr->sdata, expr, 0)) {
		free_expr(expr);
		return NULL;
	}

	return expr;
}

/**
 * Parse -D FLAG.
 */
static struct expr *parse_debug(struct parser_state *state, int arg1, int arg2) {
	struct cmdline *cmdline = state->cmdline;

	const char *arg = state->argv[0];
	const char *flag = state->argv[1];
	if (!flag) {
		cfprintf(cmdline->cerr, "%{er}error: %s needs a flag.%{rs}\n", arg);
		return NULL;
	}

	if (strcmp(flag, "help") == 0) {
		printf("Supported debug flags:\n\n");

		printf("  help:   This message.\n");
		printf("  opt:    Print optimization details.\n");
		printf("  rates:  Print predicate success rates.\n");
		printf("  stat:   Trace all stat() calls.\n");
		printf("  tree:   Print the parse tree.\n");

		state->just_info = true;
		return NULL;
	} else if (strcmp(flag, "opt") == 0) {
		cmdline->debug |= DEBUG_OPT;
	} else if (strcmp(flag, "rates") == 0) {
		cmdline->debug |= DEBUG_RATES;
	} else if (strcmp(flag, "stat") == 0) {
		cmdline->debug |= DEBUG_STAT;
	} else if (strcmp(flag, "tree") == 0) {
		cmdline->debug |= DEBUG_TREE;
	} else {
		cfprintf(cmdline->cerr, "%{wr}warning: Unrecognized debug flag '%s'.%{rs}\n\n", flag);
	}

	return parse_unary_flag(state);
}

/**
 * Parse -On.
 */
static struct expr *parse_optlevel(struct parser_state *state, int arg1, int arg2) {
	int *optlevel = &state->cmdline->optlevel;

	if (strcmp(state->argv[0], "-Ofast") == 0) {
		*optlevel = 4;
	} else if (!parse_int(state, state->argv[0] + 2, optlevel, IF_INT)) {
		return NULL;
	}

	if (*optlevel > 4) {
		cfprintf(state->cmdline->cerr, "%{wr}warning: %s is the same as -O4.%{rs}\n\n", state->argv[0]);
	}

	return parse_nullary_flag(state);
}

/**
 * Parse -[PHL], -(no)?follow.
 */
static struct expr *parse_follow(struct parser_state *state, int flags, int option) {
	struct cmdline *cmdline = state->cmdline;
	cmdline->flags &= ~(BFTW_COMFOLLOW | BFTW_LOGICAL | BFTW_DETECT_CYCLES);
	cmdline->flags |= flags;
	if (option) {
		return parse_nullary_positional_option(state);
	} else {
		return parse_nullary_flag(state);
	}
}

/**
 * Parse -X.
 */
static struct expr *parse_xargs_safe(struct parser_state *state, int arg1, int arg2) {
	state->cmdline->xargs_safe = true;
	return parse_nullary_flag(state);
}

/**
 * Parse -executable, -readable, -writable
 */
static struct expr *parse_access(struct parser_state *state, int flag, int arg2) {
	struct expr *expr = parse_nullary_test(state, eval_access);
	if (expr) {
		expr->idata = flag;
	}
	return expr;
}

/**
 * Parse -[acm]{min,time}.
 */
static struct expr *parse_acmtime(struct parser_state *state, int field, int unit) {
	struct expr *expr = parse_test_icmp(state, eval_acmtime);
	if (expr) {
		expr->reftime = state->now;
		expr->time_field = field;
		expr->time_unit = unit;
	}
	return expr;
}

/**
 * Parse -[ac]?newer.
 */
static struct expr *parse_acnewer(struct parser_state *state, int field, int arg2) {
	struct expr *expr = parse_unary_test(state, eval_acnewer);
	if (!expr) {
		return NULL;
	}

	struct stat sb;
	if (stat_arg(state, expr, &sb) != 0) {
		free_expr(expr);
		return NULL;
	}

	expr->reftime = sb.st_mtim;
	expr->time_field = field;

	return expr;
}

/**
 * Parse -(no)?color.
 */
static struct expr *parse_color(struct parser_state *state, int color, int arg2) {
	struct cmdline *cmdline = state->cmdline;
	struct colors *colors = cmdline->colors;
	if (color) {
		state->use_color = COLOR_ALWAYS;
		cmdline->cout->colors = colors;
		cmdline->cerr->colors = colors;
	} else {
		state->use_color = COLOR_NEVER;
		cmdline->cout->colors = NULL;
		cmdline->cerr->colors = NULL;
	}
	return parse_nullary_option(state);
}

/**
 * Parse -{false,true}.
 */
static struct expr *parse_const(struct parser_state *state, int value, int arg2) {
	parser_advance(state, T_TEST, 1);
	return value ? &expr_true : &expr_false;
}

/**
 * Parse -daystart.
 */
static struct expr *parse_daystart(struct parser_state *state, int arg1, int arg2) {
	struct tm tm;
	if (xlocaltime(&state->now.tv_sec, &tm) != 0) {
		perror("xlocaltime()");
		return NULL;
	}

	if (tm.tm_hour || tm.tm_min || tm.tm_sec || state->now.tv_nsec) {
		++tm.tm_mday;
	}
	tm.tm_hour = 0;
	tm.tm_min = 0;
	tm.tm_sec = 0;

	time_t time = mktime(&tm);
	if (time == -1) {
		perror("mktime()");
		return NULL;
	}

	state->now.tv_sec = time;
	state->now.tv_nsec = 0;

	return parse_nullary_positional_option(state);
}

/**
 * Parse -delete.
 */
static struct expr *parse_delete(struct parser_state *state, int arg1, int arg2) {
	state->cmdline->flags |= BFTW_DEPTH;
	return parse_nullary_action(state, eval_delete);
}

/**
 * Parse -d, -depth.
 */
static struct expr *parse_depth(struct parser_state *state, int arg1, int arg2) {
	state->cmdline->flags |= BFTW_DEPTH;
	return parse_nullary_option(state);
}

/**
 * Parse -depth [N].
 */
static struct expr *parse_depth_n(struct parser_state *state, int arg1, int arg2) {
	const char *arg = state->argv[1];
	if (arg && looks_like_icmp(arg)) {
		return parse_test_icmp(state, eval_depth);
	} else {
		return parse_depth(state, arg1, arg2);
	}
}

/**
 * Parse -{min,max}depth N.
 */
static struct expr *parse_depth_limit(struct parser_state *state, int is_min, int arg2) {
	struct cmdline *cmdline = state->cmdline;
	const char *arg = state->argv[0];
	const char *value = state->argv[1];
	if (!value) {
		cfprintf(cmdline->cerr, "%{er}error: %s needs a value.%{rs}\n", arg);
		return NULL;
	}

	int *depth = is_min ? &cmdline->mindepth : &cmdline->maxdepth;
	if (!parse_int(state, value, depth, IF_INT | IF_UNSIGNED)) {
		return NULL;
	}

	return parse_unary_option(state);
}

/**
 * Parse -empty.
 */
static struct expr *parse_empty(struct parser_state *state, int arg1, int arg2) {
	return parse_nullary_test(state, eval_empty);
}

/**
 * Parse -exec(dir)?/-ok(dir)?.
 */
static struct expr *parse_exec(struct parser_state *state, int flags, int arg2) {
	struct bfs_exec *execbuf = parse_bfs_exec(state->argv, flags, state->cmdline->cerr);
	if (!execbuf) {
		return NULL;
	}

	if ((execbuf->flags & BFS_EXEC_CHDIR) && (execbuf->flags & BFS_EXEC_MULTI)) {
		++state->cmdline->nopen_files;
	}

	struct expr *expr = parse_action(state, eval_exec, execbuf->tmpl_argc + 2);
	if (expr) {
		expr->execbuf = execbuf;
	}
	return expr;
}

/**
 * Parse -f PATH.
 */
static struct expr *parse_f(struct parser_state *state, int arg1, int arg2) {
	parser_advance(state, T_FLAG, 1);

	const char *path = state->argv[0];
	if (!path) {
		cfprintf(state->cmdline->cerr, "%{er}error: -f requires a path.%{rs}\n");
		return NULL;
	}

	if (parse_root(state, path) != 0) {
		return NULL;
	}

	parser_advance(state, T_PATH, 1);
	return &expr_true;
}

/**
 * Open a file for an expression.
 */
static int expr_open(struct parser_state *state, struct expr *expr, const char *path) {
	expr->cfile = cfopen(path, state->use_color ? state->cmdline->colors : NULL);
	if (!expr->cfile) {
		cfprintf(state->cmdline->cerr, "%{er}error: '%s': %s%{rs}\n", path, strerror(errno));
		return -1;
	}

	++state->cmdline->nopen_files;
	return 0;
}

/**
 * Parse -fls FILE.
 */
static struct expr *parse_fls(struct parser_state *state, int arg1, int arg2) {
	struct expr *expr = parse_unary_action(state, eval_fls);
	if (expr) {
		if (expr_open(state, expr, expr->sdata) != 0) {
			goto fail;
		}
		expr->reftime = state->now;
	}
	return expr;

fail:
	free_expr(expr);
	return NULL;
}

/**
 * Parse -fprint FILE.
 */
static struct expr *parse_fprint(struct parser_state *state, int arg1, int arg2) {
	struct expr *expr = parse_unary_action(state, eval_fprint);
	if (expr) {
		if (expr_open(state, expr, expr->sdata) != 0) {
			goto fail;
		}
	}
	return expr;

fail:
	free_expr(expr);
	return NULL;
}

/**
 * Parse -fprint0 FILE.
 */
static struct expr *parse_fprint0(struct parser_state *state, int arg1, int arg2) {
	struct expr *expr = parse_unary_action(state, eval_fprint0);
	if (expr) {
		if (expr_open(state, expr, expr->sdata) != 0) {
			goto fail;
		}
	}
	return expr;

fail:
	free_expr(expr);
	return NULL;
}

/**
 * Parse -fprintf FILE FORMAT.
 */
static struct expr *parse_fprintf(struct parser_state *state, int arg1, int arg2) {
	const char *arg = state->argv[0];

	const char *file = state->argv[1];
	if (!file) {
		cfprintf(state->cmdline->cerr, "%{er}error: %s needs a file.%{rs}\n", arg);
		return NULL;
	}

	const char *format = state->argv[2];
	if (!format) {
		cfprintf(state->cmdline->cerr, "%{er}error: %s needs a format string.%{rs}\n", arg);
		return NULL;
	}

	struct expr *expr = parse_action(state, eval_fprintf, 3);
	if (!expr) {
		return NULL;
	}

	if (expr_open(state, expr, file) != 0) {
		goto fail;
	}

	expr->printf = parse_bfs_printf(format, state->cmdline);
	if (!expr->printf) {
		goto fail;
	}

	return expr;

fail:
	free_expr(expr);
	return NULL;
}

/**
 * Parse -fstype TYPE.
 */
static struct expr *parse_fstype(struct parser_state *state, int arg1, int arg2) {
	if (!state->cmdline->mtab) {
		cfprintf(state->cmdline->cerr, "%{er}error: %s: Couldn't parse the mount table.%{rs}\n", state->argv[0]);
		return NULL;
	}

	return parse_unary_test(state, eval_fstype);
}

/**
 * Parse -gid/-group.
 */
static struct expr *parse_group(struct parser_state *state, int arg1, int arg2) {
	const char *arg = state->argv[0];

	struct expr *expr = parse_unary_test(state, eval_gid);
	if (!expr) {
		return NULL;
	}

	struct group *grp = getgrnam(expr->sdata);
	if (grp) {
		expr->idata = grp->gr_gid;
		expr->cmp_flag = CMP_EXACT;
	} else if (looks_like_icmp(expr->sdata)) {
		if (!parse_icmp(state, expr->sdata, expr, 0)) {
			goto fail;
		}
	} else {
		cfprintf(state->cmdline->cerr, "%{er}error: %s %s: No such group.%{rs}\n", arg, expr->sdata);
		goto fail;
	}

	return expr;

fail:
	free_expr(expr);
	return NULL;
}

/**
 * Parse -used N.
 */
static struct expr *parse_used(struct parser_state *state, int arg1, int arg2) {
	return parse_test_icmp(state, eval_used);
}

/**
 * Parse -uid/-user.
 */
static struct expr *parse_user(struct parser_state *state, int arg1, int arg2) {
	const char *arg = state->argv[0];

	struct expr *expr = parse_unary_test(state, eval_uid);
	if (!expr) {
		return NULL;
	}

	struct passwd *pwd = getpwnam(expr->sdata);
	if (pwd) {
		expr->idata = pwd->pw_uid;
		expr->cmp_flag = CMP_EXACT;
	} else if (looks_like_icmp(expr->sdata)) {
		if (!parse_icmp(state, expr->sdata, expr, 0)) {
			goto fail;
		}
	} else {
		cfprintf(state->cmdline->cerr, "%{er}error: %s %s: No such user.%{rs}\n", arg, expr->sdata);
		goto fail;
	}

	return expr;

fail:
	free_expr(expr);
	return NULL;
}

/**
 * Parse -hidden.
 */
static struct expr *parse_hidden(struct parser_state *state, int arg1, int arg2) {
	return parse_nullary_test(state, eval_hidden);
}

/**
 * Parse -(no)?ignore_readdir_race.
 */
static struct expr *parse_ignore_races(struct parser_state *state, int ignore, int arg2) {
	state->cmdline->ignore_races = ignore;
	return parse_nullary_option(state);
}

/**
 * Parse -inum N.
 */
static struct expr *parse_inum(struct parser_state *state, int arg1, int arg2) {
	return parse_test_icmp(state, eval_inum);
}

/**
 * Parse -links N.
 */
static struct expr *parse_links(struct parser_state *state, int arg1, int arg2) {
	return parse_test_icmp(state, eval_links);
}

/**
 * Parse -ls.
 */
static struct expr *parse_ls(struct parser_state *state, int arg1, int arg2) {
	struct expr *expr = parse_nullary_action(state, eval_fls);
	if (expr) {
		expr->cfile = state->cmdline->cout;
		expr->reftime = state->now;
	}
	return expr;
}

/**
 * Parse -mount, -xdev.
 */
static struct expr *parse_mount(struct parser_state *state, int arg1, int arg2) {
	state->cmdline->flags |= BFTW_XDEV;
	return parse_nullary_option(state);
}

/**
 * Set the FNM_CASEFOLD flag, if supported.
 */
static struct expr *set_fnm_casefold(const struct parser_state *state, struct expr *expr, bool casefold) {
	if (expr) {
		if (casefold) {
#ifdef FNM_CASEFOLD
			expr->idata = FNM_CASEFOLD;
#else
			cfprintf(state->cmdline->cerr, "%{er}error: %s is missing platform support.%{rs}\n", expr->argv[0]);
			free_expr(expr);
			return NULL;
#endif
		} else {
			expr->idata = 0;
		}
	}
	return expr;
}

/**
 * Parse -i?name.
 */
static struct expr *parse_name(struct parser_state *state, int casefold, int arg2) {
	struct expr *expr = parse_unary_test(state, eval_name);
	return set_fnm_casefold(state, expr, casefold);
}

/**
 * Parse -i?path, -i?wholename.
 */
static struct expr *parse_path(struct parser_state *state, int casefold, int arg2) {
	struct expr *expr = parse_unary_test(state, eval_path);
	return set_fnm_casefold(state, expr, casefold);
}

/**
 * Parse -i?lname.
 */
static struct expr *parse_lname(struct parser_state *state, int casefold, int arg2) {
	struct expr *expr = parse_unary_test(state, eval_lname);
	return set_fnm_casefold(state, expr, casefold);
}

/**
 * Parse -newerXY.
 */
static struct expr *parse_newerxy(struct parser_state *state, int arg1, int arg2) {
	const char *arg = state->argv[0];
	if (strlen(arg) != 8) {
		cfprintf(state->cmdline->cerr, "%{er}error: Expected -newerXY; found %s.%{rs}\n", arg);
		return NULL;
	}

	struct expr *expr = parse_unary_test(state, eval_acnewer);
	if (!expr) {
		goto fail;
	}

	switch (arg[6]) {
	case 'a':
		expr->time_field = ATIME;
		break;
	case 'c':
		expr->time_field = CTIME;
		break;
	case 'm':
		expr->time_field = MTIME;
		break;

	case 'B':
		cfprintf(state->cmdline->cerr, "%{er}error: %s: File birth times ('B') are not supported.%{rs}\n", arg);
		goto fail;

	default:
		cfprintf(state->cmdline->cerr, "%{er}error: %s: For -newerXY, X should be 'a', 'c', 'm', or 'B'.%{rs}\n", arg);
		goto fail;
	}

	if (arg[7] == 't') {
		cfprintf(state->cmdline->cerr, "%{er}error: %s: Explicit reference times ('t') are not supported.%{rs}\n", arg);
		goto fail;
	} else {
		struct stat sb;
		if (stat_arg(state, expr, &sb) != 0) {
			goto fail;
		}

		switch (arg[7]) {
		case 'a':
			expr->reftime = sb.st_atim;
			break;
		case 'c':
			expr->reftime = sb.st_ctim;
			break;
		case 'm':
			expr->reftime = sb.st_mtim;
			break;

		case 'B':
			cfprintf(state->cmdline->cerr, "%{er}error: %s: File birth times ('B') are not supported.%{rs}\n", arg);
			goto fail;

		default:
			cfprintf(state->cmdline->cerr,
			         "%{er}error: %s: For -newerXY, Y should be 'a', 'c', 'm', 'B', or 't'.%{rs}\n", arg);
			goto fail;
		}
	}

	return expr;

fail:
	free_expr(expr);
	return NULL;
}

/**
 * Parse -nogroup.
 */
static struct expr *parse_nogroup(struct parser_state *state, int arg1, int arg2) {
	return parse_nullary_test(state, eval_nogroup);
}

/**
 * Parse -nohidden.
 */
static struct expr *parse_nohidden(struct parser_state *state, int arg1, int arg2) {
	return parse_nullary_action(state, eval_nohidden);
}

/**
 * Parse -noleaf.
 */
static struct expr *parse_noleaf(struct parser_state *state, int arg1, int arg2) {
	if (state->warn) {
		cfprintf(state->cmdline->cerr,
		         "%{wr}warning: bfs does not apply the optimization that %s inhibits.%{rs}\n\n",
		         state->argv[0]);
	}

	return parse_nullary_option(state);
}

/**
 * Parse -nouser.
 */
static struct expr *parse_nouser(struct parser_state *state, int arg1, int arg2) {
	return parse_nullary_test(state, eval_nouser);
}

/**
 * Parse a permission mode like chmod(1).
 */
static int parse_mode(const struct parser_state *state, const char *mode, struct expr *expr) {
	if (mode[0] >= '0' && mode[0] <= '9') {
		unsigned int parsed;
		if (!parse_int(state, mode, &parsed, 8 | IF_INT | IF_UNSIGNED | IF_QUIET)) {
			goto fail;
		}
		if (parsed > 07777) {
			goto fail;
		}

		expr->file_mode = parsed;
		expr->dir_mode = parsed;
		return 0;
	}

	expr->file_mode = 0;
	expr->dir_mode = 0;

	// Parse the same grammar as chmod(1), which looks like this:
	//
	// MODE : CLAUSE ["," CLAUSE]*
	//
	// CLAUSE : WHO* ACTION+
	//
	// WHO : "u" | "g" | "o" | "a"
	//
	// ACTION : OP PERM*
	//        | OP PERMCOPY
	//
	// OP : "+" | "-" | "="
	//
	// PERM : "r" | "w" | "x" | "X" | "s" | "t"
	//
	// PERMCOPY : "u" | "g" | "o"

	// State machine state
	enum {
		MODE_CLAUSE,
		MODE_WHO,
		MODE_ACTION,
		MODE_ACTION_APPLY,
		MODE_OP,
		MODE_PERM,
	} mstate = MODE_CLAUSE;

	enum {
		MODE_PLUS,
		MODE_MINUS,
		MODE_EQUALS,
	} op;

	mode_t who;
	mode_t file_change;
	mode_t dir_change;

	const char *i = mode;
	while (true) {
		switch (mstate) {
		case MODE_CLAUSE:
			who = 0;
			mstate = MODE_WHO;
			// Fallthrough

		case MODE_WHO:
			switch (*i) {
			case 'u':
				who |= 0700;
				break;
			case 'g':
				who |= 0070;
				break;
			case 'o':
				who |= 0007;
				break;
			case 'a':
				who |= 0777;
				break;
			default:
				mstate = MODE_ACTION;
				continue;
			}
			break;

		case MODE_ACTION_APPLY:
			switch (op) {
			case MODE_EQUALS:
				expr->file_mode &= ~who;
				expr->dir_mode &= ~who;
				// Fallthrough
			case MODE_PLUS:
				expr->file_mode |= file_change;
				expr->dir_mode |= dir_change;
				break;
			case MODE_MINUS:
				expr->file_mode &= ~file_change;
				expr->dir_mode &= ~dir_change;
				break;
			}
			// Fallthrough

		case MODE_ACTION:
			if (who == 0) {
				who = 0777;
			}

			switch (*i) {
			case '+':
				op = MODE_PLUS;
				mstate = MODE_OP;
				break;
			case '-':
				op = MODE_MINUS;
				mstate = MODE_OP;
				break;
			case '=':
				op = MODE_EQUALS;
				mstate = MODE_OP;
				break;

			case ',':
				if (mstate == MODE_ACTION_APPLY) {
					mstate = MODE_CLAUSE;
				} else {
					goto fail;
				}
				break;

			case '\0':
				if (mstate == MODE_ACTION_APPLY) {
					goto done;
				} else {
					goto fail;
				}

			default:
				goto fail;
			}
			break;

		case MODE_OP:
			file_change = 0;
			dir_change = 0;

			switch (*i) {
			case 'u':
			case 'g':
			case 'o':
				// PERMCOPY (e.g. u=g) has no effect for -perm
				mstate = MODE_ACTION_APPLY;
				break;

			default:
				mstate = MODE_PERM;
				continue;
			}
			break;

		case MODE_PERM:
			switch (*i) {
			case 'r':
				file_change |= who & 0444;
				dir_change |= who & 0444;
				break;
			case 'w':
				file_change |= who & 0222;
				dir_change |= who & 0222;
				break;
			case 'x':
				file_change |= who & 0111;
				// Fallthrough
			case 'X':
				dir_change |= who & 0111;
				break;
			case 's':
				if (who & 0700) {
					file_change |= S_ISUID;
					dir_change |= S_ISUID;
				}
				if (who & 0070) {
					file_change |= S_ISGID;
					dir_change |= S_ISGID;
				}
				break;
			case 't':
				file_change |= S_ISVTX;
				dir_change |= S_ISVTX;
				break;
			default:
				mstate = MODE_ACTION_APPLY;
				continue;
			}
			break;
		}

		++i;
	}

done:
	return 0;

fail:
	cfprintf(state->cmdline->cerr, "%{er}error: '%s' is an invalid mode.%{rs}\n", mode);
	return -1;
}

/**
 * Parse -perm MODE.
 */
static struct expr *parse_perm(struct parser_state *state, int field, int arg2) {
	struct expr *expr = parse_unary_test(state, eval_perm);
	if (!expr) {
		return NULL;
	}

	const char *mode = expr->sdata;
	switch (mode[0]) {
	case '-':
		expr->mode_cmp = MODE_ALL;
		++mode;
		break;
	case '/':
		expr->mode_cmp = MODE_ANY;
		++mode;
		break;
	default:
		expr->mode_cmp = MODE_EXACT;
		break;
	}

	if (parse_mode(state, mode, expr) != 0) {
		goto fail;
	}

	return expr;

fail:
	free_expr(expr);
	return NULL;
}

/**
 * Parse -print.
 */
static struct expr *parse_print(struct parser_state *state, int arg1, int arg2) {
	struct expr *expr = parse_nullary_action(state, eval_fprint);
	if (!expr) {
		return NULL;
	}

	expr->cfile = state->cmdline->cout;
	return expr;
}

/**
 * Parse -print0.
 */
static struct expr *parse_print0(struct parser_state *state, int arg1, int arg2) {
	struct expr *expr = parse_nullary_action(state, eval_fprint0);
	if (expr) {
		expr->cfile = state->cmdline->cout;
	}
	return expr;
}

/**
 * Parse -printf FORMAT.
 */
static struct expr *parse_printf(struct parser_state *state, int arg1, int arg2) {
	struct expr *expr = parse_unary_action(state, eval_fprintf);
	if (!expr) {
		return NULL;
	}

	expr->cfile = state->cmdline->cout;

	expr->printf = parse_bfs_printf(expr->sdata, state->cmdline);
	if (!expr->printf) {
		free_expr(expr);
		return NULL;
	}

	return expr;
}

/**
 * Parse -prune.
 */
static struct expr *parse_prune(struct parser_state *state, int arg1, int arg2) {
	return parse_nullary_action(state, eval_prune);
}

/**
 * Parse -quit.
 */
static struct expr *parse_quit(struct parser_state *state, int arg1, int arg2) {
	return parse_nullary_action(state, eval_quit);
}

/**
 * Parse -i?regex.
 */
static struct expr *parse_regex(struct parser_state *state, int flags, int arg2) {
	struct expr *expr = parse_unary_test(state, eval_regex);
	if (!expr) {
		goto fail;
	}

	expr->regex = malloc(sizeof(regex_t));
	if (!expr->regex) {
		perror("malloc()");
		goto fail;
	}

	int err = regcomp(expr->regex, expr->sdata, state->regex_flags | flags);
	if (err != 0) {
		char *str = xregerror(err, expr->regex);
		if (str) {
			cfprintf(state->cmdline->cerr, "%{er}error: %s %s: %s.%{rs}\n", expr->argv[0], expr->argv[1], str);
			free(str);
		} else {
			perror("xregerror()");
		}
		goto fail_regex;
	}

	return expr;

fail_regex:
	free(expr->regex);
	expr->regex = NULL;
fail:
	free_expr(expr);
	return NULL;
}

/**
 * Parse -E.
 */
static struct expr *parse_regex_extended(struct parser_state *state, int arg1, int arg2) {
	state->regex_flags = REG_EXTENDED;
	return parse_nullary_flag(state);
}

/**
 * Parse -regextype TYPE.
 */
static struct expr *parse_regextype(struct parser_state *state, int arg1, int arg2) {
	const char *type;
	struct expr *expr = parse_unary_positional_option(state, &type);
	if (!expr) {
		goto fail;
	}

	FILE *file = stderr;

	if (strcmp(type, "posix-basic") == 0) {
		state->regex_flags = 0;
	} else if (strcmp(type, "posix-extended") == 0) {
		state->regex_flags = REG_EXTENDED;
	} else if (strcmp(type, "help") == 0) {
		state->just_info = true;
		file = stdout;
		goto fail_list_types;
	} else {
		goto fail_bad_type;
	}

	return expr;

fail_bad_type:
	cfprintf(state->cmdline->cerr, "%{er}error: Unsupported -regextype '%s'.%{rs}\n\n", type);
fail_list_types:
	fputs("Supported types are:\n\n", file);
	fputs("  posix-basic:    POSIX basic regular expressions (BRE)\n", file);
	fputs("  posix-extended: POSIX extended regular expressions (ERE)\n", file);
fail:
	free_expr(expr);
	return NULL;
}

/**
 * Parse -samefile FILE.
 */
static struct expr *parse_samefile(struct parser_state *state, int arg1, int arg2) {
	struct expr *expr = parse_unary_test(state, eval_samefile);
	if (!expr) {
		return NULL;
	}

	struct stat sb;
	if (stat_arg(state, expr, &sb) != 0) {
		free_expr(expr);
		return NULL;
	}

	expr->dev = sb.st_dev;
	expr->ino = sb.st_ino;

	return expr;
}

/**
 * Parse -size N[bcwkMG]?.
 */
static struct expr *parse_size(struct parser_state *state, int arg1, int arg2) {
	struct expr *expr = parse_unary_test(state, eval_size);
	if (!expr) {
		return NULL;
	}

	const char *unit = parse_icmp(state, expr->sdata, expr, IF_PARTIAL_OK);
	if (!unit) {
		goto fail;
	}

	if (strlen(unit) > 1) {
		goto bad_unit;
	}

	switch (*unit) {
	case '\0':
	case 'b':
		expr->size_unit = SIZE_BLOCKS;
		break;
	case 'c':
		expr->size_unit = SIZE_BYTES;
		break;
	case 'w':
		expr->size_unit = SIZE_WORDS;
		break;
	case 'k':
		expr->size_unit = SIZE_KB;
		break;
	case 'M':
		expr->size_unit = SIZE_MB;
		break;
	case 'G':
		expr->size_unit = SIZE_GB;
		break;
	case 'T':
		expr->size_unit = SIZE_TB;
		break;
	case 'P':
		expr->size_unit = SIZE_PB;
		break;

	default:
		goto bad_unit;
	}

	return expr;

bad_unit:
	cfprintf(state->cmdline->cerr,
	         "%{er}error: %s %s: Expected a size unit (one of bcwkMGTP); found %s.%{rs}\n",
	         expr->argv[0], expr->argv[1], unit);
fail:
	free_expr(expr);
	return NULL;
}

/**
 * Parse -sparse.
 */
static struct expr *parse_sparse(struct parser_state *state, int arg1, int arg2) {
	return parse_nullary_test(state, eval_sparse);
}

/**
 * Parse -x?type [bcdpflsD].
 */
static struct expr *parse_type(struct parser_state *state, int x, int arg2) {
	eval_fn *eval = x ? eval_xtype : eval_type;
	struct expr *expr = parse_unary_test(state, eval);
	if (!expr) {
		return NULL;
	}

	enum bftw_typeflag types = 0;

	const char *c = expr->sdata;
	while (true) {
		switch (*c) {
		case 'b':
			types |= BFTW_BLK;
			break;
		case 'c':
			types |= BFTW_CHR;
			break;
		case 'd':
			types |= BFTW_DIR;
			break;
		case 'D':
			types |= BFTW_DOOR;
			break;
		case 'p':
			types |= BFTW_FIFO;
			break;
		case 'f':
			types |= BFTW_REG;
			break;
		case 'l':
			types |= BFTW_LNK;
			break;
		case 's':
			types |= BFTW_SOCK;
			break;

		case '\0':
			cfprintf(state->cmdline->cerr,
			         "%{er}error: %s %s: Expected a type flag.%{rs}\n",
			         expr->argv[0], expr->argv[1]);
			goto fail;

		default:
			cfprintf(state->cmdline->cerr,
			         "%{er}error: %s %s: Unknown type flag '%c' (expected one of [bcdpflsD]).%{rs}\n",
			         expr->argv[0], expr->argv[1], *c);
			goto fail;
		}

		++c;
		if (*c == '\0') {
			break;
		} else if (*c == ',') {
			++c;
			continue;
		} else {
			cfprintf(state->cmdline->cerr,
			         "%{er}error: %s %s: Types must be comma-separated.%{rs}\n",
			         expr->argv[0], expr->argv[1]);
			goto fail;
		}
	}

	expr->idata = types;
	return expr;

fail:
	free_expr(expr);
	return NULL;
}

/**
 * Parse -(no)?warn.
 */
static struct expr *parse_warn(struct parser_state *state, int warn, int arg2) {
	state->warn = warn;
	return parse_nullary_positional_option(state);
}

/**
 * "Parse" -help.
 */
static struct expr *parse_help(struct parser_state *state, int arg1, int arg2) {
	CFILE *cout = state->cmdline->cout;
	cfprintf(cout, "Usage: %{ex}%s%{rs} [%{cyn}flags%{rs}...] [%{mag}paths%{rs}...] [%{blu}expression%{rs}...]\n\n",
		 state->command);

	cfprintf(cout, "%{ex}bfs%{rs} is compatible with %{ex}find%{rs}; see %{ex}find%{rs} %{blu}-help%{rs} or"
	               " %{ex}man%{rs} %{bld}find%{rs} for help with %{ex}find%{rs}-\n"
	               "compatible options :)\n\n");

	cfprintf(cout, "%{cyn}flags%{rs} (%{cyn}-H%{rs}/%{cyn}-L%{rs}/%{cyn}-P%{rs} etc.), %{mag}paths%{rs}, and"
	               " %{blu}expressions%{rs} may be freely mixed in any order.\n\n");

	cfprintf(cout, "%{bld}POSIX find features:%{rs}\n\n");

	cfprintf(cout, "  %{red}(%{rs} %{blu}expression%{rs} %{red})%{rs}\n");
	cfprintf(cout, "  %{red}!%{rs} %{blu}expression%{rs}\n");
	cfprintf(cout, "  %{blu}expression%{rs} [%{red}-a%{rs}] %{blu}expression%{rs}\n");
	cfprintf(cout, "  %{blu}expression%{rs} %{red}-o%{rs} %{blu}expression%{rs}\n\n");

	cfprintf(cout, "  %{cyn}-H%{rs}\n");
	cfprintf(cout, "      Follow symbolic links on the command line, but not while searching\n");
	cfprintf(cout, "  %{cyn}-L%{rs}\n");
	cfprintf(cout, "      Follow all symbolic links\n\n");

	cfprintf(cout, "  %{blu}-depth%{rs}\n");
	cfprintf(cout, "      Search in post-order (descendents first)\n");
	cfprintf(cout, "  %{blu}-xdev%{rs}\n");
	cfprintf(cout, "      Don't descend into other mount points\n\n");

	cfprintf(cout, "  %{blu}-atime%{rs} %{bld}[-+]N%{rs}\n");
	cfprintf(cout, "  %{blu}-ctime%{rs} %{bld}[-+]N%{rs}\n");
	cfprintf(cout, "  %{blu}-mtime%{rs} %{bld}[-+]N%{rs}\n");
	cfprintf(cout, "      Find files accessed/changed/modified %{bld}N%{rs} days ago\n");
	cfprintf(cout, "  %{blu}-group%{rs} %{bld}NAME%{rs}\n");
	cfprintf(cout, "  %{blu}-user%{rs}  %{bld}NAME%{rs}\n");
	cfprintf(cout, "      Find files owned by the group/user %{bld}NAME%{rs}\n");
	cfprintf(cout, "  %{blu}-links%{rs} %{bld}[-+]N%{rs}\n");
	cfprintf(cout, "      Find files with %{bld}N%{rs} hard links\n");
	cfprintf(cout, "  %{blu}-name%{rs} %{bld}GLOB%{rs}\n");
	cfprintf(cout, "      Find files whose name matches the %{bld}GLOB%{rs}\n");
	cfprintf(cout, "  %{blu}-path%{rs} %{bld}GLOB%{rs}\n");
	cfprintf(cout, "      Find files whose entire path matches the %{bld}GLOB%{rs}\n");
	cfprintf(cout, "  %{blu}-newer%{rs} %{bld}FILE%{rs}\n");
	cfprintf(cout, "      Find files newer than %{bld}FILE%{rs}\n");
	cfprintf(cout, "  %{blu}-perm%{rs} %{bld}[-]MODE%{rs}\n");
	cfprintf(cout, "      Find files with a matching mode\n");
	cfprintf(cout, "  %{blu}-type%{rs} %{bld}[bcdlpfs]%{rs}\n");
	cfprintf(cout, "      Find files of the given type\n");
	cfprintf(cout, "  %{blu}-size%{rs} %{bld}[-+]N[c]%{rs}\n");
	cfprintf(cout, "      Find files with the given size\n\n");

	cfprintf(cout, "  %{blu}-prune%{rs}\n");
	cfprintf(cout, "      Don't descend into this directory\n");
	cfprintf(cout, "  %{blu}-exec%{rs} %{bld}command ... {} ;%{rs}\n");
	cfprintf(cout, "      Execute a command\n");
	cfprintf(cout, "  %{blu}-exec%{rs} %{bld}command ... {} +%{rs}\n");
	cfprintf(cout, "      Execute a command with multiple files at once\n");
	cfprintf(cout, "  %{blu}-ok%{rs} %{bld}command ... {} ;%{rs}\n");
	cfprintf(cout, "      Prompt the user whether to execute a command\n");
	cfprintf(cout, "  %{blu}-print%{rs}\n");
	cfprintf(cout, "      Print the path to the found file\n\n");

	cfprintf(cout, "%{bld}GNU find features:%{rs}\n\n");

	cfprintf(cout, "  %{red}-not%{rs} %{blu}expression%{rs}\n");
	cfprintf(cout, "  %{blu}expression%{rs} %{red}-and%{rs} %{blu}expression%{rs}\n");
	cfprintf(cout, "  %{blu}expression%{rs} %{red}-or%{rs} %{blu}expression%{rs}\n");
	cfprintf(cout, "  %{blu}expression%{rs} %{red},%{rs} %{blu}expression%{rs}\n\n");

	cfprintf(cout, "  %{cyn}-P%{rs}\n");
	cfprintf(cout, "      Never follow symbolic links (the default)\n");
	cfprintf(cout, "  %{cyn}-D%{rs} %{bld}FLAG%{rs}\n");
	cfprintf(cout, "      Turn on a debugging flag (see -D help)\n");
	cfprintf(cout, "  %{cyn}-O%{rs}%{bld}N%{rs}\n");
	cfprintf(cout, "      Enable optimization level %{bld}N%{rs} (default: 3)\n\n");

	cfprintf(cout, "  %{blu}-d%{rs}\n");
	cfprintf(cout, "      Search in post-order (same as %{blu}-depth%{rs})\n");
	cfprintf(cout, "  %{blu}-daystart%{rs}\n");
	cfprintf(cout, "      Measure times relative to the start of today\n");
	cfprintf(cout, "  %{blu}-follow%{rs}\n");
	cfprintf(cout, "      Follow all symbolic links (same as %{cyn}-L%{rs})\n");
	cfprintf(cout, "  %{blu}-ignore_readdir_race%{rs}\n");
	cfprintf(cout, "  %{blu}-noignore_readdir_race%{rs}\n");
	cfprintf(cout, "      Whether to report an error if %{ex}bfs%{rs} detects that the file tree is modified\n");
	cfprintf(cout, "      during the search (default: %{blu}-noignore_readdir_race%{rs})\n");
	cfprintf(cout, "  %{blu}-maxdepth%{rs} %{bld}N%{rs}\n");
	cfprintf(cout, "  %{blu}-mindepth%{rs} %{bld}N%{rs}\n");
	cfprintf(cout, "      Ignore files deeper/shallower than %{bld}N%{rs}\n");
	cfprintf(cout, "  %{blu}-mount%{rs}\n");
	cfprintf(cout, "      Don't descend into other mount points (same as %{blu}-xdev%{rs})\n");
	cfprintf(cout, "  %{blu}-noleaf%{rs}\n");
	cfprintf(cout, "      Ignored, for compatibility with GNU find\n");
	cfprintf(cout, "  %{blu}-regextype%{rs} %{bld}TYPE%{rs}\n");
	cfprintf(cout, "      Use TYPE-flavored regexes (see -regextype help)\n");
	cfprintf(cout, "  %{blu}-warn%{rs}\n");
	cfprintf(cout, "  %{blu}-nowarn%{rs}\n");
	cfprintf(cout, "      Turn on or off warnings about the command line\n\n");

	cfprintf(cout, "  %{blu}-amin%{rs} %{bld}[-+]N%{rs}\n");
	cfprintf(cout, "  %{blu}-cmin%{rs} %{bld}[-+]N%{rs}\n");
	cfprintf(cout, "  %{blu}-mmin%{rs} %{bld}[-+]N%{rs}\n");
	cfprintf(cout, "      Find files accessed/changed/modified %{bld}N%{rs} minutes ago\n");
	cfprintf(cout, "  %{blu}-anewer%{rs} %{bld}FILE%{rs}\n");
	cfprintf(cout, "  %{blu}-cnewer%{rs} %{bld}FILE%{rs}\n");
	cfprintf(cout, "  %{blu}-mnewer%{rs} %{bld}FILE%{rs}\n");
	cfprintf(cout, "      Find files accessed/changed/modified more recently than %{bld}FILE%{rs} was modified\n");
	cfprintf(cout, "  %{blu}-empty%{rs}\n");
	cfprintf(cout, "      Find empty files/directories\n");
	cfprintf(cout, "  %{blu}-executable%{rs}\n");
	cfprintf(cout, "  %{blu}-readable%{rs}\n");
	cfprintf(cout, "  %{blu}-writable%{rs}\n");
	cfprintf(cout, "      Find files the current user can execute/read/write\n");
	cfprintf(cout, "  %{blu}-false%{rs}\n");
	cfprintf(cout, "  %{blu}-true%{rs}\n");
	cfprintf(cout, "      Always false/true\n");
	cfprintf(cout, "  %{blu}-fstype%{rs} %{bld}TYPE%{rs}\n");
	cfprintf(cout, "      Find files on file systems with the given %{bld}TYPE%{rs}\n");
	cfprintf(cout, "  %{blu}-gid%{rs} %{bld}[-+]N%{rs}\n");
	cfprintf(cout, "  %{blu}-uid%{rs} %{bld}[-+]N%{rs}\n");
	cfprintf(cout, "      Find files owned by group/user ID %{bld}N%{rs}\n");
	cfprintf(cout, "  %{blu}-inum%{rs} %{bld}[-+]N%{rs}\n");
	cfprintf(cout, "      Find files with inode number %{bld}N%{rs}\n");
	cfprintf(cout, "  %{blu}-lname%{rs} %{bld}GLOB%{rs}\n");
	cfprintf(cout, "      Find symbolic links whose target matches the %{bld}GLOB%{rs}\n");
	cfprintf(cout, "  %{blu}-newer%{rs}%{bld}XY%{rs} %{bld}REFERENCE%{rs}\n");
	cfprintf(cout, "      Find files whose %{bld}X%{rs} time is newer than the %{bld}Y%{rs} time of"
	               " %{bld}REFERENCE%{bld}.  %{bld}X%{rs} and %{bld}Y%{rs}\n");
	cfprintf(cout, "      can be any of [acm].\n");
	cfprintf(cout, "  %{blu}-regex%{rs} %{bld}REGEX%{rs}\n");
	cfprintf(cout, "      Find files whose entire path matches the regular expression %{bld}REGEX%{rs}\n");
	cfprintf(cout, "  %{blu}-samefile%{rs} %{bld}FILE%{rs}\n");
	cfprintf(cout, "      Find hard links to %{bld}FILE%{rs}\n");
	cfprintf(cout, "  %{blu}-size%{rs} %{bld}[-+]N[wbkMG]%{rs}\n");
	cfprintf(cout, "      2-byte %{bld}w%{rs}ords, 512-byte %{bld}b%{rs}locks, and %{bld}k%{rs}iB/%{bld}M%{rs}iB"
	               "/%{bld}G%{rs}iB\n");
	cfprintf(cout, "  %{blu}-used%{rs} %{bld}[-+]N%{rs}\n");
	cfprintf(cout, "      Find files last accessed %{bld}N%{rs} days after they were changed\n");
	cfprintf(cout, "  %{blu}-wholename%{rs} %{bld}GLOB%{rs}\n");
	cfprintf(cout, "      Find files whose entire path matches the %{bld}GLOB%{rs} (same as %{blu}-path%{rs})\n");
	cfprintf(cout, "  %{blu}-ilname%{rs} %{bld}GLOB%{rs}\n");
	cfprintf(cout, "  %{blu}-iname%{rs}  %{bld}GLOB%{rs}\n");
	cfprintf(cout, "  %{blu}-ipath%{rs}  %{bld}GLOB%{rs}\n");
	cfprintf(cout, "  %{blu}-iregex%{rs} %{bld}REGEX%{rs}\n");
	cfprintf(cout, "  %{blu}-iwholename%{rs} %{bld}GLOB%{rs}\n");
	cfprintf(cout, "      Case-insensitive versions of %{blu}-lname%{rs}/%{blu}-name%{rs}/%{blu}-path%{rs}"
	               "/%{blu}-regex%{rs}/%{blu}-wholename%{rs}\n");
	cfprintf(cout, "  %{blu}-xtype%{rs} %{bld}[bcdlpfs]%{rs}\n");
	cfprintf(cout, "      Find files of the given type, following links when %{blu}-type%{rs} would not, and\n");
	cfprintf(cout, "      vice versa\n\n");

	cfprintf(cout, "  %{blu}-delete%{rs}\n");
	cfprintf(cout, "      Delete any found files (implies %{blu}-depth%{rs})\n");
	cfprintf(cout, "  %{blu}-execdir%{rs} %{bld}command ... {} ;%{rs}\n");
	cfprintf(cout, "  %{blu}-execdir%{rs} %{bld}command ... {} +%{rs}\n");
	cfprintf(cout, "  %{blu}-okdir%{rs}   %{bld}command ... {} ;%{rs}\n");
	cfprintf(cout, "      Like %{blu}-exec%{rs}/%{blu}-ok%{rs}, but run the command in the same directory as the found\n");
	cfprintf(cout, "      file(s)\n");
	cfprintf(cout, "  %{blu}-print0%{rs}\n");
	cfprintf(cout, "      Like %{blu}-print%{rs}, but use the null character ('\\0') as a separator rather than\n");
	cfprintf(cout, "      newlines\n");
	cfprintf(cout, "  %{blu}-printf%{rs} %{bld}FORMAT%{rs}\n");
	cfprintf(cout, "      Print according to a format string (see %{ex}man%{rs} %{bld}find%{rs})\n");
	cfprintf(cout, "  %{blu}-fprint%{rs} %{bld}FILE%{rs}\n");
	cfprintf(cout, "  %{blu}-fprint0%{rs} %{bld}FILE%{rs}\n");
	cfprintf(cout, "  %{blu}-fprintf%{rs} %{bld}FORMAT%{rs} %{bld}FILE%{rs}\n");
	cfprintf(cout, "      Like %{blu}-print%{rs}/%{blu}-print0%{rs}/%{blu}-printf%{rs}, but write to %{bld}FILE%{rs} instead"
	               " of standard output\n");
	cfprintf(cout, "  %{blu}-quit%{rs}\n");
	cfprintf(cout, "      Quit immediately\n\n");

	cfprintf(cout, "  %{blu}-version%{rs}\n");
	cfprintf(cout, "      Print version information\n");
	cfprintf(cout, "  %{blu}-help%{rs}\n");
	cfprintf(cout, "      Print this help message\n\n");

	cfprintf(cout, "%{bld}BSD find features:%{rs}\n\n");

	cfprintf(cout, "  %{cyn}-E%{rs}\n");
	cfprintf(cout, "      Use extended regular expressions (same as %{blu}-regextype%{rs} %{bld}posix-extended%{rs})\n");
	cfprintf(cout, "  %{cyn}-X%{rs}\n");
	cfprintf(cout, "      Filter out files with non-%{ex}xargs%{rs}-safe names\n");
	cfprintf(cout, "  %{cyn}-x%{rs}\n");
	cfprintf(cout, "      Don't descend into other mount points (same as %{blu}-xdev%{rs})\n\n");

	cfprintf(cout, "  %{cyn}-f%{rs} %{mag}PATH%{rs}\n");
	cfprintf(cout, "      Treat %{mag}PATH%{rs} as a path to search (useful if it may begin with a dash)\n\n");

	cfprintf(cout, "  %{blu}-depth%{rs} %{bld}[-+]N%{rs}\n");
	cfprintf(cout, "      Find files with depth %{bld}N%{rs}\n");
	cfprintf(cout, "  %{blu}-gid%{rs} %{bld}NAME%{rs}\n");
	cfprintf(cout, "  %{blu}-uid%{rs} %{bld}NAME%{rs}\n");
	cfprintf(cout, "      Group/user names are supported in addition to numeric IDs\n");
	cfprintf(cout, "  %{blu}-size%{rs} %{bld}[-+]N[TP]%{rs}\n");
	cfprintf(cout, "      %{bld}T%{rs}iB/%{bld}P%{rs}iB\n");
	cfprintf(cout, "  %{blu}-sparse%{rs}\n");
	cfprintf(cout, "      Find files that occupy fewer disk blocks than expected\n\n");

	cfprintf(cout, "%{bld}Extra features:%{rs}\n\n");

	cfprintf(cout, "  %{blu}-color%{rs}\n");
	cfprintf(cout, "  %{blu}-nocolor%{rs}\n");
	cfprintf(cout, "      Turn on or off file type colorization\n\n");

	cfprintf(cout, "  %{blu}-hidden%{rs}\n");
	cfprintf(cout, "  %{blu}-nohidden%{rs}\n");
	cfprintf(cout, "      Match hidden files, or filter them out\n\n");

	cfprintf(cout, "%s\n", BFS_HOMEPAGE);

	state->just_info = true;
	return NULL;
}

/**
 * "Parse" -version.
 */
static struct expr *parse_version(struct parser_state *state, int arg1, int arg2) {
	cfprintf(state->cmdline->cout, "%{ex}bfs%{rs} %{bld}%s%{rs}\n\n", BFS_VERSION);

	printf("%s\n", BFS_HOMEPAGE);

	state->just_info = true;
	return NULL;
}

typedef struct expr *parse_fn(struct parser_state *state, int arg1, int arg2);

/**
 * An entry in the parse table for literals.
 */
struct table_entry {
	const char *arg;
	bool prefix;
	parse_fn *parse;
	int arg1;
	int arg2;
};

/**
 * The parse table for literals.
 */
static const struct table_entry parse_table[] = {
	{"D", false, parse_debug},
	{"E", false, parse_regex_extended},
	{"O", true, parse_optlevel},
	{"P", false, parse_follow, 0, false},
	{"H", false, parse_follow, BFTW_COMFOLLOW, false},
	{"L", false, parse_follow, BFTW_LOGICAL | BFTW_DETECT_CYCLES, false},
	{"X", false, parse_xargs_safe},
	{"a"},
	{"amin", false, parse_acmtime, ATIME, MINUTES},
	{"and"},
	{"atime", false, parse_acmtime, ATIME, DAYS},
	{"anewer", false, parse_acnewer, ATIME},
	{"cmin", false, parse_acmtime, CTIME, MINUTES},
	{"ctime", false, parse_acmtime, CTIME, DAYS},
	{"cnewer", false, parse_acnewer, CTIME},
	{"color", false, parse_color, true},
	{"d", false, parse_depth},
	{"daystart", false, parse_daystart},
	{"delete", false, parse_delete},
	{"depth", false, parse_depth_n},
	{"empty", false, parse_empty},
	{"exec", false, parse_exec, 0},
	{"execdir", false, parse_exec, BFS_EXEC_CHDIR},
	{"executable", false, parse_access, X_OK},
	{"f", false, parse_f},
	{"false", false, parse_const, false},
	{"fls", false, parse_fls},
	{"follow", false, parse_follow, BFTW_LOGICAL | BFTW_DETECT_CYCLES, true},
	{"fprint", false, parse_fprint},
	{"fprint0", false, parse_fprint0},
	{"fprintf", false, parse_fprintf},
	{"fstype", false, parse_fstype},
	{"gid", false, parse_group},
	{"group", false, parse_group},
	{"help", false, parse_help},
	{"hidden", false, parse_hidden},
	{"ignore_readdir_race", false, parse_ignore_races, true},
	{"ilname", false, parse_lname, true},
	{"iname", false, parse_name, true},
	{"inum", false, parse_inum},
	{"ipath", false, parse_path, true},
	{"iregex", false, parse_regex, REG_ICASE},
	{"iwholename", false, parse_path, true},
	{"links", false, parse_links},
	{"lname", false, parse_lname, false},
	{"ls", false, parse_ls},
	{"maxdepth", false, parse_depth_limit, false},
	{"mindepth", false, parse_depth_limit, true},
	{"mmin", false, parse_acmtime, MTIME, MINUTES},
	{"mnewer", false, parse_acnewer, MTIME},
	{"mount", false, parse_mount},
	{"mtime", false, parse_acmtime, MTIME, DAYS},
	{"name", false, parse_name, false},
	{"newer", false, parse_acnewer, MTIME},
	{"newer", true, parse_newerxy},
	{"nocolor", false, parse_color, false},
	{"nogroup", false, parse_nogroup},
	{"nohidden", false, parse_nohidden},
	{"noignore_readdir_race", false, parse_ignore_races, false},
	{"noleaf", false, parse_noleaf},
	{"not"},
	{"nouser", false, parse_nouser},
	{"nowarn", false, parse_warn, false},
	{"o"},
	{"ok", false, parse_exec, BFS_EXEC_CONFIRM},
	{"okdir", false, parse_exec, BFS_EXEC_CONFIRM | BFS_EXEC_CHDIR},
	{"or"},
	{"path", false, parse_path, false},
	{"perm", false, parse_perm},
	{"print", false, parse_print},
	{"print0", false, parse_print0},
	{"printf", false, parse_printf},
	{"prune", false, parse_prune},
	{"quit", false, parse_quit},
	{"readable", false, parse_access, R_OK},
	{"regex", false, parse_regex, 0},
	{"regextype", false, parse_regextype},
	{"samefile", false, parse_samefile},
	{"size", false, parse_size},
	{"sparse", false, parse_sparse},
	{"true", false, parse_const, true},
	{"type", false, parse_type, false},
	{"uid", false, parse_user},
	{"used", false, parse_used},
	{"user", false, parse_user},
	{"version", false, parse_version},
	{"warn", false, parse_warn, true},
	{"wholename", false, parse_path, false},
	{"writable", false, parse_access, W_OK},
	{"x", false, parse_mount},
	{"xdev", false, parse_mount},
	{"xtype", false, parse_type, true},
	{"-"},
	{"-help", false, parse_help},
	{"-version", false, parse_version},
	{0},
};

/** Look up an argument in the parse table. */
static const struct table_entry *table_lookup(const char *arg) {
	for (const struct table_entry *entry = parse_table; entry->arg; ++entry) {
		bool match;
		if (entry->prefix) {
			match = strncmp(arg, entry->arg, strlen(entry->arg)) == 0;
		} else {
			match = strcmp(arg, entry->arg) == 0;
		}
		if (match) {
			return entry;
		}
	}

	return NULL;
}

/** Search for a fuzzy match in the parse table. */
static const struct table_entry *table_lookup_fuzzy(const char *arg) {
	const struct table_entry *best = NULL;
	int best_dist;

	for (const struct table_entry *entry = parse_table; entry->arg; ++entry) {
		int dist = typo_distance(arg, entry->arg);
		if (!best || dist < best_dist) {
			best = entry;
			best_dist = dist;
		}
	}

	return best;
}

/**
 * LITERAL : OPTION
 *         | TEST
 *         | ACTION
 */
static struct expr *parse_literal(struct parser_state *state) {
	struct cmdline *cmdline = state->cmdline;

	// Paths are already skipped at this point
	const char *arg = state->argv[0];

	if (arg[0] != '-') {
		goto unexpected;
	}

	const struct table_entry *match = table_lookup(arg + 1);
	if (match) {
		if (!match->parse) {
			goto unexpected;
		}
		return match->parse(state, match->arg1, match->arg2);
	}

	match = table_lookup_fuzzy(arg + 1);
	cfprintf(cmdline->cerr,
	         "%{er}error: Unknown argument '%s'; did you mean '-%s'?%{rs}\n",
		 arg, match->arg);
	return NULL;

unexpected:
	cfprintf(cmdline->cerr, "%{er}error: Expected a predicate; found '%s'.%{rs}\n", arg);
	return NULL;
}

static struct expr *new_and_expr(const struct parser_state *state, struct expr *lhs, struct expr *rhs, char **argv);
static struct expr *new_or_expr(const struct parser_state *state, struct expr *lhs, struct expr *rhs, char **argv);

/**
 * Create a "not" expression.
 */
static struct expr *new_not_expr(const struct parser_state *state, struct expr *rhs, char **argv) {
	if (state->cmdline->optlevel >= 1) {
		if (rhs == &expr_true) {
			debug_opt(state, "-O1: constant propagation: (%s %e) <==> %e\n", argv[0], rhs, &expr_false);
			return &expr_false;
		} else if (rhs == &expr_false) {
			debug_opt(state, "-O1: constant propagation: (%s %e) <==> %e\n", argv[0], rhs, &expr_true);
			return &expr_true;
		} else if (rhs->eval == eval_not) {
			struct expr *expr = rhs->rhs;
			debug_opt(state, "-O1: double negation: (%s %e) <==> %e\n", argv[0], rhs, expr);
			rhs->rhs = NULL;
			free_expr(rhs);
			return expr;
		} else if ((rhs->eval == eval_and || rhs->eval == eval_or)
			   && (rhs->lhs->eval == eval_not || rhs->rhs->eval == eval_not)) {
			bool other_and = rhs->eval == eval_or;
			char **other_arg = other_and ? &fake_and_arg : &fake_or_arg;

			debug_opt(state, "-O1: De Morgan's laws: (%s %e) <==> (%s (%s %e) (%s %e))\n",
				  argv[0], rhs,
				  *other_arg, argv[0], rhs->lhs, argv[0], rhs->rhs);

			struct expr *other_lhs = new_not_expr(state, rhs->lhs, argv);
			struct expr *other_rhs = new_not_expr(state, rhs->rhs, argv);
			rhs->rhs = NULL;
			rhs->lhs = NULL;
			free_expr(rhs);
			if (!other_lhs || !other_rhs) {
				free_expr(other_rhs);
				free_expr(other_lhs);
				return NULL;
			}

			if (other_and) {
				return new_and_expr(state, other_lhs, other_rhs, other_arg);
			} else {
				return new_or_expr(state, other_lhs, other_rhs, other_arg);
			}
		}
	}

	struct expr *expr = new_unary_expr(eval_not, rhs, argv);
	if (expr) {
		expr->always_true = rhs->always_false;
		expr->always_false = rhs->always_true;
	}
	return expr;
}

/**
 * FACTOR : "(" EXPR ")"
 *        | "!" FACTOR | "-not" FACTOR
 *        | LITERAL
 */
static struct expr *parse_factor(struct parser_state *state) {
	if (skip_paths(state) != 0) {
		return NULL;
	}

	const char *arg = state->argv[0];
	if (!arg) {
		fputs("Expression terminated prematurely.\n", stderr);
		return NULL;
	}

	if (strcmp(arg, "(") == 0) {
		parser_advance(state, T_OPERATOR, 1);

		struct expr *expr = parse_expr(state);
		if (!expr) {
			return NULL;
		}

		if (skip_paths(state) != 0) {
			free_expr(expr);
			return NULL;
		}

		arg = state->argv[0];
		if (!arg || strcmp(arg, ")") != 0) {
			fputs("Expected a ')'.\n", stderr);
			free_expr(expr);
			return NULL;
		}
		parser_advance(state, T_OPERATOR, 1);

		return expr;
	} else if (strcmp(arg, "!") == 0 || strcmp(arg, "-not") == 0) {
		char **argv = parser_advance(state, T_OPERATOR, 1);

		struct expr *factor = parse_factor(state);
		if (!factor) {
			return NULL;
		}

		return new_not_expr(state, factor, argv);
	} else {
		return parse_literal(state);
	}
}

/**
 * Create an "and" expression.
 */
static struct expr *new_and_expr(const struct parser_state *state, struct expr *lhs, struct expr *rhs, char **argv) {
	int optlevel = state->cmdline->optlevel;
	if (optlevel >= 1) {
		if (lhs == &expr_true) {
			debug_opt(state, "-O1: conjunction elimination: (%s %e %e) <==> %e\n", argv[0], lhs, rhs, rhs);
			return rhs;
		} else if (rhs == &expr_true) {
			debug_opt(state, "-O1: conjunction elimination: (%s %e %e) <==> %e\n", argv[0], lhs, rhs, lhs);
			return lhs;
		} else if (lhs->always_false) {
			debug_opt(state, "-O1: short-circuit: (%s %e %e) <==> %e\n", argv[0], lhs, rhs, lhs);
			free_expr(rhs);
			return lhs;
		} else if (optlevel >= 2 && rhs->always_false && lhs->pure) {
			debug_opt(state, "-O2: purity: (%s %e %e) <==> %e\n", argv[0], lhs, rhs, rhs);
			free_expr(lhs);
			return rhs;
		} else if (lhs->eval == eval_not && rhs->eval == eval_not) {
			char **not_arg = lhs->argv;
			debug_opt(state, "-O1: De Morgan's laws: (%s %e %e) <==> (%s (%s %e %e))\n",
			          argv[0], lhs, rhs,
				  *not_arg, fake_or_arg, lhs->rhs, rhs->rhs);

			struct expr *or_expr = new_or_expr(state, lhs->rhs, rhs->rhs, &fake_or_arg);
			rhs->rhs = NULL;
			lhs->rhs = NULL;
			free_expr(rhs);
			free_expr(lhs);
			if (!or_expr) {
				return NULL;
			}

			return new_not_expr(state, or_expr, not_arg);
		}
	}

	struct expr *expr = new_binary_expr(eval_and, lhs, rhs, argv);
	if (expr) {
		expr->always_true = lhs->always_true && rhs->always_true;
		expr->always_false = lhs->always_false || rhs->always_false;
	}
	return expr;
}

/**
 * TERM : FACTOR
 *      | TERM FACTOR
 *      | TERM "-a" FACTOR
 *      | TERM "-and" FACTOR
 */
static struct expr *parse_term(struct parser_state *state) {
	struct expr *term = parse_factor(state);

	while (term) {
		if (skip_paths(state) != 0) {
			free_expr(term);
			return NULL;
		}

		const char *arg = state->argv[0];
		if (!arg) {
			break;
		}

		if (strcmp(arg, "-o") == 0 || strcmp(arg, "-or") == 0
		    || strcmp(arg, ",") == 0
		    || strcmp(arg, ")") == 0) {
			break;
		}

		char **argv = &fake_and_arg;
		if (strcmp(arg, "-a") == 0 || strcmp(arg, "-and") == 0) {
			argv = parser_advance(state, T_OPERATOR, 1);
		}

		struct expr *lhs = term;
		struct expr *rhs = parse_factor(state);
		if (!rhs) {
			free_expr(lhs);
			return NULL;
		}

		term = new_and_expr(state, lhs, rhs, argv);
	}

	return term;
}

/**
 * Create an "or" expression.
 */
static struct expr *new_or_expr(const struct parser_state *state, struct expr *lhs, struct expr *rhs, char **argv) {
	int optlevel = state->cmdline->optlevel;
	if (optlevel >= 1) {
		if (lhs->always_true) {
			debug_opt(state, "-O1: short-circuit: (%s %e %e) <==> %e\n", argv[0], lhs, rhs, lhs);
			free_expr(rhs);
			return lhs;
		} else if (lhs == &expr_false) {
			debug_opt(state, "-O1: disjunctive syllogism: (%s %e %e) <==> %e\n", argv[0], lhs, rhs, rhs);
			return rhs;
		} else if (rhs == &expr_false) {
			debug_opt(state, "-O1: disjunctive syllogism: (%s %e %e) <==> %e\n", argv[0], lhs, rhs, lhs);
			return lhs;
		} else if (optlevel >= 2 && rhs->always_true && lhs->pure) {
			debug_opt(state, "-O2: purity: (%s %e %e) <==> %e\n", argv[0], lhs, rhs, rhs);
			free_expr(lhs);
			return rhs;
		} else if (lhs->eval == eval_not && rhs->eval == eval_not) {
			char **not_arg = lhs->argv;
			debug_opt(state, "-O1: De Morgan's laws: (%s %e %e) <==> (%s (%s %e %e))\n",
			          argv[0], lhs, rhs,
				  *not_arg, fake_and_arg, lhs->rhs, rhs->rhs);

			struct expr *and_expr = new_and_expr(state, lhs->rhs, rhs->rhs, &fake_and_arg);
			rhs->rhs = NULL;
			lhs->rhs = NULL;
			free_expr(rhs);
			free_expr(lhs);
			if (!and_expr) {
				return NULL;
			}

			return new_not_expr(state, and_expr, not_arg);
		}
	}

	struct expr *expr = new_binary_expr(eval_or, lhs, rhs, argv);
	if (expr) {
		expr->always_true = lhs->always_true || rhs->always_true;
		expr->always_false = lhs->always_false && rhs->always_false;
	}
	return expr;
}

/**
 * CLAUSE : TERM
 *        | CLAUSE "-o" TERM
 *        | CLAUSE "-or" TERM
 */
static struct expr *parse_clause(struct parser_state *state) {
	struct expr *clause = parse_term(state);

	while (clause) {
		if (skip_paths(state) != 0) {
			free_expr(clause);
			return NULL;
		}

		const char *arg = state->argv[0];
		if (!arg) {
			break;
		}

		if (strcmp(arg, "-o") != 0 && strcmp(arg, "-or") != 0) {
			break;
		}

		char **argv = parser_advance(state, T_OPERATOR, 1);

		struct expr *lhs = clause;
		struct expr *rhs = parse_term(state);
		if (!rhs) {
			free_expr(lhs);
			return NULL;
		}

		clause = new_or_expr(state, lhs, rhs, argv);
	}

	return clause;
}

/**
 * Create a "comma" expression.
 */
static struct expr *new_comma_expr(const struct parser_state *state, struct expr *lhs, struct expr *rhs, char **argv) {
	int optlevel = state->cmdline->optlevel;
	if (optlevel >= 1) {
		if (lhs->eval == eval_not) {
			debug_opt(state, "-O1: ignored result: (%s %e %e) <==> (%s %e %e)\n",
			          argv[0], lhs, rhs,
				  argv[0], lhs->rhs, rhs);

			struct expr *old = lhs;
			lhs = old->rhs;
			old->rhs = NULL;
			free_expr(old);
		}

		if (optlevel >= 2 && lhs->pure) {
			debug_opt(state, "-O2: purity: (%s %e %e) <==> %e\n", argv[0], lhs, rhs, rhs);
			free_expr(lhs);
			return rhs;
		}
	}

	struct expr *expr = new_binary_expr(eval_comma, lhs, rhs, argv);
	if (expr) {
		expr->always_true = rhs->always_true;
		expr->always_false = rhs->always_false;
	}
	return expr;
}

/**
 * EXPR : CLAUSE
 *      | EXPR "," CLAUSE
 */
static struct expr *parse_expr(struct parser_state *state) {
	struct expr *expr = parse_clause(state);

	while (expr) {
		if (skip_paths(state) != 0) {
			free_expr(expr);
			return NULL;
		}

		const char *arg = state->argv[0];
		if (!arg) {
			break;
		}

		if (strcmp(arg, ",") != 0) {
			break;
		}

		char **argv = parser_advance(state, T_OPERATOR, 1);

		struct expr *lhs = expr;
		struct expr *rhs = parse_clause(state);
		if (!rhs) {
			free_expr(lhs);
			return NULL;
		}

		expr = new_comma_expr(state, lhs, rhs, argv);
	}

	return expr;
}

/**
 * Apply top-level optimizations.
 */
static struct expr *optimize_whole_expr(const struct parser_state *state, struct expr *expr) {
	int optlevel = state->cmdline->optlevel;

	if (optlevel >= 2) {
		while ((expr->eval == eval_and || expr->eval == eval_or || expr->eval == eval_comma)
		       && expr->rhs->pure) {
			debug_opt(state, "-O2: top-level purity: %e <==> %e\n", expr, expr->lhs);

			struct expr *old = expr;
			expr = old->lhs;
			old->lhs = NULL;
			free_expr(old);
		}
	}

	if (optlevel >= 4 && expr->pure && expr != &expr_false) {
		debug_opt(state, "-O4: top-level purity: %e <==> %e\n", expr, &expr_false);
		free_expr(expr);
		expr = &expr_false;
	}

	return expr;
}

/**
 * Parse the top-level expression.
 */
static struct expr *parse_whole_expr(struct parser_state *state) {
	if (skip_paths(state) != 0) {
		return NULL;
	}

	struct expr *expr = &expr_true;
	if (state->argv[0]) {
		expr = parse_expr(state);
		if (!expr) {
			return NULL;
		}
	}

	if (state->argv[0]) {
		cfprintf(state->cmdline->cerr, "%{er}error: Unexpected argument '%s'.%{rs}\n", state->argv[0]);
		goto fail;
	}

	if (state->implicit_print) {
		struct expr *print = new_expr(eval_fprint, false, 1, &fake_print_arg);
		if (!print) {
			goto fail;
		}
		print->cfile = state->cmdline->cout;

		expr = new_and_expr(state, expr, print, &fake_and_arg);
		if (!expr) {
			goto fail;
		}
	}

	expr = optimize_whole_expr(state, expr);
	return expr;

fail:
	free_expr(expr);
	return NULL;
}

/**
 * Dump the parsed form of the command line, for debugging.
 */
void dump_cmdline(const struct cmdline *cmdline, bool verbose) {
	if (cmdline->flags & BFTW_LOGICAL) {
		fputs("-L ", stderr);
	} else if (cmdline->flags & BFTW_COMFOLLOW) {
		fputs("-H ", stderr);
	} else {
		fputs("-P ", stderr);
	}

	if (cmdline->optlevel != 3) {
		fprintf(stderr, "-O%d ", cmdline->optlevel);
	}

	if (cmdline->debug & DEBUG_OPT) {
		fputs("-D opt ", stderr);
	}
	if (cmdline->debug & DEBUG_RATES) {
		fputs("-D rates ", stderr);
	}
	if (cmdline->debug & DEBUG_STAT) {
		fputs("-D stat ", stderr);
	}
	if (cmdline->debug & DEBUG_TREE) {
		fputs("-D tree ", stderr);
	}

	for (struct root *root = cmdline->roots; root; root = root->next) {
		char c = root->path[0];
		if (c == '-' || c == '(' || c == ')' || c == '!' || c == ',') {
			fputs("-f ", stderr);
		}
		fprintf(stderr, "%s ", root->path);
	}

	if (cmdline->cout->colors) {
		fputs("-color ", stderr);
	} else {
		fputs("-nocolor ", stderr);
	}
	if (cmdline->flags & BFTW_DEPTH) {
		fputs("-depth ", stderr);
	}
	if (cmdline->ignore_races) {
		fputs("-ignore_readdir_race ", stderr);
	}
	if (cmdline->flags & BFTW_XDEV) {
		fputs("-mount ", stderr);
	}
	if (cmdline->mindepth != 0) {
		fprintf(stderr, "-mindepth %d ", cmdline->mindepth);
	}
	if (cmdline->maxdepth != INT_MAX) {
		fprintf(stderr, "-maxdepth %d ", cmdline->maxdepth);
	}

	dump_expr(cmdline->expr, verbose);

	fputs("\n", stderr);
}

/**
 * Get the current time.
 */
static int parse_gettime(struct timespec *ts) {
#if _POSIX_TIMERS > 0
	int ret = clock_gettime(CLOCK_REALTIME, ts);
	if (ret != 0) {
		perror("clock_gettime()");
	}
	return ret;
#else
	struct timeval tv;
	int ret = gettimeofday(&tv, NULL);
	if (ret == 0) {
		ts->tv_sec = tv.tv_sec;
		ts->tv_nsec = tv.tv_usec * 1000L;
	} else {
		perror("gettimeofday()");
	}
	return ret;
#endif
}

/**
 * Parse the command line.
 */
struct cmdline *parse_cmdline(int argc, char *argv[]) {
	struct cmdline *cmdline = malloc(sizeof(struct cmdline));
	if (!cmdline) {
		perror("malloc()");
		goto fail;
	}

	cmdline->roots = NULL;
	cmdline->colors = NULL;
	cmdline->cout = NULL;
	cmdline->cerr = NULL;
	cmdline->mtab = NULL;
	cmdline->mindepth = 0;
	cmdline->maxdepth = INT_MAX;
	cmdline->flags = BFTW_RECOVER;
	cmdline->optlevel = 3;
	cmdline->debug = 0;
	cmdline->xargs_safe = false;
	cmdline->ignore_races = false;
	cmdline->expr = &expr_true;
	cmdline->nopen_files = 0;

	cmdline->colors = parse_colors(getenv("LS_COLORS"));
	cmdline->cout = cfdup(stdout, cmdline->colors);
	cmdline->cerr = cfdup(stderr, cmdline->colors);
	if (!cmdline->cout || !cmdline->cerr) {
		perror("cfdup()");
		goto fail;
	}

	cmdline->mtab = parse_bfs_mtab();
	if (!cmdline->mtab) {
		cfprintf(cmdline->cerr, "%{wr}warning: Couldn't parse the mount table: %s.%{rs}\n\n", strerror(errno));
	}

	struct parser_state state = {
		.cmdline = cmdline,
		.argv = argv + 1,
		.command = argv[0],
		.roots_tail = &cmdline->roots,
		.regex_flags = 0,
		.use_color = COLOR_AUTO,
		.implicit_print = true,
		.warn = isatty(STDIN_FILENO),
		.non_option_seen = false,
		.just_info = false,
	};

	if (parse_gettime(&state.now) != 0) {
		goto fail;
	}

	cmdline->expr = parse_whole_expr(&state);
	if (!cmdline->expr) {
		if (state.just_info) {
			goto done;
		} else {
			goto fail;
		}
	}

	if (!cmdline->roots) {
		if (parse_root(&state, ".") != 0) {
			goto fail;
		}
	}

	if (cmdline->debug & DEBUG_TREE) {
		dump_cmdline(cmdline, false);
	}

done:
	return cmdline;

fail:
	free_cmdline(cmdline);
	return NULL;
}
