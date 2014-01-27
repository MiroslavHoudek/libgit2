/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#ifndef INCLUDE_global_h__
#define INCLUDE_global_h__

#include "mwindow.h"
#include "hash.h"

typedef struct {
	git_error *last_error;
	git_error error_t;
} git_global_st;

extern int git__global_state(git_global_st **out);

extern git_mutex git__mwindow_mutex;

typedef void (*git_global_shutdown_fn)(void);

extern void git__on_shutdown(git_global_shutdown_fn callback);

#endif
