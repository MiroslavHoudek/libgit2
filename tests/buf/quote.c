#include "clar_libgit2.h"
#include "buffer.h"

static void expect_quote_pass(const char *expected, const char *str)
{
	git_buf buf = GIT_BUF_INIT;

	cl_git_pass(git_buf_puts(&buf, str));
	cl_git_pass(git_buf_quote(&buf));

	cl_assert_equal_s(expected, git_buf_cstr(&buf));
	cl_assert_equal_i(strlen(expected), git_buf_len(&buf));

	git_buf_free(&buf);
}

void test_buf_quote__quote_succeeds(void)
{
	expect_quote_pass("", "");
	expect_quote_pass("foo", "foo");
	expect_quote_pass("foo/bar/baz.c", "foo/bar/baz.c");
	expect_quote_pass("foo bar", "foo bar");
	expect_quote_pass("\"\\\"leading quote\"", "\"leading quote");
	expect_quote_pass("\"slash\\\\y\"", "slash\\y");
	expect_quote_pass("\"foo\\r\\nbar\"", "foo\r\nbar");
	expect_quote_pass("\"foo\\177bar\"", "foo\177bar");
	expect_quote_pass("\"foo\\001bar\"", "foo\001bar");
}

static void expect_unquote_pass(const char *expected, const char *quoted)
{
	git_buf buf = GIT_BUF_INIT;

	cl_git_pass(git_buf_puts(&buf, quoted));
	cl_git_pass(git_buf_unquote(&buf));

	cl_assert_equal_s(expected, git_buf_cstr(&buf));
	cl_assert_equal_i(strlen(expected), git_buf_len(&buf));

	git_buf_free(&buf);
}

static void expect_unquote_fail(const char *quoted)
{
	git_buf buf = GIT_BUF_INIT;

	cl_git_pass(git_buf_puts(&buf, quoted));
	cl_git_fail(git_buf_unquote(&buf));

	git_buf_free(&buf);
}

void test_buf_quote__unquote_succeeds(void)
{
	expect_unquote_pass("", "\"\"");
	expect_unquote_pass(" ", "\" \"");
	expect_unquote_pass("foo", "\"foo\"");
	expect_unquote_pass("foo bar", "\"foo bar\"");
	expect_unquote_pass("foo\"bar", "\"foo\\\"bar\"");
	expect_unquote_pass("foo\\bar", "\"foo\\\\bar\"");
	expect_unquote_pass("foo\tbar", "\"foo\\tbar\"");
	expect_unquote_pass("\vfoo\tbar\n", "\"\\vfoo\\tbar\\n\"");
	expect_unquote_pass("foo\nbar", "\"foo\\012bar\"");
	expect_unquote_pass("foo\r\nbar", "\"foo\\015\\012bar\"");
	expect_unquote_pass("foo\r\nbar", "\"\\146\\157\\157\\015\\012\\142\\141\\162\"");
	expect_unquote_pass("newline: \n", "\"newline: \\012\"");
}

void test_buf_quote__unquote_fails(void)
{
	expect_unquote_fail("no quotes at all");
	expect_unquote_fail("\"no trailing quote");
	expect_unquote_fail("no leading quote\"");
	expect_unquote_fail("\"invalid \\z escape char\"");
	expect_unquote_fail("\"\\q invalid escape char\"");
	expect_unquote_fail("\"invalid escape char \\p\"");
	expect_unquote_fail("\"invalid \\1 escape char \"");
	expect_unquote_fail("\"invalid \\14 escape char \"");
	expect_unquote_fail("\"invalid \\411 escape char\"");
	expect_unquote_fail("\"truncated escape char \\\"");
	expect_unquote_fail("\"truncated escape char \\0\"");
	expect_unquote_fail("\"truncated escape char \\01\"");
}
