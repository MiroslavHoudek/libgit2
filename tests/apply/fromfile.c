#include "clar_libgit2.h"
#include "git2/sys/repository.h"

#include "apply.h"
#include "repository.h"
#include "buf_text.h"

#include "apply_common.h"

static git_repository *repo = NULL;

void test_apply_fromfile__initialize(void)
{
	repo = cl_git_sandbox_init("renames");
}

void test_apply_fromfile__cleanup(void)
{
	cl_git_sandbox_cleanup();
}

static int apply_patchfile(
	const char *old,
	const char *new,
	const char *patchfile,
	const char *filename_expected,
	unsigned int mode_expected)
{
	git_patch *patch;
	git_buf result = GIT_BUF_INIT;
	git_buf patchbuf = GIT_BUF_INIT;
	char *filename;
	unsigned int mode;
	int error;

	cl_git_pass(git_patch_from_patchfile(&patch, patchfile, strlen(patchfile)));

	error = git_apply__patch(&result, &filename, &mode, old, old ? strlen(old) : 0, patch);

	if (error == 0) {
		if (new == NULL)
			cl_assert_equal_i(0, result.size);
		else
			cl_assert_equal_s(new, result.ptr);

		cl_assert_equal_s(filename_expected, filename);
		cl_assert_equal_i(mode_expected, mode);
	}

	git__free(filename);
	git_buf_free(&result);
	git_buf_free(&patchbuf);
	git_patch_free(patch);

	return error;
}

static int validate_and_apply_patchfile(
	const char *old,
	const char *new,
	const char *patchfile,
	const git_diff_options *diff_opts,
	const char *filename_expected,
	unsigned int mode_expected)
{
	git_patch *patch_fromdiff;
	git_buf validated = GIT_BUF_INIT;
	int error;

	cl_git_pass(git_patch_from_buffers(&patch_fromdiff,
		old, old ? strlen(old) : 0, "file.txt",
		new, new ? strlen(new) : 0, "file.txt",
		diff_opts));
	cl_git_pass(git_patch_to_buf(&validated, patch_fromdiff));

	cl_assert_equal_s(patchfile, validated.ptr);

	error = apply_patchfile(old, new, patchfile, filename_expected, mode_expected);

	git_buf_free(&validated);
	git_patch_free(patch_fromdiff);

	return error;
}

void test_apply_fromfile__change_middle(void)
{
	cl_git_pass(validate_and_apply_patchfile(FILE_ORIGINAL,
		FILE_CHANGE_MIDDLE, PATCH_ORIGINAL_TO_CHANGE_MIDDLE, NULL,
		"b/file.txt", 0100644));
}

void test_apply_fromfile__change_middle_nocontext(void)
{
	git_diff_options diff_opts = GIT_DIFF_OPTIONS_INIT;
	diff_opts.context_lines = 0;

	cl_git_pass(validate_and_apply_patchfile(FILE_ORIGINAL,
		FILE_CHANGE_MIDDLE, PATCH_ORIGINAL_TO_CHANGE_MIDDLE_NOCONTEXT,
		&diff_opts, "b/file.txt", 0100644));
}

void test_apply_fromfile__change_firstline(void)
{
	cl_git_pass(validate_and_apply_patchfile(FILE_ORIGINAL,
		FILE_CHANGE_FIRSTLINE, PATCH_ORIGINAL_TO_CHANGE_FIRSTLINE, NULL,
		"b/file.txt", 0100644));
}

void test_apply_fromfile__lastline(void)
{
	cl_git_pass(validate_and_apply_patchfile(FILE_ORIGINAL,
		FILE_CHANGE_LASTLINE, PATCH_ORIGINAL_TO_CHANGE_LASTLINE, NULL,
		"b/file.txt", 0100644));
}

void test_apply_fromfile__prepend(void)
{
	cl_git_pass(validate_and_apply_patchfile(FILE_ORIGINAL, FILE_PREPEND,
		PATCH_ORIGINAL_TO_PREPEND, NULL, "b/file.txt", 0100644));
}

void test_apply_fromfile__prepend_nocontext(void)
{
	git_diff_options diff_opts = GIT_DIFF_OPTIONS_INIT;
	diff_opts.context_lines = 0;

	cl_git_pass(validate_and_apply_patchfile(FILE_ORIGINAL, FILE_PREPEND,
		PATCH_ORIGINAL_TO_PREPEND_NOCONTEXT, &diff_opts,
		"b/file.txt", 0100644));
}

void test_apply_fromfile__append(void)
{
	cl_git_pass(validate_and_apply_patchfile(FILE_ORIGINAL, FILE_APPEND,
		PATCH_ORIGINAL_TO_APPEND, NULL, "b/file.txt", 0100644));
}

void test_apply_fromfile__append_nocontext(void)
{
	git_diff_options diff_opts = GIT_DIFF_OPTIONS_INIT;
	diff_opts.context_lines = 0;

	cl_git_pass(validate_and_apply_patchfile(FILE_ORIGINAL, FILE_APPEND,
		PATCH_ORIGINAL_TO_APPEND_NOCONTEXT, &diff_opts,
		"b/file.txt", 0100644));
}

void test_apply_fromfile__prepend_and_append(void)
{
	cl_git_pass(validate_and_apply_patchfile(FILE_ORIGINAL,
		FILE_PREPEND_AND_APPEND, PATCH_ORIGINAL_TO_PREPEND_AND_APPEND, NULL,
		"b/file.txt", 0100644));
}

void test_apply_fromfile__to_empty_file(void)
{
	cl_git_pass(validate_and_apply_patchfile(FILE_ORIGINAL, "",
		PATCH_ORIGINAL_TO_EMPTY_FILE, NULL, "b/file.txt", 0100644));
}

void test_apply_fromfile__from_empty_file(void)
{
	cl_git_pass(validate_and_apply_patchfile("", FILE_ORIGINAL,
		PATCH_EMPTY_FILE_TO_ORIGINAL, NULL, "b/file.txt", 0100644));
}

void test_apply_fromfile__add(void)
{
	cl_git_pass(validate_and_apply_patchfile(NULL, FILE_ORIGINAL,
		PATCH_ADD_ORIGINAL, NULL, "b/file.txt", 0100644));
}

void test_apply_fromfile__delete(void)
{
	cl_git_pass(validate_and_apply_patchfile(FILE_ORIGINAL, NULL,
		PATCH_DELETE_ORIGINAL, NULL, NULL, 0));
}


void test_apply_fromfile__rename_exact(void)
{
	cl_git_pass(apply_patchfile(FILE_ORIGINAL, FILE_ORIGINAL,
		PATCH_RENAME_EXACT, "b/newfile.txt", 0100644));
}

void test_apply_fromfile__rename_similar(void)
{
	cl_git_pass(apply_patchfile(FILE_ORIGINAL, FILE_CHANGE_MIDDLE,
		PATCH_RENAME_SIMILAR, "b/newfile.txt", 0100644));
}

void test_apply_fromfile__rename_similar_quotedname(void)
{
	cl_git_pass(apply_patchfile(FILE_ORIGINAL, FILE_CHANGE_MIDDLE,
		PATCH_RENAME_SIMILAR_QUOTEDNAME, "b/foo\"bar.txt", 0100644));
}

void test_apply_fromfile__modechange(void)
{
	cl_git_pass(apply_patchfile(FILE_ORIGINAL, FILE_ORIGINAL,
		PATCH_MODECHANGE_UNCHANGED, "b/file.txt", 0100755));
}

void test_apply_fromfile__modechange_with_modification(void)
{
	cl_git_pass(apply_patchfile(FILE_ORIGINAL, FILE_CHANGE_MIDDLE,
		PATCH_MODECHANGE_MODIFIED, "b/file.txt", 0100755));
}

void test_apply_fromfile__noisy(void)
{
	cl_git_pass(apply_patchfile(FILE_ORIGINAL, FILE_CHANGE_MIDDLE,
		PATCH_NOISY, "b/file.txt", 0100644));
}

void test_apply_fromfile__noisy_nocontext(void)
{
	cl_git_pass(apply_patchfile(FILE_ORIGINAL, FILE_CHANGE_MIDDLE,
		PATCH_NOISY_NOCONTEXT, "b/file.txt", 0100644));
}

void test_apply_fromfile__fail_truncated_1(void)
{
	git_patch *patch;
	cl_git_fail(git_patch_from_patchfile(&patch, PATCH_TRUNCATED_1,
		strlen(PATCH_TRUNCATED_1)));
}

void test_apply_fromfile__fail_truncated_2(void)
{
	git_patch *patch;
	cl_git_fail(git_patch_from_patchfile(&patch, PATCH_TRUNCATED_2,
		strlen(PATCH_TRUNCATED_2)));
}

void test_apply_fromfile__fail_truncated_3(void)
{
	git_patch *patch;
	cl_git_fail(git_patch_from_patchfile(&patch, PATCH_TRUNCATED_3,
		strlen(PATCH_TRUNCATED_3)));
}

void test_apply_fromfile__fail_corrupt_githeader(void)
{
	git_patch *patch;
	cl_git_fail(git_patch_from_patchfile(&patch, PATCH_CORRUPT_GIT_HEADER,
		strlen(PATCH_CORRUPT_GIT_HEADER)));
}

void test_apply_fromfile__empty_context(void)
{
	cl_git_pass(apply_patchfile(FILE_EMPTY_CONTEXT_ORIGINAL,
		FILE_EMPTY_CONTEXT_MODIFIED, PATCH_EMPTY_CONTEXT,
		"b/file.txt", 0100644));
}

void test_apply_fromfile__append_no_nl(void)
{
	cl_git_pass(validate_and_apply_patchfile(
		FILE_ORIGINAL, FILE_APPEND_NO_NL, PATCH_APPEND_NO_NL, NULL, "b/file.txt", 0100644));
}

void test_apply_fromfile__fail_missing_new_file(void)
{
	git_patch *patch;
	cl_git_fail(git_patch_from_patchfile(&patch,
		PATCH_CORRUPT_MISSING_NEW_FILE,
		strlen(PATCH_CORRUPT_MISSING_NEW_FILE)));
}

void test_apply_fromfile__fail_missing_old_file(void)
{
	git_patch *patch;
	cl_git_fail(git_patch_from_patchfile(&patch,
		PATCH_CORRUPT_MISSING_OLD_FILE,
		strlen(PATCH_CORRUPT_MISSING_OLD_FILE)));
}

void test_apply_fromfile__fail_no_changes(void)
{
	git_patch *patch;
	cl_git_fail(git_patch_from_patchfile(&patch,
		PATCH_CORRUPT_NO_CHANGES,
		strlen(PATCH_CORRUPT_NO_CHANGES)));
}

void test_apply_fromfile__fail_missing_hunk_header(void)
{
	git_patch *patch;
	cl_git_fail(git_patch_from_patchfile(&patch,
		PATCH_CORRUPT_MISSING_HUNK_HEADER,
		strlen(PATCH_CORRUPT_MISSING_HUNK_HEADER)));
}

void test_apply_fromfile__fail_not_a_patch(void)
{
	git_patch *patch;
	cl_git_fail(git_patch_from_patchfile(&patch, PATCH_NOT_A_PATCH,
		strlen(PATCH_NOT_A_PATCH)));
}
