/*----------------------------------------------------------------------------*/
/* SPDX-License-Identifier: GPL-2.0-or-later                                  */
/* Copyright (c) 2026 B. C. Services                                          */
/*                                                                            */
/* Licensed under the GNU General Public License version 2 or later --        */
/* see LICENSE beside this source.  GPL-2.0 full text in GPL-2.0.txt.         */
/*----------------------------------------------------------------------------*/
/* input.c:                                                                   */
/*                                                                            */
/* Implementation of the evdev touchscreen reader declared in input.h.        */
/* Raw device axes are read once at open (EVIOCGABS) and every touch packet   */
/* is normalized into that range, rotated per the configured orientation, and */
/* scaled to framebuffer pixels.  Both the single-touch and multitouch-B      */
/* event vocabularies are handled; slot 0 is the pointer.                     */
/*----------------------------------------------------------------------------*/

#include "input.h"
#include "log.h"
#include "primevnc.h"

#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>

#include <sys/ioctl.h>
#include <linux/input.h>

/*---------------------------------------------------------------------------*/
/* Bit-array helpers for the EVIOCGBIT capability queries.  The kernel       */
/* returns capability bitmaps as arrays of unsigned long; INP_TESTBIT checks */
/* whether a given code's bit is set.                                        */
/*---------------------------------------------------------------------------*/

#define INP_BITS_PER_LONG   (sizeof(unsigned long) * 8)                                                  // bits in one word
#define INP_NLONGS(nbits)   (((nbits) + INP_BITS_PER_LONG - 1) / INP_BITS_PER_LONG)                      // words for nbits
#define INP_TESTBIT(bit, arr) (((arr)[(bit) / INP_BITS_PER_LONG] >> ((bit) % INP_BITS_PER_LONG)) & 1UL)  // nonzero when bit is set in the array

/*---------------------------------------------------------------------------*/
/* INP_DeviceIsTouch:                                                        */
/*                                                                           */
/* Reports whether an already-open event fd looks like a touchscreen.  Every */
/* candidate must support EV_ABS with ABS_X or ABS_MT_POSITION_X.  On the    */
/* STRICT pass it must also carry a touch signature: INPUT_PROP_DIRECT (the  */
/* surface is the display, per the kernel's input properties) or a BTN_TOUCH */
/* capability (the contact button every touch driver emits).  Without that   */
/* an accelerometer or a joystick would pass, which is why the strict pass   */
/* runs first and the loose one only if nothing declared itself.  Local.     */
/*                                                                           */
/* Arguments:                                                                */
/*     iFd     : an open event device fd.                                    */
/*     bStrict : TRUE to require the touch signature as well as the axes.    */
/*                                                                           */
/* Returns:                                                                  */
/*     int : TRUE if it qualifies on this pass, else FALSE.                  */
/*---------------------------------------------------------------------------*/

static int INP_DeviceIsTouch(int iFd, int bStrict)
{
    unsigned long arrEvBits[INP_NLONGS(EV_MAX)];         // supported event types
    unsigned long arrAbsBits[INP_NLONGS(ABS_MAX)];       // supported ABS axes
    unsigned long arrKeyBits[INP_NLONGS(KEY_MAX)];       // supported keys/buttons (BTN_TOUCH lives here)
    unsigned long arrProps[INP_NLONGS(INPUT_PROP_MAX)];  // device properties (INPUT_PROP_DIRECT)

    /* Query the set of event types this device emits. */
    memset(arrEvBits, 0, sizeof(arrEvBits));

    if (ioctl(iFd, EVIOCGBIT(0, sizeof(arrEvBits)), arrEvBits) < 0)
    {   /* Cannot even query capabilities; not a candidate. */
        return(FALSE);
    }

    if (INP_TESTBIT(EV_ABS, arrEvBits) == 0)
    {   /* No absolute axes: a keyboard or mouse, not a touch panel. */
        return(FALSE);
    }

    /* Query which absolute axes it carries. */
    memset(arrAbsBits, 0, sizeof(arrAbsBits));

    if (ioctl(iFd, EVIOCGBIT(EV_ABS, sizeof(arrAbsBits)), arrAbsBits) < 0)
    {   /* ABS supported but the axis map was unreadable; skip it. */
        return(FALSE);
    }

    if ((INP_TESTBIT(ABS_MT_POSITION_X, arrAbsBits) == 0) &&
        (INP_TESTBIT(ABS_X, arrAbsBits) == 0))
    {   /* ABS device with no positional X (e.g. a jog dial); not what we want. */
        return(FALSE);
    }

    if (bStrict == FALSE)
    {   /* Loose pass: any absolute-positioning device qualifies. */
        return(TRUE);
    }

    /* Strict pass: a touch panel also DECLARES itself.  INPUT_PROP_DIRECT    */
    /* means the surface is the display itself (a touchscreen, not a tablet   */
    /* or trackpad); BTN_TOUCH is the contact button every touch driver       */
    /* emits.  Either is enough; an accelerometer or joystick has neither.    */
    memset(arrProps, 0, sizeof(arrProps));

    if ((ioctl(iFd, EVIOCGPROP(sizeof(arrProps)), arrProps) >= 0) &&
        (INP_TESTBIT(INPUT_PROP_DIRECT, arrProps) != 0))
    {   /* Declares a direct-input surface: a touchscreen. */
        return(TRUE);
    }

    memset(arrKeyBits, 0, sizeof(arrKeyBits));

    if ((ioctl(iFd, EVIOCGBIT(EV_KEY, sizeof(arrKeyBits)), arrKeyBits) >= 0) &&
        (INP_TESTBIT(BTN_TOUCH, arrKeyBits) != 0))
    {   /* Emits the touch contact button: a touchscreen. */
        return(TRUE);
    }

    /* Absolute axes but no touch signature: not accepted on the strict pass. */
    return(FALSE);
}

/*--------------------------------------------------------------------------*/
/* INP_AutoDetect:                                                          */
/*                                                                          */
/* Scans /dev/input/event0..31 for a touchscreen and copies its path into   */
/* the caller's buffer.  Two passes: the first accepts only a device with a */
/* touch signature (see INP_DeviceIsTouch), the second any device with an   */
/* absolute X axis, so an odd driver still works while a sensor cannot      */
/* outrank a real panel.  Used when no --input path was given.  Local.      */
/*                                                                          */
/* Arguments:                                                               */
/*     pOutPath : buffer to receive the chosen device path.                 */
/*     iOutLen  : size of pOutPath.                                         */
/*                                                                          */
/* Returns:                                                                 */
/*     int : SUCCESS with pOutPath set, or FAILURE if none was found.       */
/*--------------------------------------------------------------------------*/

static int INP_AutoDetect(char *pOutPath, size_t iOutLen)
{
    char sPath[64];                            // candidate device path
    int  iFd;                                  // candidate fd
    int  bIsTouch;                             // classification result
    int  iPass;                                // 0 = strict (touch signature), 1 = loose (any ABS+X)
    int  i;

    for (iPass = 0; iPass < 2; iPass++)
    {   /* Strict pass first; the loose pass runs only if nothing declared itself. */
        for (i = 0; i < 32; i++)
        {   /* Probe each event node in turn; the first match on this pass wins. */
            snprintf(sPath, sizeof(sPath), "/dev/input/event%d", i);

            iFd = open(sPath, O_RDONLY | O_NONBLOCK);

            if (iFd < 0)
            {   /* No such node (or no permission); move on. */
                continue;
            }

            bIsTouch = INP_DeviceIsTouch(iFd, (iPass == 0) ? TRUE : FALSE);
            close(iFd);

            if (bIsTouch != FALSE)
            {   /* Found one: hand its path back to the caller. */
                snprintf(pOutPath, iOutLen, "%s", sPath);

                if (iPass == 0)
                {   /* Matched on the touch signature: the confident case. */
                    LOG_Info("autodetected touch device %s (touch signature)", sPath);
                }
                else
                {   /* Matched only on absolute axes: say so, in case it is wrong. */
                    LOG_Warn("autodetected %s by absolute axes only (no touch signature)", sPath);
                }

                return(SUCCESS);
            }
        }
    }

    LOG_Error("no touchscreen device found under /dev/input/event*");
    /* No node matched on either pass; there is no device to hand back. */
    return(FAILURE);
}

/*----------------------------------------------------------------------------*/
/* INP_QueryAxis:                                                             */
/*                                                                            */
/* Reads the min/max of one absolute axis, preferring the multitouch axis and */
/* falling back to the single-touch axis.  Local to this file.                */
/*                                                                            */
/* Arguments:                                                                 */
/*     iFd     : open device fd.                                              */
/*     iMtCode : the multitouch axis code (ABS_MT_POSITION_X/Y).              */
/*     iStCode : the single-touch axis code (ABS_X/Y).                        */
/*     piMin   : out; axis minimum.                                           */
/*     piMax   : out; axis maximum.                                           */
/*                                                                            */
/* Returns:                                                                   */
/*     int : SUCCESS if a usable range was read (max > min), else FAILURE.    */
/*----------------------------------------------------------------------------*/

static int INP_QueryAxis(int iFd, int iMtCode, int iStCode, int *piMin, int *piMax)
{
    struct input_absinfo abs;                  // kernel axis descriptor

    /* Prefer the multitouch axis range when the device reports one. */
    if (ioctl(iFd, EVIOCGABS(iMtCode), &abs) == 0)
    {   /* A usable range needs max strictly above min. */
        if (abs.maximum > abs.minimum)
        {   /* Multitouch axis is calibrated; take it. */
            *piMin = abs.minimum;
            *piMax = abs.maximum;
            return(SUCCESS);
        }
    }

    /* Fall back to the single-touch axis. */
    if (ioctl(iFd, EVIOCGABS(iStCode), &abs) == 0)
    {   /* Same usability test on the fallback axis. */
        if (abs.maximum > abs.minimum)
        {   /* Single-touch axis is calibrated; take it. */
            *piMin = abs.minimum;
            *piMax = abs.maximum;
            return(SUCCESS);
        }
    }

    /* Neither axis gave a usable range. */
    return(FAILURE);
}

/*--------------------------------------------------------------------------*/
/* INP_LogDeviceName:                                                       */
/*                                                                          */
/* Logs the driver-reported name of an open event device (EVIOCGNAME) next  */
/* to its path, so a wrong autodetect pick is visible in the log instead of */
/* showing up only as touches that land nowhere.  Local to this file.       */
/*                                                                          */
/* Arguments:                                                               */
/*     iFd   : open event device fd.                                        */
/*     sPath : the path it was opened from, for the log line.               */
/*                                                                          */
/* Returns:                                                                 */
/*     void : writes one info line.                                         */
/*--------------------------------------------------------------------------*/

static void INP_LogDeviceName(int iFd, const char *sPath)
{
    char sName[80];                            // driver-reported device name

    /* EVIOCGNAME fills up to the given length; leave one byte for the NUL   */
    /* in case the name is exactly that long.                                */
    memset(sName, 0, sizeof(sName));

    if (ioctl(iFd, EVIOCGNAME(sizeof(sName) - 1), sName) < 0)
    {   /* No name available; say so rather than print an empty string. */
        snprintf(sName, sizeof(sName), "%s", "(unnamed)");
    }

    LOG_Info("touch device %s reports itself as '%s'", sPath, sName);
}

/*--------------------------------------------------------------------------*/
/* INP_Open:                                                                */
/*                                                                          */
/* Resolves the device, opens it non-blocking, logs the driver's name for   */
/* the device, reads the axis ranges and stores the mapping parameters.     */
/* See input.h for the contract.                                            */
/*                                                                          */
/* Arguments:                                                               */
/*     pInp     : device to populate.                                       */
/*     sPath    : device path, or "" to autodetect.                         */
/*     iScreenW : framebuffer width.                                        */
/*     iScreenH : framebuffer height.                                       */
/*     iRotate  : rotation applied to touch.                                */
/*                                                                          */
/* Returns:                                                                 */
/*     int : SUCCESS or FAILURE.                                            */
/*--------------------------------------------------------------------------*/

int INP_Open(INP_Device *pInp, const char *sPath, int iScreenW, int iScreenH,
             int iRotate)
{
    char sResolved[64];                        // path actually opened

    /* Zero the struct so every field has a defined starting value. */
    memset(pInp, 0, sizeof(*pInp));
    pInp->inp_iFd = -1;

    /* Resolve the device path: explicit path if given, else autodetect. */
    if ((sPath != NULL) && (sPath[0] != '\0'))
    {   /* Caller named a specific node. */
        snprintf(sResolved, sizeof(sResolved), "%s", sPath);
    }
    else
    {   /* No path: scan for a touchscreen. */
        if (INP_AutoDetect(sResolved, sizeof(sResolved)) != SUCCESS)
        {   /* Nothing usable found; the reason was logged. */
            return(FAILURE);
        }
    }

    /* Open non-blocking so INP_ReadEvents can drain to EAGAIN and stop. */
    pInp->inp_iFd = open(sResolved, O_RDONLY | O_NONBLOCK);

    if (pInp->inp_iFd < 0)
    {   /* Device vanished between detect and open, or no permission. */
        LOG_Error("open('%s') failed: %s", sResolved, strerror(errno));
        return(FAILURE);
    }

    /* Say which driver this is, so a wrong pick is visible before it is used. */
    INP_LogDeviceName(pInp->inp_iFd, sResolved);

    /* Read the raw axis ranges used to normalize touch coordinates. */
    if (INP_QueryAxis(pInp->inp_iFd, ABS_MT_POSITION_X, ABS_X,
                      &pInp->inp_iAbsXMin, &pInp->inp_iAbsXMax) != SUCCESS)
    {   /* Without a calibrated X range the mapping cannot work. */
        LOG_Error("could not read a usable X axis range from %s", sResolved);
        close(pInp->inp_iFd);
        pInp->inp_iFd = -1;
        return(FAILURE);
    }

    if (INP_QueryAxis(pInp->inp_iFd, ABS_MT_POSITION_Y, ABS_Y,
                      &pInp->inp_iAbsYMin, &pInp->inp_iAbsYMax) != SUCCESS)
    {   /* Same for Y. */
        LOG_Error("could not read a usable Y axis range from %s", sResolved);
        close(pInp->inp_iFd);
        pInp->inp_iFd = -1;
        return(FAILURE);
    }

    /* Store the mapping geometry. */
    pInp->inp_iScreenW = iScreenW;
    pInp->inp_iScreenH = iScreenH;
    pInp->inp_iRotate  = iRotate;
    pInp->inp_iCurSlot = 0;                     // slot 0 until an ABS_MT_SLOT says otherwise

    LOG_Info("touch %s: X[%d..%d] Y[%d..%d] -> %dx%d rotate %d",
             sResolved, pInp->inp_iAbsXMin, pInp->inp_iAbsXMax,
             pInp->inp_iAbsYMin, pInp->inp_iAbsYMax, iScreenW, iScreenH, iRotate);

    /* Device is open and calibrated. */
    return(SUCCESS);
}

/*--------------------------------------------------------------------------*/
/* INP_Close:                                                               */
/*                                                                          */
/* Closes the device fd.  See input.h.                                      */
/*                                                                          */
/* Arguments:                                                               */
/*     pInp : device to release.                                            */
/*                                                                          */
/* Returns:                                                                 */
/*     void : leaves the device closed.                                     */
/*--------------------------------------------------------------------------*/

void INP_Close(INP_Device *pInp)
{
    if (pInp->inp_iFd >= 0)
    {   /* Only close a real descriptor. */
        close(pInp->inp_iFd);
        pInp->inp_iFd = -1;
    }
}

/*--------------------------------------------------------------------------*/
/* INP_GetFd:                                                               */
/*                                                                          */
/* Accessor for the control loop's poll set.  See input.h.                  */
/*                                                                          */
/* Arguments:                                                               */
/*     pInp : device.                                                       */
/*                                                                          */
/* Returns:                                                                 */
/*     int : the event fd.                                                  */
/*--------------------------------------------------------------------------*/

int INP_GetFd(const INP_Device *pInp)
{
    /* The control loop needs this to add the device to its poll array. */
    return(pInp->inp_iFd);
}

/*--------------------------------------------------------------------------*/
/* INP_Clampi:                                                              */
/*                                                                          */
/* Clamps an integer to an inclusive range.  Local helper for the mapping.  */
/*                                                                          */
/* Arguments:                                                               */
/*     iVal : value to clamp.                                               */
/*     iLo  : lower bound.                                                  */
/*     iHi  : upper bound.                                                  */
/*                                                                          */
/* Returns:                                                                 */
/*     int : iVal confined to [iLo, iHi].                                   */
/*--------------------------------------------------------------------------*/

static int INP_Clampi(int iVal, int iLo, int iHi)
{
    if (iVal < iLo)
    {   /* Below the range: pin to the low bound. */
        return(iLo);
    }

    if (iVal > iHi)
    {   /* Above the range: pin to the high bound. */
        return(iHi);
    }

    /* Already inside the range. */
    return(iVal);
}

/*--------------------------------------------------------------------------*/
/* INP_MapToScreen:                                                         */
/*                                                                          */
/* Normalizes the latest raw contact coordinates into [0,1], applies the    */
/* configured rotation, scales to framebuffer pixels and clamps.  The       */
/* rotation set is the four right angles clockwise; the value for the       */
/* panel is passed as --rotate.  Local to this file.                        */
/*                                                                          */
/* Arguments:                                                               */
/*     pInp : device holding the raw coords, ranges, geometry and rotation. */
/*     piX  : out; mapped framebuffer x.                                    */
/*     piY  : out; mapped framebuffer y.                                    */
/*                                                                          */
/* Returns:                                                                 */
/*     void : writes the mapped coordinates.                                */
/*--------------------------------------------------------------------------*/

static void INP_MapToScreen(const INP_Device *pInp, int *piX, int *piY)
{
    double dNormX;                             // raw x normalized to [0,1]
    double dNormY;                             // raw y normalized to [0,1]
    int    iRangeX;                            // raw x span
    int    iRangeY;                            // raw y span
    int    iSx;                                // mapped screen x
    int    iSy;                                // mapped screen y

    /* Guard the axis spans so a mis-read range cannot divide by zero. */
    iRangeX = pInp->inp_iAbsXMax - pInp->inp_iAbsXMin;
    iRangeY = pInp->inp_iAbsYMax - pInp->inp_iAbsYMin;

    if (iRangeX <= 0)
    {   /* Degenerate X range: treat everything as the left edge. */
        dNormX = 0.0;
    }
    else
    {   /* Normal case: fraction of the way across the raw span. */
        dNormX = (double)(pInp->inp_iRawX - pInp->inp_iAbsXMin) / (double)iRangeX;
    }

    if (iRangeY <= 0)
    {   /* Degenerate Y range: treat everything as the top edge. */
        dNormY = 0.0;
    }
    else
    {   /* Normal case. */
        dNormY = (double)(pInp->inp_iRawY - pInp->inp_iAbsYMin) / (double)iRangeY;
    }

    /* Apply the panel rotation.  These four transforms are a consistent    */
    /* clockwise set; the one matching the physical mount is chosen live.   */
    if (pInp->inp_iRotate == 90)
    {   /* 90 degrees clockwise. */
        iSx = (int)(dNormY * (double)(pInp->inp_iScreenW - 1));
        iSy = (int)((1.0 - dNormX) * (double)(pInp->inp_iScreenH - 1));
    }
    else if (pInp->inp_iRotate == 180)
    {   /* 180 degrees. */
        iSx = (int)((1.0 - dNormX) * (double)(pInp->inp_iScreenW - 1));
        iSy = (int)((1.0 - dNormY) * (double)(pInp->inp_iScreenH - 1));
    }
    else if (pInp->inp_iRotate == 270)
    {   /* 270 degrees clockwise. */
        iSx = (int)((1.0 - dNormY) * (double)(pInp->inp_iScreenW - 1));
        iSy = (int)(dNormX * (double)(pInp->inp_iScreenH - 1));
    }
    else
    {   /* 0 degrees (default and the unrecognised-value fallback). */
        iSx = (int)(dNormX * (double)(pInp->inp_iScreenW - 1));
        iSy = (int)(dNormY * (double)(pInp->inp_iScreenH - 1));
    }

    /* Confine to the visible screen regardless of any raw out-of-range value. */
    *piX = INP_Clampi(iSx, 0, pInp->inp_iScreenW - 1);
    *piY = INP_Clampi(iSy, 0, pInp->inp_iScreenH - 1);
}

/*--------------------------------------------------------------------------*/
/* INP_Finalize:                                                            */
/*                                                                          */
/* Called on each SYN_REPORT: maps the current contact, and if the mapped   */
/* position or button differs from the last emitted state, appends one      */
/* pointer event.  Local to this file.                                      */
/*                                                                          */
/* Arguments:                                                               */
/*     pInp    : device with the accumulated packet state.                  */
/*     pArrOut : output array to append to.                                 */
/*     iMaxOut : capacity of pArrOut.                                       */
/*     piCount : in/out count of events already written.                    */
/*                                                                          */
/* Returns:                                                                 */
/*     void : may append one event and update the last-emitted state.       */
/*--------------------------------------------------------------------------*/

static void INP_Finalize(INP_Device *pInp, INP_PointerEvent *pArrOut, int iMaxOut,
                         int *piCount)
{
    int iX;                                    // mapped x this packet
    int iY;                                    // mapped y this packet
    int iButton;                               // button mask this packet

    /* Map the latest raw contact position to the screen. */
    INP_MapToScreen(pInp, &iX, &iY);

    /* Button mask bit 0 tracks the finger-down state. */
    iButton = (pInp->inp_bPressed != FALSE) ? 1 : 0;

    if ((pInp->inp_bHaveLast != FALSE) &&
        (iX == pInp->inp_iLastX) &&
        (iY == pInp->inp_iLastY) &&
        (iButton == pInp->inp_iLastButton))
    {   /* No change since the last emitted event: nothing to send. */
        return;
    }

    if (*piCount < iMaxOut)
    {   /* Room in the caller's array: record this pointer event. */
        pArrOut[*piCount].pe_iX          = iX;
        pArrOut[*piCount].pe_iY          = iY;
        pArrOut[*piCount].pe_iButtonMask = iButton;
        (*piCount)++;
    }

    /* Remember this state so the next SYN_REPORT can detect a change. */
    pInp->inp_iLastX      = iX;
    pInp->inp_iLastY      = iY;
    pInp->inp_iLastButton = iButton;
    pInp->inp_bHaveLast   = TRUE;
}

/*----------------------------------------------------------------------------*/
/* INP_ReadEvents:                                                            */
/*                                                                            */
/* Drains and interprets pending evdev events.  See input.h for the contract. */
/*                                                                            */
/* Arguments:                                                                 */
/*     pInp    : open device.                                                 */
/*     pArrOut : output pointer-event array.                                  */
/*     iMaxOut : capacity of pArrOut.                                         */
/*                                                                            */
/* Returns:                                                                   */
/*     int : number of pointer events written, or -1 on device error.         */
/*----------------------------------------------------------------------------*/

int INP_ReadEvents(INP_Device *pInp, INP_PointerEvent *pArrOut, int iMaxOut)
{
    struct input_event arrEv[64];              // one read()'s worth of raw events
    ssize_t            iBytes;                 // bytes from read()
    int                iNumEv;                 // events in this batch
    int                iCount;                 // pointer events produced
    int                i;

    iCount = 0;

    for (;;)
    {   /* Drain the fd in batches until it would block or errors. */
        iBytes = read(pInp->inp_iFd, arrEv, sizeof(arrEv));

        if (iBytes < 0)
        {   /* EAGAIN means drained; EINTR means retry; else a real error. */
            if (errno == EAGAIN)
            {   /* No more events queued right now. */
                break;
            }

            if (errno == EINTR)
            {   /* Interrupted before any bytes; read again. */
                continue;
            }

            LOG_Error("read from input device failed: %s", strerror(errno));
            return(-1);
        }

        if (iBytes == 0)
        {   /* Device closed / removed underneath us. */
            LOG_Warn("input device returned EOF");
            return(-1);
        }

        /* Whole events only; a partial event should never occur on evdev. */
        iNumEv = (int)(iBytes / (ssize_t)sizeof(struct input_event));

        for (i = 0; i < iNumEv; i++)
        {   /* Interpret each raw event, accumulating packet state. */
            struct input_event *pEv = &arrEv[i];

            if (pEv->type == EV_ABS)
            {   /* Absolute-axis update. */
                if (pEv->code == ABS_MT_SLOT)
                {   /* Switch the active multitouch slot. */
                    pInp->inp_iCurSlot = pEv->value;
                }
                else if (pEv->code == ABS_MT_TRACKING_ID)
                {   /* Slot 0's tracking id: -1 lifts the contact, >=0 places it. */
                    if (pInp->inp_iCurSlot == 0)
                    {   /* Only slot 0 drives the pointer. */
                        pInp->inp_bPressed = (pEv->value != -1) ? TRUE : FALSE;
                    }
                }
                else if (pEv->code == ABS_MT_POSITION_X)
                {   /* Slot 0 X position. */
                    if (pInp->inp_iCurSlot == 0)
                    {   /* Track only the first contact. */
                        pInp->inp_iRawX = pEv->value;
                    }
                }
                else if (pEv->code == ABS_MT_POSITION_Y)
                {   /* Slot 0 Y position. */
                    if (pInp->inp_iCurSlot == 0)
                    {   /* Track only the first contact. */
                        pInp->inp_iRawY = pEv->value;
                    }
                }
                else if (pEv->code == ABS_X)
                {   /* Single-touch X. */
                    pInp->inp_iRawX = pEv->value;
                }
                else if (pEv->code == ABS_Y)
                {   /* Single-touch Y. */
                    pInp->inp_iRawY = pEv->value;
                }
            }
            else if (pEv->type == EV_KEY)
            {   /* Key-style events; only the touch contact matters here. */
                if (pEv->code == BTN_TOUCH)
                {   /* Single-touch down/up. */
                    pInp->inp_bPressed = (pEv->value != 0) ? TRUE : FALSE;
                }
            }
            else if (pEv->type == EV_SYN)
            {   /* End of a packet: commit the accumulated state. */
                if (pEv->code == SYN_REPORT)
                {   /* Emit a pointer event if the mapped state changed. */
                    INP_Finalize(pInp, pArrOut, iMaxOut, &iCount);

                    if (iCount >= iMaxOut)
                    {   /* Output array full: stop draining, keep the rest queued. */
                        return(iCount);
                    }
                }
            }
        }
    }

    /* Number of pointer events produced this call. */
    return(iCount);
}
