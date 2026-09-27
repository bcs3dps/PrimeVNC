/*---------------------------------------------------------------------------*/
/* SPDX-License-Identifier: GPL-2.0-or-later                                 */
/* Copyright (c) 2026 B. C. Services                                         */
/*                                                                           */
/* Licensed under the GNU General Public License version 2 or later --       */
/* see LICENSE beside this source.  GPL-2.0 full text in GPL-2.0.txt.        */
/*---------------------------------------------------------------------------*/
/* pixfmt.c:                                                                 */
/*                                                                           */
/* Implementation of the pixel-format helpers declared in pixfmt.h: the      */
/* same-layout test and the per-pixel format converter.  Multi-byte pixel    */
/* words are assembled and disassembled honoring the format's endianness so  */
/* the same code serves the little-endian sunxi framebuffer and a big-endian */
/* server alike.                                                             */
/*---------------------------------------------------------------------------*/

#include "pixfmt.h"
#include "primevnc.h"

/*----------------------------------------------------------------------------*/
/* PIX_ReadWord:                                                              */
/*                                                                            */
/* Reads one pixel word of pFmt->pix_iBytesPerPixel bytes from pSrc and       */
/* assembles it into a uint32 according to the format's endianness.  Local to */
/* this file.                                                                 */
/*                                                                            */
/* Arguments:                                                                 */
/*     pSrc : pointer to the first byte of the pixel.                         */
/*     pFmt : the pixel format (for byte count and endianness).               */
/*                                                                            */
/* Returns:                                                                   */
/*     uint32_t : the assembled pixel value in native integer form.           */
/*----------------------------------------------------------------------------*/

static uint32_t PIX_ReadWord(const unsigned char *pSrc, const PIX_Format *pFmt)
{
    uint32_t uVal;                             // assembled pixel word
    int      i;

    uVal = 0;

    if (pFmt->pix_bBigEndian != FALSE)
    {   /* Big-endian: the first byte is the most significant. */
        for (i = 0; i < pFmt->pix_iBytesPerPixel; i++)
        {   /* One byte per iteration, most significant first. */
            uVal = (uVal << 8) | (uint32_t)pSrc[i];
        }
    }
    else
    {   /* Little-endian: the first byte is the least significant. */
        for (i = pFmt->pix_iBytesPerPixel - 1; i >= 0; i--)
        {   /* One byte per iteration, from the high byte down so the low byte lands last. */
            uVal = (uVal << 8) | (uint32_t)pSrc[i];
        }
    }

    /* The reconstructed native-integer pixel value. */
    return(uVal);
}

/*-----------------------------------------------------------------------------*/
/* PIX_WriteWord:                                                              */
/*                                                                             */
/* Stores a uint32 pixel word into pDst as pFmt->pix_iBytesPerPixel bytes in   */
/* the format's endianness - the inverse of PIX_ReadWord.  Local to this file. */
/*                                                                             */
/* Arguments:                                                                  */
/*     pDst : destination for the pixel bytes.                                 */
/*     pFmt : the pixel format (byte count and endianness).                    */
/*     uVal : the native pixel value to store.                                 */
/*                                                                             */
/* Returns:                                                                    */
/*     void : writes pFmt->pix_iBytesPerPixel bytes to pDst.                   */
/*-----------------------------------------------------------------------------*/

static void PIX_WriteWord(unsigned char *pDst, const PIX_Format *pFmt, uint32_t uVal)
{
    int i;

    if (pFmt->pix_bBigEndian != FALSE)
    {   /* Big-endian: emit the most significant byte first. */
        for (i = pFmt->pix_iBytesPerPixel - 1; i >= 0; i--)
        {   /* One byte per iteration: peel the low byte into a descending index. */
            pDst[i] = (unsigned char)(uVal & 0xFF);
            uVal >>= 8;
        }
    }
    else
    {   /* Little-endian: emit the least significant byte first. */
        for (i = 0; i < pFmt->pix_iBytesPerPixel; i++)
        {   /* Peel the low byte off each pass. */
            pDst[i] = (unsigned char)(uVal & 0xFF);
            uVal >>= 8;
        }
    }
}

/*----------------------------------------------------------------------------*/
/* PIX_Scale:                                                                 */
/*                                                                            */
/* Rescales a colour component from one channel width to another using        */
/* rounded integer arithmetic: out = (in * outMax + inMax/2) / inMax.  Guards */
/* a zero inMax (a channel absent in the source) by returning 0.  Local.      */
/*                                                                            */
/* Arguments:                                                                 */
/*     uIn     : the source channel value (0 .. uInMax).                      */
/*     uInMax  : the source channel's maximum.                                */
/*     uOutMax : the destination channel's maximum.                           */
/*                                                                            */
/* Returns:                                                                   */
/*     uint32_t : the value rescaled to 0 .. uOutMax.                         */
/*----------------------------------------------------------------------------*/

static uint32_t PIX_Scale(uint32_t uIn, uint32_t uInMax, uint32_t uOutMax)
{
    if (uInMax == 0)
    {   /* Source has no bits for this channel; nothing to contribute. */
        return(0);
    }

    /* Rounded rescale: add half the divisor before dividing. */
    return(((uIn * uOutMax) + (uInMax / 2)) / uInMax);
}

/*--------------------------------------------------------------------------*/
/* PIX_SameLayout:                                                          */
/*                                                                          */
/* Field-by-field equality of two formats.  See pixfmt.h for the contract.  */
/*                                                                          */
/* Arguments:                                                               */
/*     pA : first format.                                                   */
/*     pB : second format.                                                  */
/*                                                                          */
/* Returns:                                                                 */
/*     int : TRUE if identical, else FALSE.                                 */
/*--------------------------------------------------------------------------*/

int PIX_SameLayout(const PIX_Format *pA, const PIX_Format *pB)
{
    if (pA->pix_iBytesPerPixel != pB->pix_iBytesPerPixel)
    {   /* Different pixel size: cannot be a raw copy. */
        return(FALSE);
    }

    /* Endianness only matters for multi-byte pixels; compare it anyway since   */
    /* a mismatch there also forces the conversion path.                        */
    if ((pA->pix_iBytesPerPixel > 1) && (pA->pix_bBigEndian != pB->pix_bBigEndian))
    {   /* Same size but opposite byte order: not a raw copy. */
        return(FALSE);
    }

    if ((pA->pix_uRedMax   != pB->pix_uRedMax)   ||
        (pA->pix_uGreenMax != pB->pix_uGreenMax) ||
        (pA->pix_uBlueMax  != pB->pix_uBlueMax))
    {   /* Different channel widths. */
        return(FALSE);
    }

    if ((pA->pix_iRedShift   != pB->pix_iRedShift)   ||
        (pA->pix_iGreenShift != pB->pix_iGreenShift) ||
        (pA->pix_iBlueShift  != pB->pix_iBlueShift))
    {   /* Same widths but different positions. */
        return(FALSE);
    }

    /* Every packing-relevant field matches, so a plain copy is correct. */
    return(TRUE);
}

/*--------------------------------------------------------------------------*/
/* PIX_ConvertRow:                                                          */
/*                                                                          */
/* Per-pixel format conversion for a run of pixels.  See pixfmt.h.          */
/*                                                                          */
/* Arguments:                                                               */
/*     pDst    : destination buffer.                                        */
/*     pDstFmt : destination format.                                        */
/*     pSrc    : source buffer.                                             */
/*     pSrcFmt : source format.                                             */
/*     iCount  : number of pixels.                                          */
/*                                                                          */
/* Returns:                                                                 */
/*     void : fills pDst.                                                   */
/*--------------------------------------------------------------------------*/

void PIX_ConvertRow(unsigned char *pDst, const PIX_Format *pDstFmt,
                    const unsigned char *pSrc, const PIX_Format *pSrcFmt,
                    int iCount)
{
    uint32_t uSrc;                             // one source pixel word
    uint32_t uRed;                             // channels, source scale
    uint32_t uGreen;
    uint32_t uBlue;
    uint32_t uDst;                             // one destination pixel word
    int      i;

    for (i = 0; i < iCount; i++)
    {   /* Read, split to channels, rescale, repack, write - one pixel a pass. */

        /* Assemble the source pixel word in native form. */
        uSrc = PIX_ReadWord(pSrc, pSrcFmt);

        /* Extract each channel at the source's own bit width. */
        uRed   = (uSrc >> pSrcFmt->pix_iRedShift)   & pSrcFmt->pix_uRedMax;
        uGreen = (uSrc >> pSrcFmt->pix_iGreenShift) & pSrcFmt->pix_uGreenMax;
        uBlue  = (uSrc >> pSrcFmt->pix_iBlueShift)  & pSrcFmt->pix_uBlueMax;

        /* Rescale each channel from the source width to the destination width. */
        uRed   = PIX_Scale(uRed,   pSrcFmt->pix_uRedMax,   pDstFmt->pix_uRedMax);
        uGreen = PIX_Scale(uGreen, pSrcFmt->pix_uGreenMax, pDstFmt->pix_uGreenMax);
        uBlue  = PIX_Scale(uBlue,  pSrcFmt->pix_uBlueMax,  pDstFmt->pix_uBlueMax);

        /* Repack into the destination's positions. */
        uDst = ((uRed   & pDstFmt->pix_uRedMax)   << pDstFmt->pix_iRedShift)   |
               ((uGreen & pDstFmt->pix_uGreenMax) << pDstFmt->pix_iGreenShift) |
               ((uBlue  & pDstFmt->pix_uBlueMax)  << pDstFmt->pix_iBlueShift);

        /* Store the destination pixel and advance both cursors. */
        PIX_WriteWord(pDst, pDstFmt, uDst);

        pSrc += pSrcFmt->pix_iBytesPerPixel;
        pDst += pDstFmt->pix_iBytesPerPixel;
    }
}
