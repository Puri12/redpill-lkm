#include "intercept_execve.h"
#include "../common.h"
#include "override_symbol.h" //override_syscall()
#include <generated/uapi/asm/unistd_64.h> //syscalls numbers
#include <uapi/linux/limits.h>

#define MAX_INTERCEPTED_FILES 10

static char * intercepted_filenames[MAX_INTERCEPTED_FILES] = { NULL };

int add_blocked_execve_filename(const char * filename)
{
    if (unlikely(strlen(filename) > PATH_MAX))
        return -ENAMETOOLONG;

    unsigned int idx = 0;
    while (likely(intercepted_filenames[idx])) { //Find free spot
        if (unlikely(strcmp(filename, intercepted_filenames[idx]) == 0)) { //Does it exist already?
            pr_loc_bug("File %s was already added at %d", filename, idx);
            return -EEXIST;
        }

        if(unlikely(++idx >= MAX_INTERCEPTED_FILES)) { //Are we out of indexes?
            pr_loc_bug("Tried to add %d intercepted filename (max=%d)", idx, MAX_INTERCEPTED_FILES);
            return -ENOMEM;
        }
    }

    intercepted_filenames[idx] = kmalloc(strlen(filename)+1, GFP_KERNEL);
    strcpy(intercepted_filenames[idx], filename); //Size checked above
    if (!intercepted_filenames[idx]) {
        pr_loc_crt("kmalloc failure!");
        return -ENOMEM;
    }

    pr_loc_inf("Filename %s will be blocked from execution", filename);

    return 0;
}

//These definitions must match SYSCALL_DEFINE3(execve) as in fs/exec.c
asmlinkage long (*org_sys_execve)(const char __user *filename,
                                  const char __user *const __user *argv,
                                  const char __user *const __user *envp);

static asmlinkage long shim_sys_execve(const char __user *filename,
                                       const char __user *const __user *argv,
                                       const char __user *const __user *envp)
{
    pr_loc_dbg("%s: %s %s", __FUNCTION__, filename, argv[0]);

    for (int i = 0; i < MAX_INTERCEPTED_FILES; i++) {
        if (!intercepted_filenames[i])
            break;

        if (unlikely(strcmp(filename, intercepted_filenames[i]) == 0)) {
            pr_loc_inf("Blocked %s from running", filename);
            //We cannot just return 0 here - execve() *does NOT* return on success, but replaces the current process ctx
            do_exit(0);
        }
    }

    return org_sys_execve(filename, argv, envp);
}

int register_execve_interceptor()
{
    //This, according to many sources (e.g. https://stackoverflow.com/questions/8372912/hooking-sys-execve-on-linux-3-x)
    // should NOT work. It does work as we're not calling the sys_execve() directly but through the expected ASM stub...
    //I *think* that's why it work (or I failed to find a scenario where it doesn't yet :D)
    int out = override_syscall(__NR_execve, shim_sys_execve, (void *)&org_sys_execve);
    if (out != 0)
        return out;

    pr_loc_inf("execve() interceptor registered");
    return 0;
}

int unregister_execve_interceptor()
{
    int out = restore_syscall(__NR_execve);
    if (out != 0)
        return out;

    //Free all strings duplicated in add_blocked_execve_filename()
    unsigned int idx = 0;
    while (idx < MAX_INTERCEPTED_FILES-1 && intercepted_filenames[idx]) {
        kfree(intercepted_filenames[idx]);
        intercepted_filenames[idx] = NULL;
        idx++;
    }

    pr_loc_inf("execve() interceptor unregistered");
    return 0;
}
