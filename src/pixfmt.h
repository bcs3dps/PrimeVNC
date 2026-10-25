/*---------------------------------------------------------------------------*/
/* SPDX-License-Identifier: GPL-2.0-or-later                                 */
/* Copyright (c) 2026 B. C. Services                                         */
/*                                                                           */
/* Licensed under the GNU General Public License version 2 or later --       */
/* see LICENSE beside this source.  GPL-2.0 full text in GPL-2.0.txt.        */
/*---------------------------------------------------------------------------*/
/* pixfmt.h:                                                                 */
/*                                                                           */
/* A small pixel-format description shared by the framebuffer and the RFB    */
/* layers, plus the conversion helpers between two formats.  The intended    */
/* fast path is that the RFB client asks the server (SetPixelFormat) for the */
/* framebuffer's OWN format, so an incoming rectangle is a straight row copy */
/* with no per-pixel work.  When a server refuses and sends a different      */
/* format, PIX_ConvertRow bridges the two - correct but slower - so the      */
/* client still works rather than showing garbage.                           */
/*                                                                           */
/* A pixel is 1, 2 or 4 bytes.  Each colour channel is described by a max    */
/* value (its bit width as (1<<bits)-1) and a shift within the assembled     */
/* pixel word, matching both the RFB PIXEL_FORMAT wire fields and the Linux  */
/* fb_var_screeninfo red/green/blue bitfields.                               */
/*---------------------------------------------------------------------------*/

#ifndef PRIMEVNC_PIXFMT_H
#define PRIMEVNC_PIXFMT_H

#include <stdint.h>

/*--------------------------------------------------------------------------*/
/* PIX_Format:                                                              */
/*                                                                          */
/* Describes how a single true-colour pixel is packed.  This client only    */
/* handles true colour (no palette): a desktop VNC server and the sunxi     */
/* framebuffer are both true-colour, so a colour-map path would be dead     */
/* code.                                                                    */
/*--------------------------------------------------------------------------*/

typedef struct
{
    int pix_iBytesPerPixel;                 // 1, 2 or 4 bytes on the wire / in the fb
    int pix_iDepth;                         // significant colour bits (e.g. 16, 24)
    int pix_bBigEndian;                     // TRUE: multi-byte pixel is big-endian

    uint32_t pix_uRedMax;                   // (1 << red_bits)   - 1
    uint32_t pix_uGreenMax;                 // (1 << green_bits) - 1
    uint32_t pix_uBlueMax;                  // (1 << blue_bits)  - 1

    int pix_iRedShift;                      // left shift of red within the pixel word
    int pix_iGreenShift;                    // left shift of green
    int pix_iBlueShift;                     // left shift of blue
} PIX_Format;

/*--------------------------------------------------------------------------*/
/* PIX_SameLayout:                                                          */
/*                                                                          */
/* Reports whether two formats are byte-for-byte identical, so a rectangle  */
/* can be blitted with a plain row copy instead of per-pixel conversion.    */
/*                                                                          */
/* Arguments:                                                               */
/*     pA : first format.                                                   */
/*     pB : second format.                                                  */
/*                                                                          */
/* Returns:                                                                 */
/*     int : TRUE if identical in every field that affects packing, else    */
/*           FALSE.                                                         */
/*--------------------------------------------------------------------------*/

int PIX_SameLayout(const PIX_Format *pA, const PIX_Format *pB);

/*--------------------------------------------------------------------------*/
/* PIX_ConvertRow:                                                          */
/*                                                                          */
/* Converts iCount pixels from the source format to the destination format, */
/* one pixel at a time: read the source word, split out its R/G/B channels, */
/* rescale each channel to the destination's bit width, and repack.  Used   */
/* only on the fallback path when the server would not match the fb format. */
/*                                                                          */
/* Arguments:                                                               */
/*     pDst    : destination byte buffer (iCount * dst bytes-per-pixel).    */
/*     pDstFmt : destination pixel format.                                  */
/*     pSrc    : source byte buffer (iCount * src bytes-per-pixel).         */
/*     pSrcFmt : source pixel format.                                       */
/*     iCount  : number of pixels to convert.                               */
/*                                                                          */
/* Returns:                                                                 */
/*     void : fills pDst.                                                   */
/*--------------------------------------------------------------------------*/

void PIX_ConvertRow(unsigned char *pDst, const PIX_Format *pDstFmt,
                    const unsigned char *pSrc, const PIX_Format *pSrcFmt,
                    int iCount);

#endif // PRIMEVNC_PIXFMT_H
