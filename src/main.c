/*------------------------------------------------------------------------------*/
/* SPDX-License-Identifier: GPL-2.0-or-later                                    */
/* Copyright (c) 2026 B. C. Services                                            */
/*                                                                              */
/* Licensed under the GNU General Public License version 2 or later --          */
/* see LICENSE beside this source.  GPL-2.0 full text in GPL-2.0.txt.           */
/*------------------------------------------------------------------------------*/
/* main.c:                                                                      */
/*                                                                              */
/* PrimeVNC entry point: parse the command line, resolve the password (bare on  */
/* argv, or from an environment variable or a file descriptor when a launcher   */
/* prefers to keep it out of the process list), open the framebuffer and the    */
/* touchscreen, then run a reconnect loop of RFB sessions.  Each session        */
/* multiplexes the VNC socket against the touch device in one single-threaded   */
/* poll() control loop - pixels in to /dev/fb0, touch out as RFB PointerEvents. */
/*                                                                              */
/* When the loop finally exits - a clean stop, an unrecoverable session, or     */
/* exhausted reconnects - the process simply ends with an exit code.  It        */
/* knows nothing about what owns the display before or after it runs;           */
/* arranging that is the launcher's job (a wrapper script, or the host that     */
/* starts it over SSH), entirely outside this program.                          */
/*                                                                              */
/* The panel BACKLIGHT is the one display property beyond the pixels that       */
/* this program touches, because its previous owner may have left the panel     */
/* dark: at takeover it lights the panel by default (--backlight), it can       */
/* darken the panel after a long all-black frame and re-light it on the         */
/* first lit one (--backlight-auto), and it can be run as a one-shot that       */
/* only sets the brightness and exits (--backlight-set) for a host that         */
/* drives the panel over SSH while another instance is showing pixels.          */
/*------------------------------------------------------------------------------*/

#include "primevnc.h"
#include "log.h"
#include "net.h"
#include "framebuffer.h"
#include "input.h"
#include "rfb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>

/*--------------------------------------------------------------------------*/
/* Tunables and result codes.  The idle poll timeout drives the LIVENESS    */
/* rule: when nothing has happened for that long the loop sends a heartbeat */
/* (a non-incremental 1x1 update request, which a server MUST answer), and  */
/* counts it as unanswered until any message arrives.  A host that vanished */
/* without a FIN/RST never answers - its TCP writes still succeed into the  */
/* send buffer for ~15 minutes - so after PVNC_HEARTBEAT_MISSES unanswered  */
/* heartbeats in a row the session is declared dead and reconnects (and,    */
/* once reconnects are exhausted, the process exits).  An incremental       */
/* request is NOT a probe: an idle server legitimately holds it for ever.   */
/*--------------------------------------------------------------------------*/

#define PVNC_POLL_IDLE_MS      8000          // idle wait before a heartbeat probe
#define PVNC_HEARTBEAT_MISSES  3             // unanswered heartbeats in a row that declare the server gone
#define PVNC_BACKOFF_CAP_MS    8000          // maximum reconnect backoff
#define PVNC_MAX_POINTER_EVTS  64            // pointer events drained per input wakeup

#define PVNC_RESULT_STOP        0            // loop ended by signal: do not reconnect
#define PVNC_RESULT_RETRY       1            // connect/handshake failed: reconnect if allowed, counting this outage
#define PVNC_RESULT_RETRY_RAN   2            // a session RAN then dropped: reconnect, counting a NEW outage from zero

#define PVNC_EXIT_OK            0            // clean exit
#define PVNC_EXIT_USAGE         1            // bad command line
#define PVNC_EXIT_FB            2            // framebuffer could not be opened
#define PVNC_EXIT_INPUT         3            // touch device could not be opened

/*------------------------------------------------------------------------------*/
/* Stop flag, set by SIGINT / SIGTERM so a clean shutdown (the launcher killing */
/* the client, or Ctrl-C in testing) unwinds through the normal exit path and   */
/* releases the devices cleanly.  sig_atomic_t + volatile is the only state a   */
/* signal handler may safely touch.                                             */
/*------------------------------------------------------------------------------*/

static volatile sig_atomic_t PVNC_bStop = 0;  // set to 1 on a termination signal

/*---------------------------------------------------------------------------*/
/* PVNC_OnSignal:                                                            */
/*                                                                           */
/* Signal handler for SIGINT / SIGTERM.  Sets the stop flag and returns; the */
/* control loop notices at its next poll and exits cleanly.  Local.          */
/*                                                                           */
/* Arguments:                                                                */
/*     iSig : the delivered signal number (unused; one handler for both).    */
/*                                                                           */
/* Returns:                                                                  */
/*     void : sets the stop flag only.                                       */
/*---------------------------------------------------------------------------*/

static void PVNC_OnSignal(int iSig)
{
    int iSaved;                                // preserve errno across the handler

    /* Signals can arrive mid-syscall; do not clobber errno for the interrupted   */
    /* call that will inspect it.                                                 */
    iSaved = errno;

    /* The dummy read keeps the compiler from warning about the unused argument   */
    /* without a (void) cast, per the coding style.                               */
    if (iSig != 0)
    {   /* Any termination signal means the same thing: wind down. */
        PVNC_bStop = 1;
    }

    errno = iSaved;
}

/*--------------------------------------------------------------------------*/
/* PVNC_Usage:                                                              */
/*                                                                          */
/* Prints the command-line usage to the given stream.  Local.               */
/*                                                                          */
/* Arguments:                                                               */
/*     pStream : where to write (stdout for --help, stderr for an error).   */
/*                                                                          */
/* Returns:                                                                 */
/*     void : writes usage text.                                            */
/*--------------------------------------------------------------------------*/

static void PVNC_Usage(FILE *pStream)
{
    fprintf(pStream,
        "PrimeVNC %s - framebuffer VNC client for Allwinner sunxi Linux devices\n"
        "\n"
        "Usage: primevnc <server-ip> [options]\n"
        "\n"
        "  <server-ip>              numeric IPv4 of the VNC server (no DNS)\n"
        "  --port N                 RFB port (default 5900)\n"
        "  --fb PATH                framebuffer device (default /dev/fb0)\n"
        "  --input PATH             evdev touch node (default: autodetect)\n"
        "  --password P             VNC password on the command line (visible in ps)\n"
        "  --password-env NAME      read the VNC password from env var NAME\n"
        "  --password-fd N          read the VNC password from file descriptor N\n"
        "  --rotate 0|90|180|270    rotate touch mapping to the panel (default 0)\n"
        "  --reconnect-max N        reconnect attempts; -1 = forever (default 10)\n"
        "  --reconnect-delay MS     base reconnect backoff (default 1000)\n"
        "  --backlight on|off|keep|N panel brightness at takeover (default on): on = light\n"
        "                           it if dark, off = dark, keep = leave it, N = level 1..255;\n"
        "                           a lit level this program changed is restored at exit\n"
        "  --backlight-auto off|on|S turn the panel off after S seconds of an all-black frame\n"
        "                           (on = 60) and back on at the first lit frame (default off)\n"
        "  --backlight-set on|off|N set the brightness and exit at once - no server, no session;\n"
        "                           for a host driving the panel over ssh while a client runs\n"
        "  -v, --verbose            diagnostic logging to stderr\n"
        "  -h, --help               this help\n"
        "  --version                print version and exit\n",
        PVNC_VERSION);
}

/*--------------------------------------------------------------------------*/
/* PVNC_ArgValue:                                                           */
/*                                                                          */
/* Fetches the value that must follow an option, reporting a usage error if */
/* it is missing.  Advances the caller's index past the value.  Local.      */
/*                                                                          */
/* Arguments:                                                               */
/*     iArgc  : argument count.                                             */
/*     ppArgv : argument vector.                                            */
/*     piPos  : in/out; index of the option, advanced to its value.         */
/*                                                                          */
/* Returns:                                                                 */
/*     const char * : the value string, or NULL if none followed.           */
/*--------------------------------------------------------------------------*/

static const char *PVNC_ArgValue(int iArgc, char **ppArgv, int *piPos)
{
    if ((*piPos + 1) >= iArgc)
    {   /* Option was the last token with no value after it. */
        LOG_Error("option '%s' requires a value", ppArgv[*piPos]);
        return(NULL);
    }

    /* Step onto the value. */
    (*piPos)++;
    /* Hand the value back to the caller. */
    return(ppArgv[*piPos]);
}

/*--------------------------------------------------------------------------*/
/* PVNC_SetDefaults:                                                        */
/*                                                                          */
/* Fills a config with the built-in defaults before argv overrides them.    */
/* Local.                                                                   */
/*                                                                          */
/* Arguments:                                                               */
/*     pCfg : config to initialise.                                         */
/*                                                                          */
/* Returns:                                                                 */
/*     void : sets every field to its default.                              */
/*--------------------------------------------------------------------------*/

static void PVNC_SetDefaults(PVNC_Config *pCfg)
{
    memset(pCfg, 0, sizeof(*pCfg));

    pCfg->cfg_iPort              = 5900;                                       // display :0
    snprintf(pCfg->cfg_sFbPath, sizeof(pCfg->cfg_sFbPath), "%s", "/dev/fb0");  // default framebuffer device
    pCfg->cfg_sInputPath[0]      = '\0';                                       // empty => autodetect
    pCfg->cfg_bHavePassword      = FALSE;                                      // no password until an option supplies one
    pCfg->cfg_iRotate            = 0;                                          // no touch rotation by default
    pCfg->cfg_bVerbose           = FALSE;                                      // quiet: only errors and warnings
    pCfg->cfg_iReconnectMax      = 10;                                         // bounded, so a dead server eventually ends the process
    pCfg->cfg_iReconnectDelayMs  = 1000;                                       // base reconnect backoff, ms (doubles, capped)
    pCfg->cfg_iBacklightMode     = FB_BACKLIGHT_ON;                            // a dark panel is lit at takeover
    pCfg->cfg_iBacklightLevel    = FB_BACKLIGHT_DEFAULT;                       // used only by an explicit level
    pCfg->cfg_bBacklightSetOnly  = FALSE;                                      // run a session, not the one-shot --backlight-set
    pCfg->cfg_iBacklightAutoMs   = 0;                                          // the black-frame rule is opt-in
}

/*--------------------------------------------------------------------------*/
/* PVNC_ParseBacklight:                                                     */
/*                                                                          */
/* Turns a --backlight or --backlight-set value into a policy and a level:  */
/* "on", "off", "keep", or a whole number 1..255 (an explicit level).       */
/* Local.                                                                   */
/*                                                                          */
/* Arguments:                                                               */
/*     pVal    : the option's value string.                                 */
/*     piMode  : out; one of framebuffer.h's FB_BACKLIGHT_* policies.       */
/*     piLevel : out; the level for FB_BACKLIGHT_LEVEL, else unchanged.     */
/*                                                                          */
/* Returns:                                                                 */
/*     int : SUCCESS, or FAILURE (logged) for anything else.                */
/*--------------------------------------------------------------------------*/

static int PVNC_ParseBacklight(const char *pVal, int *piMode, int *piLevel)
{
    char *pEnd;                                // strtol's stop position
    long  lLevel;                              // the parsed number

    if (strcmp(pVal, "on") == 0)
    {   /* Light a dark panel, leave a lit one alone. */
        *piMode = FB_BACKLIGHT_ON;
        return(SUCCESS);
    }

    if (strcmp(pVal, "off") == 0)
    {   /* Dark. */
        *piMode = FB_BACKLIGHT_OFF;
        return(SUCCESS);
    }

    if (strcmp(pVal, "keep") == 0)
    {   /* Hands off. */
        *piMode = FB_BACKLIGHT_KEEP;
        return(SUCCESS);
    }

    lLevel = strtol(pVal, &pEnd, 10);

    if ((pEnd == pVal) || (*pEnd != '\0') || (lLevel < 1) || (lLevel > FB_BACKLIGHT_MAX))
    {   /* Not one of the words and not a whole number in 1..255. */
        LOG_Error("backlight value '%s' is not on, off, keep or a level 1..%d",
                  pVal, FB_BACKLIGHT_MAX);
        return(FAILURE);
    }

    /* An explicit level: report it through the out-parameters. */
    *piMode  = FB_BACKLIGHT_LEVEL;
    *piLevel = (int)lLevel;
    /* Success; the mode and level are in the out-parameters. */
    return(SUCCESS);
}

/*--------------------------------------------------------------------------*/
/* PVNC_ParseBacklightAuto:                                                 */
/*                                                                          */
/* Turns a --backlight-auto value into the rule's timeout in ms: "off" is   */
/* 0 (rule off), "on" is the default timeout, a whole number is that many   */
/* seconds of an all-black frame.  Local.                                   */
/*                                                                          */
/* Arguments:                                                               */
/*     pVal : the option's value string.                                    */
/*     piMs : out; the timeout in ms, 0 for off.                            */
/*                                                                          */
/* Returns:                                                                 */
/*     int : SUCCESS, or FAILURE (logged) for anything else.                */
/*--------------------------------------------------------------------------*/

static int PVNC_ParseBacklightAuto(const char *pVal, int *piMs)
{
    char *pEnd;                                // strtol's stop position
    long  lSeconds;                            // the parsed number

    if (strcmp(pVal, "off") == 0)
    {   /* Rule off. */
        *piMs = 0;
        return(SUCCESS);
    }

    if (strcmp(pVal, "on") == 0)
    {   /* Rule on at the default timeout. */
        *piMs = FB_BACKLIGHT_AUTO_DEFAULT_MS;
        return(SUCCESS);
    }

    lSeconds = strtol(pVal, &pEnd, 10);

    if ((pEnd == pVal) || (*pEnd != '\0') || (lSeconds < 1) || (lSeconds > 86400))
    {   /* Not a word above and not a whole number of seconds within a day. */
        LOG_Error("backlight-auto value '%s' is not off, on or seconds 1..86400", pVal);
        return(FAILURE);
    }

    /* Seconds to milliseconds. */
    *piMs = (int)(lSeconds * 1000L);
    /* Success; the timeout is in *piMs. */
    return(SUCCESS);
}

/*--------------------------------------------------------------------------*/
/* PVNC_ReadPasswordFromFd:                                                 */
/*                                                                          */
/* Reads a password from an open file descriptor up to a newline or EOF and */
/* stores it in the config, stripping a trailing newline.  Used for the     */
/* --password-fd form a launcher feeds over an SSH channel.  Local.         */
/*                                                                          */
/* Arguments:                                                               */
/*     pCfg : config whose password buffer is filled.                       */
/*     iFd  : descriptor to read from.                                      */
/*                                                                          */
/* Returns:                                                                 */
/*     int : SUCCESS if something was read, FAILURE otherwise.              */
/*--------------------------------------------------------------------------*/

static int PVNC_ReadPasswordFromFd(PVNC_Config *pCfg, int iFd)
{
    char    arrBuf[PVNC_MAX_PASSWORD];         // landing buffer
    ssize_t iGot;                              // bytes read
    int     iLen;                              // usable length after trimming

    /* One read is enough for a password; leave room for the terminator. */
    iGot = read(iFd, arrBuf, sizeof(arrBuf) - 1);

    if (iGot <= 0)
    {   /* Nothing to read, or a read error. */
        LOG_Error("could not read password from fd %d", iFd);
        return(FAILURE);
    }

    arrBuf[iGot] = '\0';
    iLen         = (int)iGot;

    /* Strip any trailing newline or carriage-return characters. */
    while ((iLen > 0) && ((arrBuf[iLen - 1] == '\n') || (arrBuf[iLen - 1] == '\r')))
    {   /* One trailing newline or CR removed per iteration. */
        arrBuf[iLen - 1] = '\0';
        iLen--;
    }

    /* Copy the cleaned password into the config. */
    snprintf(pCfg->cfg_sPassword, sizeof(pCfg->cfg_sPassword), "%s", arrBuf);
    pCfg->cfg_bHavePassword = TRUE;

    /* Scrub the local copy. */
    memset(arrBuf, 0, sizeof(arrBuf));
    /* Success; the password is now in the config. */
    return(SUCCESS);
}

/*----------------------------------------------------------------------------*/
/* PVNC_ParseArgs:                                                            */
/*                                                                            */
/* Parses argv into the config.  Handles --help / --version by printing and   */
/* signalling the caller to exit.  Local.                                     */
/*                                                                            */
/* Arguments:                                                                 */
/*     iArgc     : argument count.                                            */
/*     ppArgv    : argument vector.                                           */
/*     pCfg      : config to populate (defaults already set by the caller).   */
/*     pbExitNow : out; set TRUE when the program should exit immediately     */
/*                 (help/version) with *piExitCode.                           */
/*     piExitCode: out; the exit code to use when *pbExitNow is TRUE.         */
/*                                                                            */
/* Returns:                                                                   */
/*     int : SUCCESS if the command line was valid, FAILURE on a usage error. */
/*----------------------------------------------------------------------------*/

static int PVNC_ParseArgs(int iArgc, char **ppArgv, PVNC_Config *pCfg,
                          int *pbExitNow, int *piExitCode)
{
    const char *pVal;                          // an option's value
    int         bHaveServer;                   // TRUE once the positional IP is seen
    int         i;

    *pbExitNow  = FALSE;
    *piExitCode = PVNC_EXIT_OK;
    bHaveServer = FALSE;

    for (i = 1; i < iArgc; i++)
    {   /* Walk the tokens; options start with '-', the bare one is the server IP. */
        char *pArg = ppArgv[i];

        if (strcmp(pArg, "-h") == 0 || strcmp(pArg, "--help") == 0)
        {   /* Help to stdout, then a clean exit. */
            PVNC_Usage(stdout);
            *pbExitNow  = TRUE;
            *piExitCode = PVNC_EXIT_OK;
            return(SUCCESS);
        }

        if (strcmp(pArg, "--version") == 0)
        {   /* Version to stdout, then a clean exit. */
            printf("primevnc %s\n", PVNC_VERSION);
            *pbExitNow  = TRUE;
            *piExitCode = PVNC_EXIT_OK;
            return(SUCCESS);
        }

        if (strcmp(pArg, "-v") == 0 || strcmp(pArg, "--verbose") == 0)
        {   /* Enable diagnostic logging. */
            pCfg->cfg_bVerbose = TRUE;
            continue;
        }

        if (strcmp(pArg, "--port") == 0)
        {   /* TCP port for the RFB connection. */
            pVal = PVNC_ArgValue(iArgc, ppArgv, &i);

            if (pVal == NULL)
            {   /* The option had no value; PVNC_ArgValue already logged it. */
                return(FAILURE);
            }

            pCfg->cfg_iPort = (int)strtol(pVal, NULL, 10);
            continue;
        }

        if (strcmp(pArg, "--fb") == 0)
        {   /* Framebuffer device path. */
            pVal = PVNC_ArgValue(iArgc, ppArgv, &i);

            if (pVal == NULL)
            {   /* The option had no value; PVNC_ArgValue already logged it. */
                return(FAILURE);
            }

            snprintf(pCfg->cfg_sFbPath, sizeof(pCfg->cfg_sFbPath), "%s", pVal);
            continue;
        }

        if (strcmp(pArg, "--input") == 0)
        {   /* Explicit touch device path (else autodetect). */
            pVal = PVNC_ArgValue(iArgc, ppArgv, &i);

            if (pVal == NULL)
            {   /* The option had no value; PVNC_ArgValue already logged it. */
                return(FAILURE);
            }

            snprintf(pCfg->cfg_sInputPath, sizeof(pCfg->cfg_sInputPath), "%s", pVal);
            continue;
        }

        if (strcmp(pArg, "--password") == 0)
        {   /* The password given bare on the command line (visible in ps; the   */
            /* env and fd forms exist for a launcher that minds).                */
            pVal = PVNC_ArgValue(iArgc, ppArgv, &i);

            if (pVal == NULL)
            {   /* The option had no value; PVNC_ArgValue already logged it. */
                return(FAILURE);
            }

            snprintf(pCfg->cfg_sPassword, sizeof(pCfg->cfg_sPassword), "%s", pVal);
            pCfg->cfg_bHavePassword = TRUE;
            continue;
        }

        if (strcmp(pArg, "--password-env") == 0)
        {   /* Read the password from the named environment variable. */
            pVal = PVNC_ArgValue(iArgc, ppArgv, &i);

            if (pVal == NULL)
            {   /* The option had no value; PVNC_ArgValue already logged it. */
                return(FAILURE);
            }

            {
                const char *pEnv = getenv(pVal);

                if (pEnv == NULL)
                {   /* The named variable is not set in the environment. */
                    LOG_Error("environment variable '%s' is not set", pVal);
                    return(FAILURE);
                }

                snprintf(pCfg->cfg_sPassword, sizeof(pCfg->cfg_sPassword), "%s", pEnv);
                pCfg->cfg_bHavePassword = TRUE;
            }
            continue;
        }

        if (strcmp(pArg, "--password-fd") == 0)
        {   /* Read the password from a file descriptor. */
            pVal = PVNC_ArgValue(iArgc, ppArgv, &i);

            if (pVal == NULL)
            {   /* The option had no value; PVNC_ArgValue already logged it. */
                return(FAILURE);
            }

            if (PVNC_ReadPasswordFromFd(pCfg, (int)strtol(pVal, NULL, 10)) != SUCCESS)
            {   /* The read already logged the reason. */
                return(FAILURE);
            }
            continue;
        }

        if (strcmp(pArg, "--rotate") == 0)
        {   /* Panel rotation for the touch mapping. */
            pVal = PVNC_ArgValue(iArgc, ppArgv, &i);

            if (pVal == NULL)
            {   /* The option had no value; PVNC_ArgValue already logged it. */
                return(FAILURE);
            }

            pCfg->cfg_iRotate = (int)strtol(pVal, NULL, 10);
            continue;
        }

        if (strcmp(pArg, "--reconnect-max") == 0)
        {   /* Reconnect attempt cap; -1 for unlimited. */
            pVal = PVNC_ArgValue(iArgc, ppArgv, &i);

            if (pVal == NULL)
            {   /* The option had no value; PVNC_ArgValue already logged it. */
                return(FAILURE);
            }

            pCfg->cfg_iReconnectMax = (int)strtol(pVal, NULL, 10);
            continue;
        }

        if (strcmp(pArg, "--reconnect-delay") == 0)
        {   /* Base backoff in milliseconds. */
            pVal = PVNC_ArgValue(iArgc, ppArgv, &i);

            if (pVal == NULL)
            {   /* The option had no value; PVNC_ArgValue already logged it. */
                return(FAILURE);
            }

            pCfg->cfg_iReconnectDelayMs = (int)strtol(pVal, NULL, 10);
            continue;
        }

        if (strcmp(pArg, "--backlight") == 0)
        {   /* Brightness policy applied when the panel is taken over. */
            pVal = PVNC_ArgValue(iArgc, ppArgv, &i);

            if (pVal == NULL)
            {   /* The option had no value; PVNC_ArgValue already logged it. */
                return(FAILURE);
            }

            if (PVNC_ParseBacklight(pVal, &pCfg->cfg_iBacklightMode,
                                    &pCfg->cfg_iBacklightLevel) != SUCCESS)
            {   /* Not a valid policy; already logged. */
                return(FAILURE);
            }

            continue;
        }

        if (strcmp(pArg, "--backlight-set") == 0)
        {   /* One-shot: apply the policy now and exit, no server and no session. */
            pVal = PVNC_ArgValue(iArgc, ppArgv, &i);

            if (pVal == NULL)
            {   /* The option had no value; PVNC_ArgValue already logged it. */
                return(FAILURE);
            }

            if (PVNC_ParseBacklight(pVal, &pCfg->cfg_iBacklightMode,
                                    &pCfg->cfg_iBacklightLevel) != SUCCESS)
            {   /* Not a valid policy; already logged. */
                return(FAILURE);
            }

            pCfg->cfg_bBacklightSetOnly = TRUE;
            continue;
        }

        if (strcmp(pArg, "--backlight-auto") == 0)
        {   /* The black-frame rule: off, on (default timeout) or seconds. */
            pVal = PVNC_ArgValue(iArgc, ppArgv, &i);

            if (pVal == NULL)
            {   /* The option had no value; PVNC_ArgValue already logged it. */
                return(FAILURE);
            }

            if (PVNC_ParseBacklightAuto(pVal, &pCfg->cfg_iBacklightAutoMs) != SUCCESS)
            {   /* Not a valid setting; already logged. */
                return(FAILURE);
            }

            continue;
        }

        if (pArg[0] == '-')
        {   /* An unknown dashed token is a usage error, not a server address. */
            LOG_Error("unknown option '%s'", pArg);
            return(FAILURE);
        }

        /* A bare token is the server IP; only one is allowed. */
        if (bHaveServer != FALSE)
        {   /* A second positional argument is ambiguous. */
            LOG_Error("unexpected extra argument '%s'", pArg);
            return(FAILURE);
        }

        snprintf(pCfg->cfg_sServerIp, sizeof(pCfg->cfg_sServerIp), "%s", pArg);
        bHaveServer = TRUE;
    }

    if ((bHaveServer == FALSE) && (pCfg->cfg_bBacklightSetOnly == FALSE))
    {   /* The server address is mandatory for a session; the one-shot needs none. */
        LOG_Error("no server IP given");
        return(FAILURE);
    }

    /* Command line accepted. */
    return(SUCCESS);
}

/*---------------------------------------------------------------------------*/
/* PVNC_SleepMs:                                                             */
/*                                                                           */
/* Sleeps for the given milliseconds but returns early if a stop signal      */
/* arrives, so a long reconnect backoff never delays a clean shutdown.  Uses */
/* poll() with no descriptors as a portable interruptible sleep.  Local.     */
/*                                                                           */
/* Arguments:                                                                */
/*     iMs : milliseconds to sleep.                                          */
/*                                                                           */
/* Returns:                                                                  */
/*     void : returns after the delay or on a signal.                        */
/*---------------------------------------------------------------------------*/

static void PVNC_SleepMs(int iMs)
{
    /* poll with a NULL set is a clean interruptible sleep; EINTR from a signal   */
    /* returns immediately, and the caller re-checks PVNC_bStop.                  */
    poll(NULL, 0, iMs);
}

/*---------------------------------------------------------------------------*/
/* PVNC_ShouldRetry:                                                         */
/*                                                                           */
/* Decides whether another reconnect attempt is allowed given the policy and */
/* the attempts already made.  Local.                                        */
/*                                                                           */
/* Arguments:                                                                */
/*     pCfg      : config carrying the reconnect policy.                     */
/*     iAttempts : how many reconnects have already been tried this outage.  */
/*                                                                           */
/* Returns:                                                                  */
/*     int : TRUE if another attempt is permitted, else FALSE.               */
/*---------------------------------------------------------------------------*/

static int PVNC_ShouldRetry(const PVNC_Config *pCfg, int iAttempts)
{
    if (pCfg->cfg_iReconnectMax < 0)
    {   /* Negative means retry forever. */
        return(TRUE);
    }

    if (pCfg->cfg_iReconnectMax == 0)
    {   /* Zero means never reconnect. */
        return(FALSE);
    }

    /* Otherwise allow up to the configured number of attempts. */
    return((iAttempts < pCfg->cfg_iReconnectMax) ? TRUE : FALSE);
}

/*----------------------------------------------------------------------------*/
/* PVNC_Loop:                                                                 */
/*                                                                            */
/* The single-threaded control loop for one connected session: poll the       */
/* socket and the touch device, draw incoming updates, forward outgoing       */
/* touch, keep one incremental update request outstanding, and probe an       */
/* idle server with heartbeats (see the tunables legend).  Runs until a       */
/* signal (stop), a socket/protocol failure, or PVNC_HEARTBEAT_MISSES         */
/* unanswered heartbeats (retry).  Local.                                     */
/*                                                                            */
/* Arguments:                                                                 */
/*     pSess : an initialised, negotiated, set-up session with an initial     */
/*             full update already requested.                                 */
/*     pInp  : the open touch device (its fd is polled alongside the socket). */
/*                                                                            */
/* Returns:                                                                   */
/*     int : PVNC_RESULT_STOP if a signal ended it, PVNC_RESULT_RETRY on a    */
/*           session failure the caller may reconnect from.                   */
/*----------------------------------------------------------------------------*/

static int PVNC_Loop(RFB_Session *pSess, INP_Device *pInp)
{
    struct pollfd    arrPfd[2];                         // [0] socket, [1] touch device
    INP_PointerEvent arrEvents[PVNC_MAX_POINTER_EVTS];  // drained touch events
    FB_Device       *pFb;                               // the framebuffer the session draws into
    int              iNumEvents;                        // events from one input drain
    int              bWasUpdate;                        // TRUE if a message was a fb update
    int              bInputOk;                          // FALSE once the touch device errors
    int              iPoll;                             // poll() result
    int              iMissed;                           // heartbeats sent with no message received since
    int              i;

    pFb      = pSess->rfb_pFb;
    bInputOk = TRUE;
    iMissed  = 0;

    for (;;)
    {   /* One pass handles at most one socket message and one input drain. */

        if (PVNC_bStop != 0)
        {   /* A termination signal arrived: end this session cleanly. */
            return(PVNC_RESULT_STOP);
        }

        /* The black-frame rule's clock is checked once per pass, so a frame that     */
        /* has stayed black long enough darkens the panel within one idle period      */
        /* even when the server sends nothing more (a no-op unless the rule is on).   */
        FB_BacklightAutoTick(pFb, NET_NowMs());

        /* Socket is always watched for readability and hang-up. */
        arrPfd[0].fd      = pSess->rfb_iFd;
        arrPfd[0].events  = POLLIN;
        arrPfd[0].revents = 0;

        /* The touch device is watched only while it is healthy. */
        arrPfd[1].fd      = (bInputOk != FALSE) ? INP_GetFd(pInp) : -1;
        arrPfd[1].events  = POLLIN;
        arrPfd[1].revents = 0;

        iPoll = poll(arrPfd, 2, PVNC_POLL_IDLE_MS);

        if (iPoll < 0)
        {   /* A signal interrupts the wait; anything else is a real error. */
            if (errno == EINTR)
            {   /* Loop back and re-check the stop flag. */
                continue;
            }

            LOG_Error("poll() in control loop failed: %s", strerror(errno));
            return(PVNC_RESULT_RETRY);
        }

        if (iPoll == 0)
        {   /* Idle timeout: apply the liveness rule, then probe again. */

            if (iMissed >= PVNC_HEARTBEAT_MISSES)
            {   /* Each of the last heartbeats went unanswered for a whole    */
                /* idle period: the server host is gone without a FIN/RST.    */
                /* Reconnect; the launcher sees the exit if that fails too.   */
                LOG_Warn("server silent across %d heartbeats; dropping the session",
                         iMissed);
                return(PVNC_RESULT_RETRY);
            }

            /* A non-incremental 1x1 request MUST be answered, so its reply (or   */
            /* any other message) is what clears the miss count below.  A write   */
            /* failure means the socket itself is already dead.                   */
            if (RFB_SendHeartbeat(pSess) != SUCCESS)
            {   /* Server unreachable: reconnect. */
                return(PVNC_RESULT_RETRY);
            }

            iMissed++;
            continue;
        }

        /* Socket error / hang-up: the server closed on us. */
        if ((arrPfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {   /* Treat as a dropped session. */
            LOG_Info("socket reported hang-up");
            return(PVNC_RESULT_RETRY);
        }

        /* Readable socket: process exactly one server message. */
        if ((arrPfd[0].revents & POLLIN) != 0)
        {   /* One message per pass keeps input latency low. */
            if (RFB_HandleMessage(pSess, &bWasUpdate) != SUCCESS)
            {   /* Protocol or socket failure: reconnect. */
                return(PVNC_RESULT_RETRY);
            }

            /* Any message at all proves the server is alive: the liveness rule   */
            /* starts over.                                                       */
            iMissed = 0;

            if (bWasUpdate != FALSE)
            {   /* A frame was presented: let the black-frame rule classify it (a   */
                /* lit frame re-lights a darkened panel at once), then keep one     */
                /* incremental request outstanding so updates keep flowing.         */
                FB_BacklightAutoFrame(pFb, NET_NowMs());

                if (RFB_RequestUpdate(pSess, TRUE) != SUCCESS)
                {   /* Could not ask for the next update. */
                    return(PVNC_RESULT_RETRY);
                }
            }
        }

        /* Readable touch device: drain events and forward them as PointerEvents. */
        if ((bInputOk != FALSE) && ((arrPfd[1].revents & POLLIN) != 0))
        {   /* Coalesced pointer events, oldest first. */
            iNumEvents = INP_ReadEvents(pInp, arrEvents, PVNC_MAX_POINTER_EVTS);

            if (iNumEvents < 0)
            {   /* The touch device errored/vanished; keep the display alive   */
                /* without input rather than tearing down the whole session.   */
                LOG_Warn("touch device lost; continuing display-only");
                bInputOk = FALSE;
            }
            else
            {   /* Send each pointer event to the server. */
                for (i = 0; i < iNumEvents; i++)
                {   /* One pointer event sent per iteration. */
                    if (RFB_SendPointerEvent(pSess, arrEvents[i].pe_iX,
                                             arrEvents[i].pe_iY,
                                             arrEvents[i].pe_iButtonMask) != SUCCESS)
                    {   /* Cannot forward touch: session is dead. */
                        return(PVNC_RESULT_RETRY);
                    }
                }
            }
        }

        /* Touch device error/hang-up flagged by poll itself. */
        if ((bInputOk != FALSE) && ((arrPfd[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0))
        {   /* Stop polling it; the display keeps running. */
            LOG_Warn("touch device hang-up; continuing display-only");
            bInputOk = FALSE;
        }
    }
}

/*-----------------------------------------------------------------------------*/
/* PVNC_RunSession:                                                            */
/*                                                                             */
/* Opens one TCP connection, negotiates and sets up an RFB session, requests   */
/* the first full update, and runs the control loop.  Cleans up the socket and */
/* session on the way out.  Local.                                             */
/*                                                                             */
/* Arguments:                                                                  */
/*     pCfg  : configuration (server, port, password).                         */
/*     pFb   : the open framebuffer to draw into.                              */
/*     pInp  : the open touch device.                                          */
/*     pbHardFail : out; TRUE if the failure is one reconnecting cannot fix    */
/*                  (nothing today sets it, but it lets a future auth-specific */
/*                  error stop the loop rather than hammer a bad password).    */
/*                                                                             */
/* Returns:                                                                    */
/*     int : PVNC_RESULT_STOP (signal), PVNC_RESULT_RETRY (connect or          */
/*           handshake failed), or PVNC_RESULT_RETRY_RAN (a session ran and    */
/*           then dropped, so the caller restarts its attempt count).          */
/*-----------------------------------------------------------------------------*/

static int PVNC_RunSession(const PVNC_Config *pCfg, FB_Device *pFb, INP_Device *pInp,
                           int *pbHardFail)
{
    RFB_Session   sess;                        // this connection's RFB state
    int           iFd;                         // the socket
    int           iResult;                     // loop outcome
    int           iPassLen;                    // password length for auth

    *pbHardFail = FALSE;

    /* Connect to the VNC server (numeric IP only). */
    iFd = NET_ConnectIp(pCfg->cfg_sServerIp, pCfg->cfg_iPort);

    if (iFd < 0)
    {   /* Server not up yet or unreachable; a reconnect may succeed. */
        return(PVNC_RESULT_RETRY);
    }

    /* Bind the session to this socket and the framebuffer. */
    RFB_Init(&sess, iFd, pFb);

    /* Password length (0 when none supplied, for a None-security server). */
    iPassLen = (pCfg->cfg_bHavePassword != FALSE) ? (int)strlen(pCfg->cfg_sPassword) : 0;

    /* Handshake, security, auth, ClientInit, ServerInit. */
    if (RFB_Negotiate(&sess, (const unsigned char *)pCfg->cfg_sPassword, iPassLen) != SUCCESS)
    {   /* Negotiation failed (often a wrong password or a not-ready server).     */
        /* Left as a retryable failure so a still-starting server recovers; a     */
        /* persistent wrong password simply exhausts the bounded attempt count.   */
        RFB_Free(&sess);
        NET_Close(iFd);
        return(PVNC_RESULT_RETRY);
    }

    /* SetPixelFormat + SetEncodings + row-buffer allocation. */
    if (RFB_Setup(&sess) != SUCCESS)
    {   /* Setup failed; reconnect. */
        RFB_Free(&sess);
        NET_Close(iFd);
        return(PVNC_RESULT_RETRY);
    }

    /* Clear the panel and ask for the first full frame. */
    FB_Clear(pFb);

    if (RFB_RequestUpdate(&sess, FALSE) != SUCCESS)
    {   /* Could not request the initial image. */
        RFB_Free(&sess);
        NET_Close(iFd);
        return(PVNC_RESULT_RETRY);
    }

    LOG_Info("session established; entering control loop");

    /* Run until a signal or a session failure. */
    iResult = PVNC_Loop(&sess, pInp);

    /* Tear down this connection's resources. */
    RFB_Free(&sess);
    NET_Close(iFd);

    if (iResult == PVNC_RESULT_RETRY)
    {   /* The session was established and then dropped: tell the caller so the      */
        /* reconnect count starts a fresh outage instead of accumulating for life.   */
        return(PVNC_RESULT_RETRY_RAN);
    }

    /* A signal-driven stop is passed through unchanged. */
    return(iResult);
}

/*--------------------------------------------------------------------------*/
/* main:                                                                    */
/*                                                                          */
/* Program entry point.  Parses the command line, installs signal handlers, */
/* opens the framebuffer and touch device, runs the reconnect loop of RFB   */
/* sessions, releases the devices and exits.                                */
/*                                                                          */
/* Arguments:                                                               */
/*     iArgc  : argument count.                                             */
/*     ppArgv : argument vector.                                            */
/*                                                                          */
/* Returns:                                                                 */
/*     int : a PVNC_EXIT_* code - 0 on a clean run, non-zero on a startup   */
/*           failure (bad args, no framebuffer, no touch device).           */
/*--------------------------------------------------------------------------*/

int main(int iArgc, char **ppArgv)
{
    PVNC_Config       cfg;        // resolved configuration
    FB_Device         fb;         // the framebuffer (opened once)
    INP_Device        inp;        // the touch device (opened once)
    struct sigaction  sa;         // handler installer
    int               bExitNow;   // help/version early-exit flag
    int               iExitCode;  // early-exit code
    int               iAttempts;  // reconnects tried this outage
    int               iResult;    // session outcome
    int               bHardFail;  // unrecoverable-session flag

    /* Defaults first, then the command line overrides them. */
    PVNC_SetDefaults(&cfg);

    if (PVNC_ParseArgs(iArgc, ppArgv, &cfg, &bExitNow, &iExitCode) != SUCCESS)
    {   /* Bad command line: usage to stderr and a usage exit code. */
        PVNC_Usage(stderr);
        return(PVNC_EXIT_USAGE);
    }

    if (bExitNow != FALSE)
    {   /* --help or --version already printed; exit as requested. */
        return(iExitCode);
    }

    /* Apply the verbosity choice to the logger. */
    LOG_SetVerbose(cfg.cfg_bVerbose);

    if (cfg.cfg_bBacklightSetOnly != FALSE)
    {   /* The one-shot brightness command: no framebuffer, no touch device, no       */
        /* server.  A host driving the panel over ssh runs this while (or before)     */
        /* another instance shows pixels; the running instance is unaffected, since   */
        /* it restores only a brightness it changed itself.  FB_BacklightTake works   */
        /* on the zeroed device and nothing closes it, so the setting stays.          */
        memset(&fb, 0, sizeof(fb));
        fb.fb_iFd = -1;

        if (FB_BacklightTake(&fb, cfg.cfg_iBacklightMode, cfg.cfg_iBacklightLevel) != SUCCESS)
        {   /* Could not read or write the brightness; the display code says why. */
            return(PVNC_EXIT_FB);
        }

        return(PVNC_EXIT_OK);
    }

    /* Install SIGINT / SIGTERM handlers so a stop unwinds through the normal exit. */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = PVNC_OnSignal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Open the framebuffer.  The launcher must have stopped whatever owned the    */
    /* display first: fbdev has NO exclusive lock, so open() succeeds even while   */
    /* another process is alive on it and both would map the same buffer - the     */
    /* ordering is the launcher's discipline, not something the kernel enforces.   */
    if (FB_Open(&fb, cfg.cfg_sFbPath) != SUCCESS)
    {   /* No display: nothing this program can do.  The distinct exit code is   */
        /* how a launcher learns it; whatever must happen to the panel next      */
        /* is the launcher's job, not this program's.                            */
        return(PVNC_EXIT_FB);
    }

    /* Open (or autodetect) the touchscreen, sized to the framebuffer. */
    if (INP_Open(&inp, cfg.cfg_sInputPath, fb.fb_iWidth, fb.fb_iHeight, cfg.cfg_iRotate) != SUCCESS)
    {   /* No touch input; not useful as an interactive screen, so release the   */
        /* fb and exit with a distinct code rather than run a display the user   */
        /* cannot touch.                                                         */
        FB_Close(&fb);
        return(PVNC_EXIT_INPUT);
    }

    /* Both devices are ours: apply the backlight policy.  The brightness lives    */
    /* in the display driver and survives the previous owner's death, so a panel   */
    /* that owner had turned off stays dark under our frames unless it is lit      */
    /* here.  A failure is logged and is not fatal - the pixels still go out.      */
    FB_BacklightTake(&fb, cfg.cfg_iBacklightMode, cfg.cfg_iBacklightLevel);

    if (cfg.cfg_iBacklightAutoMs > 0)
    {   /* The black-frame rule: the remote desktop's own blanking reaches the panel. */
        FB_BacklightAutoEnable(&fb, cfg.cfg_iBacklightAutoMs);
    }

    /* Reconnect loop: run sessions until a signal or exhausted attempts. */
    iAttempts = 0;

    for (;;)
    {   /* Each iteration is one connection attempt and, if it connects, a session. */

        if (PVNC_bStop != 0)
        {   /* A signal during backoff or between sessions ends the loop. */
            break;
        }

        iResult = PVNC_RunSession(&cfg, &fb, &inp, &bHardFail);

        if (iResult == PVNC_RESULT_STOP)
        {   /* A clean signal-driven stop: leave the loop. */
            break;
        }

        if (bHardFail != FALSE)
        {   /* Reserved for a future unrecoverable classification; stop now. */
            break;
        }

        if (iResult == PVNC_RESULT_RETRY_RAN)
        {   /* A real session ran and then dropped: this is a NEW outage, so the     */
            /* bounded attempt count starts again rather than accumulating for the   */
            /* life of the process (a kiosk must survive many host restarts).        */
            iAttempts = 0;
        }

        /* A session dropped (or a connect failed).  Reconnect if policy allows. */
        if (PVNC_ShouldRetry(&cfg, iAttempts) == FALSE)
        {   /* Out of attempts: give up; the launcher sees the exit. */
            LOG_Warn("reconnect attempts exhausted; exiting");
            break;
        }

        /* Exponential backoff, capped, and interruptible by a signal. */
        {
            int iDelay;

            iDelay = cfg.cfg_iReconnectDelayMs << iAttempts;

            if ((iDelay > PVNC_BACKOFF_CAP_MS) || (iDelay < 0))
            {   /* Cap the growth (and guard the shift overflowing to negative). */
                iDelay = PVNC_BACKOFF_CAP_MS;
            }

            LOG_Info("reconnecting in %d ms (attempt %d)", iDelay, iAttempts + 1);
            PVNC_SleepMs(iDelay);
        }

        iAttempts++;
    }

    /* Release the devices (FB_Close restores the driver's cache default) so   */
    /* whatever the launcher starts next finds /dev/fb0 free.                  */
    INP_Close(&inp);
    FB_Close(&fb);

    /* Scrub the password from memory now that no session needs it. */
    memset(cfg.cfg_sPassword, 0, sizeof(cfg.cfg_sPassword));

    /* A completed run (clean stop or exhausted reconnects) is exit 0; startup   */
    /* failures returned earlier with their specific codes.                      */
    return(PVNC_EXIT_OK);
}
