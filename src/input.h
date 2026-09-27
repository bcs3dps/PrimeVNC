/*----------------------------------------------------------------------------*/
/* SPDX-License-Identifier: GPL-2.0-or-later                                  */
/* Copyright (c) 2026 B. C. Services                                          */
/*                                                                            */
/* Licensed under the GNU General Public License version 2 or later --        */
/* see LICENSE beside this source.  GPL-2.0 full text in GPL-2.0.txt.         */
/*----------------------------------------------------------------------------*/
/* input.h:                                                                   */
/*                                                                            */
/* Touchscreen input for the RFB client, read from a Linux evdev node.  It    */
/* supports both single-touch (ABS_X / ABS_Y / BTN_TOUCH) and multitouch      */
/* protocol B (ABS_MT_SLOT / ABS_MT_TRACKING_ID / ABS_MT_POSITION_X/Y), using */
/* the first contact (slot 0) as the pointer - a touch panel is a single      */
/* pointer as far as VNC is concerned.  Raw device coordinates are read from  */
/* the driver (EVIOCGABS) and mapped to framebuffer coordinates, with an      */
/* optional rotation for a panel mounted turned relative to the touch axes.   */
/*                                                                            */
/* On the reference device (a sun8iw20 printer panel) the touch is            */
/* /dev/input/event0, driver sitronix_ts_i2c, absolute axes X[0..531]         */
/* Y[0..299] (1:1 with the 532x300 framebuffer) and rotation 0; autodetect    */
/* picks it and a touch maps to the matching framebuffer pixel.  Every        */
/* value stays overridable from the command line for another panel.           */
/*                                                                            */
/* No EVIOCGRAB is taken: the launcher guarantees nothing else reads the      */
/* panel while this client holds it, and a grab would only block whatever     */
/* the launcher starts next.                                                  */
/*----------------------------------------------------------------------------*/

#ifndef PRIMEVNC_INPUT_H
#define PRIMEVNC_INPUT_H

/*---------------------------------------------------------------------------*/
/* INP_PointerEvent:                                                         */
/*                                                                           */
/* One pointer state to send to the server: mapped framebuffer coordinates   */
/* and the RFB button mask (bit 0 set while the finger is down).  A drain of */
/* the device can yield several of these (e.g. a quick tap is a down then an */
/* up in one batch), so they are collected into an array by INP_ReadEvents.  */
/*---------------------------------------------------------------------------*/

typedef struct
{
    int pe_iX;                               // framebuffer x (0 .. width-1)
    int pe_iY;                               // framebuffer y (0 .. height-1)
    int pe_iButtonMask;                      // RFB button mask; bit 0 = touch down
} INP_PointerEvent;

/*--------------------------------------------------------------------------*/
/* INP_Device:                                                              */
/*                                                                          */
/* An open evdev touchscreen plus the calibration and running contact state */
/* needed to turn raw events into mapped pointer events.  Held by value in  */
/* the session; no owned memory beyond the fd.                              */
/*--------------------------------------------------------------------------*/

typedef struct
{
    int inp_iFd;                             // open event device fd, -1 when closed

    int inp_iAbsXMin;                        // raw ABS_X / ABS_MT_POSITION_X minimum
    int inp_iAbsXMax;                        // raw maximum
    int inp_iAbsYMin;                        // raw ABS_Y / ABS_MT_POSITION_Y minimum
    int inp_iAbsYMax;                        // raw maximum

    int inp_iScreenW;                        // framebuffer width  for the mapping
    int inp_iScreenH;                        // framebuffer height for the mapping
    int inp_iRotate;                         // 0/90/180/270 applied during mapping

    int inp_iCurSlot;                        // current multitouch slot from ABS_MT_SLOT
    int inp_iRawX;                           // latest raw x for the tracked contact
    int inp_iRawY;                           // latest raw y for the tracked contact
    int inp_bPressed;                        // TRUE while the tracked contact is down

    int inp_iLastX;                          // last EMITTED mapped x (for change detection)
    int inp_iLastY;                          // last emitted mapped y
    int inp_iLastButton;                     // last emitted button mask
    int inp_bHaveLast;                       // TRUE once a first event has been emitted
} INP_Device;

/*--------------------------------------------------------------------------*/
/* INP_Open:                                                                */
/*                                                                          */
/* Opens the touchscreen (or autodetects one when sPath is empty: the first */
/* event node with absolute X that also declares a touch signature,         */
/* INPUT_PROP_DIRECT or BTN_TOUCH, else the first with absolute X at all),  */
/* logs the driver's device name, reads its ABS axis ranges, and stores the */
/* screen geometry and rotation for mapping.  The fd is put in non-blocking */
/* mode so INP_ReadEvents can drain it fully.                               */
/*                                                                          */
/* Arguments:                                                               */
/*     pInp     : device struct to populate.                                */
/*     sPath    : event node path, or "" to autodetect a touch device.      */
/*     iScreenW : framebuffer width, for coordinate mapping.                */
/*     iScreenH : framebuffer height.                                       */
/*     iRotate  : panel rotation applied to touch (0/90/180/270).           */
/*                                                                          */
/* Returns:                                                                 */
/*     int : SUCCESS with *pInp ready, or FAILURE (reason logged).          */
/*--------------------------------------------------------------------------*/

int INP_Open(INP_Device *pInp, const char *sPath, int iScreenW, int iScreenH,
             int iRotate);

/*--------------------------------------------------------------------------*/
/* INP_Close:                                                               */
/*                                                                          */
/* Closes the event fd if open.  Safe on a zeroed or closed device.         */
/*                                                                          */
/* Arguments:                                                               */
/*     pInp : device to release.                                            */
/*                                                                          */
/* Returns:                                                                 */
/*     void : leaves the device closed.                                     */
/*--------------------------------------------------------------------------*/

void INP_Close(INP_Device *pInp);

/*----------------------------------------------------------------------------*/
/* INP_GetFd:                                                                 */
/*                                                                            */
/* Returns the event fd so the control loop can poll it alongside the socket. */
/*                                                                            */
/* Arguments:                                                                 */
/*     pInp : open device.                                                    */
/*                                                                            */
/* Returns:                                                                   */
/*     int : the event fd (>= 0 when open).                                   */
/*----------------------------------------------------------------------------*/

int INP_GetFd(const INP_Device *pInp);

/*-----------------------------------------------------------------------------*/
/* INP_ReadEvents:                                                             */
/*                                                                             */
/* Drains all currently-available input events (the fd is non-blocking),       */
/* tracking the contact state, and appends one INP_PointerEvent to pArrOut for */
/* each SYN_REPORT that changed the mapped position or button.  Coalescing to  */
/* "changed only" keeps redundant events off the wire while preserving every   */
/* down, up and move.                                                          */
/*                                                                             */
/* Arguments:                                                                  */
/*     pInp     : open device.                                                 */
/*     pArrOut  : caller array to receive pointer events.                      */
/*     iMaxOut  : capacity of pArrOut.                                         */
/*                                                                             */
/* Returns:                                                                    */
/*     int : number of pointer events written (0 or more), or -1 if the        */
/*           device errored / disappeared (caller should stop reading it).     */
/*-----------------------------------------------------------------------------*/

int INP_ReadEvents(INP_Device *pInp, INP_PointerEvent *pArrOut, int iMaxOut);

#endif // PRIMEVNC_INPUT_H
