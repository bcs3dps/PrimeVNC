/*--------------------------------------------------------------------------*/
/* SPDX-License-Identifier: GPL-2.0-or-later                                */
/* Copyright (c) 2026 B. C. Services                                        */
/*                                                                          */
/* Licensed under the GNU General Public License version 2 or later --      */
/* see LICENSE beside this source.  GPL-2.0 full text in GPL-2.0.txt.       */
/*--------------------------------------------------------------------------*/
/* log.c:                                                                   */
/*                                                                          */
/* Implementation of the PrimeVNC diagnostic logger declared in log.h.      */
/* Three severities share one vfprintf-based core; the only difference is   */
/* the prefix and, for LOG_Info, the verbosity gate.  stderr is unbuffered  */
/* and each line is flushed as it is written, so lines from an SSH-launched */
/* instance interleave sensibly with the launcher's own capture.            */
/*--------------------------------------------------------------------------*/

#include "log.h"
#include "primevnc.h"

#include <stdio.h>
#include <stdarg.h>

/*--------------------------------------------------------------------------*/
/* Module state: whether diagnostic (LOG_Info) output is enabled.  Off by   */
/* default so a normally-launched client is quiet on stderr except for real */
/* problems.                                                                */
/*--------------------------------------------------------------------------*/

static int LOG_bVerbose = FALSE;             // set by LOG_SetVerbose

/*--------------------------------------------------------------------------*/
/* LOG_Emit:                                                                */
/*                                                                          */
/* Shared back end for the three public functions.  Writes the prefix, then */
/* the caller's formatted message, then a newline to stderr, and flushes.   */
/* These are three separate writes, not one atomic call; nothing splits a   */
/* line because the client is single-threaded.  Local to this file.         */
/*                                                                          */
/* Arguments:                                                               */
/*     sPrefix : the severity tag already including trailing space.         */
/*     sFmt    : printf format string from the caller.                      */
/*     args    : the caller's va_list, already started.                     */
/*                                                                          */
/* Returns:                                                                 */
/*     void : writes to stderr and returns.                                 */
/*--------------------------------------------------------------------------*/

static void LOG_Emit(const char *sPrefix, const char *sFmt, va_list args)
{
    /* Prefix first so every line is greppable by severity. */
    fputs(sPrefix, stderr);

    /* The caller's message, formatted with the forwarded argument list. */
    vfprintf(stderr, sFmt, args);

    /* One newline per call; callers pass no trailing newline. */
    fputc('\n', stderr);

    /* stderr is unbuffered on most libc, but flush anyway so a crash right   */
    /* after a log call does not lose the line that would explain it.         */
    fflush(stderr);
}

/*--------------------------------------------------------------------------*/
/* LOG_SetVerbose:                                                          */
/*                                                                          */
/* Records the verbosity flag for later LOG_Info calls.  See log.h.         */
/*                                                                          */
/* Arguments:                                                               */
/*     bVerbose : TRUE to enable diagnostic output.                         */
/*                                                                          */
/* Returns:                                                                 */
/*     void : updates module state only.                                    */
/*--------------------------------------------------------------------------*/

void LOG_SetVerbose(int bVerbose)
{
    /* Store the flag; every later LOG_Info consults it. */
    LOG_bVerbose = bVerbose;
}

/*--------------------------------------------------------------------------*/
/* LOG_Error:                                                               */
/*                                                                          */
/* Always-on error line.  See log.h for the contract.                       */
/*                                                                          */
/* Arguments:                                                               */
/*     sFmt : printf format string.                                         */
/*     ...  : format arguments.                                             */
/*                                                                          */
/* Returns:                                                                 */
/*     void : reports through stderr.                                       */
/*--------------------------------------------------------------------------*/

void LOG_Error(const char *sFmt, ...)
{
    va_list args;

    /* Forward the variadic tail to the shared emitter under the error tag. */
    va_start(args, sFmt);
    LOG_Emit("primevnc: error: ", sFmt, args);
    va_end(args);
}

/*--------------------------------------------------------------------------*/
/* LOG_Warn:                                                                */
/*                                                                          */
/* Always-on warning line.  See log.h for the contract.                     */
/*                                                                          */
/* Arguments:                                                               */
/*     sFmt : printf format string.                                         */
/*     ...  : format arguments.                                             */
/*                                                                          */
/* Returns:                                                                 */
/*     void : reports through stderr.                                       */
/*--------------------------------------------------------------------------*/

void LOG_Warn(const char *sFmt, ...)
{
    va_list args;

    /* Same path as LOG_Error, different tag. */
    va_start(args, sFmt);
    LOG_Emit("primevnc: warn: ", sFmt, args);
    va_end(args);
}

/*--------------------------------------------------------------------------*/
/* LOG_Info:                                                                */
/*                                                                          */
/* Diagnostic line, printed only when verbose mode is on.  See log.h.       */
/*                                                                          */
/* Arguments:                                                               */
/*     sFmt : printf format string.                                         */
/*     ...  : format arguments.                                             */
/*                                                                          */
/* Returns:                                                                 */
/*     void : reports through stderr only if verbose.                       */
/*--------------------------------------------------------------------------*/

void LOG_Info(const char *sFmt, ...)
{
    va_list args;

    if (LOG_bVerbose == FALSE)
    {   /* Verbosity off: this is a diagnostic aid, so stay silent. */
        return;
    }

    /* Verbose: emit under the plain prefix (no severity word). */
    va_start(args, sFmt);
    LOG_Emit("primevnc: ", sFmt, args);
    va_end(args);
}
