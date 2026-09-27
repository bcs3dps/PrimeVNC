/*--------------------------------------------------------------------------*/
/* SPDX-License-Identifier: GPL-2.0-or-later                                */
/* Copyright (c) 2026 B. C. Services                                        */
/*                                                                          */
/* Licensed under the GNU General Public License version 2 or later --      */
/* see LICENSE beside this source.  GPL-2.0 full text in GPL-2.0.txt.       */
/*--------------------------------------------------------------------------*/
/* framebuffer.h:                                                           */
/*                                                                          */
/* Linux fbdev output for the RFB client.  On an Allwinner sunxi device the */
/* panel is a disp2 framebuffer at /dev/fb0 with no X, no Wayland and no    */
/* DRM, so this module owns the whole display: open the device, read its    */
/* geometry and pixel format at runtime (nothing is hard-coded - resolution */
/* and packing come from the driver), mmap the pixels, and blit RFB         */
/* rectangles straight into them.                                           */
/*                                                                          */
/* All coordinates are in visible-screen pixels.  Rectangles are clipped to */
/* the screen so a misbehaving server cannot write outside the mapping.     */
/*--------------------------------------------------------------------------*/

#ifndef PRIMEVNC_FRAMEBUFFER_H
#define PRIMEVNC_FRAMEBUFFER_H

#include "pixfmt.h"

/*----------------------------------------------------------------------------*/
/* FB_Device:                                                                 */
/*                                                                            */
/* An open framebuffer: the fd, the mmap'd pixel memory and the geometry read */
/* from the driver.  fb_iLineLength is the driver's stride in bytes and may   */
/* exceed fb_iWidth * bytes-per-pixel (padding), so every row address is      */
/* computed from it rather than from the width.                               */
/*----------------------------------------------------------------------------*/

typedef struct
{
    int            fb_iFd;      // open /dev/fb0 descriptor, -1 when closed
    unsigned char *fb_pMem;     // mmap base of the pixel memory
    unsigned long  fb_iMemLen;  // mapped length in bytes (fix.smem_len)

    int            fb_iWidth;                // visible width  (var.xres)
    int            fb_iHeight;               // visible height (var.yres)
    int            fb_iLineLength;           // stride in bytes (fix.line_length)
    int            fb_iBytesPerPixel;        // (var.bits_per_pixel + 7) / 8

    int            fb_iYResVirtual;          // var.yres_virtual (>= yres if pannable)

    int            fb_iNumBuffers;           // 1 (write-through) or 2 (page-flipped panel)
    int            fb_iBufOffset[2];         // byte offset of each buffer within the mmap
    int            fb_iBackIndex;            // buffer currently drawn into (the back buffer)
    int            fb_iDrawByteOffset;       // = fb_iBufOffset[fb_iBackIndex]; blits use this

    PIX_Format     fb_Format;                // packing derived from var_screeninfo

    uint32_t       fb_uAlphaMask;            // transp bits (0xFF000000 here) OR'd into every pixel written; 0 = no alpha field
    int            fb_iPresentErrs;          // consecutive present-path ioctl failures (rate-limits the error log)

    int            fb_iBacklightAtOpen;      // panel brightness found by FB_BacklightTake (0..255); -1 = not read
    int            fb_bBacklightRestore;     // TRUE when FB_Close must put a non-zero found brightness back

    int            fb_iBacklightAutoMs;      // auto rule: ms an all-black frame must persist before the panel goes dark; 0 = rule off
    int            fb_iBacklightLitLevel;    // brightness the auto rule returns to when it re-lights the panel
    int            fb_bBacklightAutoDark;    // TRUE while the auto rule has the panel dark
    long long      fb_llBlackSinceMs;        // monotonic ms when the presented frame first read all black; 0 = frame not black
} FB_Device;

/*---------------------------------------------------------------------------*/
/* FB_Open:                                                                  */
/*                                                                           */
/* Opens the framebuffer, queries FBIOGET_VSCREENINFO / FBIOGET_FSCREENINFO, */
/* derives the pixel format and the alpha mask, selects the CACHED mapping   */
/* mode (FBIO_ENABLE_CACHE - honoured by the driver only BEFORE mmap), then  */
/* mmaps the pixel memory.  On any failure it logs the reason, releases      */
/* whatever it had opened, and returns FAILURE.                              */
/*                                                                           */
/* Arguments:                                                                */
/*     pFb   : device struct to populate; zeroed and filled on success.      */
/*     sPath : device path, typically "/dev/fb0".                            */
/*                                                                           */
/* Returns:                                                                  */
/*     int : SUCCESS with *pFb ready to blit, or FAILURE.                    */
/*---------------------------------------------------------------------------*/

int FB_Open(FB_Device *pFb, const char *sPath);

/*--------------------------------------------------------------------------*/
/* FB_Close:                                                                */
/*                                                                          */
/* Puts back a non-zero brightness that FB_BacklightTake changed, restores  */
/* the driver's default (uncached) mapping mode, unmaps the pixel memory    */
/* and closes the fd if open.  Safe to call on a zeroed or already-closed   */
/* device.                                                                  */
/*                                                                          */
/* Arguments:                                                               */
/*     pFb : device to release.                                             */
/*                                                                          */
/* Returns:                                                                 */
/*     void : leaves *pFb closed (fd -1, pMem NULL).                        */
/*--------------------------------------------------------------------------*/

void FB_Close(FB_Device *pFb);

/*--------------------------------------------------------------------------*/
/* Backlight policy for FB_BacklightTake.  The panel's brightness is not    */
/* fbdev's: on the sunxi disp2 driver it is set through /dev/disp, and the  */
/* value survives whatever process last owned the display, so a client that */
/* takes over a panel its previous owner had turned off draws into an unlit */
/* screen unless it lights it.  FB_BACKLIGHT_ON is the default for that     */
/* reason; OFF and LEVEL exist for tests and for a launcher that wants a    */
/* particular brightness.                                                   */
/*--------------------------------------------------------------------------*/

#define FB_BACKLIGHT_KEEP     0              // leave the brightness exactly as found; remember nothing
#define FB_BACKLIGHT_ON       1              // light the panel if it is dark (0 -> FB_BACKLIGHT_DEFAULT), otherwise leave it
#define FB_BACKLIGHT_OFF      2              // set brightness 0 (a test setting: shows what a dark takeover looks like)
#define FB_BACKLIGHT_LEVEL    3              // set the brightness given (1..FB_BACKLIGHT_MAX)
#define FB_BACKLIGHT_DEFAULT  175            // brightness used to light a dark panel (the reference device's factory setting)
#define FB_BACKLIGHT_MAX      255            // largest brightness the driver accepts
#define FB_BACKLIGHT_AUTO_DEFAULT_MS  60000  // the auto rule's black-frame timeout when the option says only "on"

/*--------------------------------------------------------------------------*/
/* FB_BacklightGet:                                                         */
/*                                                                          */
/* Reads the panel brightness from the display-engine driver (/dev/disp),   */
/* independently of the framebuffer: no FB_Device is needed and the node is */
/* opened and closed within the call.                                       */
/*                                                                          */
/* Arguments:                                                               */
/*     piLevel : out; the brightness, 0 (off) to FB_BACKLIGHT_MAX.          */
/*                                                                          */
/* Returns:                                                                 */
/*     int : SUCCESS with *piLevel set, or FAILURE (logged) when the node   */
/*           cannot be opened or the driver refuses the request.            */
/*--------------------------------------------------------------------------*/

int FB_BacklightGet(int *piLevel);

/*--------------------------------------------------------------------------*/
/* FB_BacklightSet:                                                         */
/*                                                                          */
/* Sets the panel brightness through the display-engine driver.  0 turns    */
/* the backlight off (the framebuffer keeps scanning, unseen); the value is */
/* clamped to 0..FB_BACKLIGHT_MAX.  Independent of any FB_Device.           */
/*                                                                          */
/* Arguments:                                                               */
/*     iLevel : the brightness wanted.                                      */
/*                                                                          */
/* Returns:                                                                 */
/*     int : SUCCESS, or FAILURE (logged).                                  */
/*--------------------------------------------------------------------------*/

int FB_BacklightSet(int iLevel);

/*--------------------------------------------------------------------------*/
/* FB_BacklightTake:                                                        */
/*                                                                          */
/* Applies a backlight policy when the panel is taken over: reads the       */
/* brightness as found, remembers it in *pFb, and changes it as the policy  */
/* says (ON lights a dark panel at FB_BACKLIGHT_DEFAULT and leaves a lit    */
/* one alone; OFF sets 0; LEVEL sets iLevel; KEEP touches nothing).  When   */
/* it changed the value, FB_Close puts the found value back - except a      */
/* found value of 0, because handing back an unlit panel is the one thing   */
/* never to do.  Works on a zeroed FB_Device as well as an open one, which  */
/* is how the one-shot --backlight-set command uses it.                     */
/*                                                                          */
/* Arguments:                                                               */
/*     pFb    : device whose backlight fields record what was found.        */
/*     iMode  : one of the FB_BACKLIGHT_* policies.                         */
/*     iLevel : the brightness for FB_BACKLIGHT_LEVEL; ignored otherwise.   */
/*                                                                          */
/* Returns:                                                                 */
/*     int : SUCCESS, or FAILURE (logged) when the brightness could not be  */
/*           read or written; the caller decides whether that matters.      */
/*--------------------------------------------------------------------------*/

int FB_BacklightTake(FB_Device *pFb, int iMode, int iLevel);

/*--------------------------------------------------------------------------*/
/* FB_BacklightAutoEnable:                                                  */
/*                                                                          */
/* Turns on the auto rule: once the presented frame has been entirely black */
/* for iBlackMs (FB_BacklightAutoTick decides when), the panel is turned    */
/* off, and the first presented frame with any non-black pixel turns it     */
/* back on (FB_BacklightAutoFrame).  The level it returns to is the one lit */
/* at takeover, read here if FB_BacklightTake did not.  This is how a       */
/* remote desktop's own screen-blanking reaches the panel over a plain RFB  */
/* link, which carries pixels and nothing about backlights.                 */
/*                                                                          */
/* Arguments:                                                               */
/*     pFb      : device to arm; FB_BacklightTake should have run first.    */
/*     iBlackMs : how long the frame must stay all black, in ms (> 0).      */
/*                                                                          */
/* Returns:                                                                 */
/*     void : arms the rule; the two calls below then do the work.          */
/*--------------------------------------------------------------------------*/

void FB_BacklightAutoEnable(FB_Device *pFb, int iBlackMs);

/*--------------------------------------------------------------------------*/
/* FB_BacklightAutoFrame:                                                   */
/*                                                                          */
/* Called after every presented frame when the auto rule is armed: scans    */
/* the page now on screen; a non-black pixel re-lights a panel the rule     */
/* darkened and resets the black clock, an all-black page starts the clock  */
/* if it is not already running.  A no-op when the rule is off.  The scan   */
/* stops at the first non-black pixel, so a normal picture costs almost     */
/* nothing and only a black frame is read in full.                          */
/*                                                                          */
/* Arguments:                                                               */
/*     pFb     : the device whose front page was just presented.            */
/*     llNowMs : the monotonic clock now, in ms.                            */
/*                                                                          */
/* Returns:                                                                 */
/*     void : may set the brightness; updates the clock and the dark flag.  */
/*--------------------------------------------------------------------------*/

void FB_BacklightAutoFrame(FB_Device *pFb, long long llNowMs);

/*--------------------------------------------------------------------------*/
/* FB_BacklightAutoTick:                                                    */
/*                                                                          */
/* Called on every pass of the control loop when the auto rule is armed:    */
/* if the black clock has run for the rule's timeout and the panel is still */
/* lit, turns it off.  A no-op when the rule is off, the frame is not       */
/* black, or the panel is already dark.                                     */
/*                                                                          */
/* Arguments:                                                               */
/*     pFb     : the device.                                                */
/*     llNowMs : the monotonic clock now, in ms.                            */
/*                                                                          */
/* Returns:                                                                 */
/*     void : may set the brightness to 0 and raise the dark flag.          */
/*--------------------------------------------------------------------------*/

void FB_BacklightAutoTick(FB_Device *pFb, long long llNowMs);

/*--------------------------------------------------------------------------*/
/* FB_BlitRaw:                                                              */
/*                                                                          */
/* Copies a w-by-h rectangle of source pixels into the framebuffer at (iX,  */
/* iY).  When the source format matches the framebuffer's, each row is a    */
/* memcpy; otherwise each row is converted with PIX_ConvertRow.  The        */
/* rectangle is clipped to the visible screen.                              */
/*                                                                          */
/* Arguments:                                                               */
/*     pFb     : open device.                                               */
/*     iX      : left edge in screen pixels.                                */
/*     iY      : top edge in screen pixels.                                 */
/*     iW      : rectangle width in pixels.                                 */
/*     iH      : rectangle height in pixels.                                */
/*     pSrc    : source pixels, iH rows of iW pixels, tightly packed at     */
/*               pSrcFmt->pix_iBytesPerPixel per pixel.                     */
/*     pSrcFmt : the source pixel format.                                   */
/*                                                                          */
/* Returns:                                                                 */
/*     void : writes into the mmap'd framebuffer.                           */
/*--------------------------------------------------------------------------*/

void FB_BlitRaw(FB_Device *pFb, int iX, int iY, int iW, int iH,
                const unsigned char *pSrc, const PIX_Format *pSrcFmt);

/*---------------------------------------------------------------------------*/
/* FB_CopyRect:                                                              */
/*                                                                           */
/* Moves a w-by-h rectangle within the framebuffer from a source origin to a */
/* destination origin, implementing the RFB CopyRect encoding.  Handles      */
/* overlap by choosing the row-copy direction, and uses memmove per row so   */
/* horizontal overlap is safe too.  Both rectangles are clipped to screen.   */
/*                                                                           */
/* Arguments:                                                                */
/*     pFb   : open device.                                                  */
/*     iSrcX : source left edge.                                             */
/*     iSrcY : source top edge.                                              */
/*     iDstX : destination left edge.                                        */
/*     iDstY : destination top edge.                                         */
/*     iW    : width in pixels.                                              */
/*     iH    : height in pixels.                                             */
/*                                                                           */
/* Returns:                                                                  */
/*     void : moves pixels within the framebuffer.                           */
/*---------------------------------------------------------------------------*/

void FB_CopyRect(FB_Device *pFb, int iSrcX, int iSrcY, int iDstX, int iDstY,
                 int iW, int iH);

/*--------------------------------------------------------------------------*/
/* FB_Clear:                                                                */
/*                                                                          */
/* Fills the visible rows of the BACK page with OPAQUE black (alpha set on  */
/* a per-pixel-alpha panel, so nothing below the layer shows through).  It  */
/* is not presented by itself: the panel keeps its last frame until the     */
/* first update is presented, so there is no black flash at start-up.       */
/*                                                                          */
/* Arguments:                                                               */
/*     pFb : open device.                                                   */
/*                                                                          */
/* Returns:                                                                 */
/*     void : the back page's visible rows hold opaque black.               */
/*--------------------------------------------------------------------------*/

void FB_Clear(FB_Device *pFb);

/*--------------------------------------------------------------------------*/
/* FB_BeginFrame:                                                           */
/*                                                                          */
/* Prepares the back buffer to receive one frame of (possibly incremental)  */
/* updates.  On a page-flipped panel the two buffers hold different frames, */
/* so the last presented frame (the front buffer) is copied into the back   */
/* buffer first; incremental rectangles then modify a complete image rather */
/* than a stale one.  On a single-buffer panel this is a no-op.             */
/*                                                                          */
/* Arguments:                                                               */
/*     pFb : open device.                                                   */
/*                                                                          */
/* Returns:                                                                 */
/*     void : back buffer holds the current image, ready for blits.         */
/*--------------------------------------------------------------------------*/

void FB_BeginFrame(FB_Device *pFb);

/*---------------------------------------------------------------------------*/
/* FB_Present:                                                               */
/*                                                                           */
/* Presents the back buffer to the panel: FBIO_CACHE_SYNC cleans the CPU     */
/* cache to the RAM the display engine scans (our mapping is cached), then   */
/* FBIOPAN_DISPLAY flips the layer to the back page and waits for vsync.     */
/* On a page-flipped panel front/back are then swapped (so the next frame    */
/* is drawn into what was just shown); on a single-buffer panel it           */
/* re-presents buffer 0.  A failed pan leaves the pages unswapped; failures  */
/* are logged with errno, rate-limited.  The memory-backed test device       */
/* (fd -1) skips the ioctls but still swaps, so the page-flip logic runs     */
/* without hardware.                                                         */
/*                                                                           */
/* Arguments:                                                                */
/*     pFb : open device.                                                    */
/*                                                                           */
/* Returns:                                                                  */
/*     void : the panel shows the just-drawn frame; the back index advances. */
/*---------------------------------------------------------------------------*/

void FB_Present(FB_Device *pFb);

#endif // PRIMEVNC_FRAMEBUFFER_H
