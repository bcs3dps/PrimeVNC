/*--------------------------------------------------------------------------*/
/* SPDX-License-Identifier: GPL-2.0-or-later                                */
/* Copyright (c) 2026 B. C. Services                                        */
/*                                                                          */
/* Licensed under the GNU General Public License version 2 or later --      */
/* see LICENSE beside this source.  GPL-2.0 full text in GPL-2.0.txt.       */
/*--------------------------------------------------------------------------*/
/* framebuffer.c:                                                           */
/*                                                                          */
/* Implementation of the fbdev output declared in framebuffer.h.  Geometry  */
/* and packing come entirely from the driver at runtime via the two         */
/* FBIOGET_*SCREENINFO ioctls, so this code adapts to whatever the panel    */
/* actually is rather than assuming a resolution or format.                 */
/*                                                                          */
/* On the Allwinner sunxi disp2 driver (the reference device: sun8iw20 /    */
/* T113, Tina Linux 5.4.61, 532x300 panel) the fb is ONE ION                */
/* allocation holding TWO pages (yres_virtual = 2 * yres), scanned out of   */
/* physical RAM by the display engine.  Our user mapping is CACHED because  */
/* FB_Open asks for that BEFORE mmap (FBIO_ENABLE_CACHE - the driver reads  */
/* the flag only inside its mmap), so every present first cleans the cache  */
/* to RAM (FBIO_CACHE_SYNC) and then page-flips with FBIOPAN_DISPLAY, which */
/* waits for vsync.  The fb layer uses PER-PIXEL alpha, so every pixel this */
/* module writes carries 0xFF in the transparency bits (the RFB stream      */
/* carries none).  FB_Close restores the driver's uncached default so the   */
/* next process to map the device starts from a known mode.                 */
/*                                                                          */
/* The panel BACKLIGHT is not fbdev's on this driver: brightness is set and */
/* read through the display-engine node /dev/disp, and the value survives   */
/* the process that last owned the display, so a client taking over a panel */
/* its previous owner had turned off draws into an unlit screen.  The       */
/* FB_Backlight* functions carry that control; FB_BacklightTake lights a    */
/* dark panel at takeover and FB_Close puts back a non-zero brightness it   */
/* changed.                                                                 */
/*--------------------------------------------------------------------------*/

#include "framebuffer.h"
#include "pixfmt.h"
#include "log.h"
#include "primevnc.h"

#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

/*--------------------------------------------------------------------------*/
/* Allwinner sunxi disp2 framebuffer ioctls (Tina Linux 5.4, sun8iw20 /     */
/* T113 / R528 and its siblings).  They are not in a standard toolchain's   */
/* linux/fb.h; the numbers are the driver's own (dev_fb.c, "custom ioctl    */
/* command here").                                                          */
/*                                                                          */
/* The driver decides inside its mmap handler whether a user mapping is     */
/* CACHED, from a kernel-global flag that FBIO_ENABLE_CACHE sets - so that  */
/* ioctl only has an effect when issued BEFORE mmap, and the flag persists  */
/* across processes (whatever mapped the device last decides it).  A        */
/* cached mapping must be cleaned to RAM with FBIO_CACHE_SYNC before every  */
/* pan, or the display engine keeps scanning stale memory.  0x4632 and      */
/* 0x4633 are ION-fd / physical-address queries, not cache operations, and  */
/* are deliberately not defined here.                                       */
/*--------------------------------------------------------------------------*/

#define FBIO_CACHE_SYNC      0x4630          // clean the WHOLE fb to RAM; the driver ignores the third arg
#define FBIO_ENABLE_CACHE    0x4631          // arg: unsigned long[2]; word 0: 1 = cached mmap, 0 = uncached

/*--------------------------------------------------------------------------*/
/* Allwinner sunxi disp2 display-engine ioctls, issued on /dev/disp rather  */
/* than on the framebuffer.  The panel backlight is theirs: brightness is   */
/* set and read with DISP_LCD_SET_BRIGHTNESS / DISP_LCD_GET_BRIGHTNESS,     */
/* each taking an unsigned long[4] whose word 0 is the screen (0) and word  */
/* 1 the value (0..255) for a set; a get returns the value as the ioctl's   */
/* result.  The numbers and the argument layout were read from the          */
/* reference device's own GUI, which turns its screen off with a            */
/* brightness of 0.                                                         */
/*--------------------------------------------------------------------------*/

#define FB_DISP_DEVICE           "/dev/disp"  // the display-engine control node (root only)
#define DISP_LCD_SET_BRIGHTNESS  0x102        // arg[1] = brightness 0..255 for screen arg[0]; returns 0
#define DISP_LCD_GET_BRIGHTNESS  0x103        // returns the brightness of screen arg[0]

/*--------------------------------------------------------------------------*/
/* Module state: whether the first successful present has been logged.      */
/* One verbose line per process is the bring-up evidence that the cache     */
/* sync and the pan both returned 0; after that the path stays quiet.       */
/*--------------------------------------------------------------------------*/

static int FB_bPresentLogged = FALSE;       // set after the first successful FB_Present is logged

/*------------------------------------------------------------------------------*/
/* FB_FormatFromVar:                                                            */
/*                                                                              */
/* Builds a PIX_Format from a fb_var_screeninfo.  Framebuffer memory is         */
/* CPU-native byte order, which is little-endian on both the ARMv7 target and   */
/* the x86 host used for native testing, so the format is marked little-endian. */
/* Local to this file.                                                          */
/*                                                                              */
/* Arguments:                                                                   */
/*     pVar : the driver's variable screen info.                                */
/*     pOut : format to fill.                                                   */
/*                                                                              */
/* Returns:                                                                     */
/*     void : fills *pOut.                                                      */
/*------------------------------------------------------------------------------*/

static void FB_FormatFromVar(const struct fb_var_screeninfo *pVar, PIX_Format *pOut)
{
    /* Pixel word size in bytes, rounded up from the bit depth. */
    pOut->pix_iBytesPerPixel = (int)((pVar->bits_per_pixel + 7) / 8);

    /* Significant depth is the sum of the colour channel widths. */
    pOut->pix_iDepth = (int)(pVar->red.length + pVar->green.length + pVar->blue.length);

    /* Framebuffer memory is CPU-native order (little-endian here). */
    pOut->pix_bBigEndian = FALSE;

    /* Channel maxima come from each bitfield's length. */
    pOut->pix_uRedMax   = (pVar->red.length   >= 32) ? 0xFFFFFFFFu : ((1u << pVar->red.length)   - 1u);
    pOut->pix_uGreenMax = (pVar->green.length >= 32) ? 0xFFFFFFFFu : ((1u << pVar->green.length) - 1u);
    pOut->pix_uBlueMax  = (pVar->blue.length  >= 32) ? 0xFFFFFFFFu : ((1u << pVar->blue.length)  - 1u);

    /* Channel positions are the bitfield offsets. */
    pOut->pix_iRedShift   = (int)pVar->red.offset;
    pOut->pix_iGreenShift = (int)pVar->green.offset;
    pOut->pix_iBlueShift  = (int)pVar->blue.offset;
}

/*--------------------------------------------------------------------------*/
/* FB_Open:                                                                 */
/*                                                                          */
/* Opens and maps the framebuffer.  See framebuffer.h for the contract.     */
/*                                                                          */
/* Arguments:                                                               */
/*     pFb   : device to populate.                                          */
/*     sPath : device path.                                                 */
/*                                                                          */
/* Returns:                                                                 */
/*     int : SUCCESS or FAILURE.                                            */
/*--------------------------------------------------------------------------*/

int FB_Open(FB_Device *pFb, const char *sPath)
{
    struct fb_var_screeninfo var;              // variable info (geometry, format)
    struct fb_fix_screeninfo fix;              // fixed info (stride, mem length)
    unsigned long            arrCacheArg[2];   // FBIO_ENABLE_CACHE argument: {mode, 0}
    int                      iRc;              // ioctl result

    /* Start from a known-empty device so a partial failure closes cleanly.     */
    /* The backlight is not read here: FB_BacklightTake does that on request.   */
    memset(pFb, 0, sizeof(*pFb));
    pFb->fb_iFd               = -1;
    pFb->fb_pMem              = NULL;
    pFb->fb_iBacklightAtOpen  = -1;
    pFb->fb_bBacklightRestore = FALSE;

    /* Open the framebuffer read/write; we both read (for CopyRect) and write. */
    pFb->fb_iFd = open(sPath, O_RDWR);

    if (pFb->fb_iFd < 0)
    {   /* Missing device or no permission.  Note: open() does NOT fail while       */
        /* another process has the device open (fbdev has no exclusive lock); the   */
        /* caller must make sure nothing else is driving the display.               */
        LOG_Error("open('%s') failed: %s", sPath, strerror(errno));
        return(FAILURE);
    }

    /* Read the variable info: resolution, bit depth, channel bitfields. */
    if (ioctl(pFb->fb_iFd, FBIOGET_VSCREENINFO, &var) != 0)
    {   /* Not a framebuffer, or the driver refused the query. */
        LOG_Error("FBIOGET_VSCREENINFO failed: %s", strerror(errno));
        close(pFb->fb_iFd);
        pFb->fb_iFd = -1;
        return(FAILURE);
    }

    /* Read the fixed info: line stride and mappable memory length. */
    if (ioctl(pFb->fb_iFd, FBIOGET_FSCREENINFO, &fix) != 0)
    {   /* Same driver, second query - report and unwind. */
        LOG_Error("FBIOGET_FSCREENINFO failed: %s", strerror(errno));
        close(pFb->fb_iFd);
        pFb->fb_iFd = -1;
        return(FAILURE);
    }

    /* Record the geometry the rest of the module works from. */
    pFb->fb_iWidth        = (int)var.xres;
    pFb->fb_iHeight       = (int)var.yres;
    pFb->fb_iLineLength   = (int)fix.line_length;
    pFb->fb_iBytesPerPixel = (int)((var.bits_per_pixel + 7) / 8);
    pFb->fb_iYResVirtual  = (int)var.yres_virtual;
    pFb->fb_iMemLen       = (unsigned long)fix.smem_len;

    /* Derive the pixel format the blit path and SetPixelFormat request use. */
    FB_FormatFromVar(&var, &pFb->fb_Format);

    /* The transparency bitfield, if the driver advertises one.  This panel's    */
    /* layer uses PER-PIXEL alpha (ARGB8888, transp at bits 24..31), so every    */
    /* pixel we write must carry all-ones there or the display engine shows it   */
    /* transparent.  A zero-length field means no alpha byte; the mask stays 0   */
    /* and the forcing becomes a no-op.                                          */
    if ((var.transp.length > 0) && (var.transp.length < 32))
    {   /* Build the all-ones mask at the transparency field's position. */
        pFb->fb_uAlphaMask = ((1u << var.transp.length) - 1u) << var.transp.offset;
    }
    else
    {   /* No transparency channel: nothing to force. */
        pFb->fb_uAlphaMask = 0;
    }

    /* A zero stride or memory length means the driver gave us nothing usable. */
    if ((pFb->fb_iLineLength <= 0) || (pFb->fb_iMemLen == 0))
    {   /* Defensive: avoid mmap(0) and a divide/stride of zero later. */
        LOG_Error("framebuffer reports zero stride or size (stride=%d len=%lu)",
                  pFb->fb_iLineLength, pFb->fb_iMemLen);
        close(pFb->fb_iFd);
        pFb->fb_iFd = -1;
        return(FAILURE);
    }

    /* Select a CACHED mapping BEFORE mmap.  The driver reads this flag inside      */
    /* its mmap handler and nowhere else, so issuing it afterwards would change     */
    /* nothing about the mapping already made - and the flag persists across        */
    /* processes, so an inherited value must never be relied on.  Cached is right   */
    /* for this client because it READS the fb (the page seed copy, CopyRect);      */
    /* the price is one FBIO_CACHE_SYNC per presented frame (FB_Present).  Best     */
    /* effort: an fb without the sunxi extension answers -1/ENOTTY and maps with    */
    /* the driver's own default.                                                    */
    arrCacheArg[0] = 1;
    arrCacheArg[1] = 0;

    iRc = ioctl(pFb->fb_iFd, FBIO_ENABLE_CACHE, arrCacheArg);

    if (iRc != 0)
    {   /* Not fatal, but it is the first place a black panel can be traced to. */
        LOG_Warn("FBIO_ENABLE_CACHE(1) failed: %s (mapping will use the driver default)",
                 strerror(errno));
    }
    else
    {   /* Accepted: the mmap below is a cached mapping. */
        LOG_Info("FBIO_ENABLE_CACHE(1) ok: cached mapping selected");
    }

    /* Map the pixel memory shared so writes reach the panel. */
    pFb->fb_pMem = (unsigned char *)mmap(NULL, pFb->fb_iMemLen,
                                         PROT_READ | PROT_WRITE, MAP_SHARED,
                                         pFb->fb_iFd, 0);

    if (pFb->fb_pMem == MAP_FAILED)
    {   /* Mapping failed; the fd is useless without it. */
        LOG_Error("mmap of %lu bytes failed: %s", pFb->fb_iMemLen, strerror(errno));
        pFb->fb_pMem = NULL;
        close(pFb->fb_iFd);
        pFb->fb_iFd = -1;
        return(FAILURE);
    }

    /* Set up buffering.  A panel with room for two visible frames                 */
    /* (yres_virtual >= 2*yres) is treated as page-flipped: we draw into the       */
    /* BACK buffer and present it, because this sunxi panel only refreshes on an   */
    /* explicit FBIOPAN_DISPLAY (a plain memory write never appears).  Otherwise   */
    /* a single write-through buffer that we still present each frame.             */
    pFb->fb_iBufOffset[0] = 0;
    pFb->fb_iBufOffset[1] = pFb->fb_iHeight * pFb->fb_iLineLength;

    if (pFb->fb_iYResVirtual >= (pFb->fb_iHeight * 2))
    {   /* Two buffers: start drawing into whichever is NOT currently shown. */
        int iFront;

        iFront = ((int)var.yoffset >= pFb->fb_iHeight) ? 1 : 0;   // buffer on screen now
        pFb->fb_iNumBuffers = 2;
        pFb->fb_iBackIndex  = 1 - iFront;
    }
    else
    {   /* One buffer: draw and present buffer 0. */
        pFb->fb_iNumBuffers = 1;
        pFb->fb_iBackIndex  = 0;
    }

    /* Blits target the current back buffer. */
    pFb->fb_iDrawByteOffset = pFb->fb_iBufOffset[pFb->fb_iBackIndex];

    LOG_Info("framebuffer %s: %dx%d, %d bpp, stride %d, %lu bytes mapped, buffers %d back %d (yvirt %d)",
             sPath, pFb->fb_iWidth, pFb->fb_iHeight, pFb->fb_iBytesPerPixel * 8,
             pFb->fb_iLineLength, pFb->fb_iMemLen, pFb->fb_iNumBuffers,
             pFb->fb_iBackIndex, pFb->fb_iYResVirtual);

    /* The bitfields decide the blit's fast path and the alpha forcing; log them   */
    /* so a format surprise is visible on the first run, not deduced later.        */
    LOG_Info("framebuffer bitfields: red %u/%u green %u/%u blue %u/%u transp %u/%u -> alpha mask 0x%08lx",
             var.red.offset, var.red.length, var.green.offset, var.green.length,
             var.blue.offset, var.blue.length, var.transp.offset, var.transp.length,
             (unsigned long)pFb->fb_uAlphaMask);

    /* Device is open and mapped. */
    return(SUCCESS);
}

/*--------------------------------------------------------------------------*/
/* FB_Close:                                                                */
/*                                                                          */
/* Puts the backlight back if this process changed it, restores the cache   */
/* mode, unmaps and closes.  See framebuffer.h.                             */
/*                                                                          */
/* Arguments:                                                               */
/*     pFb : device to release.                                             */
/*                                                                          */
/* Returns:                                                                 */
/*     void : leaves *pFb closed.                                           */
/*--------------------------------------------------------------------------*/

void FB_Close(FB_Device *pFb)
{
    unsigned long arrCacheArg[2];              // FBIO_ENABLE_CACHE argument: {0, 0}

    if (pFb->fb_bBacklightAutoDark != FALSE)
    {   /* The auto rule has the panel dark: re-light it before leaving, so the   */
        /* next owner never inherits an unlit panel from us.                      */
        FB_BacklightSet(pFb->fb_iBacklightLitLevel);
        pFb->fb_bBacklightAutoDark = FALSE;
        LOG_Info("backlight auto: re-lit at exit");
    }

    if (pFb->fb_bBacklightRestore != FALSE)
    {   /* FB_BacklightTake changed the brightness; hand back what was found -   */
        /* unless that was 0, since the next owner must never inherit an unlit   */
        /* panel from us.                                                        */
        if (pFb->fb_iBacklightAtOpen > 0)
        {   /* A lit level was found and changed (a test setting): restore it. */
            FB_BacklightSet(pFb->fb_iBacklightAtOpen);
        }
        else
        {   /* Found dark and lit by us: stays lit. */
            LOG_Info("backlight was found dark at start; left lit");
        }

        pFb->fb_bBacklightRestore = FALSE;
    }

    if (pFb->fb_iFd >= 0)
    {   /* Restore the driver's default (uncached) mode for whoever maps next:    */
        /* the flag persists across processes, so a mapper that does not choose   */
        /* its own mode would otherwise inherit ours.  Best effort.               */
        arrCacheArg[0] = 0;
        arrCacheArg[1] = 0;
        ioctl(pFb->fb_iFd, FBIO_ENABLE_CACHE, arrCacheArg);
    }

    if (pFb->fb_pMem != NULL)
    {   /* Release the mapping first, while the fd is still valid. */
        munmap(pFb->fb_pMem, pFb->fb_iMemLen);
        pFb->fb_pMem = NULL;
    }

    if (pFb->fb_iFd >= 0)
    {   /* Then the descriptor. */
        close(pFb->fb_iFd);
        pFb->fb_iFd = -1;
    }
}

/*--------------------------------------------------------------------------*/
/* FB_DispIoctl:                                                            */
/*                                                                          */
/* Opens the display-engine node, issues one ioctl with the four-word       */
/* argument the disp2 driver expects ({screen 0, value, 0, 0}), and closes  */
/* it again.  The node is opened per call rather than held: the calls are   */
/* rare (takeover, exit, an operator command) and holding a second device   */
/* open for the life of the process buys nothing.  Local.                   */
/*                                                                          */
/* Arguments:                                                               */
/*     uRequest : the ioctl request (a DISP_LCD_* value).                   */
/*     uValue   : word 1 of the argument: the brightness for a set, 0 for   */
/*                a get.                                                    */
/*     piResult : out; the ioctl's return value on success.                 */
/*                                                                          */
/* Returns:                                                                 */
/*     int : SUCCESS with *piResult set, or FAILURE (logged with errno).    */
/*--------------------------------------------------------------------------*/

static int FB_DispIoctl(unsigned long uRequest, unsigned long uValue, int *piResult)
{
    unsigned long arrArg[4];                   // {screen, value, 0, 0}
    int           iFd;                         // /dev/disp descriptor
    int           iRc;                         // ioctl result
    int           iErr;                        // errno from the ioctl, saved across close()

    iFd = open(FB_DISP_DEVICE, O_RDWR);

    if (iFd < 0)
    {   /* No display-engine node: not this driver, or not root.  Say which. */
        LOG_Warn("open('%s') failed: %s", FB_DISP_DEVICE, strerror(errno));
        return(FAILURE);
    }

    /* Screen 0 in word 0, the value in word 1, the rest zero. */
    arrArg[0] = 0;
    arrArg[1] = uValue;
    arrArg[2] = 0;
    arrArg[3] = 0;

    iRc  = ioctl(iFd, uRequest, arrArg);
    iErr = errno;
    close(iFd);

    if (iRc < 0)
    {   /* The driver refused it: the number is wrong for this driver, or the node is not disp2. */
        LOG_Warn("ioctl(0x%lx) on %s failed: %s", uRequest, FB_DISP_DEVICE, strerror(iErr));
        return(FAILURE);
    }

    *piResult = iRc;
    /* Success; the value in *piResult is the brightness for a get, 0 for a set. */
    return(SUCCESS);
}

/*--------------------------------------------------------------------------*/
/* FB_BacklightGet:                                                         */
/*                                                                          */
/* Reads the brightness.  See framebuffer.h.                                */
/*                                                                          */
/* Arguments:                                                               */
/*     piLevel : out; the brightness 0..255.                                */
/*                                                                          */
/* Returns:                                                                 */
/*     int : SUCCESS or FAILURE.                                            */
/*--------------------------------------------------------------------------*/

int FB_BacklightGet(int *piLevel)
{
    int iRc;                                   // the driver's answer

    if (FB_DispIoctl(DISP_LCD_GET_BRIGHTNESS, 0, &iRc) != SUCCESS)
    {   /* Already logged. */
        return(FAILURE);
    }

    *piLevel = iRc;
    /* Success; the brightness is in *piLevel. */
    return(SUCCESS);
}

/*--------------------------------------------------------------------------*/
/* FB_BacklightSet:                                                         */
/*                                                                          */
/* Sets the brightness, clamped.  See framebuffer.h.                        */
/*                                                                          */
/* Arguments:                                                               */
/*     iLevel : the brightness wanted, 0 = off.                             */
/*                                                                          */
/* Returns:                                                                 */
/*     int : SUCCESS or FAILURE.                                            */
/*--------------------------------------------------------------------------*/

int FB_BacklightSet(int iLevel)
{
    int iRc;                                   // the driver's answer (0 on success)

    if (iLevel < 0)
    {   /* Nothing darker than off. */
        iLevel = 0;
    }

    if (iLevel > FB_BACKLIGHT_MAX)
    {   /* Nothing brighter than full. */
        iLevel = FB_BACKLIGHT_MAX;
    }

    if (FB_DispIoctl(DISP_LCD_SET_BRIGHTNESS, (unsigned long)iLevel, &iRc) != SUCCESS)
    {   /* Already logged. */
        return(FAILURE);
    }

    LOG_Info("backlight set to %d", iLevel);
    /* Success; the panel brightness is now iLevel. */
    return(SUCCESS);
}

/*--------------------------------------------------------------------------*/
/* FB_BacklightTake:                                                        */
/*                                                                          */
/* Applies the takeover policy.  See framebuffer.h.                         */
/*                                                                          */
/* Arguments:                                                               */
/*     pFb    : records what was found and whether to restore it.           */
/*     iMode  : an FB_BACKLIGHT_* policy.                                   */
/*     iLevel : the brightness for FB_BACKLIGHT_LEVEL.                      */
/*                                                                          */
/* Returns:                                                                 */
/*     int : SUCCESS or FAILURE.                                            */
/*--------------------------------------------------------------------------*/

int FB_BacklightTake(FB_Device *pFb, int iMode, int iLevel)
{
    int iFound;                                // brightness as found
    int iTarget;                               // brightness wanted

    pFb->fb_iBacklightAtOpen  = -1;
    pFb->fb_bBacklightRestore = FALSE;

    if (iMode == FB_BACKLIGHT_KEEP)
    {   /* The launcher owns the brightness: touch nothing, remember nothing. */
        LOG_Info("backlight left as found");
        return(SUCCESS);
    }

    if (FB_BacklightGet(&iFound) != SUCCESS)
    {   /* Cannot read it, so do not write it either; the panel stays as it is. */
        return(FAILURE);
    }

    pFb->fb_iBacklightAtOpen = iFound;

    if (iMode == FB_BACKLIGHT_OFF)
    {   /* Test setting: a dark takeover. */
        iTarget = 0;
    }
    else if (iMode == FB_BACKLIGHT_LEVEL)
    {   /* An explicit brightness. */
        iTarget = iLevel;
    }
    else
    {   /* ON: light a dark panel at the default; leave a lit one at its own level. */
        iTarget = iFound;

        if (iFound == 0)
        {   /* The previous owner turned it off; nothing drawn shows until it is lit. */
            iTarget = FB_BACKLIGHT_DEFAULT;
        }
    }

    if (iTarget == iFound)
    {   /* Nothing to change, so nothing to restore later. */
        LOG_Info("backlight found at %d, left there", iFound);
        return(SUCCESS);
    }

    if (FB_BacklightSet(iTarget) != SUCCESS)
    {   /* Already logged; the found value still stands and needs no restoring. */
        return(FAILURE);
    }

    /* Changed it: FB_Close puts the found value back, unless that value was 0. */
    pFb->fb_bBacklightRestore = TRUE;
    LOG_Info("backlight found at %d, set to %d", iFound, iTarget);
    /* Success; the takeover changed the brightness and will restore it at close. */
    return(SUCCESS);
}

/*--------------------------------------------------------------------------*/
/* FB_IsFrontBlack:                                                         */
/*                                                                          */
/* Tells whether the page now on screen is entirely black in its visible    */
/* rows.  For a 32-bit pixel the colour bits are the word with the alpha    */
/* field masked out, so a pixel written opaque black (alpha forced) and one */
/* left transparent black both count as black; for other depths every byte  */
/* must be zero.  Stops at the first non-black pixel.  Local.               */
/*                                                                          */
/* Arguments:                                                               */
/*     pFb : the device; its front page is the one FB_Present last showed.  */
/*                                                                          */
/* Returns:                                                                 */
/*     int : TRUE if no visible pixel carries colour, FALSE otherwise.      */
/*--------------------------------------------------------------------------*/

static int FB_IsFrontBlack(const FB_Device *pFb)
{
    const unsigned char *pRow;                 // start of the current visible row
    const uint32_t      *pWord;                // 32-bit view of the row
    uint32_t             uColourMask;          // the bits that carry colour
    int                  iFront;               // index of the page on screen
    int                  iRowBytes;            // visible bytes per row
    int                  x;
    int                  y;

    /* After FB_Present the drawn page is the front one: the other index on a   */
    /* two-page panel, page 0 on a single-page one.                             */
    iFront = 0;

    if (pFb->fb_iNumBuffers == 2)
    {   /* Two pages: the front is whichever is not being drawn into. */
        iFront = 1 - pFb->fb_iBackIndex;
    }

    iRowBytes   = pFb->fb_iWidth * pFb->fb_iBytesPerPixel;
    uColourMask = ~pFb->fb_uAlphaMask;

    for (y = 0; y < pFb->fb_iHeight; y++)
    {   /* One visible row at a time, at the driver's stride. */
        pRow = pFb->fb_pMem + pFb->fb_iBufOffset[iFront] + (y * pFb->fb_iLineLength);

        if (pFb->fb_iBytesPerPixel == 4)
        {   /* Word view: a pixel is black when its colour bits are all zero. */
            pWord = (const uint32_t *)pRow;

            for (x = 0; x < pFb->fb_iWidth; x++)
            {   /* One pixel per iteration; the first colour settles it. */
                if ((pWord[x] & uColourMask) != 0)
                {   /* Something is lit in this frame. */
                    return(FALSE);
                }
            }
        }
        else
        {   /* Other depths: no alpha field is forced, so black is all-zero bytes. */
            for (x = 0; x < iRowBytes; x++)
            {   /* One byte per iteration; the first non-zero settles it. */
                if (pRow[x] != 0)
                {   /* Something is lit in this frame. */
                    return(FALSE);
                }
            }
        }
    }

    /* Every visible pixel is black. */
    return(TRUE);
}

/*--------------------------------------------------------------------------*/
/* FB_BacklightAutoEnable:                                                  */
/*                                                                          */
/* Arms the auto rule.  See framebuffer.h.                                  */
/*                                                                          */
/* Arguments:                                                               */
/*     pFb      : device to arm.                                            */
/*     iBlackMs : the black-frame timeout in ms.                            */
/*                                                                          */
/* Returns:                                                                 */
/*     void : arms the rule.                                                */
/*--------------------------------------------------------------------------*/

void FB_BacklightAutoEnable(FB_Device *pFb, int iBlackMs)
{
    int iLevel;                                // the brightness to return to

    pFb->fb_iBacklightAutoMs   = iBlackMs;
    pFb->fb_bBacklightAutoDark = FALSE;
    pFb->fb_llBlackSinceMs     = 0;

    /* The level to re-light to is the one lit at takeover: what FB_BacklightTake   */
    /* found if that was lit, the default if it found the panel dark, and a fresh   */
    /* read if it never looked (the KEEP policy).                                   */
    iLevel = pFb->fb_iBacklightAtOpen;

    if (iLevel < 0)
    {   /* Not read at takeover: read it now. */
        if (FB_BacklightGet(&iLevel) != SUCCESS)
        {   /* Cannot read it; the default is the only sensible level. */
            iLevel = 0;
        }
    }

    if (iLevel <= 0)
    {   /* Dark or unknown: re-light to the default, never to 0. */
        iLevel = FB_BACKLIGHT_DEFAULT;
    }

    pFb->fb_iBacklightLitLevel = iLevel;
    LOG_Info("backlight auto: off after %d ms of a black frame, back to %d on the first lit one",
             iBlackMs, iLevel);
}

/*--------------------------------------------------------------------------*/
/* FB_BacklightAutoFrame:                                                   */
/*                                                                          */
/* Classifies the frame just presented.  See framebuffer.h.                 */
/*                                                                          */
/* Arguments:                                                               */
/*     pFb     : the device.                                                */
/*     llNowMs : the monotonic clock now, in ms.                            */
/*                                                                          */
/* Returns:                                                                 */
/*     void : may re-light the panel; runs or resets the black clock.       */
/*--------------------------------------------------------------------------*/

void FB_BacklightAutoFrame(FB_Device *pFb, long long llNowMs)
{
    if (pFb->fb_iBacklightAutoMs <= 0)
    {   /* Rule off. */
        return;
    }

    if (FB_IsFrontBlack(pFb) == FALSE)
    {   /* A lit frame: the clock stops, and a dark panel comes back on. */
        pFb->fb_llBlackSinceMs = 0;

        if (pFb->fb_bBacklightAutoDark != FALSE)
        {   /* The remote desktop woke up; wake the panel with it. */
            FB_BacklightSet(pFb->fb_iBacklightLitLevel);
            pFb->fb_bBacklightAutoDark = FALSE;
            LOG_Info("backlight auto: frame lit; panel on");
        }

        return;
    }

    if (pFb->fb_llBlackSinceMs == 0)
    {   /* First black frame of a run: start the clock. */
        pFb->fb_llBlackSinceMs = llNowMs;
    }
}

/*--------------------------------------------------------------------------*/
/* FB_BacklightAutoTick:                                                    */
/*                                                                          */
/* Expires the black clock.  See framebuffer.h.                             */
/*                                                                          */
/* Arguments:                                                               */
/*     pFb     : the device.                                                */
/*     llNowMs : the monotonic clock now, in ms.                            */
/*                                                                          */
/* Returns:                                                                 */
/*     void : may turn the panel off.                                       */
/*--------------------------------------------------------------------------*/

void FB_BacklightAutoTick(FB_Device *pFb, long long llNowMs)
{
    if ((pFb->fb_iBacklightAutoMs <= 0) || (pFb->fb_bBacklightAutoDark != FALSE) ||
        (pFb->fb_llBlackSinceMs == 0))
    {   /* Rule off, panel already dark, or the frame is not black: nothing to do. */
        return;
    }

    if ((llNowMs - pFb->fb_llBlackSinceMs) < (long long)pFb->fb_iBacklightAutoMs)
    {   /* Black, but not for long enough yet. */
        return;
    }

    /* The remote desktop has shown nothing but black for the whole timeout:   */
    /* treat that as its screen saver and turn the panel off with it.          */
    if (FB_BacklightSet(0) == SUCCESS)
    {   /* Dark until the next lit frame (or exit) re-lights it. */
        pFb->fb_bBacklightAutoDark = TRUE;
        LOG_Info("backlight auto: frame black for %d ms; panel off", pFb->fb_iBacklightAutoMs);
    }
}

/*---------------------------------------------------------------------------*/
/* FB_ClipRect:                                                              */
/*                                                                           */
/* Clips a rectangle to the visible screen, adjusting a paired source origin */
/* by the same top-left shift so a clipped blit still lines its source up.   */
/* Local to this file.                                                       */
/*                                                                           */
/* Arguments:                                                                */
/*     pFb    : device (for width/height bounds).                            */
/*     piX    : in/out destination left edge.                                */
/*     piY    : in/out destination top edge.                                 */
/*     piW    : in/out width.                                                */
/*     piH    : in/out height.                                               */
/*     piSrcX : in/out paired source left edge (shifted by the same dx), or  */
/*              NULL when there is no source to track.                       */
/*     piSrcY : in/out paired source top edge, or NULL.                      */
/*                                                                           */
/* Returns:                                                                  */
/*     int : TRUE if a non-empty rectangle remains, FALSE if fully clipped.  */
/*---------------------------------------------------------------------------*/

static int FB_ClipRect(const FB_Device *pFb, int *piX, int *piY, int *piW, int *piH,
                       int *piSrcX, int *piSrcY)
{
    int iDx;                                   // left-edge shift from clipping
    int iDy;                                   // top-edge shift from clipping

    iDx = 0;
    iDy = 0;

    /* Clip the left/top edges, remembering how far each moved. */
    if (*piX < 0)
    {   /* Off the left: shrink width and shift source right by the same amount. */
        iDx  = -(*piX);
        *piW -= iDx;
        *piX  = 0;
    }

    if (*piY < 0)
    {   /* Off the top: shrink height and shift source down. */
        iDy  = -(*piY);
        *piH -= iDy;
        *piY  = 0;
    }

    /* Clip the right/bottom edges against the visible extent. */
    if ((*piX + *piW) > pFb->fb_iWidth)
    {   /* Trim the overhang past the right edge. */
        *piW = pFb->fb_iWidth - *piX;
    }

    if ((*piY + *piH) > pFb->fb_iHeight)
    {   /* Trim the overhang past the bottom edge. */
        *piH = pFb->fb_iHeight - *piY;
    }

    /* Apply the top-left shift to the source origin if one is tracked. */
    if (piSrcX != NULL)
    {   /* Keep the source aligned with the clipped destination. */
        *piSrcX += iDx;
    }

    if (piSrcY != NULL)
    {   /* Same for the vertical shift. */
        *piSrcY += iDy;
    }

    /* Nothing left to draw if either dimension collapsed. */
    if ((*piW <= 0) || (*piH <= 0))
    {   /* Fully clipped away. */
        return(FALSE);
    }

    /* A drawable rectangle remains. */
    return(TRUE);
}

/*--------------------------------------------------------------------------*/
/* FB_ForceAlphaRow:                                                        */
/*                                                                          */
/* ORs the framebuffer's alpha mask into every 32-bit pixel of one row that */
/* was just written, so the display engine - which uses per-pixel alpha on  */
/* this panel - treats the pixels as opaque.  The RFB stream carries no     */
/* alpha (the top byte is padding, usually zero) and the format converter   */
/* packs only R, G, B, so without this every pixel drawn would be fully     */
/* transparent.  A no-op when the fb has no transparency field (mask 0) or  */
/* is not 32 bpp.  Rows start 4-byte aligned (page-aligned base, stride a   */
/* multiple of 4, x * 4), so 32-bit access is safe.  Local to this file.    */
/*                                                                          */
/* Arguments:                                                               */
/*     pFb     : open device (for the mask and bytes-per-pixel).            */
/*     pDstRow : first byte of the row just written into the framebuffer.   */
/*     iW      : number of pixels in the row.                               */
/*                                                                          */
/* Returns:                                                                 */
/*     void : rewrites the alpha bits of iW pixels in place.                */
/*--------------------------------------------------------------------------*/

static void FB_ForceAlphaRow(const FB_Device *pFb, unsigned char *pDstRow, int iW)
{
    uint32_t *pWord;                           // walking 32-bit pixel cursor
    int       i;

    if ((pFb->fb_uAlphaMask == 0) || (pFb->fb_iBytesPerPixel != 4))
    {   /* No transparency field, or not a 32-bit pixel: nothing to force. */
        return;
    }

    /* Word view of the row; alignment is guaranteed by the fb geometry. */
    pWord = (uint32_t *)pDstRow;

    for (i = 0; i < iW; i++)
    {   /* One pixel per iteration: set the alpha bits, colour bits unchanged. */
        pWord[i] |= pFb->fb_uAlphaMask;
    }
}

/*--------------------------------------------------------------------------*/
/* FB_LogPresentError:                                                      */
/*                                                                          */
/* Reports a failed ioctl on the present path with its errno, rate-limited: */
/* the first failure of a streak is logged, then every hundredth, so a      */
/* permanently failing panel does not flood stderr at frame rate while the  */
/* first failure - the one that explains a black panel - is never missed.   */
/* Local to this file.                                                      */
/*                                                                          */
/* Arguments:                                                               */
/*     pFb   : device whose failure-streak counter is advanced.             */
/*     sWhat : the ioctl's name for the log line.                           */
/*                                                                          */
/* Returns:                                                                 */
/*     void : writes to the error log when the streak position calls for    */
/*            it.                                                           */
/*--------------------------------------------------------------------------*/

static void FB_LogPresentError(FB_Device *pFb, const char *sWhat)
{
    int iSavedErrno;                           // errno as the failed ioctl left it

    iSavedErrno = errno;
    pFb->fb_iPresentErrs++;

    if ((pFb->fb_iPresentErrs == 1) || ((pFb->fb_iPresentErrs % 100) == 0))
    {   /* First of a streak, or a periodic reminder that it is still failing. */
        LOG_Error("%s failed (%d in a row): %s", sWhat, pFb->fb_iPresentErrs,
                  strerror(iSavedErrno));
    }
}

/*--------------------------------------------------------------------------*/
/* FB_BlitRaw:                                                              */
/*                                                                          */
/* Row-by-row rectangle blit with an optional per-pixel conversion.  See    */
/* framebuffer.h.                                                           */
/*                                                                          */
/* Arguments:                                                               */
/*     pFb     : open device.                                               */
/*     iX,iY   : destination top-left in screen pixels.                     */
/*     iW,iH   : rectangle size.                                            */
/*     pSrc    : tightly packed source pixels.                              */
/*     pSrcFmt : source pixel format.                                       */
/*                                                                          */
/* Returns:                                                                 */
/*     void : writes into the framebuffer.                                  */
/*--------------------------------------------------------------------------*/

void FB_BlitRaw(FB_Device *pFb, int iX, int iY, int iW, int iH,
                const unsigned char *pSrc, const PIX_Format *pSrcFmt)
{
    int                  iSrcRowBytes;  // bytes per source row (pre-clip width)
    int                  iSrcX;         // source left, tracked through clipping
    int                  iSrcY;         // source top, tracked through clipping
    int                  iFullW;        // original width before clipping
    int                  bSame;         // TRUE when formats allow a raw copy
    const unsigned char *pSrcRow;       // source cursor for the current row
    unsigned char       *pDstRow;       // destination cursor for the current row
    int                  r;

    /* The source buffer's stride is set by the ORIGINAL width, so remember it   */
    /* before clipping narrows iW.                                               */
    iFullW       = iW;
    iSrcRowBytes = iFullW * pSrcFmt->pix_iBytesPerPixel;

    /* Track the source origin so a clip off the top/left skips source pixels too. */
    iSrcX = 0;
    iSrcY = 0;

    if (FB_ClipRect(pFb, &iX, &iY, &iW, &iH, &iSrcX, &iSrcY) == FALSE)
    {   /* Rectangle lies entirely off-screen; nothing to draw. */
        return;
    }

    /* Decide once whether rows can be copied verbatim or must be converted. */
    bSame = PIX_SameLayout(&pFb->fb_Format, pSrcFmt);

    for (r = 0; r < iH; r++)
    {   /* One screen row per pass, honoring the framebuffer stride. */

        /* Source row: skip clipped rows (iSrcY) and clipped columns (iSrcX). */
        pSrcRow = pSrc + ((iSrcY + r) * iSrcRowBytes) + (iSrcX * pSrcFmt->pix_iBytesPerPixel);

        /* Destination row: base + visible-buffer offset + row stride + column. */
        pDstRow = pFb->fb_pMem + pFb->fb_iDrawByteOffset +
                  ((iY + r) * pFb->fb_iLineLength) + (iX * pFb->fb_iBytesPerPixel);

        if (bSame != FALSE)
        {   /* Identical packing: a straight copy of iW pixels. */
            memcpy(pDstRow, pSrcRow, (size_t)(iW * pFb->fb_iBytesPerPixel));
        }
        else
        {   /* Mismatched packing: convert pixel by pixel into the fb row. */
            PIX_ConvertRow(pDstRow, &pFb->fb_Format, pSrcRow, pSrcFmt, iW);
        }

        /* Either way the row now needs its alpha bits set for the display engine. */
        FB_ForceAlphaRow(pFb, pDstRow, iW);
    }
}

/*--------------------------------------------------------------------------*/
/* FB_CopyRect:                                                             */
/*                                                                          */
/* In-framebuffer rectangle move for the CopyRect encoding.  See            */
/* framebuffer.h.                                                           */
/*                                                                          */
/* Arguments:                                                               */
/*     pFb         : open device.                                           */
/*     iSrcX,iSrcY : source origin.                                         */
/*     iDstX,iDstY : destination origin.                                    */
/*     iW,iH       : size.                                                  */
/*                                                                          */
/* Returns:                                                                 */
/*     void : moves pixels within the framebuffer.                          */
/*--------------------------------------------------------------------------*/

void FB_CopyRect(FB_Device *pFb, int iSrcX, int iSrcY, int iDstX, int iDstY,
                 int iW, int iH)
{
    int            iBpp;                       // bytes per pixel
    int            iRowBytes;                  // bytes to move per row
    unsigned char *pSrcRow;                    // source row cursor
    unsigned char *pDstRow;                    // destination row cursor
    int            r;

    /* Clip the DESTINATION to the screen, shifting the source origin with it   */
    /* so the two stay aligned; the source region is assumed on-screen (RFB     */
    /* guarantees it references the current framebuffer).                       */
    if (FB_ClipRect(pFb, &iDstX, &iDstY, &iW, &iH, &iSrcX, &iSrcY) == FALSE)
    {   /* Destination fully off-screen; nothing to move. */
        return;
    }

    iBpp      = pFb->fb_iBytesPerPixel;
    iRowBytes = iW * iBpp;

    if (iDstY > iSrcY)
    {   /* Destination lower than source: copy bottom-up so we do not overwrite   */
        /* source rows we have not read yet.                                      */
        for (r = iH - 1; r >= 0; r--)
        {   /* One row per iteration, bottom-up; memmove also covers horizontal overlap. */
            pSrcRow = pFb->fb_pMem + pFb->fb_iDrawByteOffset + ((iSrcY + r) * pFb->fb_iLineLength) + (iSrcX * iBpp);
            pDstRow = pFb->fb_pMem + pFb->fb_iDrawByteOffset + ((iDstY + r) * pFb->fb_iLineLength) + (iDstX * iBpp);
            memmove(pDstRow, pSrcRow, (size_t)iRowBytes);
        }
    }
    else
    {   /* Destination at or above source: top-down is safe. */
        for (r = 0; r < iH; r++)
        {   /* One row per iteration, top-down; same per-row memmove. */
            pSrcRow = pFb->fb_pMem + pFb->fb_iDrawByteOffset + ((iSrcY + r) * pFb->fb_iLineLength) + (iSrcX * iBpp);
            pDstRow = pFb->fb_pMem + pFb->fb_iDrawByteOffset + ((iDstY + r) * pFb->fb_iLineLength) + (iDstX * iBpp);
            memmove(pDstRow, pSrcRow, (size_t)iRowBytes);
        }
    }
}

/*---------------------------------------------------------------------------*/
/* FB_Clear:                                                                 */
/*                                                                           */
/* Fills the back page's visible rows with OPAQUE black.  See framebuffer.h. */
/*                                                                           */
/* Arguments:                                                                */
/*     pFb : open device.                                                    */
/*                                                                           */
/* Returns:                                                                  */
/*     void : the back page's visible rows hold opaque black.                */
/*---------------------------------------------------------------------------*/

void FB_Clear(FB_Device *pFb)
{
    unsigned char *pRow;                       // current row cursor
    uint32_t      *pWord;                      // 32-bit cursor for the opaque fill
    int            iRowBytes;                  // visible bytes per row
    int            r;
    int            i;

    /* Only the visible width times bpp; padding past it is left untouched. */
    iRowBytes = pFb->fb_iWidth * pFb->fb_iBytesPerPixel;

    for (r = 0; r < pFb->fb_iHeight; r++)
    {   /* Black each visible row of the back page. */
        pRow = pFb->fb_pMem + pFb->fb_iDrawByteOffset + (r * pFb->fb_iLineLength);

        if ((pFb->fb_uAlphaMask != 0) && (pFb->fb_iBytesPerPixel == 4))
        {   /* Per-pixel-alpha panel: OPAQUE black (alpha bits set), so nothing   */
            /* below this layer can show through a transparent-black clear.       */
            pWord = (uint32_t *)pRow;

            for (i = 0; i < pFb->fb_iWidth; i++)
            {   /* One pixel per iteration: colour bits 0, alpha bits 1. */
                pWord[i] = pFb->fb_uAlphaMask;
            }
        }
        else
        {   /* No alpha channel: 0 is black in every RGB packing. */
            memset(pRow, 0, (size_t)iRowBytes);
        }
    }
}

/*---------------------------------------------------------------------------*/
/* FB_BeginFrame:                                                            */
/*                                                                           */
/* Copies the on-screen (front) buffer into the back buffer before a frame's */
/* rectangles are applied, on a page-flipped panel.  See framebuffer.h.      */
/*                                                                           */
/* Arguments:                                                                */
/*     pFb : open device.                                                    */
/*                                                                           */
/* Returns:                                                                  */
/*     void : back buffer holds the current image.                           */
/*---------------------------------------------------------------------------*/

void FB_BeginFrame(FB_Device *pFb)
{
    unsigned char *pFrontRow;                  // source row in the front buffer
    unsigned char *pBackRow;                   // destination row in the back buffer
    int            iFront;                     // index of the on-screen buffer
    int            iRowBytes;                  // visible bytes per row
    int            r;

    if (pFb->fb_iNumBuffers < 2)
    {   /* Single buffer: the back IS the front, so there is nothing to copy. */
        return;
    }

    /* The front buffer is the one not currently drawn into. */
    iFront    = 1 - pFb->fb_iBackIndex;
    iRowBytes = pFb->fb_iWidth * pFb->fb_iBytesPerPixel;

    for (r = 0; r < pFb->fb_iHeight; r++)
    {   /* Seed the back buffer with the last presented frame, row by row. */
        pFrontRow = pFb->fb_pMem + pFb->fb_iBufOffset[iFront] + (r * pFb->fb_iLineLength);
        pBackRow  = pFb->fb_pMem + pFb->fb_iBufOffset[pFb->fb_iBackIndex] + (r * pFb->fb_iLineLength);
        memcpy(pBackRow, pFrontRow, (size_t)iRowBytes);
    }
}

/*--------------------------------------------------------------------------*/
/* FB_Present:                                                              */
/*                                                                          */
/* Cleans the cache to RAM (FBIO_CACHE_SYNC), pans the panel to the back    */
/* page (FBIOPAN_DISPLAY, which waits for vsync) and, when double-buffered, */
/* swaps front/back.  See framebuffer.h.                                    */
/*                                                                          */
/* Arguments:                                                               */
/*     pFb : open device.                                                   */
/*                                                                          */
/* Returns:                                                                 */
/*     void : the panel shows the drawn frame; the back index advances.     */
/*--------------------------------------------------------------------------*/

void FB_Present(FB_Device *pFb)
{
    struct fb_var_screeninfo var;              // current mode, amended for the pan
    unsigned long            arrRegion[2];     // FBIO_CACHE_SYNC region: the whole mapping
    int                      iRc;              // ioctl result

    if (pFb->fb_iFd >= 0)
    {   /* A real device: clean the cache to RAM, then flip the panel to the back    */
        /* page.  The memory-backed test device (fd -1) skips the ioctls but still   */
        /* swaps below, so the page-flip logic is exercised without hardware.        */

        /* Read the current mode so the pan request carries valid fields. */
        if (ioctl(pFb->fb_iFd, FBIOGET_VSCREENINFO, &var) != 0)
        {   /* Cannot query the mode: neither pan nor swap.  The front page stays on   */
            /* screen and the next frame redraws the same back page.                   */
            FB_LogPresentError(pFb, "FBIOGET_VSCREENINFO");
            return;
        }

        /* Clean every dirty cache line of the mapping to RAM.  The driver cleans    */
        /* the WHOLE buffer and ignores the region on this driver; a build that      */
        /* honours it is handed the whole mapping, so either handler is satisfied.   */
        arrRegion[0] = 0;
        arrRegion[1] = pFb->fb_iMemLen;

        iRc = ioctl(pFb->fb_iFd, FBIO_CACHE_SYNC, arrRegion);

        if (iRc != 0)
        {   /* Not fatal on its own (an uncached mapping needs no clean); log it. */
            FB_LogPresentError(pFb, "FBIO_CACHE_SYNC");
        }

        /* Point the visible origin at the back page.  reserved[] must describe the   */
        /* full screen: the driver's software-rotation path treats a zero rect as     */
        /* "nothing to do" and returns without panning at all.                        */
        var.xoffset     = 0;
        var.yoffset     = (unsigned int)(pFb->fb_iBackIndex * pFb->fb_iHeight);
        var.reserved[0] = 0;
        var.reserved[1] = 0;
        var.reserved[2] = (unsigned int)pFb->fb_iWidth;
        var.reserved[3] = (unsigned int)pFb->fb_iHeight;
        var.activate    = FB_ACTIVATE_NOW;

        /* FBIOPAN_DISPLAY reprograms the layer's source crop and waits for vsync. */
        iRc = ioctl(pFb->fb_iFd, FBIOPAN_DISPLAY, &var);

        if (iRc != 0)
        {   /* The flip did not happen: keep drawing into the same back page. */
            FB_LogPresentError(pFb, "FBIOPAN_DISPLAY");
            return;
        }

        /* A good present ends any failure streak and, once, confirms the path. */
        pFb->fb_iPresentErrs = 0;

        if (FB_bPresentLogged == FALSE)
        {   /* First successful present of the process: the bring-up evidence. */
            LOG_Info("first present ok: cache sync + pan to yoffset %u", var.yoffset);
            FB_bPresentLogged = TRUE;
        }
    }

    if (pFb->fb_iNumBuffers >= 2)
    {   /* Swap: the buffer just shown is now the front; draw into the other next. */
        pFb->fb_iBackIndex      = 1 - pFb->fb_iBackIndex;
        pFb->fb_iDrawByteOffset = pFb->fb_iBufOffset[pFb->fb_iBackIndex];
    }
}
