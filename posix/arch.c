/*
 *
 * honggfuzz - architecture dependent code (POSIX / SIGNAL)
 * -----------------------------------------
 *
 * Author: Robert Swiecki <swiecki@google.com>
 *
 * Copyright 2010-2018 by Google Inc. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain
 * a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
 * implied. See the License for the specific language governing
 * permissions and limitations under the License.
 *
 */

#include "arch.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/cdefs.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "fuzz.h"
#include "libhfcommon/common.h"
#include "libhfcommon/files.h"
#include "libhfcommon/log.h"
#include "libhfcommon/util.h"
#include "sancov.h"
#include "subproc.h"

struct {
    bool important;
    const char* descr;
} arch_sigs[NSIG] = {
    [0 ...(NSIG - 1)].important = false,
    [0 ...(NSIG - 1)].descr = "UNKNOWN",

    [SIGILL].important = true,
    [SIGILL].descr = "SIGILL",

    [SIGFPE].important = true,
    [SIGFPE].descr = "SIGFPE",

    [SIGSEGV].important = true,
    [SIGSEGV].descr = "SIGSEGV",

    [SIGBUS].important = true,
    [SIGBUS].descr = "SIGBUS",

    /* Is affected from monitorSIGABRT flag */
    [SIGABRT].important = false,
    [SIGABRT].descr = "SIGABRT",

    /* Is affected from tmout_vtalrm flag */
    [SIGVTALRM].important = false,
    [SIGVTALRM].descr = "SIGVTALRM-TMOUT",
};

/*
 * Returns true if a process exited (so, presumably, we can delete an input
 * file)
 */
static bool arch_analyzeSignal(run_t* run, int status) {
    /*
     * Resumed by delivery of SIGCONT
     */
    if (WIFCONTINUED(status)) {
        return false;
    }

    if (WIFEXITED(status) || WIFSIGNALED(status)) {
        sancov_Analyze(run);
    }

    /*
     * Boring, the process just exited
     */
    if (WIFEXITED(status)) {
        LOG_D("Process (pid %d) exited normally with status %d", run->pid, WEXITSTATUS(status));
        return true;
    }

    /*
     * Shouldn't really happen, but, well..
     */
    if (!WIFSIGNALED(status)) {
        LOG_E("Process (pid %d) exited with the following status %d, please report that as a bug",
            run->pid, status);
        return true;
    }

    int termsig = WTERMSIG(status);
    LOG_D("Process (pid %d) killed by signal %d '%s'", run->pid, termsig, strsignal(termsig));
    if (!arch_sigs[termsig].important) {
        LOG_D("It's not that important signal, skipping");
        return true;
    }

    char localtmstr[PATH_MAX];
    util_getLocalTime("%F.%H.%M.%S", localtmstr, sizeof(localtmstr), time(NULL));

    char newname[PATH_MAX];

    /* If dry run mode, copy file with same name into workspace */
    if (run->global->mutate.mutationsPerRun == 0U && run->global->useVerifier) {
        snprintf(newname, sizeof(newname), "%s", run->origFileName);
    } else {
        snprintf(newname, sizeof(newname), "%s/%s.PID.%d.TIME.%s.%s", run->global->io.crashDir,
            arch_sigs[termsig].descr, run->pid, localtmstr, run->global->io.fileExtn);
    }

    LOG_I("Ok, that's interesting, saving input '%s'", newname);

    /*
     * All crashes are marked as unique due to lack of information in POSIX arch
     */
    ATOMIC_POST_INC(run->global->cnts.crashesCnt);
    ATOMIC_POST_INC(run->global->cnts.uniqueCrashesCnt);

    if (files_writeBufToFile(
            newname, run->dynamicFile, run->dynamicFileSz, O_CREAT | O_EXCL | O_WRONLY) == false) {
        LOG_E("Couldn't save crash to '%s'", run->crashFileName);
    }

    return true;
}

pid_t arch_fork(run_t* fuzzer HF_ATTR_UNUSED) {
    return fork();
}

bool arch_launchChild(run_t* run) {
#define ARGS_MAX 512
    const char* args[ARGS_MAX + 2];
    char argData[PATH_MAX];

    char inputFile[PATH_MAX];
    snprintf(inputFile, sizeof(inputFile), "/dev/fd/%d", run->dynamicFileCopyFd);

    int x;
    for (x = 0; x < ARGS_MAX && x < run->global->exe.argc; x++) {
        if (run->global->exe.persistent || run->global->exe.fuzzStdin) {
            args[x] = run->global->exe.cmdline[x];
        } else if (!strcmp(run->global->exe.cmdline[x], _HF_FILE_PLACEHOLDER)) {
            args[x] = inputFile;
        } else if (strstr(run->global->exe.cmdline[x], _HF_FILE_PLACEHOLDER)) {
            const char* off = strstr(run->global->exe.cmdline[x], _HF_FILE_PLACEHOLDER);
            snprintf(argData, sizeof(argData), "%.*s%s", (int)(off - run->global->exe.cmdline[x]),
                run->global->exe.cmdline[x], inputFile);
            args[x] = argData;
        } else {
            args[x] = run->global->exe.cmdline[x];
        }
    }
    args[x++] = NULL;

    LOG_D("Launching '%s' on file '%s'", args[0], inputFile);

    /* alarm persists across forks, so disable it here */
    alarm(0);
    execvp(args[0], (char* const*)args);
    alarm(1);

    return false;
}

void arch_prepareParent(run_t* fuzzer HF_ATTR_UNUSED) {
}

void arch_prepareParentAfterFork(run_t* fuzzer HF_ATTR_UNUSED) {
}

void arch_reapChild(run_t* run) {
    for (;;) {
        if (run->global->exe.persistent) {
            struct pollfd pfd = {
                .fd = run->persistentSock,
                .events = POLLIN,
            };
            int r = poll(&pfd, 1, 250 /* 0.25s */);
            if (r == 0 || (r == -1 && errno == EINTR)) {
                subproc_checkTimeLimit(run);
                subproc_checkTermination(run);
            }
            if (r == -1 && errno != EINTR) {
                PLOG_F("poll(fd=%d)", run->persistentSock);
            }
        }
        if (subproc_persistentModeRoundDone(run) == true) {
            break;
        }

        int status;
        int flags = run->global->exe.persistent ? WNOHANG : 0;
        int ret = waitpid(run->pid, &status, flags);
        if (ret == 0) {
            continue;
        }
        if (ret == -1 && errno == EINTR) {
            subproc_checkTimeLimit(run);
            continue;
        }
        if (ret == -1) {
            PLOG_W("waitpid(pid=%d)", run->pid);
            continue;
        }
        if (ret != run->pid) {
            continue;
        }

        char strStatus[4096];
        if (run->global->exe.persistent && ret == run->persistentPid &&
            (WIFEXITED(status) || WIFSIGNALED(status))) {
            run->persistentPid = 0;
            if (fuzz_isTerminating() == false) {
                LOG_W("Persistent mode: PID %d exited with status: %s", ret,
                    subproc_StatusToStr(status, strStatus, sizeof(strStatus)));
            }
        }

        LOG_D("Process (pid %d) came back with status: %s", run->pid,
            subproc_StatusToStr(status, strStatus, sizeof(strStatus)));

        if (arch_analyzeSignal(run, status)) {
            break;
        }
    }
}

bool arch_archInit(honggfuzz_t* hfuzz) {
    /* Default is true for all platforms except Android */
    arch_sigs[SIGABRT].important = hfuzz->monitorSIGABRT;
    /* Default is false */
    arch_sigs[SIGVTALRM].important = hfuzz->timing.tmoutVTALRM;

    /* Make %'d work */
    setlocale(LC_NUMERIC, "en_US.UTF-8");

    return true;
}

void arch_sigFunc(int sig HF_ATTR_UNUSED) {
    return;
}

bool arch_archThreadInit(run_t* fuzzer HF_ATTR_UNUSED) {
    return true;
}
