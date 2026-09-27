/*----------------------------------------------------------------------------*/
/* SPDX-License-Identifier: GPL-2.0-or-later                                  */
/* Copyright (c) 2026 B. C. Services                                          */
/*                                                                            */
/* Licensed under the GNU General Public License version 2 or later --        */
/* see LICENSE beside this source.  GPL-2.0 full text in GPL-2.0.txt.         */
/*----------------------------------------------------------------------------*/
/* rfb.h:                                                                     */
/*                                                                            */
/* The RFB (VNC) protocol, client side, per RFC 6143 (RFB 3.8, with the       */
/* 3.3/3.7 handshake variants handled for robustness).  This is a minimal     */
/* client: it negotiates the connection, authenticates (VNC-Authentication    */
/* via the in-house DES, or None), asks the server to send pixels in the      */
/* framebuffer's OWN format so updates blit without conversion, requests RAW  */
/* and CopyRect encodings, and forwards touch as PointerEvents.  Everything   */
/* the server can send that we did not ask for (colour maps, bells, cut text) */
/* is consumed and ignored.                                                   */
/*                                                                            */
/* Wire integers are big-endian (RFC 6143 section 7); the read/write helpers  */
/* in rfb.c do that conversion so the protocol code reads plainly.            */
/*----------------------------------------------------------------------------*/

#ifndef PRIMEVNC_RFB_H
#define PRIMEVNC_RFB_H

#include "pixfmt.h"
#include "framebuffer.h"

/*----------------------------------------------------------------------------*/
/* RFB_Session:                                                               */
/*                                                                            */
/* One connected RFB session: the socket, the server's advertised             */
/* framebuffer size, the pixel format pixels arrive in (which we set to the   */
/* framebuffer's own format), the blit target, and a reusable row buffer for  */
/* reading RAW rectangles a row at a time so a full-screen update never needs */
/* a multi-megabyte allocation.                                               */
/*----------------------------------------------------------------------------*/

typedef struct
{
    int            rfb_iFd;                  // connected socket fd

    int            rfb_iWidth;               // server framebuffer width  (ServerInit)
    int            rfb_iHeight;              // server framebuffer height (ServerInit)

    PIX_Format     rfb_StreamFormat;         // format incoming pixels are in (== fb format)

    FB_Device     *rfb_pFb;                  // framebuffer to draw into (not owned)

    unsigned char *rfb_pRowBuf;              // reusable buffer for one RAW row
    int            rfb_iRowBufLen;           // allocated size of rfb_pRowBuf in bytes
} RFB_Session;

/*------------------------------------------------------------------------------*/
/* RFB_Negotiate:                                                               */
/*                                                                              */
/* Runs the full connection bring-up on an already-connected socket: the        */
/* ProtocolVersion exchange, the security handshake, authentication, ClientInit */
/* and ServerInit.  On return the session knows the server's framebuffer size   */
/* and the connection is ready for RFB_Setup.                                   */
/*                                                                              */
/* Arguments:                                                                   */
/*     pSess     : session to initialise; its fd and fb pointer must be set     */
/*                 by the caller first (see RFB_Init).                          */
/*     pPassword : password bytes for VNC-Authentication.                       */
/*     iPassLen  : length of the password.                                      */
/*                                                                              */
/* Returns:                                                                     */
/*     int : SUCCESS when authenticated and initialised, else FAILURE.          */
/*------------------------------------------------------------------------------*/

int RFB_Negotiate(RFB_Session *pSess, const unsigned char *pPassword, int iPassLen);

/*----------------------------------------------------------------------------*/
/* RFB_Init:                                                                  */
/*                                                                            */
/* Prepares a session struct before RFB_Negotiate: records the socket and the */
/* framebuffer, clears the rest, and does not allocate yet (the row buffer is */
/* sized after ServerInit).                                                   */
/*                                                                            */
/* Arguments:                                                                 */
/*     pSess : session to prepare.                                            */
/*     iFd   : connected socket fd.                                           */
/*     pFb   : framebuffer to draw into.                                      */
/*                                                                            */
/* Returns:                                                                   */
/*     void : fills the session's fixed fields.                               */
/*----------------------------------------------------------------------------*/

void RFB_Init(RFB_Session *pSess, int iFd, FB_Device *pFb);

/*------------------------------------------------------------------------------*/
/* RFB_Setup:                                                                   */
/*                                                                              */
/* Sends SetPixelFormat (requesting the framebuffer's own format) and           */
/* SetEncodings (RAW + CopyRect), then allocates the RAW row buffer.  Call once */
/* after RFB_Negotiate and before the first update request.                     */
/*                                                                              */
/* Arguments:                                                                   */
/*     pSess : negotiated session.                                              */
/*                                                                              */
/* Returns:                                                                     */
/*     int : SUCCESS or FAILURE.                                                */
/*------------------------------------------------------------------------------*/

int RFB_Setup(RFB_Session *pSess);

/*--------------------------------------------------------------------------*/
/* RFB_RequestUpdate:                                                       */
/*                                                                          */
/* Sends a FramebufferUpdateRequest covering the whole framebuffer.  The    */
/* first request of a session is non-incremental (bIncremental FALSE) to    */
/* fetch the full image; every later whole-screen request is incremental    */
/* (the idle-time liveness probe is RFB_SendHeartbeat, not this).           */
/*                                                                          */
/* Arguments:                                                               */
/*     pSess         : active session.                                      */
/*     bIncremental  : FALSE for a full refresh, TRUE for changes only.     */
/*                                                                          */
/* Returns:                                                                 */
/*     int : SUCCESS or FAILURE.                                            */
/*--------------------------------------------------------------------------*/

int RFB_RequestUpdate(RFB_Session *pSess, int bIncremental);

/*--------------------------------------------------------------------------*/
/* RFB_SendHeartbeat:                                                       */
/*                                                                          */
/* Sends a NON-incremental FramebufferUpdateRequest for one pixel at (0,0). */
/* A server must answer a non-incremental request (RFC 6143 section 7.5.3), */
/* so a reply proves it is alive, where an idle server may hold an          */
/* incremental request indefinitely.  The control loop sends one per idle   */
/* period and gives the session up when several go unanswered.              */
/*                                                                          */
/* Arguments:                                                               */
/*     pSess : active session.                                              */
/*                                                                          */
/* Returns:                                                                 */
/*     int : SUCCESS or FAILURE.                                            */
/*--------------------------------------------------------------------------*/

int RFB_SendHeartbeat(RFB_Session *pSess);

/*-----------------------------------------------------------------------------*/
/* RFB_HandleMessage:                                                          */
/*                                                                             */
/* Reads and processes exactly one server-to-client message (the control loop  */
/* calls this when the socket is readable).  A FramebufferUpdate is blitted    */
/* into the framebuffer; other messages are consumed and ignored.  Sets        */
/* *pbWasUpdate so the caller knows to request the next incremental update.    */
/*                                                                             */
/* Arguments:                                                                  */
/*     pSess       : active session.                                           */
/*     pbWasUpdate : out; set TRUE if the message was a FramebufferUpdate.     */
/*                                                                             */
/* Returns:                                                                    */
/*     int : SUCCESS after one message, or FAILURE on protocol / socket error. */
/*-----------------------------------------------------------------------------*/

int RFB_HandleMessage(RFB_Session *pSess, int *pbWasUpdate);

/*----------------------------------------------------------------------------*/
/* RFB_SendPointerEvent:                                                      */
/*                                                                            */
/* Sends one PointerEvent (RFC 6143 section 7.5.5) with the given button mask */
/* and framebuffer coordinates.  Coordinates are clamped to the server's      */
/* framebuffer size.                                                          */
/*                                                                            */
/* Arguments:                                                                 */
/*     pSess       : active session.                                          */
/*     iX          : pointer x in framebuffer pixels.                         */
/*     iY          : pointer y in framebuffer pixels.                         */
/*     iButtonMask : RFB button mask (bit 0 = left/touch).                    */
/*                                                                            */
/* Returns:                                                                   */
/*     int : SUCCESS or FAILURE.                                              */
/*----------------------------------------------------------------------------*/

int RFB_SendPointerEvent(RFB_Session *pSess, int iX, int iY, int iButtonMask);

/*----------------------------------------------------------------------------*/
/* RFB_Free:                                                                  */
/*                                                                            */
/* Releases the session's row buffer.  Does not close the socket - the caller */
/* owns that (see main's session teardown).                                   */
/*                                                                            */
/* Arguments:                                                                 */
/*     pSess : session to release.                                            */
/*                                                                            */
/* Returns:                                                                   */
/*     void : frees owned memory.                                             */
/*----------------------------------------------------------------------------*/

void RFB_Free(RFB_Session *pSess);

#endif // PRIMEVNC_RFB_H
