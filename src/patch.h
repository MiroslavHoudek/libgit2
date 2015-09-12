/*
* Copyright (C) the libgit2 contributors. All rights reserved.
*
* This file is part of libgit2, distributed under the GNU GPL v2 with
* a Linking Exception. For full terms see the included COPYING file.
*/
#ifndef INCLUDE_patch_h__
#define INCLUDE_patch_h__

#include "git2/patch.h"
#include "array.h"

/* cached information about a hunk in a patch */
typedef struct git_patch_hunk {
	git_diff_hunk hunk;
	size_t line_start;
	size_t line_count;
} git_patch_hunk;

struct git_patch {
	git_refcount rc;

	git_diff_delta *delta;
	git_diff_binary binary;
	git_array_t(git_patch_hunk) hunks;
	git_array_t(git_diff_line) lines;

	size_t header_size;
	size_t content_size;
	size_t context_size;

	const git_diff_file *(*newfile)(git_patch *patch);
	const git_diff_file *(*oldfile)(git_patch *patch);
	void (*free_fn)(git_patch *patch);
};

extern int git_patch_line_stats(
	size_t *total_ctxt,
	size_t *total_adds,
	size_t *total_dels,
	const git_patch *patch);

extern void git_patch_free(git_patch *patch);


#endif
