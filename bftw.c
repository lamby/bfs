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

/**
 * bftw() implementation.
 *
 * The goal of this implementation is to avoid re-traversal by using openat() as
 * much as possible.  Since the number of open file descriptors is limited, the
 * 'dircache' maintains a priority queue of open 'dircache_entry's, ordered by
 * their reference counts to keep the most-referenced parent directories open.
 *
 * The 'dirqueue' is a simple FIFO of 'dircache_entry's left to explore.
 */

#include "bftw.h"
#include "dstring.h"
#include "util.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/**
 * A single entry in the dircache.
 */
struct dircache_entry {
	/** The parent entry, if any. */
	struct dircache_entry *parent;
	/** This directory's depth in the walk. */
	size_t depth;

	/** Reference count. */
	size_t refcount;
	/** Index in the priority queue. */
	size_t heap_index;

	/** An open file descriptor to this directory, or -1. */
	int fd;

	/** The device number, for cycle detection. */
	dev_t dev;
	/** The inode number, for cycle detection. */
	ino_t ino;

	/** The offset of this directory in the full path. */
	size_t nameoff;
	/** The length of the directory's name. */
	size_t namelen;
	/** The directory's name. */
	char name[];
};

/**
 * A directory cache.
 */
struct dircache {
	/** A min-heap of open entries, ordered by refcount. */
	struct dircache_entry **heap;
	/** Current heap size. */
	size_t size;
	/** Maximum heap size. */
	size_t capacity;
};

/** Initialize a dircache. */
static int dircache_init(struct dircache *cache, size_t capacity) {
	cache->heap = malloc(capacity*sizeof(struct dircache_entry *));
	if (!cache->heap) {
		return -1;
	}

	cache->size = 0;
	cache->capacity = capacity;
	return 0;
}

/** Destroy a dircache. */
static void dircache_free(struct dircache *cache) {
	assert(cache->size == 0);
	free(cache->heap);
}

/** Check if two dircache entries are in heap order. */
static bool dircache_heap_check(const struct dircache_entry *parent, const struct dircache_entry *child) {
	if (parent->depth > child->depth) {
		return true;
	} else if (parent->depth < child->depth) {
		return false;
	} else {
		return parent->refcount <= child->refcount;
	}
}

/** Move an entry to a particular place in the heap. */
static void dircache_heap_move(struct dircache *cache, struct dircache_entry *entry, size_t i) {
	cache->heap[i] = entry;
	entry->heap_index = i;
}

/** Bubble an entry up the heap. */
static void dircache_bubble_up(struct dircache *cache, struct dircache_entry *entry) {
	size_t i = entry->heap_index;

	while (i > 0) {
		size_t pi = (i - 1)/2;
		struct dircache_entry *parent = cache->heap[pi];
		if (dircache_heap_check(parent, entry)) {
			break;
		}

		dircache_heap_move(cache, parent, i);
		i = pi;
	}

	dircache_heap_move(cache, entry, i);
}

/** Bubble an entry down the heap. */
static void dircache_bubble_down(struct dircache *cache, struct dircache_entry *entry) {
	size_t i = entry->heap_index;

	while (true) {
		size_t ci = 2*i + 1;
		if (ci >= cache->size) {
			break;
		}

		struct dircache_entry *child = cache->heap[ci];

		size_t ri = ci + 1;
		if (ri < cache->size) {
			struct dircache_entry *right = cache->heap[ri];
			if (!dircache_heap_check(child, right)) {
				ci = ri;
				child = right;
			}
		}

		dircache_heap_move(cache, child, i);
		i = ci;
	}

	dircache_heap_move(cache, entry, i);
}

/** Increment a dircache_entry's reference count. */
static void dircache_entry_incref(struct dircache *cache, struct dircache_entry *entry) {
	++entry->refcount;

	if (entry->fd >= 0) {
		dircache_bubble_down(cache, entry);
	}
}

/** Decrement a dircache_entry's reference count. */
static void dircache_entry_decref(struct dircache *cache, struct dircache_entry *entry) {
	--entry->refcount;

	if (entry->fd >= 0) {
		dircache_bubble_up(cache, entry);
	}
}

/** Add a dircache_entry to the priority queue. */
static void dircache_push(struct dircache *cache, struct dircache_entry *entry) {
	assert(cache->size < cache->capacity);
	assert(entry->fd >= 0);

	size_t size = cache->size++;
	entry->heap_index = size;
	dircache_bubble_up(cache, entry);
}

/** Close a dircache_entry and remove it from the priority queue. */
static void dircache_pop(struct dircache *cache, struct dircache_entry *entry) {
	assert(cache->size > 0);
	assert(entry->fd >= 0);

	close(entry->fd);
	entry->fd = -1;

	size_t size = --cache->size;
	size_t i = entry->heap_index;
	if (i != size) {
		struct dircache_entry *end = cache->heap[size];
		end->heap_index = i;
		dircache_bubble_down(cache, end);
	}
}

/** Add an entry to the dircache. */
static struct dircache_entry *dircache_add(struct dircache *cache, struct dircache_entry *parent, const char *name) {
	size_t namelen = strlen(name);
	size_t size = sizeof(struct dircache_entry) + namelen;

	bool needs_slash = false;
	if (namelen == 0 || name[namelen - 1] != '/') {
		needs_slash = true;
		++size;
	}

	struct dircache_entry *entry = malloc(size);
	if (!entry) {
		return NULL;
	}

	entry->parent = parent;

	if (parent) {
		entry->depth = parent->depth + 1;
		entry->nameoff = parent->nameoff + parent->namelen;
		dircache_entry_incref(cache, parent);
	} else {
		entry->depth = 0;
		entry->nameoff = 0;
	}

	entry->refcount = 1;
	entry->fd = -1;

	entry->dev = -1;
	entry->ino = -1;

	memcpy(entry->name, name, namelen);
	if (needs_slash) {
		entry->name[namelen++] = '/';
	}
	entry->namelen = namelen;

	return entry;
}

/**
 * Get the appropriate (fd, path) pair for the *at() family of functions.
 *
 * @param cache
 *         The cache containing the entry.
 * @param entry
 *         The entry being accessed.
 * @param[out] at_fd
 *         Will hold the appropriate file descriptor to use.
 * @param[in,out] at_path
 *         Will hold the appropriate path to use.
 * @return The closest open ancestor entry.
 */
static struct dircache_entry *dircache_entry_base(struct dircache *cache, struct dircache_entry *entry, int *at_fd, const char **at_path) {
	struct dircache_entry *base = entry;

	do {
		base = base->parent;
	} while (base && base->fd < 0);

	if (base) {
		*at_fd = base->fd;
		*at_path += base->nameoff + base->namelen;
	}

	return base;
}

/**
 * Check if we should retry an operation due to EMFILE.
 *
 * @param cache
 *         The cache in question.
 * @param save
 *         A dircache_entry that must be preserved.
 */
static bool dircache_should_retry(struct dircache *cache, const struct dircache_entry *save) {
	if (errno == EMFILE && cache->size > 1) {
		// Too many open files, shrink the cache
		struct dircache_entry *entry = cache->heap[0];
		if (entry == save) {
			entry = cache->heap[1];
		}

		dircache_pop(cache, entry);
		cache->capacity = cache->size;
		return true;
	} else {
		return false;
	}
}

/**
 * Open a dircache_entry.
 *
 * @param cache
 *         The cache containing the entry.
 * @param entry
 *         The entry to open.
 * @param path
 *         The full path to the entry.
 * @return
 *         The opened DIR *, or NULL on error.
 */
static DIR *dircache_entry_open(struct dircache *cache, struct dircache_entry *entry, const char *path) {
	assert(entry->fd < 0);

	if (cache->size == cache->capacity) {
		dircache_pop(cache, cache->heap[0]);
	}

	int at_fd = AT_FDCWD;
	const char *at_path = path;
	struct dircache_entry *base = dircache_entry_base(cache, entry, &at_fd, &at_path);

	int flags = O_RDONLY | O_CLOEXEC;
#ifdef O_DIRECTORY
	flags |= O_DIRECTORY;
#endif
	int fd = openat(at_fd, at_path, flags);

	if (fd < 0 && dircache_should_retry(cache, base)) {
		fd = openat(at_fd, at_path, flags);
	}
	if (fd < 0) {
		return NULL;
	}

	entry->fd = fd;
	dircache_push(cache, entry);

	// Now we dup() the fd and pass it to fdopendir().  This way we can
	// close the DIR* as soon as we're done with it, reducing the memory
	// footprint significantly, while keeping the fd around for future
	// openat() calls.

	fd = dup_cloexec(entry->fd);

	if (fd < 0 && dircache_should_retry(cache, entry)) {
		fd = dup_cloexec(entry->fd);
	}
	if (fd < 0) {
		return NULL;
	}

	DIR *dir = fdopendir(fd);
	if (!dir) {
		close(fd);
	}
	return dir;
}

/** Free a dircache_entry. */
static void dircache_entry_free(struct dircache *cache, struct dircache_entry *entry) {
	if (entry) {
		assert(entry->refcount == 0);

		if (entry->fd >= 0) {
			dircache_pop(cache, entry);
		}

		free(entry);
	}
}

/**
 * A queue of 'dircache_entry's to examine.
 */
struct dirqueue {
	/** The circular buffer of entries. */
	struct dircache_entry **entries;
	/** The head of the queue. */
	size_t head;
	/** The size of the queue. */
	size_t size;
	/** The capacity of the queue (always a power of two). */
	size_t capacity;
};

/** Initialize a dirqueue. */
static int dirqueue_init(struct dirqueue *queue) {
	queue->head = 0;
	queue->size = 0;
	queue->capacity = 256;
	queue->entries = malloc(queue->capacity*sizeof(struct dircache_entry *));
	if (queue->entries) {
		return 0;
	} else {
		return -1;
	}
}

/** Add an entry to the dirqueue. */
static int dirqueue_push(struct dirqueue *queue, struct dircache_entry *entry) {
	struct dircache_entry **entries = queue->entries;
	size_t head = queue->head;
	size_t size = queue->size;
	size_t tail = head + size;
	size_t capacity = queue->capacity;

	if (size == capacity) {
		capacity *= 2;
		entries = realloc(entries, capacity*sizeof(struct dircache_entry *));
		if (!entries) {
			return -1;
		}

		for (size_t i = size; i < tail; ++i) {
			entries[i] = entries[i - size];
		}

		queue->entries = entries;
		queue->capacity = capacity;
	}

	tail &= capacity - 1;
	entries[tail] = entry;
	++queue->size;
	return 0;
}

/** Remove an entry from the dirqueue. */
static struct dircache_entry *dirqueue_pop(struct dirqueue *queue) {
	if (queue->size == 0) {
		return NULL;
	}

	struct dircache_entry *entry = queue->entries[queue->head];
	--queue->size;
	++queue->head;
	queue->head &= queue->capacity - 1;
	return entry;
}

/** Destroy a dirqueue. */
static void dirqueue_free(struct dirqueue *queue) {
	free(queue->entries);
}

/** Call stat() and use the results. */
static int ftwbuf_stat(struct BFTW *ftwbuf, struct stat *sb) {
	int ret = fstatat(ftwbuf->at_fd, ftwbuf->at_path, sb, ftwbuf->at_flags);
	if (ret != 0) {
		return ret;
	}

	ftwbuf->statbuf = sb;
	ftwbuf->typeflag = mode_to_typeflag(sb->st_mode);

	return 0;
}

/**
 * Possible bftw() traversal statuses.
 */
enum bftw_status {
	/** The current path is state.current. */
	BFTW_CURRENT,
	/** The current path is a child of state.current. */
	BFTW_CHILD,
	/** dircache_entry's are being garbage collected. */
	BFTW_GC,
};

/**
 * Holds the current state of the bftw() traversal.
 */
struct bftw_state {
	/** bftw() callback. */
	bftw_fn *fn;
	/** bftw() flags. */
	int flags;
	/** bftw() callback data. */
	void *ptr;

	/** The appropriate errno value, if any. */
	int error;

	/** The cache of open directories. */
	struct dircache cache;

	/** The queue of directories left to explore. */
	struct dirqueue queue;
	/** The current dircache entry. */
	struct dircache_entry *current;
	/** The previous dircache entry. */
	struct dircache_entry *last;
	/** The currently open directory. */
	DIR *dir;
	/** The current traversal status. */
	enum bftw_status status;

	/** The root path of the walk. */
	const char *root;
	/** The current path being explored. */
	char *path;

	/** Extra data about the current file. */
	struct BFTW ftwbuf;
	/** stat() buffer for the current file. */
	struct stat statbuf;
};

/**
 * Initialize the bftw() state.
 */
static int bftw_state_init(struct bftw_state *state, const char *root, bftw_fn *fn, int nopenfd, int flags, void *ptr) {
	state->root = root;
	state->fn = fn;
	state->flags = flags;
	state->ptr = ptr;

	state->error = 0;

	if (nopenfd < 2) {
		errno = EMFILE;
		return -1;
	}

	// -1 to account for dup()
	if (dircache_init(&state->cache, nopenfd - 1) != 0) {
		goto err;
	}

	if (dirqueue_init(&state->queue) != 0) {
		goto err_cache;
	}

	state->current = NULL;
	state->last = NULL;
	state->dir = NULL;
	state->status = BFTW_CURRENT;

	state->path = dstralloc(0);
	if (!state->path) {
		goto err_queue;
	}

	return 0;

err_queue:
	dirqueue_free(&state->queue);
err_cache:
	dircache_free(&state->cache);
err:
	return -1;
}

/**
 * Compute the path to the current dircache_entry.
 */
static int bftw_build_path(struct bftw_state *state) {
	const struct dircache_entry *entry = state->current;
	size_t namelen = entry->namelen;
	size_t pathlen = entry->nameoff + namelen;

	if (dstresize(&state->path, pathlen) != 0) {
		return -1;
	}

	// Only rebuild the part of the path that changes
	const struct dircache_entry *last = state->last;
	while (last && last->depth > entry->depth) {
		last = last->parent;
	}

	// Build the path backwards
	char *path = state->path;
	while (entry != last) {
		char *segment = path + entry->nameoff;
		namelen = entry->namelen;
		memcpy(segment, entry->name, namelen);

		if (last && last->depth == entry->depth) {
			last = last->parent;
		}
		entry = entry->parent;
	}

	state->last = state->current;

	return 0;
}

/**
 * Concatenate a subpath to the current path.
 */
static int bftw_path_concat(struct bftw_state *state, const char *subpath) {
	size_t nameoff = 0;

	struct dircache_entry *current = state->current;
	if (current) {
		nameoff = current->nameoff + current->namelen;
	}

	state->status = BFTW_CHILD;

	if (dstresize(&state->path, nameoff) != 0) {
		return -1;
	}
	return dstrcat(&state->path, subpath);
}

/**
 * Trim the path to just the current directory.
 */
static void bftw_path_trim(struct bftw_state *state) {
	struct dircache_entry *current = state->current;

	size_t length;
	if (current->depth == 0) {
		// Use exactly the string passed to bftw(), including any
		// trailing slashes
		length = strlen(state->root);
	} else {
		length = current->nameoff + current->namelen;
		if (current->namelen > 1) {
			// Trim the trailing slash
			--length;
			state->last = current->parent;
		}
	}
	dstresize(&state->path, length);

	if (state->status == BFTW_CHILD) {
		state->status = BFTW_CURRENT;
	}
}

/**
 * Open the current directory.
 */
static int bftw_opendir(struct bftw_state *state) {
	assert(!state->dir);

	state->dir = dircache_entry_open(&state->cache, state->current, state->path);
	if (state->dir) {
		return 0;
	} else {
		return 1;
	}
}

/**
 * Close the current directory.
 */
static int bftw_closedir(struct bftw_state *state) {
	DIR *dir = state->dir;
	state->dir = NULL;
	if (dir) {
		return closedir(dir);
	} else {
		return 0;
	}
}

/**
 * Record an error.
 */
static void bftw_set_error(struct bftw_state *state, int error) {
	state->ftwbuf.error = error;
	state->ftwbuf.typeflag = BFTW_ERROR;

	if (!(state->flags & BFTW_RECOVER)) {
		state->error = error;
	}
}

/**
 * Initialize the buffers with data about the current path.
 */
static void bftw_init_buffers(struct bftw_state *state, const struct dirent *de) {
	struct BFTW *ftwbuf = &state->ftwbuf;
	ftwbuf->path = state->path;
	ftwbuf->root = state->root;
	ftwbuf->error = 0;
	ftwbuf->visit = (state->status == BFTW_GC ? BFTW_POST : BFTW_PRE);
	ftwbuf->statbuf = NULL;
	ftwbuf->at_fd = AT_FDCWD;
	ftwbuf->at_path = ftwbuf->path;

	struct dircache_entry *current = state->current;
	if (current) {
		ftwbuf->nameoff = current->nameoff;
		ftwbuf->depth = current->depth;

		if (state->status == BFTW_CHILD) {
			ftwbuf->nameoff += current->namelen;
			++ftwbuf->depth;

			ftwbuf->at_fd = current->fd;
			ftwbuf->at_path += ftwbuf->nameoff;
		} else {
			dircache_entry_base(&state->cache, current, &ftwbuf->at_fd, &ftwbuf->at_path);
		}
	} else {
		ftwbuf->depth = 0;
	}

	if (ftwbuf->depth == 0) {
		// Compute the name offset for root paths like "foo/bar"
		ftwbuf->nameoff = xbasename(ftwbuf->path) - ftwbuf->path;
	}

	ftwbuf->typeflag = BFTW_UNKNOWN;
	if (de) {
		ftwbuf->typeflag = dirent_to_typeflag(de);
	} else if (state->status != BFTW_CHILD) {
		ftwbuf->typeflag = BFTW_DIR;
	}

	int follow_flags = BFTW_LOGICAL;
	if (ftwbuf->depth == 0) {
		follow_flags |= BFTW_COMFOLLOW;
	}
	bool follow = state->flags & follow_flags;
	ftwbuf->at_flags = follow ? 0 : AT_SYMLINK_NOFOLLOW;

	bool detect_cycles = (state->flags & BFTW_DETECT_CYCLES)
		&& state->status == BFTW_CHILD;

	bool xdev = state->flags & BFTW_XDEV;

	if ((state->flags & BFTW_STAT)
	    || ftwbuf->typeflag == BFTW_UNKNOWN
	    || (ftwbuf->typeflag == BFTW_LNK && follow)
	    || (ftwbuf->typeflag == BFTW_DIR && (detect_cycles || xdev))) {
		int ret = ftwbuf_stat(ftwbuf, &state->statbuf);
		if (ret != 0 && follow && errno == ENOENT) {
			// Could be a broken symlink, retry without following
			ftwbuf->at_flags = AT_SYMLINK_NOFOLLOW;
			ret = ftwbuf_stat(ftwbuf, &state->statbuf);
		}

		if (ret != 0) {
			bftw_set_error(state, errno);
			return;
		}

		if (ftwbuf->typeflag == BFTW_DIR && detect_cycles) {
			dev_t dev = ftwbuf->statbuf->st_dev;
			ino_t ino = ftwbuf->statbuf->st_ino;
			for (const struct dircache_entry *entry = current; entry; entry = entry->parent) {
				if (dev == entry->dev && ino == entry->ino) {
					bftw_set_error(state, ELOOP);
					return;
				}
			}
		}
	}
}

/** internal action: Abort the traversal. */
#define BFTW_FAIL (-1)

/**
 * Invoke the callback on the given path.
 */
static int bftw_handle_path(struct bftw_state *state) {
	// Never give the callback BFTW_ERROR unless BFTW_RECOVER is specified
	if (state->ftwbuf.typeflag == BFTW_ERROR && !(state->flags & BFTW_RECOVER)) {
		return BFTW_FAIL;
	}

	// Defensive copy
	struct BFTW ftwbuf = state->ftwbuf;

	enum bftw_action action = state->fn(&ftwbuf, state->ptr);
	switch (action) {
	case BFTW_CONTINUE:
	case BFTW_SKIP_SIBLINGS:
	case BFTW_SKIP_SUBTREE:
	case BFTW_STOP:
		return action;

	default:
		state->error = EINVAL;
		return BFTW_FAIL;
	}
}

/**
 * Add a new entry to the cache.
 */
static struct dircache_entry *bftw_add(struct bftw_state *state, const char *name) {
	struct dircache_entry *entry = dircache_add(&state->cache, state->current, name);
	if (!entry) {
		return NULL;
	}

	const struct stat *statbuf = state->ftwbuf.statbuf;
	if (statbuf) {
		entry->dev = statbuf->st_dev;
		entry->ino = statbuf->st_ino;
	}

	return entry;
}

/**
 * Garbage-collect a dircache entry.
 */
static int bftw_gc(struct bftw_state *state, struct dircache_entry *entry, bool invoke_callback) {
	int ret = BFTW_CONTINUE;

	if (!(state->flags & BFTW_DEPTH)) {
		invoke_callback = false;
	}

	if (entry && invoke_callback) {
		if (bftw_build_path(state) != 0) {
			ret = BFTW_FAIL;
			invoke_callback = false;
		}
	}

	state->status = BFTW_GC;

	while (entry) {
		dircache_entry_decref(&state->cache, entry);
		if (entry->refcount > 0) {
			state->last = entry;
			break;
		}

		if (invoke_callback) {
			state->current = entry;
			bftw_path_trim(state);
			bftw_init_buffers(state, NULL);

			int action = bftw_handle_path(state);
			switch (action) {
			case BFTW_CONTINUE:
			case BFTW_SKIP_SIBLINGS:
			case BFTW_SKIP_SUBTREE:
				break;

			case BFTW_STOP:
			case BFTW_FAIL:
				ret = action;
				invoke_callback = false;
				break;
			}
		}

		struct dircache_entry *parent = entry->parent;
		dircache_entry_free(&state->cache, entry);
		entry = parent;
	}

	return ret;
}

/**
 * Push a new entry onto the queue.
 */
static int bftw_push(struct bftw_state *state, const char *name) {
	struct dircache_entry *entry = bftw_add(state, name);
	if (!entry) {
		return -1;
	}

	if (dirqueue_push(&state->queue, entry) != 0) {
		state->error = errno;
		bftw_gc(state, entry, false);
		return -1;
	}

	return 0;
}

/**
 * Pop an entry off the queue.
 */
static int bftw_pop(struct bftw_state *state, bool invoke_callback) {
	int ret = bftw_gc(state, state->current, invoke_callback);
	state->current = dirqueue_pop(&state->queue);
	state->status = BFTW_CURRENT;
	return ret;
}

/**
 * Dispose of the bftw() state.
 */
static void bftw_state_free(struct bftw_state *state) {
	bftw_closedir(state);

	while (state->current) {
		bftw_pop(state, false);
	}
	dirqueue_free(&state->queue);

	dircache_free(&state->cache);

	dstrfree(state->path);
}

int bftw(const char *path, bftw_fn *fn, int nopenfd, enum bftw_flags flags, void *ptr) {
	int ret = -1, error;

	struct bftw_state state;
	if (bftw_state_init(&state, path, fn, nopenfd, flags, ptr) != 0) {
		return -1;
	}

	// Handle 'path' itself first

	if (bftw_path_concat(&state, path) != 0) {
		goto fail;
	}

	bftw_init_buffers(&state, NULL);

	switch (bftw_handle_path(&state)) {
	case BFTW_CONTINUE:
	case BFTW_SKIP_SIBLINGS:
		break;

	case BFTW_SKIP_SUBTREE:
	case BFTW_STOP:
		goto done;

	case BFTW_FAIL:
		goto fail;
	}

	if (state.ftwbuf.typeflag != BFTW_DIR) {
		goto done;
	}

	// Now start the breadth-first search

	state.current = bftw_add(&state, path);
	if (!state.current) {
		goto fail;
	}

	do {
		if (bftw_build_path(&state) != 0) {
			goto fail;
		}

		if (bftw_opendir(&state) != 0) {
			goto dir_error;
		}

		while (true) {
			struct dirent *de;
			if (xreaddir(state.dir, &de) != 0) {
				goto dir_error;
			}
			if (!de) {
				break;
			}

			if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
				continue;
			}

			if (bftw_path_concat(&state, de->d_name) != 0) {
				goto fail;
			}

			bftw_init_buffers(&state, de);

			switch (bftw_handle_path(&state)) {
			case BFTW_CONTINUE:
				break;

			case BFTW_SKIP_SIBLINGS:
				goto next;

			case BFTW_SKIP_SUBTREE:
				continue;

			case BFTW_STOP:
				goto done;

			case BFTW_FAIL:
				goto fail;
			}

			if (state.ftwbuf.typeflag == BFTW_DIR) {
				const struct stat *statbuf = state.ftwbuf.statbuf;
				if ((flags & BFTW_XDEV)
				    && statbuf
				    && statbuf->st_dev != state.current->dev) {
					continue;
				}

				if (bftw_push(&state, de->d_name) != 0) {
					goto fail;
				}
			}
		}

	next:
		if (bftw_closedir(&state) != 0) {
			goto dir_error;
		}

		switch (bftw_pop(&state, true)) {
		case BFTW_CONTINUE:
		case BFTW_SKIP_SIBLINGS:
		case BFTW_SKIP_SUBTREE:
			break;

		case BFTW_STOP:
			goto done;

		case BFTW_FAIL:
			goto fail;
		}
		continue;

	dir_error:
		error = errno;
		bftw_closedir(&state);
		bftw_path_trim(&state);
		bftw_init_buffers(&state, NULL);
		bftw_set_error(&state, error);

		switch (bftw_handle_path(&state)) {
		case BFTW_CONTINUE:
		case BFTW_SKIP_SIBLINGS:
		case BFTW_SKIP_SUBTREE:
			goto next;

		case BFTW_STOP:
			goto done;

		case BFTW_FAIL:
			goto fail;
		}
	} while (state.current);

done:
	if (state.error == 0) {
		ret = 0;
	}

fail:
	if (state.error == 0) {
		state.error = errno;
	}

	bftw_state_free(&state);

	errno = state.error;
	return ret;
}
