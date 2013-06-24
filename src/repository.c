/*
 * Copyright (C) the libgit2 contributors. All rights reserved.
 *
 * This file is part of libgit2, distributed under the GNU GPL v2 with
 * a Linking Exception. For full terms see the included COPYING file.
 */
#include <stdarg.h>
#include <ctype.h>

#include "git2/object.h"
#include "git2/refdb.h"
#include "git2/sys/repository.h"

#include "common.h"
#include "repository.h"
#include "commit.h"
#include "tag.h"
#include "blob.h"
#include "fileops.h"
#include "filebuf.h"
#include "index.h"
#include "config.h"
#include "refs.h"
#include "filter.h"
#include "odb.h"
#include "remote.h"
#include "merge.h"
#include "diff_driver.h"

#define GIT_FILE_CONTENT_PREFIX "gitdir:"

#define GIT_BRANCH_MASTER "master"

#define GIT_REPO_VERSION 0

#define GIT_TEMPLATE_DIR "/usr/share/git-core/templates"

static void set_odb(git_repository *repo, git_odb *odb)
{
	if (odb) {
		GIT_REFCOUNT_OWN(odb, repo);
		GIT_REFCOUNT_INC(odb);
	}

	if ((odb = git__swap(repo->_odb, odb)) != NULL) {
		GIT_REFCOUNT_OWN(odb, NULL);
		git_odb_free(odb);
	}
}

static void set_refdb(git_repository *repo, git_refdb *refdb)
{
	if (refdb) {
		GIT_REFCOUNT_OWN(refdb, repo);
		GIT_REFCOUNT_INC(refdb);
	}

	if ((refdb = git__swap(repo->_refdb, refdb)) != NULL) {
		GIT_REFCOUNT_OWN(refdb, NULL);
		git_refdb_free(refdb);
	}
}

static void set_config(git_repository *repo, git_config *config)
{
	if (config) {
		GIT_REFCOUNT_OWN(config, repo);
		GIT_REFCOUNT_INC(config);
	}

	if ((config = git__swap(repo->_config, config)) != NULL) {
		GIT_REFCOUNT_OWN(config, NULL);
		git_config_free(config);
	}

	git_repository__cvar_cache_clear(repo);
}

static void set_index(git_repository *repo, git_index *index)
{
	if (index) {
		GIT_REFCOUNT_OWN(index, repo);
		GIT_REFCOUNT_INC(index);
	}

	if ((index = git__swap(repo->_index, index)) != NULL) {
		GIT_REFCOUNT_OWN(index, NULL);
		git_index_free(index);
	}
}

void git_repository__cleanup(git_repository *repo)
{
	assert(repo);

	git_cache_clear(&repo->objects);
	git_attr_cache_flush(repo);

	set_config(repo, NULL);
	set_index(repo, NULL);
	set_odb(repo, NULL);
	set_refdb(repo, NULL);
}

void git_repository_free(git_repository *repo)
{
	if (repo == NULL)
		return;

	git_repository__cleanup(repo);

	git_cache_free(&repo->objects);
	git_submodule_config_free(repo);

	git_diff_driver_registry_free(repo->diff_drivers);
	repo->diff_drivers = NULL;

	git__free(repo->path_repository);
	git__free(repo->workdir);
	git__free(repo->namespace);

	git__memzero(repo, sizeof(*repo));
	git__free(repo);
}

/*
 * Git repository open methods
 *
 * Open a repository object from its path
 */
static bool valid_repository_path(git_buf *repository_path)
{
	/* Check OBJECTS_DIR first, since it will generate the longest path name */
	if (git_path_contains_dir(repository_path, GIT_OBJECTS_DIR) == false)
		return false;

	/* Ensure HEAD file exists */
	if (git_path_contains_file(repository_path, GIT_HEAD_FILE) == false)
		return false;

	if (git_path_contains_dir(repository_path, GIT_REFS_DIR)  == false)
		return false;

	return true;
}

static git_repository *repository_alloc(void)
{
	git_repository *repo = git__calloc(1, sizeof(git_repository));
	if (!repo)
		return NULL;

	if (git_cache_init(&repo->objects) < 0) {
		git__free(repo);
		return NULL;
	}

	/* set all the entries in the cvar cache to `unset` */
	git_repository__cvar_cache_clear(repo);

	return repo;
}

int git_repository_new(git_repository **out)
{
	*out = repository_alloc();
	return 0;
}

static int load_config_data(git_repository *repo)
{
	int is_bare;
	git_config *config;

	if (git_repository_config__weakptr(&config, repo) < 0)
		return -1;

	/* Try to figure out if it's bare, default to non-bare if it's not set */
	if (git_config_get_bool(&is_bare, config, "core.bare") < 0)
		repo->is_bare = 0;
	else
		repo->is_bare = is_bare;

	return 0;
}

static int load_workdir(git_repository *repo, git_buf *parent_path)
{
	int         error;
	git_config *config;
	const char *worktree;
	git_buf     worktree_buf = GIT_BUF_INIT;

	if (repo->is_bare)
		return 0;

	if (git_repository_config__weakptr(&config, repo) < 0)
		return -1;

	error = git_config_get_string(&worktree, config, "core.worktree");
	if (!error && worktree != NULL) {
		error = git_path_prettify_dir(
			&worktree_buf, worktree, repo->path_repository);
		if (error < 0)
			return error;
		repo->workdir = git_buf_detach(&worktree_buf);
	}
	else if (error != GIT_ENOTFOUND)
		return error;
	else {
		giterr_clear();

		if (parent_path && git_path_isdir(parent_path->ptr))
			repo->workdir = git_buf_detach(parent_path);
		else {
			git_path_dirname_r(&worktree_buf, repo->path_repository);
			git_path_to_dir(&worktree_buf);
			repo->workdir = git_buf_detach(&worktree_buf);
		}
	}

	GITERR_CHECK_ALLOC(repo->workdir);

	return 0;
}

/*
 * This function returns furthest offset into path where a ceiling dir
 * is found, so we can stop processing the path at that point.
 *
 * Note: converting this to use git_bufs instead of GIT_PATH_MAX buffers on
 * the stack could remove directories name limits, but at the cost of doing
 * repeated malloc/frees inside the loop below, so let's not do it now.
 */
static int find_ceiling_dir_offset(
	const char *path,
	const char *ceiling_directories)
{
	char buf[GIT_PATH_MAX + 1];
	char buf2[GIT_PATH_MAX + 1];
	const char *ceil, *sep;
	size_t len, max_len = 0, min_len;

	assert(path);

	min_len = (size_t)(git_path_root(path) + 1);

	if (ceiling_directories == NULL || min_len == 0)
		return (int)min_len;

	for (sep = ceil = ceiling_directories; *sep; ceil = sep + 1) {
		for (sep = ceil; *sep && *sep != GIT_PATH_LIST_SEPARATOR; sep++);
		len = sep - ceil;

		if (len == 0 || len >= sizeof(buf) || git_path_root(ceil) == -1)
			continue;

		strncpy(buf, ceil, len);
		buf[len] = '\0';

		if (p_realpath(buf, buf2) == NULL)
			continue;

		len = strlen(buf2);
		if (len > 0 && buf2[len-1] == '/')
			buf[--len] = '\0';

		if (!strncmp(path, buf2, len) &&
			path[len] == '/' &&
			len > max_len)
		{
			max_len = len;
		}
	}

	return (int)(max_len <= min_len ? min_len : max_len);
}

/*
 * Read the contents of `file_path` and set `path_out` to the repo dir that
 * it points to.  Before calling, set `path_out` to the base directory that
 * should be used if the contents of `file_path` are a relative path.
 */
static int read_gitfile(git_buf *path_out, const char *file_path)
{
	int     error = 0;
	git_buf file = GIT_BUF_INIT;
	size_t  prefix_len = strlen(GIT_FILE_CONTENT_PREFIX);

	assert(path_out && file_path);

	if (git_futils_readbuffer(&file, file_path) < 0)
		return -1;

	git_buf_rtrim(&file);

	if (git_buf_len(&file) <= prefix_len ||
		memcmp(git_buf_cstr(&file), GIT_FILE_CONTENT_PREFIX, prefix_len) != 0)
	{
		giterr_set(GITERR_REPOSITORY, "The `.git` file at '%s' is malformed", file_path);
		error = -1;
	}
	else if ((error = git_path_dirname_r(path_out, file_path)) >= 0) {
		const char *gitlink = git_buf_cstr(&file) + prefix_len;
		while (*gitlink && git__isspace(*gitlink)) gitlink++;
		error = git_path_prettify_dir(
			path_out, gitlink, git_buf_cstr(path_out));
	}

	git_buf_free(&file);
	return error;
}

static int find_repo(
	git_buf *repo_path,
	git_buf *parent_path,
	const char *start_path,
	uint32_t flags,
	const char *ceiling_dirs)
{
	int error;
	git_buf path = GIT_BUF_INIT;
	struct stat st;
	dev_t initial_device = 0;
	bool try_with_dot_git = false;
	int ceiling_offset;

	git_buf_free(repo_path);

	if ((error = git_path_prettify_dir(&path, start_path, NULL)) < 0)
		return error;

	ceiling_offset = find_ceiling_dir_offset(path.ptr, ceiling_dirs);

	if ((error = git_buf_joinpath(&path, path.ptr, DOT_GIT)) < 0)
		return error;

	while (!error && !git_buf_len(repo_path)) {
		if (p_stat(path.ptr, &st) == 0) {
			/* check that we have not crossed device boundaries */
			if (initial_device == 0)
				initial_device = st.st_dev;
			else if (st.st_dev != initial_device &&
				(flags & GIT_REPOSITORY_OPEN_CROSS_FS) == 0)
				break;

			if (S_ISDIR(st.st_mode)) {
				if (valid_repository_path(&path)) {
					git_path_to_dir(&path);
					git_buf_set(repo_path, path.ptr, path.size);
					break;
				}
			}
			else if (S_ISREG(st.st_mode)) {
				git_buf repo_link = GIT_BUF_INIT;

				if (!(error = read_gitfile(&repo_link, path.ptr))) {
					if (valid_repository_path(&repo_link))
						git_buf_swap(repo_path, &repo_link);

					git_buf_free(&repo_link);
					break;
				}
				git_buf_free(&repo_link);
			}
		}

		/* move up one directory level */
		if (git_path_dirname_r(&path, path.ptr) < 0) {
			error = -1;
			break;
		}

		if (try_with_dot_git) {
			/* if we tried original dir with and without .git AND either hit
			 * directory ceiling or NO_SEARCH was requested, then be done.
			 */
			if (path.ptr[ceiling_offset] == '\0' ||
				(flags & GIT_REPOSITORY_OPEN_NO_SEARCH) != 0)
				break;
			/* otherwise look first for .git item */
			error = git_buf_joinpath(&path, path.ptr, DOT_GIT);
		}
		try_with_dot_git = !try_with_dot_git;
	}

	if (!error && parent_path != NULL) {
		if (!git_buf_len(repo_path))
			git_buf_clear(parent_path);
		else {
			git_path_dirname_r(parent_path, path.ptr);
			git_path_to_dir(parent_path);
		}
		if (git_buf_oom(parent_path))
			return -1;
	}

	git_buf_free(&path);

	if (!git_buf_len(repo_path) && !error) {
		giterr_set(GITERR_REPOSITORY,
			"Could not find repository from '%s'", start_path);
		error = GIT_ENOTFOUND;
	}

	return error;
}

int git_repository_open_bare(
	git_repository **repo_ptr,
	const char *bare_path)
{
	int error;
	git_buf path = GIT_BUF_INIT;
	git_repository *repo = NULL;

	if ((error = git_path_prettify_dir(&path, bare_path, NULL)) < 0)
		return error;

	if (!valid_repository_path(&path)) {
		git_buf_free(&path);
		giterr_set(GITERR_REPOSITORY, "Path is not a repository: %s", bare_path);
		return GIT_ENOTFOUND;
	}

	repo = repository_alloc();
	GITERR_CHECK_ALLOC(repo);

	repo->path_repository = git_buf_detach(&path);
	GITERR_CHECK_ALLOC(repo->path_repository);

	/* of course we're bare! */
	repo->is_bare = 1;
	repo->workdir = NULL;

	*repo_ptr = repo;
	return 0;
}

int git_repository_open_ext(
	git_repository **repo_ptr,
	const char *start_path,
	unsigned int flags,
	const char *ceiling_dirs)
{
	int error;
	git_buf path = GIT_BUF_INIT, parent = GIT_BUF_INIT;
	git_repository *repo;

	if (repo_ptr)
		*repo_ptr = NULL;

	error = find_repo(&path, &parent, start_path, flags, ceiling_dirs);
	if (error < 0 || !repo_ptr)
		return error;

	repo = repository_alloc();
	GITERR_CHECK_ALLOC(repo);

	repo->path_repository = git_buf_detach(&path);
	GITERR_CHECK_ALLOC(repo->path_repository);

	if ((error = load_config_data(repo)) < 0 ||
		(error = load_workdir(repo, &parent)) < 0)
	{
		git_repository_free(repo);
		return error;
	}

	git_buf_free(&parent);
	*repo_ptr = repo;
	return 0;
}

int git_repository_open(git_repository **repo_out, const char *path)
{
	return git_repository_open_ext(
		repo_out, path, GIT_REPOSITORY_OPEN_NO_SEARCH, NULL);
}

int git_repository_wrap_odb(git_repository **repo_out, git_odb *odb)
{
	git_repository *repo;

	repo = repository_alloc();
	GITERR_CHECK_ALLOC(repo);

	git_repository_set_odb(repo, odb);
	*repo_out = repo;

	return 0;
}

int git_repository_discover(
	char *repository_path,
	size_t size,
	const char *start_path,
	int across_fs,
	const char *ceiling_dirs)
{
	git_buf path = GIT_BUF_INIT;
	uint32_t flags = across_fs ? GIT_REPOSITORY_OPEN_CROSS_FS : 0;
	int error;

	assert(start_path && repository_path && size > 0);

	*repository_path = '\0';

	if ((error = find_repo(&path, NULL, start_path, flags, ceiling_dirs)) < 0)
		return error != GIT_ENOTFOUND ? -1 : error;

	if (size < (size_t)(path.size + 1)) {
		giterr_set(GITERR_REPOSITORY,
			"The given buffer is too small to store the discovered path");
		git_buf_free(&path);
		return -1;
	}

	/* success: we discovered a repository */
	git_buf_copy_cstr(repository_path, size, &path);
	git_buf_free(&path);
	return 0;
}

static int load_config(
	git_config **out,
	git_repository *repo,
	const char *global_config_path,
	const char *xdg_config_path,
	const char *system_config_path)
{
	int error;
	git_buf config_path = GIT_BUF_INIT;
	git_config *cfg = NULL;

	assert(repo && out);

	if ((error = git_config_new(&cfg)) < 0)
		return error;

	error = git_buf_joinpath(
		&config_path, repo->path_repository, GIT_CONFIG_FILENAME_INREPO);
	if (error < 0)
		goto on_error;

	if ((error = git_config_add_file_ondisk(
			cfg, config_path.ptr, GIT_CONFIG_LEVEL_LOCAL, 0)) < 0 &&
		error != GIT_ENOTFOUND)
		goto on_error;

	git_buf_free(&config_path);

	if (global_config_path != NULL &&
		(error = git_config_add_file_ondisk(
			cfg, global_config_path, GIT_CONFIG_LEVEL_GLOBAL, 0)) < 0 &&
		error != GIT_ENOTFOUND)
		goto on_error;

	if (xdg_config_path != NULL &&
		(error = git_config_add_file_ondisk(
			cfg, xdg_config_path, GIT_CONFIG_LEVEL_XDG, 0)) < 0 &&
		error != GIT_ENOTFOUND)
		goto on_error;

	if (system_config_path != NULL &&
		(error = git_config_add_file_ondisk(
			cfg, system_config_path, GIT_CONFIG_LEVEL_SYSTEM, 0)) < 0 &&
		error != GIT_ENOTFOUND)
		goto on_error;

	giterr_clear(); /* clear any lingering ENOTFOUND errors */

	*out = cfg;
	return 0;

on_error:
	git_buf_free(&config_path);
	git_config_free(cfg);
	*out = NULL;
	return error;
}

static const char *path_unless_empty(git_buf *buf)
{
	return git_buf_len(buf) > 0 ? git_buf_cstr(buf) : NULL;
}

int git_repository_config__weakptr(git_config **out, git_repository *repo)
{
	int error = 0;

	if (repo->_config == NULL) {
		git_buf global_buf = GIT_BUF_INIT;
		git_buf xdg_buf = GIT_BUF_INIT;
		git_buf system_buf = GIT_BUF_INIT;
		git_config *config;

		git_config_find_global_r(&global_buf);
		git_config_find_xdg_r(&xdg_buf);
		git_config_find_system_r(&system_buf);

		/* If there is no global file, open a backend for it anyway */
		if (git_buf_len(&global_buf) == 0)
			git_config__global_location(&global_buf);

		error = load_config(
			&config, repo,
			path_unless_empty(&global_buf),
			path_unless_empty(&xdg_buf),
			path_unless_empty(&system_buf));
		if (!error) {
			GIT_REFCOUNT_OWN(config, repo);

			config = git__compare_and_swap(&repo->_config, NULL, config);
			if (config != NULL) {
				GIT_REFCOUNT_OWN(config, NULL);
				git_config_free(config);
			}
		}

		git_buf_free(&global_buf);
		git_buf_free(&xdg_buf);
		git_buf_free(&system_buf);
	}

	*out = repo->_config;
	return error;
}

int git_repository_config(git_config **out, git_repository *repo)
{
	if (git_repository_config__weakptr(out, repo) < 0)
		return -1;

	GIT_REFCOUNT_INC(*out);
	return 0;
}

void git_repository_set_config(git_repository *repo, git_config *config)
{
	assert(repo && config);
	set_config(repo, config);
}

int git_repository_odb__weakptr(git_odb **out, git_repository *repo)
{
	int error = 0;

	assert(repo && out);

	if (repo->_odb == NULL) {
		git_buf odb_path = GIT_BUF_INIT;
		git_odb *odb;

		git_buf_joinpath(&odb_path, repo->path_repository, GIT_OBJECTS_DIR);

		error = git_odb_open(&odb, odb_path.ptr);
		if (!error) {
			GIT_REFCOUNT_OWN(odb, repo);

			odb = git__compare_and_swap(&repo->_odb, NULL, odb);
			if (odb != NULL) {
				GIT_REFCOUNT_OWN(odb, NULL);
				git_odb_free(odb);
			}
		}

		git_buf_free(&odb_path);
	}

	*out = repo->_odb;
	return error;
}

int git_repository_odb(git_odb **out, git_repository *repo)
{
	if (git_repository_odb__weakptr(out, repo) < 0)
		return -1;

	GIT_REFCOUNT_INC(*out);
	return 0;
}

void git_repository_set_odb(git_repository *repo, git_odb *odb)
{
	assert(repo && odb);
	set_odb(repo, odb);
}

int git_repository_refdb__weakptr(git_refdb **out, git_repository *repo)
{
	int error = 0;

	assert(out && repo);

	if (repo->_refdb == NULL) {
		git_refdb *refdb;

		error = git_refdb_open(&refdb, repo);
		if (!error) {
			GIT_REFCOUNT_OWN(refdb, repo);

			refdb = git__compare_and_swap(&repo->_refdb, NULL, refdb);
			if (refdb != NULL) {
				GIT_REFCOUNT_OWN(refdb, NULL);
				git_refdb_free(refdb);
			}
		}
	}

	*out = repo->_refdb;
	return error;
}

int git_repository_refdb(git_refdb **out, git_repository *repo)
{
	if (git_repository_refdb__weakptr(out, repo) < 0)
		return -1;

	GIT_REFCOUNT_INC(*out);
	return 0;
}

void git_repository_set_refdb(git_repository *repo, git_refdb *refdb)
{
	assert(repo && refdb);
	set_refdb(repo, refdb);
}

int git_repository_index__weakptr(git_index **out, git_repository *repo)
{
	int error = 0;

	assert(out && repo);

	if (repo->_index == NULL) {
		git_buf index_path = GIT_BUF_INIT;
		git_index *index;

		git_buf_joinpath(&index_path, repo->path_repository, GIT_INDEX_FILE);

		error = git_index_open(&index, index_path.ptr);
		if (!error) {
			GIT_REFCOUNT_OWN(index, repo);

			index = git__compare_and_swap(&repo->_index, NULL, index);
			if (index != NULL) {
				GIT_REFCOUNT_OWN(index, NULL);
				git_index_free(index);
			}

			error = git_index_set_caps(repo->_index, GIT_INDEXCAP_FROM_OWNER);
		}

		git_buf_free(&index_path);
	}

	*out = repo->_index;
	return error;
}

int git_repository_index(git_index **out, git_repository *repo)
{
	if (git_repository_index__weakptr(out, repo) < 0)
		return -1;

	GIT_REFCOUNT_INC(*out);
	return 0;
}

void git_repository_set_index(git_repository *repo, git_index *index)
{
	assert(repo && index);
	set_index(repo, index);
}

int git_repository_set_namespace(git_repository *repo, const char *namespace)
{
	git__free(repo->namespace);

	if (namespace == NULL) {
		repo->namespace = NULL;
		return 0;
	}

	return (repo->namespace = git__strdup(namespace)) ? 0 : -1;
}

const char *git_repository_get_namespace(git_repository *repo)
{
	return repo->namespace;
}

static int check_repositoryformatversion(git_config *config)
{
	int version;

	if (git_config_get_int32(&version, config, "core.repositoryformatversion") < 0)
		return -1;

	if (GIT_REPO_VERSION < version) {
		giterr_set(GITERR_REPOSITORY,
			"Unsupported repository version %d. Only versions up to %d are supported.",
			version, GIT_REPO_VERSION);
		return -1;
	}

	return 0;
}

static int repo_init_create_head(const char *git_dir, const char *ref_name)
{
	git_buf ref_path = GIT_BUF_INIT;
	git_filebuf ref = GIT_FILEBUF_INIT;
	const char *fmt;

	if (git_buf_joinpath(&ref_path, git_dir, GIT_HEAD_FILE) < 0 ||
		git_filebuf_open(&ref, ref_path.ptr, 0) < 0)
		goto fail;

	if (!ref_name)
		ref_name = GIT_BRANCH_MASTER;

	if (git__prefixcmp(ref_name, GIT_REFS_DIR) == 0)
		fmt = "ref: %s\n";
	else
		fmt = "ref: " GIT_REFS_HEADS_DIR "%s\n";

	if (git_filebuf_printf(&ref, fmt, ref_name) < 0 ||
		git_filebuf_commit(&ref, GIT_REFS_FILE_MODE) < 0)
		goto fail;

	git_buf_free(&ref_path);
	return 0;

fail:
	git_buf_free(&ref_path);
	git_filebuf_cleanup(&ref);
	return -1;
}

static bool is_chmod_supported(const char *file_path)
{
	struct stat st1, st2;
	static int _is_supported = -1;

	if (_is_supported > -1)
		return _is_supported;

	if (p_stat(file_path, &st1) < 0)
		return false;

	if (p_chmod(file_path, st1.st_mode ^ S_IXUSR) < 0)
		return false;

	if (p_stat(file_path, &st2) < 0)
		return false;

	_is_supported = (st1.st_mode != st2.st_mode);

	return _is_supported;
}

static bool is_filesystem_case_insensitive(const char *gitdir_path)
{
	git_buf path = GIT_BUF_INIT;
	static int _is_insensitive = -1;

	if (_is_insensitive > -1)
		return _is_insensitive;

	if (git_buf_joinpath(&path, gitdir_path, "CoNfIg") < 0)
		goto cleanup;

	_is_insensitive = git_path_exists(git_buf_cstr(&path));

cleanup:
	git_buf_free(&path);
	return _is_insensitive;
}

static bool are_symlinks_supported(const char *wd_path)
{
	git_buf path = GIT_BUF_INIT;
	int fd;
	struct stat st;
	static int _symlinks_supported = -1;

	if (_symlinks_supported > -1)
		return _symlinks_supported;

	if ((fd = git_futils_mktmp(&path, wd_path)) < 0 ||
		p_close(fd) < 0 ||
		p_unlink(path.ptr) < 0 ||
		p_symlink("testing", path.ptr) < 0 ||
		p_lstat(path.ptr, &st) < 0)
		_symlinks_supported = false;
	else
		_symlinks_supported = (S_ISLNK(st.st_mode) != 0);

	(void)p_unlink(path.ptr);
	git_buf_free(&path);

	return _symlinks_supported;
}

static int create_empty_file(const char *path, mode_t mode)
{
	int fd;

	if ((fd = p_creat(path, mode)) < 0) {
		giterr_set(GITERR_OS, "Error while creating '%s'", path);
		return -1;
	}

	if (p_close(fd) < 0) {
		giterr_set(GITERR_OS, "Error while closing '%s'", path);
		return -1;
	}

	return 0;
}

static int repo_init_config(
	const char *repo_dir,
	const char *work_dir,
	git_repository_init_options *opts)
{
	int error = 0;
	git_buf cfg_path = GIT_BUF_INIT;
	git_config *config = NULL;

#define SET_REPO_CONFIG(TYPE, NAME, VAL) do {\
	if ((error = git_config_set_##TYPE(config, NAME, VAL)) < 0) \
		goto cleanup; } while (0)

	if (git_buf_joinpath(&cfg_path, repo_dir, GIT_CONFIG_FILENAME_INREPO) < 0)
		return -1;

	if (!git_path_isfile(git_buf_cstr(&cfg_path)) &&
		create_empty_file(git_buf_cstr(&cfg_path), GIT_CONFIG_FILE_MODE) < 0) {
			git_buf_free(&cfg_path);
			return -1;
	}

	if (git_config_open_ondisk(&config, git_buf_cstr(&cfg_path)) < 0) {
		git_buf_free(&cfg_path);
		return -1;
	}

	if ((opts->flags & GIT_REPOSITORY_INIT__IS_REINIT) != 0 &&
		(error = check_repositoryformatversion(config)) < 0)
		goto cleanup;

	SET_REPO_CONFIG(
		bool, "core.bare", (opts->flags & GIT_REPOSITORY_INIT_BARE) != 0);
	SET_REPO_CONFIG(
		int32, "core.repositoryformatversion", GIT_REPO_VERSION);
	SET_REPO_CONFIG(
		bool, "core.filemode", is_chmod_supported(git_buf_cstr(&cfg_path)));

	if (!(opts->flags & GIT_REPOSITORY_INIT_BARE)) {
		SET_REPO_CONFIG(bool, "core.logallrefupdates", true);

		if (!are_symlinks_supported(work_dir))
			SET_REPO_CONFIG(bool, "core.symlinks", false);

		if (!(opts->flags & GIT_REPOSITORY_INIT__NATURAL_WD)) {
			SET_REPO_CONFIG(string, "core.worktree", work_dir);
		}
		else if ((opts->flags & GIT_REPOSITORY_INIT__IS_REINIT) != 0) {
			if (git_config_delete_entry(config, "core.worktree") < 0)
				giterr_clear();
		}
	} else {
		if (!are_symlinks_supported(repo_dir))
			SET_REPO_CONFIG(bool, "core.symlinks", false);
	}

	if (!(opts->flags & GIT_REPOSITORY_INIT__IS_REINIT) &&
		is_filesystem_case_insensitive(repo_dir))
		SET_REPO_CONFIG(bool, "core.ignorecase", true);

	if (opts->mode == GIT_REPOSITORY_INIT_SHARED_GROUP) {
		SET_REPO_CONFIG(int32, "core.sharedrepository", 1);
		SET_REPO_CONFIG(bool, "receive.denyNonFastforwards", true);
	}
	else if (opts->mode == GIT_REPOSITORY_INIT_SHARED_ALL) {
		SET_REPO_CONFIG(int32, "core.sharedrepository", 2);
		SET_REPO_CONFIG(bool, "receive.denyNonFastforwards", true);
	}

cleanup:
	git_buf_free(&cfg_path);
	git_config_free(config);

	return error;
}

static int repo_write_template(
	const char *git_dir,
	bool allow_overwrite,
	const char *file,
	mode_t mode,
	bool hidden,
	const char *content)
{
	git_buf path = GIT_BUF_INIT;
	int fd, error = 0, flags;

	if (git_buf_joinpath(&path, git_dir, file) < 0)
		return -1;

	if (allow_overwrite)
		flags = O_WRONLY | O_CREAT | O_TRUNC;
	else
		flags = O_WRONLY | O_CREAT | O_EXCL;

	fd = p_open(git_buf_cstr(&path), flags, mode);

	if (fd >= 0) {
		error = p_write(fd, content, strlen(content));

		p_close(fd);
	}
	else if (errno != EEXIST)
		error = fd;

#ifdef GIT_WIN32
	if (!error && hidden) {
		if (p_hide_directory__w32(path.ptr) < 0)
			error = -1;
	}
#else
	GIT_UNUSED(hidden);
#endif

	git_buf_free(&path);

	if (error)
		giterr_set(GITERR_OS,
			"Failed to initialize repository with template '%s'", file);

	return error;
}

static int repo_write_gitlink(
	const char *in_dir, const char *to_repo)
{
	int error;
	git_buf buf = GIT_BUF_INIT;
	struct stat st;

	git_path_dirname_r(&buf, to_repo);
	git_path_to_dir(&buf);
	if (git_buf_oom(&buf))
		return -1;

	/* don't write gitlink to natural workdir */
	if (git__suffixcmp(to_repo, "/" DOT_GIT "/") == 0 &&
		strcmp(in_dir, buf.ptr) == 0)
	{
		error = GIT_PASSTHROUGH;
		goto cleanup;
	}

	if ((error = git_buf_joinpath(&buf, in_dir, DOT_GIT)) < 0)
		goto cleanup;

	if (!p_stat(buf.ptr, &st) && !S_ISREG(st.st_mode)) {
		giterr_set(GITERR_REPOSITORY,
			"Cannot overwrite gitlink file into path '%s'", in_dir);
		error = GIT_EEXISTS;
		goto cleanup;
	}

	git_buf_clear(&buf);

	error = git_buf_printf(&buf, "%s %s", GIT_FILE_CONTENT_PREFIX, to_repo);

	if (!error)
		error = repo_write_template(in_dir, true, DOT_GIT, 0666, true, buf.ptr);

cleanup:
	git_buf_free(&buf);
	return error;
}

static mode_t pick_dir_mode(git_repository_init_options *opts)
{
	if (opts->mode == GIT_REPOSITORY_INIT_SHARED_UMASK)
		return 0777;
	if (opts->mode == GIT_REPOSITORY_INIT_SHARED_GROUP)
		return (0775 | S_ISGID);
	if (opts->mode == GIT_REPOSITORY_INIT_SHARED_ALL)
		return (0777 | S_ISGID);
	return opts->mode;
}

#include "repo_template.h"

static int repo_init_structure(
	const char *repo_dir,
	const char *work_dir,
	git_repository_init_options *opts)
{
	int error = 0;
	repo_template_item *tpl;
	bool external_tpl =
		((opts->flags & GIT_REPOSITORY_INIT_EXTERNAL_TEMPLATE) != 0);
	mode_t dmode = pick_dir_mode(opts);

	/* Hide the ".git" directory */
#ifdef GIT_WIN32
	if ((opts->flags & GIT_REPOSITORY_INIT__HAS_DOTGIT) != 0) {
		if (p_hide_directory__w32(repo_dir) < 0) {
			giterr_set(GITERR_REPOSITORY,
				"Failed to mark Git repository folder as hidden");
			return -1;
		}
	}
#endif

	/* Create the .git gitlink if appropriate */
	if ((opts->flags & GIT_REPOSITORY_INIT_BARE) == 0 &&
		(opts->flags & GIT_REPOSITORY_INIT__NATURAL_WD) == 0)
	{
		if (repo_write_gitlink(work_dir, repo_dir) < 0)
			return -1;
	}

	/* Copy external template if requested */
	if (external_tpl) {
		git_config *cfg;
		const char *tdir;

		if (opts->template_path)
			tdir = opts->template_path;
		else if ((error = git_config_open_default(&cfg)) < 0)
			return error;
		else {
			error = git_config_get_string(&tdir, cfg, "init.templatedir");

			git_config_free(cfg);

			if (error && error != GIT_ENOTFOUND)
				return error;

			giterr_clear();
			tdir = GIT_TEMPLATE_DIR;
		}

		error = git_futils_cp_r(tdir, repo_dir,
			GIT_CPDIR_COPY_SYMLINKS | GIT_CPDIR_CHMOD_DIRS |
			GIT_CPDIR_SIMPLE_TO_MODE, dmode);

		if (error < 0) {
			if (strcmp(tdir, GIT_TEMPLATE_DIR) != 0)
				return error;

			/* if template was default, ignore error and use internal */
			giterr_clear();
			external_tpl = false;
			error = 0;
		}
	}

	/* Copy internal template
	 * - always ensure existence of dirs
	 * - only create files if no external template was specified
	 */
	for (tpl = repo_template; !error && tpl->path; ++tpl) {
		if (!tpl->content)
			error = git_futils_mkdir(
				tpl->path, repo_dir, dmode, GIT_MKDIR_PATH | GIT_MKDIR_CHMOD);
		else if (!external_tpl) {
			const char *content = tpl->content;

			if (opts->description && strcmp(tpl->path, GIT_DESC_FILE) == 0)
				content = opts->description;

			error = repo_write_template(
				repo_dir, false, tpl->path, tpl->mode, false, content);
		}
	}

	return error;
}

static int mkdir_parent(git_buf *buf, uint32_t mode, bool skip2)
{
	/* When making parent directories during repository initialization
	 * don't try to set gid or grant world write access
	 */
	return git_futils_mkdir(
		buf->ptr, NULL, mode & ~(S_ISGID | 0002),
		GIT_MKDIR_PATH | GIT_MKDIR_VERIFY_DIR |
		(skip2 ? GIT_MKDIR_SKIP_LAST2 : GIT_MKDIR_SKIP_LAST));
}

static int repo_init_directories(
	git_buf *repo_path,
	git_buf *wd_path,
	const char *given_repo,
	git_repository_init_options *opts)
{
	int error = 0;
	bool is_bare, add_dotgit, has_dotgit, natural_wd;
	mode_t dirmode;

	/* There are three possible rules for what we are allowed to create:
	 * - MKPATH means anything we need
	 * - MKDIR means just the .git directory and its parent and the workdir
	 * - Neither means only the .git directory can be created
	 *
	 * There are 5 "segments" of path that we might need to deal with:
	 * 1. The .git directory
	 * 2. The parent of the .git directory
	 * 3. Everything above the parent of the .git directory
	 * 4. The working directory (often the same as #2)
	 * 5. Everything above the working directory (often the same as #3)
	 *
	 * For all directories created, we start with the init_mode value for
	 * permissions and then strip off bits in some cases:
	 *
	 * For MKPATH, we create #3 (and #5) paths without S_ISGID or S_IWOTH
	 * For MKPATH and MKDIR, we create #2 (and #4) without S_ISGID
	 * For all rules, we create #1 using the untouched init_mode
	 */

	/* set up repo path */

	is_bare = ((opts->flags & GIT_REPOSITORY_INIT_BARE) != 0);

	add_dotgit =
		(opts->flags & GIT_REPOSITORY_INIT_NO_DOTGIT_DIR) == 0 &&
		!is_bare &&
		git__suffixcmp(given_repo, "/" DOT_GIT) != 0 &&
		git__suffixcmp(given_repo, "/" GIT_DIR) != 0;

	if (git_buf_joinpath(repo_path, given_repo, add_dotgit ? GIT_DIR : "") < 0)
		return -1;

	has_dotgit = (git__suffixcmp(repo_path->ptr, "/" GIT_DIR) == 0);
	if (has_dotgit)
		opts->flags |= GIT_REPOSITORY_INIT__HAS_DOTGIT;

	/* set up workdir path */

	if (!is_bare) {
		if (opts->workdir_path) {
			if (git_path_join_unrooted(
					wd_path, opts->workdir_path, repo_path->ptr, NULL) < 0)
				return -1;
		} else if (has_dotgit) {
			if (git_path_dirname_r(wd_path, repo_path->ptr) < 0)
				return -1;
		} else {
			giterr_set(GITERR_REPOSITORY, "Cannot pick working directory"
				" for non-bare repository that isn't a '.git' directory");
			return -1;
		}

		if (git_path_to_dir(wd_path) < 0)
			return -1;
	} else {
		git_buf_clear(wd_path);
	}

	natural_wd =
		has_dotgit &&
		wd_path->size > 0 &&
		wd_path->size + strlen(GIT_DIR) == repo_path->size &&
		memcmp(repo_path->ptr, wd_path->ptr, wd_path->size) == 0;
	if (natural_wd)
		opts->flags |= GIT_REPOSITORY_INIT__NATURAL_WD;

	/* create directories as needed / requested */

	dirmode = pick_dir_mode(opts);

	if ((opts->flags & GIT_REPOSITORY_INIT_MKPATH) != 0) {
		/* create path #5 */
		if (wd_path->size > 0 &&
			(error = mkdir_parent(wd_path, dirmode, false)) < 0)
			return error;

		/* create path #3 (if not the same as #5) */
		if (!natural_wd &&
			(error = mkdir_parent(repo_path, dirmode, has_dotgit)) < 0)
			return error;
	}

	if ((opts->flags & GIT_REPOSITORY_INIT_MKDIR) != 0 ||
		(opts->flags & GIT_REPOSITORY_INIT_MKPATH) != 0)
	{
		/* create path #4 */
		if (wd_path->size > 0 &&
			(error = git_futils_mkdir(
				wd_path->ptr, NULL, dirmode & ~S_ISGID,
				GIT_MKDIR_VERIFY_DIR)) < 0)
			return error;

		/* create path #2 (if not the same as #4) */
		if (!natural_wd &&
			(error = git_futils_mkdir(
				repo_path->ptr, NULL, dirmode & ~S_ISGID,
				GIT_MKDIR_VERIFY_DIR | GIT_MKDIR_SKIP_LAST)) < 0)
			return error;
	}

	if ((opts->flags & GIT_REPOSITORY_INIT_MKDIR) != 0 ||
		(opts->flags & GIT_REPOSITORY_INIT_MKPATH) != 0 ||
		has_dotgit)
	{
		/* create path #1 */
		error = git_futils_mkdir(repo_path->ptr, NULL, dirmode,
			GIT_MKDIR_VERIFY_DIR | ((dirmode & S_ISGID) ? GIT_MKDIR_CHMOD : 0));
	}

	/* prettify both directories now that they are created */

	if (!error) {
		error = git_path_prettify_dir(repo_path, repo_path->ptr, NULL);

		if (!error && wd_path->size > 0)
			error = git_path_prettify_dir(wd_path, wd_path->ptr, NULL);
	}

	return error;
}

static int repo_init_create_origin(git_repository *repo, const char *url)
{
	int error;
	git_remote *remote;

	if (!(error = git_remote_create(&remote, repo, GIT_REMOTE_ORIGIN, url))) {
		git_remote_free(remote);
	}

	return error;
}

int git_repository_init(
	git_repository **repo_out, const char *path, unsigned is_bare)
{
	git_repository_init_options opts = GIT_REPOSITORY_INIT_OPTIONS_INIT;

	opts.flags = GIT_REPOSITORY_INIT_MKPATH; /* don't love this default */
	if (is_bare)
		opts.flags |= GIT_REPOSITORY_INIT_BARE;

	return git_repository_init_ext(repo_out, path, &opts);
}

int git_repository_init_ext(
	git_repository **out,
	const char *given_repo,
	git_repository_init_options *opts)
{
	int error;
	git_buf repo_path = GIT_BUF_INIT, wd_path = GIT_BUF_INIT;

	assert(out && given_repo && opts);

	GITERR_CHECK_VERSION(opts, GIT_REPOSITORY_INIT_OPTIONS_VERSION, "git_repository_init_options");

	error = repo_init_directories(&repo_path, &wd_path, given_repo, opts);
	if (error < 0)
		goto cleanup;

	if (valid_repository_path(&repo_path)) {

		if ((opts->flags & GIT_REPOSITORY_INIT_NO_REINIT) != 0) {
			giterr_set(GITERR_REPOSITORY,
				"Attempt to reinitialize '%s'", given_repo);
			error = GIT_EEXISTS;
			goto cleanup;
		}

		opts->flags |= GIT_REPOSITORY_INIT__IS_REINIT;

		error = repo_init_config(
			git_buf_cstr(&repo_path), git_buf_cstr(&wd_path), opts);

		/* TODO: reinitialize the templates */
	}
	else {
		if (!(error = repo_init_structure(
				git_buf_cstr(&repo_path), git_buf_cstr(&wd_path), opts)) &&
			!(error = repo_init_config(
				git_buf_cstr(&repo_path), git_buf_cstr(&wd_path), opts)))
			error = repo_init_create_head(
				git_buf_cstr(&repo_path), opts->initial_head);
	}
	if (error < 0)
		goto cleanup;

	error = git_repository_open(out, git_buf_cstr(&repo_path));

	if (!error && opts->origin_url)
		error = repo_init_create_origin(*out, opts->origin_url);

cleanup:
	git_buf_free(&repo_path);
	git_buf_free(&wd_path);

	return error;
}

int git_repository_head_detached(git_repository *repo)
{
	git_reference *ref;
	git_odb *odb = NULL;
	int exists;

	if (git_repository_odb__weakptr(&odb, repo) < 0)
		return -1;

	if (git_reference_lookup(&ref, repo, GIT_HEAD_FILE) < 0)
		return -1;

	if (git_reference_type(ref) == GIT_REF_SYMBOLIC) {
		git_reference_free(ref);
		return 0;
	}

	exists = git_odb_exists(odb, git_reference_target(ref));

	git_reference_free(ref);
	return exists;
}

int git_repository_head(git_reference **head_out, git_repository *repo)
{
	git_reference *head;
	int error;

	if ((error = git_reference_lookup(&head, repo, GIT_HEAD_FILE)) < 0)
		return error;

	if (git_reference_type(head) == GIT_REF_OID) {
		*head_out = head;
		return 0;
	}

	error = git_reference_lookup_resolved(head_out, repo, git_reference_symbolic_target(head), -1);
	git_reference_free(head);

	return error == GIT_ENOTFOUND ? GIT_EORPHANEDHEAD : error;
}

int git_repository_head_orphan(git_repository *repo)
{
	git_reference *ref = NULL;
	int error;

	error = git_repository_head(&ref, repo);
	git_reference_free(ref);

	if (error == GIT_EORPHANEDHEAD)
		return 1;

	if (error < 0)
		return -1;

	return 0;
}

static int at_least_one_cb(const char *refname, void *payload)
{
	GIT_UNUSED(refname);
	GIT_UNUSED(payload);

	return GIT_EUSER;
}

static int repo_contains_no_reference(git_repository *repo)
{
	int error = git_reference_foreach_name(repo, &at_least_one_cb, NULL);

	if (error == GIT_EUSER)
		return 0;

	if (!error)
		return 1;

	return error;
}

int git_repository_is_empty(git_repository *repo)
{
	git_reference *head = NULL;
	int error;

	if (git_reference_lookup(&head, repo, GIT_HEAD_FILE) < 0)
		return -1;

	if (!(error = git_reference_type(head) == GIT_REF_SYMBOLIC))
		goto cleanup;

	if (!(error = strcmp(
		git_reference_symbolic_target(head),
		GIT_REFS_HEADS_DIR "master") == 0))
			goto cleanup;

	error = repo_contains_no_reference(repo);

cleanup:
	git_reference_free(head);
	return error < 0 ? -1 : error;
}

const char *git_repository_path(git_repository *repo)
{
	assert(repo);
	return repo->path_repository;
}

const char *git_repository_workdir(git_repository *repo)
{
	assert(repo);

	if (repo->is_bare)
		return NULL;

	return repo->workdir;
}

int git_repository_set_workdir(
	git_repository *repo, const char *workdir, int update_gitlink)
{
	int error = 0;
	git_buf path = GIT_BUF_INIT;

	assert(repo && workdir);

	if (git_path_prettify_dir(&path, workdir, NULL) < 0)
		return -1;

	if (repo->workdir && strcmp(repo->workdir, path.ptr) == 0)
		return 0;

	if (update_gitlink) {
		git_config *config;

		if (git_repository_config__weakptr(&config, repo) < 0)
			return -1;

		error = repo_write_gitlink(path.ptr, git_repository_path(repo));

		/* passthrough error means gitlink is unnecessary */
		if (error == GIT_PASSTHROUGH)
			error = git_config_delete_entry(config, "core.worktree");
		else if (!error)
			error = git_config_set_string(config, "core.worktree", path.ptr);

		if (!error)
			error = git_config_set_bool(config, "core.bare", false);
	}

	if (!error) {
		char *old_workdir = repo->workdir;

		repo->workdir = git_buf_detach(&path);
		repo->is_bare = 0;

		git__free(old_workdir);
	}

	return error;
}

int git_repository_is_bare(git_repository *repo)
{
	assert(repo);
	return repo->is_bare;
}

int git_repository_head_tree(git_tree **tree, git_repository *repo)
{
	git_reference *head;
	git_object *obj;
	int error;

	if ((error = git_repository_head(&head, repo)) < 0)
		return error;

	if ((error = git_reference_peel(&obj, head, GIT_OBJ_TREE)) < 0)
		goto cleanup;

	*tree = (git_tree *)obj;

cleanup:
	git_reference_free(head);
	return error;
}

int git_repository_message(char *buffer, size_t len, git_repository *repo)
{
	git_buf buf = GIT_BUF_INIT, path = GIT_BUF_INIT;
	struct stat st;
	int error;

	if (buffer != NULL)
		*buffer = '\0';

	if (git_buf_joinpath(&path, repo->path_repository, GIT_MERGE_MSG_FILE) < 0)
		return -1;

	if ((error = p_stat(git_buf_cstr(&path), &st)) < 0) {
		if (errno == ENOENT)
			error = GIT_ENOTFOUND;
		giterr_set(GITERR_OS, "Could not access message file");
	}
	else if (buffer != NULL) {
		error = git_futils_readbuffer(&buf, git_buf_cstr(&path));
		git_buf_copy_cstr(buffer, len, &buf);
	}

	git_buf_free(&path);
	git_buf_free(&buf);

	if (!error)
		error = (int)st.st_size + 1; /* add 1 for NUL byte */

	return error;
}

int git_repository_message_remove(git_repository *repo)
{
	git_buf path = GIT_BUF_INIT;
	int error;

	if (git_buf_joinpath(&path, repo->path_repository, GIT_MERGE_MSG_FILE) < 0)
		return -1;

	error = p_unlink(git_buf_cstr(&path));
	git_buf_free(&path);

	return error;
}

int git_repository_hashfile(
	git_oid *out,
	git_repository *repo,
	const char *path,
	git_otype type,
	const char *as_path)
{
	int error;
	git_vector filters = GIT_VECTOR_INIT;
	git_file fd = -1;
	git_off_t len;
	git_buf full_path = GIT_BUF_INIT;

	assert(out && path && repo); /* as_path can be NULL */

	/* At some point, it would be nice if repo could be NULL to just
	 * apply filter rules defined in system and global files, but for
	 * now that is not possible because git_filters_load() needs it.
	 */

	error = git_path_join_unrooted(
		&full_path, path, repo ? git_repository_workdir(repo) : NULL, NULL);
	if (error < 0)
		return error;

	if (!as_path)
		as_path = path;

	/* passing empty string for "as_path" indicated --no-filters */
	if (strlen(as_path) > 0) {
		error = git_filters_load(&filters, repo, as_path, GIT_FILTER_TO_ODB);
		if (error < 0)
			return error;
	} else {
		error = 0;
	}

	/* at this point, error is a count of the number of loaded filters */

	fd = git_futils_open_ro(full_path.ptr);
	if (fd < 0) {
		error = fd;
		goto cleanup;
	}

	len = git_futils_filesize(fd);
	if (len < 0) {
		error = (int)len;
		goto cleanup;
	}

	if (!git__is_sizet(len)) {
		giterr_set(GITERR_OS, "File size overflow for 32-bit systems");
		error = -1;
		goto cleanup;
	}

	error = git_odb__hashfd_filtered(out, fd, (size_t)len, type, &filters, as_path);

cleanup:
	if (fd >= 0)
		p_close(fd);
	git_filters_free(&filters);
	git_buf_free(&full_path);

	return error;
}

static bool looks_like_a_branch(const char *refname)
{
	return git__prefixcmp(refname, GIT_REFS_HEADS_DIR) == 0;
}

int git_repository_set_head(
	git_repository* repo,
	const char* refname)
{
	git_reference *ref,
		*new_head = NULL;
	int error;

	assert(repo && refname);

	error = git_reference_lookup(&ref, repo, refname);
	if (error < 0 && error != GIT_ENOTFOUND)
		return error;

	if (!error) {
		if (git_reference_is_branch(ref))
			error = git_reference_symbolic_create(&new_head, repo, GIT_HEAD_FILE, git_reference_name(ref), 1);
		else
			error = git_repository_set_head_detached(repo, git_reference_target(ref));
	} else if (looks_like_a_branch(refname))
		error = git_reference_symbolic_create(&new_head, repo, GIT_HEAD_FILE, refname, 1);

	git_reference_free(ref);
	git_reference_free(new_head);
	return error;
}

int git_repository_set_head_detached(
	git_repository* repo,
	const git_oid* commitish)
{
	int error;
	git_object *object,
		*peeled = NULL;
	git_reference *new_head = NULL;

	assert(repo && commitish);

	if ((error = git_object_lookup(&object, repo, commitish, GIT_OBJ_ANY)) < 0)
		return error;

	if ((error = git_object_peel(&peeled, object, GIT_OBJ_COMMIT)) < 0)
		goto cleanup;

	error = git_reference_create(&new_head, repo, GIT_HEAD_FILE, git_object_id(peeled), 1);

cleanup:
	git_object_free(object);
	git_object_free(peeled);
	git_reference_free(new_head);
	return error;
}

int git_repository_detach_head(
	git_repository* repo)
{
	git_reference *old_head = NULL,
		*new_head = NULL;
	git_object *object = NULL;
	int error;

	assert(repo);

	if ((error = git_repository_head(&old_head, repo)) < 0)
		return error;

	if ((error = git_object_lookup(&object, repo, git_reference_target(old_head), GIT_OBJ_COMMIT)) < 0)
		goto cleanup;

	error = git_reference_create(&new_head, repo, GIT_HEAD_FILE, git_reference_target(old_head), 1);

cleanup:
	git_object_free(object);
	git_reference_free(old_head);
	git_reference_free(new_head);
	return error;
}

/**
 * Loosely ported from git.git
 * https://github.com/git/git/blob/master/contrib/completion/git-prompt.sh#L198-289
 */
int git_repository_state(git_repository *repo)
{
	git_buf repo_path = GIT_BUF_INIT;
	int state = GIT_REPOSITORY_STATE_NONE;

	assert(repo);

	if (git_buf_puts(&repo_path, repo->path_repository) < 0)
		return -1;

	if (git_path_contains_file(&repo_path, GIT_REBASE_MERGE_INTERACTIVE_FILE))
		state = GIT_REPOSITORY_STATE_REBASE_INTERACTIVE;
	else if (git_path_contains_dir(&repo_path, GIT_REBASE_MERGE_DIR))
		state = GIT_REPOSITORY_STATE_REBASE_MERGE;
	else if (git_path_contains_file(&repo_path, GIT_REBASE_APPLY_REBASING_FILE))
		state = GIT_REPOSITORY_STATE_REBASE;
	else if (git_path_contains_file(&repo_path, GIT_REBASE_APPLY_APPLYING_FILE))
		state = GIT_REPOSITORY_STATE_APPLY_MAILBOX;
	else if (git_path_contains_dir(&repo_path, GIT_REBASE_APPLY_DIR))
		state = GIT_REPOSITORY_STATE_APPLY_MAILBOX_OR_REBASE;
	else if (git_path_contains_file(&repo_path, GIT_MERGE_HEAD_FILE))
		state = GIT_REPOSITORY_STATE_MERGE;
	else if(git_path_contains_file(&repo_path, GIT_REVERT_HEAD_FILE))
		state = GIT_REPOSITORY_STATE_REVERT;
	else if(git_path_contains_file(&repo_path, GIT_CHERRY_PICK_HEAD_FILE))
		state = GIT_REPOSITORY_STATE_CHERRY_PICK;
	else if(git_path_contains_file(&repo_path, GIT_BISECT_LOG_FILE))
		state = GIT_REPOSITORY_STATE_BISECT;

	git_buf_free(&repo_path);
	return state;
}

int git_repository_is_shallow(git_repository *repo)
{
	git_buf path = GIT_BUF_INIT;
	struct stat st;
	int error;

	git_buf_joinpath(&path, repo->path_repository, "shallow");
	error = git_path_lstat(path.ptr, &st);
	git_buf_free(&path);

	if (error == GIT_ENOTFOUND)
		return 0;
	if (error < 0)
		return -1;
	return st.st_size == 0 ? 0 : 1;
}
