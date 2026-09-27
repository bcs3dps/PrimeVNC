/*--------------------------------------------------------------------------*/
/* SPDX-License-Identifier: GPL-2.0-or-later                                */
/* Copyright (c) 2026 B. C. Services                                        */
/*                                                                          */
/* Licensed under the GNU General Public License version 2 or later --      */
/* see LICENSE beside this source.  GPL-2.0 full text in GPL-2.0.txt.       */
/*--------------------------------------------------------------------------*/
/* primevnc.h:                                                              */
/*                                                                          */
/* Shared configuration, return conventions and version for PrimeVNC - a    */
/* tiny framebuffer RFB (VNC) client for Allwinner sunxi Linux devices      */
/* (disp2 fbdev, ARMv7-A hard-float; developed on a sun8iw20 / T113 Tina    */
/* Linux 3D-printer panel).  The client runs ON the device: it draws a      */
/* remote desktop's framebuffer to /dev/fb0 and forwards the device's       */
/* touchscreen (/dev/input/eventN) back to the server as RFB PointerEvents. */
/* It is a pure sink/source with no threads and no external libraries, so   */
/* it cross-compiles to one small static ELF, and only framebuffer.c and    */
/* input.c know anything about the hardware.                                */
/*                                                                          */
/* This header is included by every module.  It carries only what the whole */
/* program shares: the boolean / status defines, the version string, and    */
/* the PVNC_Config struct that main() fills from argv and hands down.  Each */
/* subsystem (net, des, rfb, framebuffer, input, log) has its own header.   */
/*--------------------------------------------------------------------------*/

#ifndef PRIMEVNC_H
#define PRIMEVNC_H

/*--------------------------------------------------------------------------*/
/* Boolean and status conventions (project coding style).  SUCCESS is 0 so  */
/* a function can be tested with a plain "if (CALL() != SUCCESS)", and      */
/* FAILURE is its logical negation.  These are the only truth values used.  */
/*--------------------------------------------------------------------------*/

#define TRUE     (!FALSE)                    // logical true
#define FALSE    0                           // logical false
#define SUCCESS  0                           // a function completed its job
#define FAILURE  (!SUCCESS)                  // a function did not

/*--------------------------------------------------------------------------*/
/* Version string, reported by --version and in the startup banner.  Bumped */
/* by hand; there is no build-time stamping in a single-file cross-build.   */
/*--------------------------------------------------------------------------*/

#define PVNC_VERSION  "0.3.0-dev"            // bumped by hand at each functional change

/*--------------------------------------------------------------------------*/
/* Fixed limits.  These size the config buffers; they are generous for a    */
/* LAN pixel link to a desktop VNC server and keep every string inside the  */
/* struct rather than on the heap.                                          */
/*--------------------------------------------------------------------------*/

#define PVNC_MAX_IP        64               // dotted-quad plus slack
#define PVNC_MAX_PATH     256               // device paths
#define PVNC_MAX_PASSWORD  64               // full password; DES uses first 8

/*--------------------------------------------------------------------------*/
/* PVNC_Config:                                                             */
/*                                                                          */
/* Everything main() resolves from the command line before any device is    */
/* opened, then passes by pointer to the session code.  It is a plain value */
/* struct with no owned resources, so it is copied and discarded freely.    */
/*                                                                          */
/* The password lives here only as long as sessions need it and is scrubbed */
/* at exit (see main).  It arrives bare on argv (--password), or through an */
/* environment variable or a file descriptor for a launcher that prefers to */
/* keep it out of the process list (see the parsing in main.c).             */
/*--------------------------------------------------------------------------*/

typedef struct
{
    char cfg_sServerIp[PVNC_MAX_IP];        // numeric server IP; NO name resolution
    int  cfg_iPort;                         // RFB TCP port (5900 + display number)

    char cfg_sFbPath[PVNC_MAX_PATH];        // framebuffer device, default /dev/fb0
    char cfg_sInputPath[PVNC_MAX_PATH];     // evdev node; "" means autodetect

    char cfg_sPassword[PVNC_MAX_PASSWORD];  // VNC/root password (first 8 bytes used)
    int  cfg_bHavePassword;                 // TRUE once a password was supplied

    int  cfg_iRotate;                       // panel rotation applied to touch: 0/90/180/270

    int  cfg_bVerbose;                      // TRUE enables diagnostic (non-error) logging

    int  cfg_iReconnectMax;                 // reconnect attempts: 0 none, -1 forever
    int  cfg_iReconnectDelayMs;             // base backoff between attempts (doubles, capped)

    int  cfg_iBacklightMode;                // brightness policy at takeover: an FB_BACKLIGHT_* value (framebuffer.h)
    int  cfg_iBacklightLevel;               // the brightness for FB_BACKLIGHT_LEVEL (1..255)
    int  cfg_bBacklightSetOnly;             // TRUE for --backlight-set: apply the policy and exit, no session
    int  cfg_iBacklightAutoMs;              // --backlight-auto: ms of an all-black frame before the panel goes dark; 0 = off
} PVNC_Config;

#endif // PRIMEVNC_H
