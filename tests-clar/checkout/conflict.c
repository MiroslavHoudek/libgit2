#include "clar_libgit2.h"
#include "git2/repository.h"
#include "git2/sys/index.h"
#include "fileops.h"

static git_repository *g_repo;
static git_index *g_index;

#define TEST_REPO_PATH "merge-resolve"

#define CONFLICTING_ANCESTOR_OID   "d427e0b2e138501a3d15cc376077a3631e15bd46"
#define CONFLICTING_OURS_OID       "4e886e602529caa9ab11d71f86634bd1b6e0de10"
#define CONFLICTING_THEIRS_OID     "2bd0a343aeef7a2cf0d158478966a6e587ff3863"

#define AUTOMERGEABLE_ANCESTOR_OID "6212c31dab5e482247d7977e4f0dd3601decf13b"
#define AUTOMERGEABLE_OURS_OID     "ee3fa1b8c00aff7fe02065fdb50864bb0d932ccf"
#define AUTOMERGEABLE_THEIRS_OID   "058541fc37114bfc1dddf6bd6bffc7fae5c2e6fe"

#define CONFLICTING_OURS_FILE \
	"this file is changed in master and branch\n"
#define CONFLICTING_THEIRS_FILE \
	"this file is changed in branch and master\n"
#define CONFLICTING_DIFF3_FILE \
	"<<<<<<< ours\n" \
	"this file is changed in master and branch\n" \
	"=======\n" \
	"this file is changed in branch and master\n" \
	">>>>>>> theirs\n"

#define AUTOMERGEABLE_MERGED_FILE \
	"this file is changed in master\n" \
	"this file is automergeable\n" \
	"this file is automergeable\n" \
	"this file is automergeable\n" \
	"this file is automergeable\n" \
	"this file is automergeable\n" \
	"this file is automergeable\n" \
	"this file is automergeable\n" \
	"this file is changed in branch\n"

struct checkout_index_entry {
	uint16_t mode;
	char oid_str[41];
	int stage;
	char path[128];
};

void test_checkout_conflict__initialize(void)
{
	g_repo = cl_git_sandbox_init(TEST_REPO_PATH);
	git_repository_index(&g_index, g_repo);

	cl_git_rewritefile(
		TEST_REPO_PATH "/.gitattributes",
		"* text eol=lf\n");
}

void test_checkout_conflict__cleanup(void)
{
	git_index_free(g_index);
	cl_git_sandbox_cleanup();
}

static void create_index(struct checkout_index_entry *entries, size_t entries_len)
{
	git_buf path = GIT_BUF_INIT;
	size_t i;

	for (i = 0; i < entries_len; i++) {
		git_buf_joinpath(&path, TEST_REPO_PATH, entries[i].path);
		p_unlink(git_buf_cstr(&path));

		git_index_remove_bypath(g_index, entries[i].path);
	}

	for (i = 0; i < entries_len; i++) {
		git_index_entry entry = {0};

		entry.mode = entries[i].mode;
		entry.flags = entries[i].stage << GIT_IDXENTRY_STAGESHIFT;
		git_oid_fromstr(&entry.oid, entries[i].oid_str);
		entry.path = entries[i].path;

		git_index_add(g_index, &entry);
	}

	git_buf_free(&path);
}

static void create_conflicting_index(void)
{
	struct checkout_index_entry checkout_index_entries[] = {
		{ 0100644, CONFLICTING_ANCESTOR_OID, 1, "conflicting.txt" },
		{ 0100644, CONFLICTING_OURS_OID, 2, "conflicting.txt" },
		{ 0100644, CONFLICTING_THEIRS_OID, 3, "conflicting.txt" },
	};

	create_index(checkout_index_entries, 3);
	git_index_write(g_index);
}

static void ensure_workdir_contents(const char *oid_str, const char *path)
{
	git_oid expected, actual;

	cl_git_pass(git_oid_fromstr(&expected, oid_str));
	cl_git_pass(git_repository_hashfile(&actual, g_repo, path, GIT_OBJ_BLOB, NULL));
	cl_assert(git_oid_cmp(&expected, &actual) == 0);
}

void test_checkout_conflict__fails(void)
{
	git_buf conflicting_buf = GIT_BUF_INIT;
	git_checkout_opts opts = GIT_CHECKOUT_OPTS_INIT;

	opts.checkout_strategy |= GIT_CHECKOUT_USE_OURS;

	create_conflicting_index();

	cl_git_pass(git_checkout_index(g_repo, g_index, &opts));

	cl_git_pass(git_futils_readbuffer(&conflicting_buf,
		TEST_REPO_PATH "/conflicting.txt"));
	cl_assert(strcmp(conflicting_buf.ptr, CONFLICTING_OURS_FILE) == 0);
	git_buf_free(&conflicting_buf);
}

void test_checkout_conflict__ignored(void)
{
	git_buf conflicting_buf = GIT_BUF_INIT;
	git_checkout_opts opts = GIT_CHECKOUT_OPTS_INIT;

	opts.checkout_strategy |= GIT_CHECKOUT_SKIP_UNMERGED;

	create_conflicting_index();

	cl_git_pass(git_checkout_index(g_repo, g_index, &opts));

	cl_assert(!git_path_exists(TEST_REPO_PATH "/conflicting.txt"));
}

void test_checkout_conflict__ours(void)
{
	git_buf conflicting_buf = GIT_BUF_INIT;
	git_checkout_opts opts = GIT_CHECKOUT_OPTS_INIT;

	opts.checkout_strategy |= GIT_CHECKOUT_USE_OURS;

	create_conflicting_index();

	cl_git_pass(git_checkout_index(g_repo, g_index, &opts));

	cl_git_pass(git_futils_readbuffer(&conflicting_buf,
		TEST_REPO_PATH "/conflicting.txt"));
	cl_assert(strcmp(conflicting_buf.ptr, CONFLICTING_OURS_FILE) == 0);
	git_buf_free(&conflicting_buf);
}

void test_checkout_conflict__theirs(void)
{
	git_buf conflicting_buf = GIT_BUF_INIT;
	git_checkout_opts opts = GIT_CHECKOUT_OPTS_INIT;

	opts.checkout_strategy |= GIT_CHECKOUT_USE_THEIRS;

	create_conflicting_index();

	cl_git_pass(git_checkout_index(g_repo, g_index, &opts));

	cl_git_pass(git_futils_readbuffer(&conflicting_buf,
		TEST_REPO_PATH "/conflicting.txt"));
	cl_assert(strcmp(conflicting_buf.ptr, CONFLICTING_THEIRS_FILE) == 0);
	git_buf_free(&conflicting_buf);
}

void test_checkout_conflict__diff3(void)
{
	git_buf conflicting_buf = GIT_BUF_INIT;
	git_checkout_opts opts = GIT_CHECKOUT_OPTS_INIT;

	create_conflicting_index();

	cl_git_pass(git_checkout_index(g_repo, g_index, &opts));

	cl_git_pass(git_futils_readbuffer(&conflicting_buf,
		TEST_REPO_PATH "/conflicting.txt"));
	cl_assert(strcmp(conflicting_buf.ptr, CONFLICTING_DIFF3_FILE) == 0);
	git_buf_free(&conflicting_buf);
}

void test_checkout_conflict__automerge(void)
{
	git_buf conflicting_buf = GIT_BUF_INIT;
	git_checkout_opts opts = GIT_CHECKOUT_OPTS_INIT;

	struct checkout_index_entry checkout_index_entries[] = {
		{ 0100644, AUTOMERGEABLE_ANCESTOR_OID, 1, "automergeable.txt" },
		{ 0100644, AUTOMERGEABLE_OURS_OID, 2, "automergeable.txt" },
		{ 0100644, AUTOMERGEABLE_THEIRS_OID, 3, "automergeable.txt" },
	};

	create_index(checkout_index_entries, 3);
	git_index_write(g_index);

	cl_git_pass(git_checkout_index(g_repo, g_index, &opts));

	cl_git_pass(git_futils_readbuffer(&conflicting_buf,
		TEST_REPO_PATH "/automergeable.txt"));
	cl_assert(strcmp(conflicting_buf.ptr, AUTOMERGEABLE_MERGED_FILE) == 0);
	git_buf_free(&conflicting_buf);
}

void test_checkout_conflict__directory_file(void)
{
	git_checkout_opts opts = GIT_CHECKOUT_OPTS_INIT;

	struct checkout_index_entry checkout_index_entries[] = {
		{ 0100644, CONFLICTING_ANCESTOR_OID, 1, "df-1" },
		{ 0100644, CONFLICTING_OURS_OID, 2, "df-1" },
		{ 0100644, CONFLICTING_THEIRS_OID, 0, "df-1/file" },

		{ 0100644, CONFLICTING_ANCESTOR_OID, 1, "df-2" },
		{ 0100644, CONFLICTING_THEIRS_OID, 3, "df-2" },
		{ 0100644, CONFLICTING_OURS_OID, 0, "df-2/file" },

		{ 0100644, CONFLICTING_THEIRS_OID, 3, "df-3" },
		{ 0100644, CONFLICTING_ANCESTOR_OID, 1, "df-3/file" },
		{ 0100644, CONFLICTING_OURS_OID, 2, "df-3/file" },

		{ 0100644, CONFLICTING_OURS_OID, 2, "df-4" },
		{ 0100644, CONFLICTING_ANCESTOR_OID, 1, "df-4/file" },
		{ 0100644, CONFLICTING_THEIRS_OID, 3, "df-4/file" },
	};

	opts.checkout_strategy |= GIT_CHECKOUT_SAFE;

	create_index(checkout_index_entries, 12);
	git_index_write(g_index);

	cl_git_pass(git_checkout_index(g_repo, g_index, &opts));

	ensure_workdir_contents(CONFLICTING_THEIRS_OID, "df-1/file");
	ensure_workdir_contents(CONFLICTING_OURS_OID, "df-1~ours");
	ensure_workdir_contents(CONFLICTING_OURS_OID, "df-2/file");
	ensure_workdir_contents(CONFLICTING_THEIRS_OID, "df-2~theirs");
	ensure_workdir_contents(CONFLICTING_OURS_OID, "df-3/file");
	ensure_workdir_contents(CONFLICTING_THEIRS_OID, "df-3~theirs");
	ensure_workdir_contents(CONFLICTING_OURS_OID, "df-4~ours");
	ensure_workdir_contents(CONFLICTING_THEIRS_OID, "df-4/file");
}

void test_checkout_conflict__directory_file_with_custom_labels(void)
{
	git_checkout_opts opts = GIT_CHECKOUT_OPTS_INIT;

	struct checkout_index_entry checkout_index_entries[] = {
		{ 0100644, CONFLICTING_ANCESTOR_OID, 1, "df-1" },
		{ 0100644, CONFLICTING_OURS_OID, 2, "df-1" },
		{ 0100644, CONFLICTING_THEIRS_OID, 0, "df-1/file" },

		{ 0100644, CONFLICTING_ANCESTOR_OID, 1, "df-2" },
		{ 0100644, CONFLICTING_THEIRS_OID, 3, "df-2" },
		{ 0100644, CONFLICTING_OURS_OID, 0, "df-2/file" },

		{ 0100644, CONFLICTING_THEIRS_OID, 3, "df-3" },
		{ 0100644, CONFLICTING_ANCESTOR_OID, 1, "df-3/file" },
		{ 0100644, CONFLICTING_OURS_OID, 2, "df-3/file" },

		{ 0100644, CONFLICTING_OURS_OID, 2, "df-4" },
		{ 0100644, CONFLICTING_ANCESTOR_OID, 1, "df-4/file" },
		{ 0100644, CONFLICTING_THEIRS_OID, 3, "df-4/file" },
	};

	opts.checkout_strategy |= GIT_CHECKOUT_SAFE;
	opts.our_label = "HEAD";
	opts.their_label = "branch";

	create_index(checkout_index_entries, 12);
	git_index_write(g_index);

	cl_git_pass(git_checkout_index(g_repo, g_index, &opts));

	ensure_workdir_contents(CONFLICTING_THEIRS_OID, "df-1/file");
	ensure_workdir_contents(CONFLICTING_OURS_OID, "df-1~HEAD");
	ensure_workdir_contents(CONFLICTING_OURS_OID, "df-2/file");
	ensure_workdir_contents(CONFLICTING_THEIRS_OID, "df-2~branch");
	ensure_workdir_contents(CONFLICTING_OURS_OID, "df-3/file");
	ensure_workdir_contents(CONFLICTING_THEIRS_OID, "df-3~branch");
	ensure_workdir_contents(CONFLICTING_OURS_OID, "df-4~HEAD");
	ensure_workdir_contents(CONFLICTING_THEIRS_OID, "df-4/file");
}
