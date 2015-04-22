/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#ifndef INCLUDE_diff_patch_h__
#define INCLUDE_diff_patch_h__

#include "common.h"
#include "diff.h"
#include "diff_file.h"
#include "array.h"
#include "git2/patch.h"

/* cached information about a hunk in a diff */

typedef struct {
	git_diff_hunk hunk;
	size_t line_start;
	size_t line_count;
} diff_patch_hunk;

struct git_patch {
	git_refcount rc;
	git_repository *repo;
	git_diff_options opts;
	git_diff_delta *delta;
	size_t delta_index;
	git_diff_file_content ofile;
	git_diff_file_content nfile;
	uint32_t flags;
	git_array_t(diff_patch_hunk) hunks;
	git_array_t(git_diff_line)   lines;
	size_t content_size, context_size, header_size;
	git_pool flattened;
};

extern git_diff *git_patch__diff(git_patch *);

extern git_diff_driver *git_patch__driver(git_patch *);

extern void git_patch__old_data(char **, size_t *, git_patch *);
extern void git_patch__new_data(char **, size_t *, git_patch *);

extern int git_patch__invoke_callbacks(
	git_patch *patch,
	git_diff_file_cb file_cb,
	git_diff_hunk_cb hunk_cb,
	git_diff_line_cb line_cb,
	void *payload);

typedef struct git_diff_output git_diff_output;
struct git_diff_output {
	git_diff *diff;

	/* these callbacks are issued with the diff data */
	git_diff_file_cb file_cb;
	git_diff_hunk_cb hunk_cb;
	git_diff_line_cb data_cb;
	void *payload;

	/* this records the actual error in cases where it may be obscured */
	int error;

	/* this callback is used to do the diff and drive the other callbacks.
	 * see diff_xdiff.h for how to use this in practice for now.
	 */
	int (*diff_cb)(git_diff_output *output, git_patch *patch);
};

#endif
