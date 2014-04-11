#include "clar_libgit2.h"
#include "posix.h"

#ifdef GIT_WIN32
static bool is_administrator(void)
{
	HANDLE proc = GetCurrentProcess();
	HANDLE proc_token = NULL;
	SID *admin_sid = NULL;
	DWORD len;
	BOOL is_admin = 0;

	cl_assert(admin_sid = LocalAlloc(LMEM_FIXED, SECURITY_MAX_SID_SIZE));

	cl_win32_pass(OpenProcessToken(proc, TOKEN_QUERY, &proc_token));
	cl_win32_pass(CreateWellKnownSid(WinBuiltinAdministratorsSid, NULL, admin_sid, &len));
	cl_win32_pass(CheckTokenMembership(NULL, admin_sid, &is_admin));

	LocalFree(admin_sid);

	if (proc_token)
		CloseHandle(proc_token);

	CloseHandle(proc);

	return is_admin ? true : false;
}
#endif

static void do_symlink(char *old, char *new)
{
#ifndef GIT_WIN32
	cl_must_pass(symlink(old, new));
#else
	typedef DWORD (WINAPI *create_symlink_func)(LPTSTR, LPTSTR, DWORD);
	HMODULE module;
	create_symlink_func pCreateSymbolicLink;

	if (!is_administrator())
		clar__skip();

	cl_assert(module = GetModuleHandle("kernel32"));
	cl_assert(pCreateSymbolicLink = (create_symlink_func)GetProcAddress(module, "CreateSymbolicLinkA"));

	cl_win32_pass(pCreateSymbolicLink(new, old, 0));
#endif
}

static void do_hardlink(const char *old, const char *new)
{
#ifndef GIT_WIN32
	cl_must_pass(link(old, new));
#else
	typedef DWORD (WINAPI *create_hardlink_func)(LPCTSTR, LPCTSTR, LPSECURITY_ATTRIBUTES);
	HMODULE module;
	create_hardlink_func pCreateHardLink;

	if (!is_administrator())
		clar__skip();

	cl_assert(module = GetModuleHandle("kernel32"));
	cl_assert(pCreateHardLink = (create_hardlink_func)GetProcAddress(module, "CreateHardLinkA"));

	cl_win32_pass(pCreateHardLink(new, old, 0));
#endif
}

void test_core_link__stat_symlink(void)
{
	struct stat st;

	cl_git_rewritefile("stat_target", "This is the target of a symbolic link.\n");
	do_symlink("stat_target", "stat_symlink");

	cl_must_pass(p_stat("stat_target", &st));
	cl_assert(S_ISREG(st.st_mode));
	cl_assert_equal_i(39, st.st_size);

	cl_must_pass(p_stat("stat_symlink", &st));
	cl_assert(S_ISREG(st.st_mode));
	cl_assert_equal_i(39, st.st_size);
}

void test_core_link__stat_dangling_symlink(void)
{
	struct stat st;

	do_symlink("stat_nonexistent", "stat_dangling");

	cl_must_fail(p_stat("stat_nonexistent", &st));
	cl_must_fail(p_stat("stat_dangling", &st));
}

void test_core_link__lstat_symlink(void)
{
	struct stat st;

	cl_git_rewritefile("lstat_target", "This is the target of a symbolic link.\n");
	do_symlink("lstat_target", "lstat_symlink");

	cl_must_pass(p_lstat("lstat_target", &st));
	cl_assert(S_ISREG(st.st_mode));
	cl_assert_equal_i(39, st.st_size);

	cl_must_pass(p_lstat("lstat_symlink", &st));
	cl_assert(S_ISLNK(st.st_mode));
	cl_assert_equal_i(strlen("lstat_target"), st.st_size);
}

void test_core_link__lstat_dangling_symlink(void)
{
	struct stat st;

	do_symlink("lstat_nonexistent", "lstat_dangling");

	cl_must_fail(p_lstat("lstat_nonexistent", &st));

	cl_must_pass(p_lstat("lstat_dangling", &st));
	cl_assert(S_ISLNK(st.st_mode));
	cl_assert_equal_i(strlen("lstat_nonexistent"), st.st_size);
}

void test_core_link__stat_hardlink(void)
{
	struct stat st;

	cl_git_rewritefile("stat_hardlink1", "This file has many names!\n");
	do_hardlink("stat_hardlink1", "stat_hardlink2");

	cl_must_pass(p_stat("stat_hardlink1", &st));
	cl_assert(S_ISREG(st.st_mode));
	cl_assert_equal_i(26, st.st_size);

	cl_must_pass(p_stat("stat_hardlink2", &st));
	cl_assert(S_ISREG(st.st_mode));
	cl_assert_equal_i(26, st.st_size);
}

void test_core_link__lstat_hardlink(void)
{
	struct stat st;

	cl_git_rewritefile("lstat_hardlink1", "This file has many names!\n");
	do_hardlink("lstat_hardlink1", "lstat_hardlink2");

	cl_must_pass(p_lstat("lstat_hardlink1", &st));
	cl_assert(S_ISREG(st.st_mode));
	cl_assert_equal_i(26, st.st_size);

	cl_must_pass(p_lstat("lstat_hardlink2", &st));
	cl_assert(S_ISREG(st.st_mode));
	cl_assert_equal_i(26, st.st_size);
}
