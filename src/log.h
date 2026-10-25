/*--------------------------------------------------------------------------*/
/* SPDX-License-Identifier: GPL-2.0-or-later                                */
/* Copyright (c) 2026 B. C. Services                                        */
/*                                                                          */
/* Licensed under the GNU General Public License version 2 or later --      */
/* see LICENSE beside this source.  GPL-2.0 full text in GPL-2.0.txt.       */
/*--------------------------------------------------------------------------*/
/* log.h:                                                                   */
/*                                                                          */
/* Minimal diagnostic logging for PrimeVNC.  Everything goes to stderr so   */
/* the pixel path on stdout (unused today, but reserved) is never polluted, */
/* and so a launcher that starts this client over SSH can capture the       */
/* channel's stderr separately.                                             */
/*                                                                          */
/* The style is error-only logging.  LOG_Error and LOG_Warn always print;   */
/* LOG_Info is a diagnostic aid gated behind --verbose (LOG_SetVerbose) and */
/* is NOT success logging - it exists so a live bring-up on a device can be */
/* traced without a debugger, and stays silent in normal operation.         */
/*--------------------------------------------------------------------------*/

#ifndef PRIMEVNC_LOG_H
#define PRIMEVNC_LOG_H

/*--------------------------------------------------------------------------*/
/* LOG_SetVerbose:                                                          */
/*                                                                          */
/* Turns diagnostic (LOG_Info) output on or off for the process.  Called    */
/* once from main() after the command line is parsed.                       */
/*                                                                          */
/* Arguments:                                                               */
/*     bVerbose : TRUE to enable LOG_Info output, FALSE to suppress it.     */
/*                                                                          */
/* Returns:                                                                 */
/*     void : sets the module-level verbosity flag only.                    */
/*--------------------------------------------------------------------------*/

void LOG_SetVerbose(int bVerbose);

/*--------------------------------------------------------------------------*/
/* LOG_Error:                                                               */
/*                                                                          */
/* Prints a printf-style error line to stderr, prefixed "primevnc: error:". */
/* Always emitted regardless of verbosity.  Does not exit; the caller       */
/* decides how to handle the failure it just reported.                      */
/*                                                                          */
/* Arguments:                                                               */
/*     sFmt : printf format string.                                         */
/*     ...  : the format's arguments.                                       */
/*                                                                          */
/* Returns:                                                                 */
/*     void : reports through stderr only.                                  */
/*--------------------------------------------------------------------------*/

void LOG_Error(const char *sFmt, ...);

/*---------------------------------------------------------------------------*/
/* LOG_Warn:                                                                 */
/*                                                                           */
/* Prints a printf-style warning line to stderr, prefixed "primevnc: warn:". */
/* Always emitted.  Used for recoverable conditions (a server that refused a */
/* requested pixel format, a reconnect attempt) that are not fatal.          */
/*                                                                           */
/* Arguments:                                                                */
/*     sFmt : printf format string.                                          */
/*     ...  : the format's arguments.                                        */
/*                                                                           */
/* Returns:                                                                  */
/*     void : reports through stderr only.                                   */
/*---------------------------------------------------------------------------*/

void LOG_Warn(const char *sFmt, ...);

/*--------------------------------------------------------------------------*/
/* LOG_Info:                                                                */
/*                                                                          */
/* Prints a printf-style diagnostic line to stderr, prefixed "primevnc:",   */
/* only when verbose mode is on.  For bring-up tracing, never for routine   */
/* success reporting in normal operation.                                   */
/*                                                                          */
/* Arguments:                                                               */
/*     sFmt : printf format string.                                         */
/*     ...  : the format's arguments.                                       */
/*                                                                          */
/* Returns:                                                                 */
/*     void : reports through stderr only, and only if verbose.             */
/*--------------------------------------------------------------------------*/

void LOG_Info(const char *sFmt, ...);

#endif // PRIMEVNC_LOG_H
