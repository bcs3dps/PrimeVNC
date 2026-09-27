/*----------------------------------------------------------------------------*/
/* SPDX-License-Identifier: GPL-2.0-or-later                                  */
/* Copyright (c) 2026 B. C. Services                                          */
/*                                                                            */
/* Licensed under the GNU General Public License version 2 or later --        */
/* see LICENSE beside this source.  GPL-2.0 full text in GPL-2.0.txt.         */
/*----------------------------------------------------------------------------*/
/* rfb.c:                                                                     */
/*                                                                            */
/* Implementation of the client-side RFB protocol declared in rfb.h, per      */
/* RFC 6143.  The negotiation supports the 3.8 handshake (and the 3.7 / 3.3   */
/* variants for robustness); authentication is VNC-Authentication via the     */
/* in-house DES, or None; and the update loop decodes RAW and CopyRect        */
/* rectangles straight into the framebuffer, reading RAW a row at a time so a */
/* full-screen update needs only a one-row buffer.                            */
/*                                                                            */
/* All wire integers are big-endian (RFC 6143 section 7).  The RFB_Get* /     */
/* RFB_Put* helpers localise that so the protocol steps read in host order.   */
/*----------------------------------------------------------------------------*/

#include "rfb.h"
#include "net.h"
#include "des.h"
#include "pixfmt.h"
#include "framebuffer.h"
#include "log.h"
#include "primevnc.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/*---------------------------------------------------------------------------*/
/* Protocol constants (RFC 6143).  Message type codes are the first byte of  */
/* each message; encoding numbers are signed 32-bit in the rectangle header. */
/*---------------------------------------------------------------------------*/

#define RFB_READ_TIMEOUT_MS   20000          // per-chunk read timeout during a message

#define RFB_SMSG_UPDATE        0             // server: FramebufferUpdate
#define RFB_SMSG_COLOURMAP     1             // server: SetColourMapEntries
#define RFB_SMSG_BELL          2             // server: Bell
#define RFB_SMSG_CUTTEXT       3             // server: ServerCutText

#define RFB_CMSG_SETPIXFMT     0             // client: SetPixelFormat
#define RFB_CMSG_SETENCODINGS  2             // client: SetEncodings
#define RFB_CMSG_UPDATEREQ     3             // client: FramebufferUpdateRequest
#define RFB_CMSG_POINTER       5             // client: PointerEvent

#define RFB_ENC_RAW            0             // encoding: raw pixels
#define RFB_ENC_COPYRECT       1             // encoding: copy from elsewhere in the fb

#define RFB_SEC_NONE           1             // security type: None
#define RFB_SEC_VNCAUTH        2             // security type: VNC Authentication

#define RFB_SCRATCH            512           // discard-buffer size

/*--------------------------------------------------------------------------*/
/* RFB_PutU16:                                                              */
/*                                                                          */
/* Stores a 16-bit value big-endian into a two-byte buffer.  Local.         */
/*                                                                          */
/* Arguments:                                                               */
/*     pBuf : two-byte destination.                                         */
/*     uVal : value to store.                                               */
/*                                                                          */
/* Returns:                                                                 */
/*     void : writes two bytes.                                             */
/*--------------------------------------------------------------------------*/

static void RFB_PutU16(unsigned char *pBuf, unsigned int uVal)
{
    /* High byte first (network order). */
    pBuf[0] = (unsigned char)((uVal >> 8) & 0xFF);
    pBuf[1] = (unsigned char)(uVal & 0xFF);
}

/*--------------------------------------------------------------------------*/
/* RFB_PutU32:                                                              */
/*                                                                          */
/* Stores a 32-bit value big-endian into a four-byte buffer.  Local.        */
/*                                                                          */
/* Arguments:                                                               */
/*     pBuf : four-byte destination.                                        */
/*     uVal : value to store.                                               */
/*                                                                          */
/* Returns:                                                                 */
/*     void : writes four bytes.                                            */
/*--------------------------------------------------------------------------*/

static void RFB_PutU32(unsigned char *pBuf, unsigned long uVal)
{
    /* Most significant byte first. */
    pBuf[0] = (unsigned char)((uVal >> 24) & 0xFF);
    pBuf[1] = (unsigned char)((uVal >> 16) & 0xFF);
    pBuf[2] = (unsigned char)((uVal >> 8) & 0xFF);
    pBuf[3] = (unsigned char)(uVal & 0xFF);
}

/*--------------------------------------------------------------------------*/
/* RFB_GetU16:                                                              */
/*                                                                          */
/* Reads a big-endian 16-bit value from a two-byte buffer.  Local.          */
/*                                                                          */
/* Arguments:                                                               */
/*     pBuf : two-byte source.                                              */
/*                                                                          */
/* Returns:                                                                 */
/*     unsigned int : the decoded value.                                    */
/*--------------------------------------------------------------------------*/

static unsigned int RFB_GetU16(const unsigned char *pBuf)
{
    /* Reassemble high byte then low byte. */
    return(((unsigned int)pBuf[0] << 8) | (unsigned int)pBuf[1]);
}

/*--------------------------------------------------------------------------*/
/* RFB_GetU32:                                                              */
/*                                                                          */
/* Reads a big-endian 32-bit value from a four-byte buffer.  Local.         */
/*                                                                          */
/* Arguments:                                                               */
/*     pBuf : four-byte source.                                             */
/*                                                                          */
/* Returns:                                                                 */
/*     unsigned long : the decoded value.                                   */
/*--------------------------------------------------------------------------*/

static unsigned long RFB_GetU32(const unsigned char *pBuf)
{
    /* Reassemble from most significant byte down. */
    return(((unsigned long)pBuf[0] << 24) |
           ((unsigned long)pBuf[1] << 16) |
           ((unsigned long)pBuf[2] << 8)  |
           ((unsigned long)pBuf[3]));
}

/*--------------------------------------------------------------------------*/
/* RFB_Read:                                                                */
/*                                                                          */
/* Reads exactly iLen bytes with the protocol read timeout.  A thin wrapper */
/* over NET_ReadFull so the RFB steps do not repeat the timeout argument.   */
/* Local to this file.                                                      */
/*                                                                          */
/* Arguments:                                                               */
/*     pSess : session (for the fd).                                        */
/*     pBuf  : destination buffer.                                          */
/*     iLen  : bytes to read.                                               */
/*                                                                          */
/* Returns:                                                                 */
/*     int : SUCCESS or FAILURE.                                            */
/*--------------------------------------------------------------------------*/

static int RFB_Read(RFB_Session *pSess, void *pBuf, size_t iLen)
{
    /* All RFB reads share one timeout so a stalled server is caught. */
    return(NET_ReadFull(pSess->rfb_iFd, pBuf, iLen, RFB_READ_TIMEOUT_MS));
}

/*---------------------------------------------------------------------------*/
/* RFB_Discard:                                                              */
/*                                                                           */
/* Reads and throws away iLen bytes (a server name, a colour map, cut text). */
/* Reads in scratch-sized chunks so an arbitrarily large field costs no      */
/* allocation.  Local to this file.                                          */
/*                                                                           */
/* Arguments:                                                                */
/*     pSess : session.                                                      */
/*     iLen  : number of bytes to consume.                                   */
/*                                                                           */
/* Returns:                                                                  */
/*     int : SUCCESS if all consumed, FAILURE on read error.                 */
/*---------------------------------------------------------------------------*/

static int RFB_Discard(RFB_Session *pSess, unsigned long iLen)
{
    unsigned char arrScratch[RFB_SCRATCH];     // throwaway landing buffer
    unsigned long iRemaining;                  // bytes still to consume
    size_t        iChunk;                      // bytes this pass

    iRemaining = iLen;

    while (iRemaining > 0)
    {   /* Consume up to a scratch-buffer's worth at a time. */
        iChunk = (iRemaining > (unsigned long)RFB_SCRATCH) ? (size_t)RFB_SCRATCH : (size_t)iRemaining;

        if (RFB_Read(pSess, arrScratch, iChunk) != SUCCESS)
        {   /* Underlying read failed or timed out. */
            return(FAILURE);
        }

        iRemaining -= (unsigned long)iChunk;
    }

    /* All requested bytes were read and dropped. */
    return(SUCCESS);
}

/*----------------------------------------------------------------------------*/
/* RFB_EnsureRowBuf:                                                          */
/*                                                                            */
/* Grows the reusable RAW-row buffer to at least iBytes, if needed.  Called   */
/* before reading each RAW rectangle's row so an unexpectedly wide rectangle  */
/* cannot overflow the buffer.  Local to this file.                           */
/*                                                                            */
/* Arguments:                                                                 */
/*     pSess  : session holding the buffer.                                   */
/*     iBytes : minimum capacity required.                                    */
/*                                                                            */
/* Returns:                                                                   */
/*     int : SUCCESS if the buffer is large enough, FAILURE on alloc failure. */
/*----------------------------------------------------------------------------*/

static int RFB_EnsureRowBuf(RFB_Session *pSess, int iBytes)
{
    unsigned char *pNew;                        // reallocated buffer

    if (iBytes <= pSess->rfb_iRowBufLen)
    {   /* Already big enough. */
        return(SUCCESS);
    }

    /* Grow to the requested size (rectangles do not oscillate in width, so a   */
    /* plain grow-to-fit is fine here).                                         */
    pNew = (unsigned char *)realloc(pSess->rfb_pRowBuf, (size_t)iBytes);

    if (pNew == NULL)
    {   /* Out of memory; keep the old buffer and fail the update. */
        LOG_Error("could not grow RAW row buffer to %d bytes", iBytes);
        return(FAILURE);
    }

    pSess->rfb_pRowBuf    = pNew;
    pSess->rfb_iRowBufLen = iBytes;
    /* The buffer now holds at least iBytes. */
    return(SUCCESS);
}

/*-----------------------------------------------------------------------------*/
/* RFB_FormatToWire:                                                           */
/*                                                                             */
/* Serialises a PIX_Format into the 16-byte RFB PIXEL_FORMAT structure (RFC    */
/* 6143 section 7.4).  Used to tell the server which format to send pixels in. */
/* Local to this file.                                                         */
/*                                                                             */
/* Arguments:                                                                  */
/*     pFmt  : the pixel format to serialise.                                  */
/*     pWire : 16-byte destination buffer.                                     */
/*                                                                             */
/* Returns:                                                                    */
/*     void : fills 16 bytes.                                                  */
/*-----------------------------------------------------------------------------*/

static void RFB_FormatToWire(const PIX_Format *pFmt, unsigned char *pWire)
{
    /* bits-per-pixel, depth, endianness flag, true-colour flag. */
    pWire[0] = (unsigned char)(pFmt->pix_iBytesPerPixel * 8);
    pWire[1] = (unsigned char)pFmt->pix_iDepth;
    pWire[2] = (unsigned char)((pFmt->pix_bBigEndian != FALSE) ? 1 : 0);
    pWire[3] = 1;                              // true colour: this client has no palette path

    /* Channel maxima (big-endian u16 each). */
    RFB_PutU16(&pWire[4], (unsigned int)pFmt->pix_uRedMax);
    RFB_PutU16(&pWire[6], (unsigned int)pFmt->pix_uGreenMax);
    RFB_PutU16(&pWire[8], (unsigned int)pFmt->pix_uBlueMax);

    /* Channel shifts (u8 each). */
    pWire[10] = (unsigned char)pFmt->pix_iRedShift;
    pWire[11] = (unsigned char)pFmt->pix_iGreenShift;
    pWire[12] = (unsigned char)pFmt->pix_iBlueShift;

    /* Three padding bytes. */
    pWire[13] = 0;
    pWire[14] = 0;
    pWire[15] = 0;
}

/*--------------------------------------------------------------------------*/
/* RFB_Init:                                                                */
/*                                                                          */
/* Prepares the session struct.  See rfb.h.                                 */
/*                                                                          */
/* Arguments:                                                               */
/*     pSess : session to prepare.                                          */
/*     iFd   : connected socket.                                            */
/*     pFb   : framebuffer to draw into.                                    */
/*                                                                          */
/* Returns:                                                                 */
/*     void : fills the fixed fields.                                       */
/*--------------------------------------------------------------------------*/

void RFB_Init(RFB_Session *pSess, int iFd, FB_Device *pFb)
{
    /* Clear everything, then set the two externally owned handles. */
    memset(pSess, 0, sizeof(*pSess));
    pSess->rfb_iFd        = iFd;
    pSess->rfb_pFb        = pFb;
    pSess->rfb_pRowBuf    = NULL;
    pSess->rfb_iRowBufLen = 0;
}

/*--------------------------------------------------------------------------*/
/* RFB_NegotiateVersion:                                                    */
/*                                                                          */
/* Exchanges ProtocolVersion strings and returns the agreed minor version   */
/* (3, 7 or 8).  Local to this file.                                        */
/*                                                                          */
/* Arguments:                                                               */
/*     pSess    : session (for the fd).                                     */
/*     piMinor  : out; the agreed minor version number.                     */
/*                                                                          */
/* Returns:                                                                 */
/*     int : SUCCESS or FAILURE.                                            */
/*--------------------------------------------------------------------------*/

static int RFB_NegotiateVersion(RFB_Session *pSess, int *piMinor)
{
    unsigned char arrVer[12];                  // server's 12-byte version banner
    char          sReply[16];                  // our reply, formatted (needs room for NUL)
    int           iServerMinor;                // minor the server offered
    int           iUseMinor;                   // minor we will speak

    /* Read the server's 12-byte version banner. */
    if (RFB_Read(pSess, arrVer, 12) != SUCCESS)
    {   /* No banner: not an RFB server, or the connection died. */
        LOG_Error("failed to read server ProtocolVersion");
        return(FAILURE);
    }

    if (memcmp(arrVer, "RFB ", 4) != 0)
    {   /* The banner must start with the RFB signature. */
        LOG_Error("server did not send an RFB ProtocolVersion banner");
        return(FAILURE);
    }

    /* Parse the minor from the three digits after the dot ("008" -> 8). */
    iServerMinor = ((arrVer[8] - '0') * 100) + ((arrVer[9] - '0') * 10) + (arrVer[10] - '0');

    /* Speak the highest version we support that the server also supports. */
    if (iServerMinor >= 8)
    {   /* Full 3.8. */
        iUseMinor = 8;
    }
    else if (iServerMinor >= 7)
    {   /* 3.7 handshake (u8 security list, no SecurityResult reason string). */
        iUseMinor = 7;
    }
    else
    {   /* Anything older is handled as 3.3. */
        iUseMinor = 3;
    }

    /* Reply with our chosen version.  The wire form is exactly 12 bytes with no   */
    /* terminator; format into a larger buffer (so snprintf's NUL fits) and send   */
    /* the first 12 bytes.                                                         */
    snprintf(sReply, sizeof(sReply), "RFB 003.%03d\n", iUseMinor);

    if (NET_WriteFull(pSess->rfb_iFd, sReply, 12) != SUCCESS)
    {   /* Could not send our version. */
        LOG_Error("failed to send client ProtocolVersion");
        return(FAILURE);
    }

    *piMinor = iUseMinor;
    LOG_Info("RFB version negotiated: 3.%d (server offered 3.%d)", iUseMinor, iServerMinor);
    /* Version agreed; the minor is in *piMinor. */
    return(SUCCESS);
}

/*--------------------------------------------------------------------------*/
/* RFB_ChooseSecurity:                                                      */
/*                                                                          */
/* Performs the security-type handshake for the agreed version and returns  */
/* the security type to use (RFB_SEC_VNCAUTH preferred, else RFB_SEC_NONE). */
/* Handles both the 3.7/3.8 list form and the 3.3 single-u32 form.  Local.  */
/*                                                                          */
/* Arguments:                                                               */
/*     pSess     : session.                                                 */
/*     iMinor    : the agreed minor version.                                */
/*     piSecType : out; the chosen security type.                           */
/*                                                                          */
/* Returns:                                                                 */
/*     int : SUCCESS or FAILURE.                                            */
/*--------------------------------------------------------------------------*/

static int RFB_ChooseSecurity(RFB_Session *pSess, int iMinor, int *piSecType)
{
    unsigned char arrCount[1];                 // number of security types (3.7+)
    unsigned char arrTypes[256];               // the offered type bytes
    unsigned char arrU32[4];                   // a 32-bit field
    unsigned char ucChosen;                    // the type we select
    int           iNum;                        // count of offered types
    int           bHaveVnc;                    // TRUE if VNC auth was offered
    int           bHaveNone;                   // TRUE if None was offered
    int           i;

    if (iMinor >= 7)
    {   /* 3.7 / 3.8: a count byte, then that many one-byte type codes. */

        if (RFB_Read(pSess, arrCount, 1) != SUCCESS)
        {   /* No security list arrived. */
            LOG_Error("failed to read security type count");
            return(FAILURE);
        }

        iNum = (int)arrCount[0];

        if (iNum == 0)
        {   /* Zero types means the server is refusing us; a reason follows. */
            if (RFB_Read(pSess, arrU32, 4) == SUCCESS)
            {   /* Consume the reason string so the error is clean. */
                RFB_Discard(pSess, RFB_GetU32(arrU32));
            }

            LOG_Error("server offered no security types (connection refused)");
            return(FAILURE);
        }

        /* Read the offered type codes. */
        if (RFB_Read(pSess, arrTypes, (size_t)iNum) != SUCCESS)
        {   /* Truncated list. */
            LOG_Error("failed to read security type list");
            return(FAILURE);
        }

        /* Scan the list for the types we can perform. */
        bHaveVnc  = FALSE;
        bHaveNone = FALSE;

        for (i = 0; i < iNum; i++)
        {   /* Note each type we recognise. */
            if (arrTypes[i] == RFB_SEC_VNCAUTH)
            {   /* Preferred: password challenge-response. */
                bHaveVnc = TRUE;
            }
            else if (arrTypes[i] == RFB_SEC_NONE)
            {   /* Acceptable fallback: an open server. */
                bHaveNone = TRUE;
            }
        }

        if (bHaveVnc != FALSE)
        {   /* Prefer VNC authentication when available. */
            ucChosen = RFB_SEC_VNCAUTH;
        }
        else if (bHaveNone != FALSE)
        {   /* Otherwise accept an unauthenticated server. */
            ucChosen = RFB_SEC_NONE;
        }
        else
        {   /* Nothing we can do (e.g. TLS/VeNCrypt only). */
            LOG_Error("server offers no security type this client supports");
            return(FAILURE);
        }

        /* Tell the server which type we chose. */
        if (NET_WriteFull(pSess->rfb_iFd, &ucChosen, 1) != SUCCESS)
        {   /* Could not send the selection. */
            LOG_Error("failed to send chosen security type");
            return(FAILURE);
        }

        *piSecType = (int)ucChosen;
        return(SUCCESS);
    }

    /* 3.3: the server dictates a single 32-bit security type; we do not reply. */
    if (RFB_Read(pSess, arrU32, 4) != SUCCESS)
    {   /* No security word. */
        LOG_Error("failed to read 3.3 security type");
        return(FAILURE);
    }

    iNum = (int)RFB_GetU32(arrU32);

    if (iNum == 0)
    {   /* Refused: a reason string follows. */
        if (RFB_Read(pSess, arrU32, 4) == SUCCESS)
        {   /* Consume the reason. */
            RFB_Discard(pSess, RFB_GetU32(arrU32));
        }

        LOG_Error("3.3 server refused the connection");
        return(FAILURE);
    }

    if ((iNum != RFB_SEC_NONE) && (iNum != RFB_SEC_VNCAUTH))
    {   /* We only implement None and VNC auth. */
        LOG_Error("3.3 server requires unsupported security type %d", iNum);
        return(FAILURE);
    }

    *piSecType = iNum;
    /* Accepted the 3.3 server's dictated type. */
    return(SUCCESS);
}

/*--------------------------------------------------------------------------*/
/* RFB_Authenticate:                                                        */
/*                                                                          */
/* Completes authentication for the chosen security type and reads the      */
/* SecurityResult where the version/type require it.  Local to this file.   */
/*                                                                          */
/* Arguments:                                                               */
/*     pSess     : session.                                                 */
/*     iMinor    : agreed minor version.                                    */
/*     iSecType  : chosen security type.                                    */
/*     pPassword : password bytes (VNC auth only).                          */
/*     iPassLen  : password length.                                         */
/*                                                                          */
/* Returns:                                                                 */
/*     int : SUCCESS if authenticated, else FAILURE.                        */
/*--------------------------------------------------------------------------*/

static int RFB_Authenticate(RFB_Session *pSess, int iMinor, int iSecType,
                            const unsigned char *pPassword, int iPassLen)
{
    unsigned char arrChallenge[16];  // server challenge
    unsigned char arrResponse[16];   // DES response
    unsigned char arrU32[4];         // SecurityResult / reason length
    int           bExpectResult;     // TRUE if a SecurityResult follows
    unsigned long uResult;           // 0 = OK

    if (iSecType == RFB_SEC_VNCAUTH)
    {   /* VNC Authentication: DES challenge-response (RFC 6143 section 7.2.2). */

        if (iPassLen <= 0)
        {   /* The server wants a password and none was supplied. */
            LOG_Error("server requires VNC authentication but no password was provided");
            return(FAILURE);
        }

        /* Read the 16-byte challenge. */
        if (RFB_Read(pSess, arrChallenge, 16) != SUCCESS)
        {   /* No challenge arrived. */
            LOG_Error("failed to read VNC auth challenge");
            return(FAILURE);
        }

        /* Encrypt it with the (bit-reversed) password key and reply. */
        DES_VncEncryptChallenge(pPassword, (size_t)iPassLen, arrChallenge, arrResponse);

        if (NET_WriteFull(pSess->rfb_iFd, arrResponse, 16) != SUCCESS)
        {   /* Could not send the response. */
            LOG_Error("failed to send VNC auth response");
            return(FAILURE);
        }

        /* A SecurityResult always follows VNC auth, in every version. */
        bExpectResult = TRUE;
    }
    else
    {   /* Security type None: nothing to send.  Only 3.8 sends a SecurityResult   */
        /* for None; 3.3 and 3.7 proceed straight to ClientInit.                   */
        bExpectResult = (iMinor >= 8) ? TRUE : FALSE;
    }

    if (bExpectResult == FALSE)
    {   /* No result word to read; authentication is implicitly accepted. */
        return(SUCCESS);
    }

    /* Read the 4-byte SecurityResult: 0 = OK, non-zero = failed. */
    if (RFB_Read(pSess, arrU32, 4) != SUCCESS)
    {   /* Result missing. */
        LOG_Error("failed to read SecurityResult");
        return(FAILURE);
    }

    uResult = RFB_GetU32(arrU32);

    if (uResult != 0)
    {   /* Authentication failed - usually a wrong password. */
        if (iMinor >= 8)
        {   /* 3.8 appends a human-readable reason string. */
            if (RFB_Read(pSess, arrU32, 4) == SUCCESS)
            {   /* Drop the reason text (already know it failed). */
                RFB_Discard(pSess, RFB_GetU32(arrU32));
            }
        }

        LOG_Error("authentication failed (wrong password?)");
        return(FAILURE);
    }

    /* Authenticated. */
    return(SUCCESS);
}

/*--------------------------------------------------------------------------*/
/* RFB_Negotiate:                                                           */
/*                                                                          */
/* Runs version, security, authentication, ClientInit and ServerInit.  See  */
/* rfb.h for the contract.                                                  */
/*                                                                          */
/* Arguments:                                                               */
/*     pSess     : initialised session.                                     */
/*     pPassword : password bytes.                                          */
/*     iPassLen  : password length.                                         */
/*                                                                          */
/* Returns:                                                                 */
/*     int : SUCCESS or FAILURE.                                            */
/*--------------------------------------------------------------------------*/

int RFB_Negotiate(RFB_Session *pSess, const unsigned char *pPassword, int iPassLen)
{
    unsigned char arrInit[24];                 // ServerInit fixed part (w,h,fmt)
    unsigned char ucShared;                    // ClientInit shared flag
    unsigned long uNameLen;                    // length of the server name
    int           iMinor;                      // agreed protocol minor
    int           iSecType;                    // chosen security type

    /* ProtocolVersion. */
    if (RFB_NegotiateVersion(pSess, &iMinor) != SUCCESS)
    {   /* Version step failed. */
        return(FAILURE);
    }

    /* Security type selection. */
    if (RFB_ChooseSecurity(pSess, iMinor, &iSecType) != SUCCESS)
    {   /* Security step failed. */
        return(FAILURE);
    }

    /* Authentication (and SecurityResult where applicable). */
    if (RFB_Authenticate(pSess, iMinor, iSecType, pPassword, iPassLen) != SUCCESS)
    {   /* Auth step failed. */
        return(FAILURE);
    }

    /* ClientInit: request a shared session so the stock server keeps other   */
    /* viewers (there are none here, but shared is the safe default).         */
    ucShared = 1;

    if (NET_WriteFull(pSess->rfb_iFd, &ucShared, 1) != SUCCESS)
    {   /* Could not send ClientInit. */
        LOG_Error("failed to send ClientInit");
        return(FAILURE);
    }

    /* ServerInit: width(u16) height(u16) pixel-format(16) name-length(u32). */
    if (RFB_Read(pSess, arrInit, 24) != SUCCESS)
    {   /* No ServerInit. */
        LOG_Error("failed to read ServerInit");
        return(FAILURE);
    }

    pSess->rfb_iWidth  = (int)RFB_GetU16(&arrInit[0]);
    pSess->rfb_iHeight = (int)RFB_GetU16(&arrInit[2]);
    uNameLen           = RFB_GetU32(&arrInit[20]);

    /* Consume the desktop name; the client does not display it. */
    if (RFB_Discard(pSess, uNameLen) != SUCCESS)
    {   /* Name field truncated. */
        LOG_Error("failed to read ServerInit desktop name");
        return(FAILURE);
    }

    LOG_Info("ServerInit: %dx%d desktop", pSess->rfb_iWidth, pSess->rfb_iHeight);

    /* Warn (do not fail) if the server size differs from the panel: a server      */
    /* meant for this panel is sized 1:1 to it, so a mismatch means that has not   */
    /* been set up; the blit still clips safely.                                   */
    if ((pSess->rfb_iWidth != pSess->rfb_pFb->fb_iWidth) ||
        (pSess->rfb_iHeight != pSess->rfb_pFb->fb_iHeight))
    {   /* Sizes differ: usable but not pixel-perfect. */
        LOG_Warn("server %dx%d != panel %dx%d; expected a server sized 1:1 to the panel",
                 pSess->rfb_iWidth, pSess->rfb_iHeight,
                 pSess->rfb_pFb->fb_iWidth, pSess->rfb_pFb->fb_iHeight);
    }

    /* Negotiation complete. */
    return(SUCCESS);
}

/*---------------------------------------------------------------------------*/
/* RFB_Setup:                                                                */
/*                                                                           */
/* Sends SetPixelFormat and SetEncodings, then allocates the RAW row buffer. */
/* See rfb.h.                                                                */
/*                                                                           */
/* Arguments:                                                                */
/*     pSess : negotiated session.                                           */
/*                                                                           */
/* Returns:                                                                  */
/*     int : SUCCESS or FAILURE.                                             */
/*---------------------------------------------------------------------------*/

int RFB_Setup(RFB_Session *pSess)
{
    unsigned char arrPixMsg[20];               // SetPixelFormat message
    unsigned char arrEncMsg[16];               // SetEncodings message
    int           iMaxW;                       // widest row we might read

    /* The pixels we ask for match the framebuffer exactly, so a rectangle blits   */
    /* with a plain copy and no per-pixel conversion (the fast path).              */
    pSess->rfb_StreamFormat = pSess->rfb_pFb->fb_Format;

    /* SetPixelFormat: type(1) + 3 padding + 16-byte pixel format. */
    arrPixMsg[0] = RFB_CMSG_SETPIXFMT;
    arrPixMsg[1] = 0;
    arrPixMsg[2] = 0;
    arrPixMsg[3] = 0;
    RFB_FormatToWire(&pSess->rfb_StreamFormat, &arrPixMsg[4]);

    if (NET_WriteFull(pSess->rfb_iFd, arrPixMsg, 20) != SUCCESS)
    {   /* Could not set the pixel format. */
        LOG_Error("failed to send SetPixelFormat");
        return(FAILURE);
    }

    /* SetEncodings: type(1) + 1 padding + count(u16) + count * u32 encodings.   */
    /* We advertise RAW and CopyRect only; the server will send nothing else.    */
    arrEncMsg[0] = RFB_CMSG_SETENCODINGS;
    arrEncMsg[1] = 0;
    RFB_PutU16(&arrEncMsg[2], 2);
    RFB_PutU32(&arrEncMsg[4], (unsigned long)RFB_ENC_RAW);
    RFB_PutU32(&arrEncMsg[8], (unsigned long)RFB_ENC_COPYRECT);

    if (NET_WriteFull(pSess->rfb_iFd, arrEncMsg, 12) != SUCCESS)
    {   /* Could not set encodings. */
        LOG_Error("failed to send SetEncodings");
        return(FAILURE);
    }

    /* Size the RAW row buffer to the widest of server and panel width. */
    iMaxW = (pSess->rfb_iWidth > pSess->rfb_pFb->fb_iWidth)
                ? pSess->rfb_iWidth : pSess->rfb_pFb->fb_iWidth;

    if (RFB_EnsureRowBuf(pSess, iMaxW * pSess->rfb_StreamFormat.pix_iBytesPerPixel) != SUCCESS)
    {   /* Allocation failed. */
        return(FAILURE);
    }

    /* Ready to request updates. */
    return(SUCCESS);
}

/*--------------------------------------------------------------------------*/
/* RFB_RequestUpdate:                                                       */
/*                                                                          */
/* Sends a whole-screen FramebufferUpdateRequest.  See rfb.h.               */
/*                                                                          */
/* Arguments:                                                               */
/*     pSess        : active session.                                       */
/*     bIncremental : FALSE for a full refresh, TRUE for changes only.      */
/*                                                                          */
/* Returns:                                                                 */
/*     int : SUCCESS or FAILURE.                                            */
/*--------------------------------------------------------------------------*/

int RFB_RequestUpdate(RFB_Session *pSess, int bIncremental)
{
    unsigned char arrReq[10];                   // FramebufferUpdateRequest

    /* type(1), incremental flag(1), x(u16), y(u16), w(u16), h(u16). */
    arrReq[0] = RFB_CMSG_UPDATEREQ;
    arrReq[1] = (unsigned char)((bIncremental != FALSE) ? 1 : 0);
    RFB_PutU16(&arrReq[2], 0);
    RFB_PutU16(&arrReq[4], 0);
    RFB_PutU16(&arrReq[6], (unsigned int)pSess->rfb_iWidth);
    RFB_PutU16(&arrReq[8], (unsigned int)pSess->rfb_iHeight);

    if (NET_WriteFull(pSess->rfb_iFd, arrReq, 10) != SUCCESS)
    {   /* Could not ask for an update. */
        LOG_Error("failed to send FramebufferUpdateRequest");
        return(FAILURE);
    }

    /* Request sent. */
    return(SUCCESS);
}

/*---------------------------------------------------------------------------*/
/* RFB_SendHeartbeat:                                                        */
/*                                                                           */
/* Sends the liveness probe: a NON-incremental FramebufferUpdateRequest for  */
/* the single pixel at (0,0).  RFC 6143 section 7.5.3 obliges the server to  */
/* answer a non-incremental request, whereas an idle server may legitimately */
/* hold an incremental one forever - so this, not the incremental keepalive, */
/* is what the control loop counts replies to.  One pixel keeps the reply    */
/* small on a real server (the test rig answers with its whole frame).       */
/*                                                                           */
/* Arguments:                                                                */
/*     pSess : active session.                                               */
/*                                                                           */
/* Returns:                                                                  */
/*     int : SUCCESS or FAILURE (the write failed: the socket is gone).      */
/*---------------------------------------------------------------------------*/

int RFB_SendHeartbeat(RFB_Session *pSess)
{
    unsigned char arrReq[10];                   // FramebufferUpdateRequest

    /* type(1), incremental = 0, x = 0, y = 0, w = 1, h = 1. */
    arrReq[0] = RFB_CMSG_UPDATEREQ;
    arrReq[1] = 0;
    RFB_PutU16(&arrReq[2], 0);
    RFB_PutU16(&arrReq[4], 0);
    RFB_PutU16(&arrReq[6], 1);
    RFB_PutU16(&arrReq[8], 1);

    if (NET_WriteFull(pSess->rfb_iFd, arrReq, 10) != SUCCESS)
    {   /* Could not send the probe: the socket is already dead. */
        LOG_Error("failed to send heartbeat FramebufferUpdateRequest");
        return(FAILURE);
    }

    /* Probe sent; the caller counts whether anything comes back. */
    return(SUCCESS);
}

/*---------------------------------------------------------------------------*/
/* RFB_ReadRawRect:                                                          */
/*                                                                           */
/* Reads a RAW-encoded rectangle a row at a time and blits each row into the */
/* framebuffer.  Reading per row bounds memory to one row regardless of      */
/* rectangle size.  Local to this file.                                      */
/*                                                                           */
/* Arguments:                                                                */
/*     pSess : active session.                                               */
/*     iX,iY : rectangle top-left.                                           */
/*     iW,iH : rectangle size.                                               */
/*                                                                           */
/* Returns:                                                                  */
/*     int : SUCCESS or FAILURE.                                             */
/*---------------------------------------------------------------------------*/

static int RFB_ReadRawRect(RFB_Session *pSess, int iX, int iY, int iW, int iH)
{
    int iRowBytes;                             // bytes in one rectangle row
    int r;

    /* Bytes for one row of this rectangle in the stream's pixel format. */
    iRowBytes = iW * pSess->rfb_StreamFormat.pix_iBytesPerPixel;

    /* Make sure the reusable buffer can hold a full row. */
    if (RFB_EnsureRowBuf(pSess, iRowBytes) != SUCCESS)
    {   /* Cannot size the buffer; abort this update. */
        return(FAILURE);
    }

    for (r = 0; r < iH; r++)
    {   /* One network row read, one framebuffer row blit. */

        if (RFB_Read(pSess, pSess->rfb_pRowBuf, (size_t)iRowBytes) != SUCCESS)
        {   /* Truncated rectangle data. */
            return(FAILURE);
        }

        /* Blit this single row at its screen position; FB clips as needed. */
        FB_BlitRaw(pSess->rfb_pFb, iX, iY + r, iW, 1,
                   pSess->rfb_pRowBuf, &pSess->rfb_StreamFormat);
    }

    /* Whole rectangle drawn. */
    return(SUCCESS);
}

/*----------------------------------------------------------------------------*/
/* RFB_ReadUpdate:                                                            */
/*                                                                            */
/* Reads the body of a FramebufferUpdate: the rectangle count and each        */
/* rectangle (RAW or CopyRect).  An encoding we did not request is a protocol */
/* error, since its length is then unknown and the stream cannot be resynced. */
/* Local to this file.                                                        */
/*                                                                            */
/* Arguments:                                                                 */
/*     pSess : active session.                                                */
/*                                                                            */
/* Returns:                                                                   */
/*     int : SUCCESS or FAILURE.                                              */
/*----------------------------------------------------------------------------*/

static int RFB_ReadUpdate(RFB_Session *pSess)
{
    unsigned char arrHdr[4];                    // padding(1) + rect count(u16) ...
    unsigned char arrRect[12];                  // x,y,w,h,encoding per rectangle
    unsigned char arrCopy[4];                   // CopyRect source x,y
    long          lEncoding;                    // rectangle encoding (signed)
    int           iNumRects;                    // rectangles in this update
    int           iX;                           // rectangle left, from the wire
    int           iY;                           // rectangle top
    int           iW;                           // rectangle width
    int           iH;                           // rectangle height
    int           iSrcX;                        // CopyRect source left
    int           iSrcY;                        // CopyRect source top
    int           iLimW;                        // width a CopyRect source must fit in
    int           iLimH;                        // height a CopyRect source must fit in
    int           i;

    /* One padding byte then the 16-bit rectangle count. */
    if (RFB_Read(pSess, arrHdr, 3) != SUCCESS)
    {   /* Truncated update header. */
        return(FAILURE);
    }

    iNumRects = (int)RFB_GetU16(&arrHdr[1]);

    for (i = 0; i < iNumRects; i++)
    {   /* Each rectangle carries its own position, size and encoding. */

        if (RFB_Read(pSess, arrRect, 12) != SUCCESS)
        {   /* Truncated rectangle header. */
            return(FAILURE);
        }

        iX        = (int)RFB_GetU16(&arrRect[0]);
        iY        = (int)RFB_GetU16(&arrRect[2]);
        iW        = (int)RFB_GetU16(&arrRect[4]);
        iH        = (int)RFB_GetU16(&arrRect[6]);
        lEncoding = (long)(int)RFB_GetU32(&arrRect[8]);   // encoding is signed 32-bit

        if (lEncoding == RFB_ENC_RAW)
        {   /* Raw pixels follow, row-major, in the negotiated format. */
            if (RFB_ReadRawRect(pSess, iX, iY, iW, iH) != SUCCESS)
            {   /* Read/blit failed. */
                return(FAILURE);
            }
        }
        else if (lEncoding == RFB_ENC_COPYRECT)
        {   /* A source position follows; copy that region within the fb. */
            if (RFB_Read(pSess, arrCopy, 4) != SUCCESS)
            {   /* Truncated CopyRect. */
                return(FAILURE);
            }

            iSrcX = (int)RFB_GetU16(&arrCopy[0]);
            iSrcY = (int)RFB_GetU16(&arrCopy[2]);

            /* The source must fit inside BOTH the server's framebuffer         */
            /* (protocol validity) and our own page: FB_CopyRect clips only     */
            /* the destination, so a bad source would read past the mapping.    */
            /* CopyRect carries no pixel payload, so skipping a bad one keeps   */
            /* the stream in sync and the session up on this trusted link.      */
            iLimW = pSess->rfb_iWidth;
            iLimH = pSess->rfb_iHeight;

            if (pSess->rfb_pFb->fb_iWidth < iLimW)
            {   /* The panel is narrower than the server: our page is the tighter bound. */
                iLimW = pSess->rfb_pFb->fb_iWidth;
            }

            if (pSess->rfb_pFb->fb_iHeight < iLimH)
            {   /* Likewise for the height. */
                iLimH = pSess->rfb_pFb->fb_iHeight;
            }

            if ((iSrcX + iW > iLimW) || (iSrcY + iH > iLimH))
            {   /* Source rectangle overruns the framebuffer: refuse this copy. */
                LOG_Warn("CopyRect source (%d,%d %dx%d) exceeds %dx%d; rectangle ignored",
                         iSrcX, iSrcY, iW, iH, iLimW, iLimH);
            }
            else
            {   /* Source is valid: copy within the back page. */
                FB_CopyRect(pSess->rfb_pFb, iSrcX, iSrcY, iX, iY, iW, iH);
            }
        }
        else
        {   /* We only advertised RAW and CopyRect; anything else is unframed. */
            LOG_Error("server sent unrequested encoding %ld; cannot resync", lEncoding);
            return(FAILURE);
        }
    }

    /* Whole update applied. */
    return(SUCCESS);
}

/*--------------------------------------------------------------------------*/
/* RFB_HandleMessage:                                                       */
/*                                                                          */
/* Reads and dispatches one server message.  See rfb.h.                     */
/*                                                                          */
/* Arguments:                                                               */
/*     pSess       : active session.                                        */
/*     pbWasUpdate : out; TRUE if the message was a FramebufferUpdate.      */
/*                                                                          */
/* Returns:                                                                 */
/*     int : SUCCESS or FAILURE.                                            */
/*--------------------------------------------------------------------------*/

int RFB_HandleMessage(RFB_Session *pSess, int *pbWasUpdate)
{
    unsigned char ucType;       // server message type
    unsigned char arrHdr[8];    // small fixed headers
    unsigned long uLen;         // variable-length field size
    unsigned int  uNumColours;  // colour-map entry count

    *pbWasUpdate = FALSE;

    /* The one leading byte identifies the message. */
    if (RFB_Read(pSess, &ucType, 1) != SUCCESS)
    {   /* Peer closed, or the read timed out. */
        return(FAILURE);
    }

    if (ucType == RFB_SMSG_UPDATE)
    {   /* FramebufferUpdate: the only message that draws.  Seed the back buffer       */
        /* with the current frame, apply the rectangles, then present it - this        */
        /* panel only refreshes on the present, and page-flips when double-buffered.   */
        FB_BeginFrame(pSess->rfb_pFb);

        if (RFB_ReadUpdate(pSess) != SUCCESS)
        {   /* A malformed or truncated update is fatal to the session. */
            return(FAILURE);
        }

        FB_Present(pSess->rfb_pFb);

        *pbWasUpdate = TRUE;
        return(SUCCESS);
    }

    if (ucType == RFB_SMSG_COLOURMAP)
    {   /* SetColourMapEntries: padding(1) first(u16) count(u16), then 6 bytes   */
        /* per colour.  This client is true-colour, so consume and ignore.       */
        if (RFB_Read(pSess, arrHdr, 5) != SUCCESS)
        {   /* Truncated header. */
            return(FAILURE);
        }

        uNumColours = RFB_GetU16(&arrHdr[3]);
        return(RFB_Discard(pSess, (unsigned long)uNumColours * 6));
    }

    if (ucType == RFB_SMSG_BELL)
    {   /* Bell: no body.  Ignore. */
        return(SUCCESS);
    }

    if (ucType == RFB_SMSG_CUTTEXT)
    {   /* ServerCutText: padding(3) length(u32) text.  Ignore the clipboard. */
        if (RFB_Read(pSess, arrHdr, 7) != SUCCESS)
        {   /* Truncated header. */
            return(FAILURE);
        }

        uLen = RFB_GetU32(&arrHdr[3]);
        return(RFB_Discard(pSess, uLen));
    }

    LOG_Error("unknown server message type %u", (unsigned int)ucType);
    /* Any other type means the stream is out of sync; bail and reconnect. */
    return(FAILURE);
}

/*--------------------------------------------------------------------------*/
/* RFB_SendPointerEvent:                                                    */
/*                                                                          */
/* Sends one PointerEvent.  See rfb.h.                                      */
/*                                                                          */
/* Arguments:                                                               */
/*     pSess       : active session.                                        */
/*     iX,iY       : pointer position.                                      */
/*     iButtonMask : RFB button mask.                                       */
/*                                                                          */
/* Returns:                                                                 */
/*     int : SUCCESS or FAILURE.                                            */
/*--------------------------------------------------------------------------*/

int RFB_SendPointerEvent(RFB_Session *pSess, int iX, int iY, int iButtonMask)
{
    unsigned char arrMsg[6];                    // PointerEvent message

    /* Confine coordinates to the server's framebuffer, just in case the panel   */
    /* mapping produced something outside the server's own bounds.               */
    if (iX < 0)
    {   /* Clamp left. */
        iX = 0;
    }

    if (iY < 0)
    {   /* Clamp top. */
        iY = 0;
    }

    if (iX > (pSess->rfb_iWidth - 1))
    {   /* Clamp right. */
        iX = pSess->rfb_iWidth - 1;
    }

    if (iY > (pSess->rfb_iHeight - 1))
    {   /* Clamp bottom. */
        iY = pSess->rfb_iHeight - 1;
    }

    /* type(1), button-mask(1), x(u16), y(u16). */
    arrMsg[0] = RFB_CMSG_POINTER;
    arrMsg[1] = (unsigned char)(iButtonMask & 0xFF);
    RFB_PutU16(&arrMsg[2], (unsigned int)iX);
    RFB_PutU16(&arrMsg[4], (unsigned int)iY);

    if (NET_WriteFull(pSess->rfb_iFd, arrMsg, 6) != SUCCESS)
    {   /* Could not send the pointer event. */
        return(FAILURE);
    }

    /* Sent. */
    return(SUCCESS);
}

/*--------------------------------------------------------------------------*/
/* RFB_Free:                                                                */
/*                                                                          */
/* Frees the row buffer.  See rfb.h.                                        */
/*                                                                          */
/* Arguments:                                                               */
/*     pSess : session to release.                                          */
/*                                                                          */
/* Returns:                                                                 */
/*     void : frees the row buffer.                                         */
/*--------------------------------------------------------------------------*/

void RFB_Free(RFB_Session *pSess)
{
    if (pSess->rfb_pRowBuf != NULL)
    {   /* Release the reusable RAW row buffer. */
        free(pSess->rfb_pRowBuf);
        pSess->rfb_pRowBuf    = NULL;
        pSess->rfb_iRowBufLen = 0;
    }
}
