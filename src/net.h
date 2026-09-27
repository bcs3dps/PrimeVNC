/*---------------------------------------------------------------------------*/
/* SPDX-License-Identifier: GPL-2.0-or-later                                 */
/* Copyright (c) 2026 B. C. Services                                         */
/*                                                                           */
/* Licensed under the GNU General Public License version 2 or later --       */
/* see LICENSE beside this source.  GPL-2.0 full text in GPL-2.0.txt.        */
/*---------------------------------------------------------------------------*/
/* net.h:                                                                    */
/*                                                                           */
/* TCP transport for the RFB client.  Deliberately tiny and deliberately     */
/* NAME-RESOLUTION-FREE: the server is always given as a numeric IPv4        */
/* address (the launcher knows its own address and passes it in), so this    */
/* module calls inet_pton and never getaddrinfo/gethostbyname.  That is what */
/* makes a fully static build safe here - glibc's NSS resolver is the one    */
/* facility that misbehaves when statically linked, and this program never   */
/* touches it.                                                               */
/*                                                                           */
/* The reads use poll() for an overall timeout so a wedged or vanished       */
/* server cannot hang the single-threaded control loop forever; that         */
/* timeout is what turns a dropped server into a failed read the control     */
/* loop can act on.                                                          */
/*---------------------------------------------------------------------------*/

#ifndef PRIMEVNC_NET_H
#define PRIMEVNC_NET_H

#include <stddef.h>

/*--------------------------------------------------------------------------*/
/* NET_ConnectIp:                                                           */
/*                                                                          */
/* Opens a TCP connection to a numeric IPv4 address and port with a BOUNDED */
/* connect (5 s, so a dropped SYN cannot hang the client for ~2 minutes),   */
/* then enables TCP_NODELAY (RFB is request/response with small control     */
/* messages, so Nagle only adds latency).  The socket handed back is        */
/* BLOCKING.  No DNS: a non-numeric host is rejected.                       */
/*                                                                          */
/* Arguments:                                                               */
/*     sIp   : dotted-quad IPv4 string, e.g. "192.168.10.50".               */
/*     iPort : TCP port (RFB is 5900 + display number).                     */
/*                                                                          */
/* Returns:                                                                 */
/*     int : a connected socket fd on success, or -1 on failure (the reason */
/*           is logged).  The caller closes it with NET_Close.              */
/*--------------------------------------------------------------------------*/

int NET_ConnectIp(const char *sIp, int iPort);

/*---------------------------------------------------------------------------*/
/* NET_ReadFull:                                                             */
/*                                                                           */
/* Reads exactly iLen bytes into pBuf, blocking (with an overall poll        */
/* timeout) until they all arrive.  A short read is retried; EINTR is        */
/* retried; a closed peer or the timeout is a failure.  This is the only     */
/* read path the RFB layer uses, so RFB code never has to loop on partial    */
/* reads itself.                                                             */
/*                                                                           */
/* Arguments:                                                                */
/*     iFd       : connected socket fd.                                      */
/*     pBuf      : destination buffer of at least iLen bytes.                */
/*     iLen      : number of bytes to read.                                  */
/*     iTimeoutMs: overall timeout in milliseconds; <= 0 means wait forever. */
/*                                                                           */
/* Returns:                                                                  */
/*     int : SUCCESS when all iLen bytes were read, FAILURE otherwise.       */
/*---------------------------------------------------------------------------*/

int NET_ReadFull(int iFd, void *pBuf, size_t iLen, int iTimeoutMs);

/*--------------------------------------------------------------------------*/
/* NET_WriteFull:                                                           */
/*                                                                          */
/* Writes exactly iLen bytes from pBuf, looping over short writes and       */
/* retrying EINTR.  SIGPIPE is suppressed process-wide (see NET_ConnectIp), */
/* so a write to a dead peer returns an error rather than killing us.       */
/*                                                                          */
/* Arguments:                                                               */
/*     iFd  : connected socket fd.                                          */
/*     pBuf : source buffer of at least iLen bytes.                         */
/*     iLen : number of bytes to write.                                     */
/*                                                                          */
/* Returns:                                                                 */
/*     int : SUCCESS when all bytes were written, FAILURE otherwise.        */
/*--------------------------------------------------------------------------*/

int NET_WriteFull(int iFd, const void *pBuf, size_t iLen);

/*--------------------------------------------------------------------------*/
/* NET_PollReadable:                                                        */
/*                                                                          */
/* Waits until the socket has data to read or the timeout elapses.  A thin  */
/* wrapper over poll() that retries EINTR; used by the control loop when it */
/* multiplexes the socket against the input device.  A hang-up with data    */
/* still pending is reported as READABLE so the peer's last bytes drain;    */
/* only POLLERR / POLLNVAL, or a hang-up with nothing left, report -1.      */
/*                                                                          */
/* Arguments:                                                               */
/*     iFd       : socket fd.                                               */
/*     iTimeoutMs: milliseconds to wait; <= 0 means wait forever.           */
/*                                                                          */
/* Returns:                                                                 */
/*     int : 1 if readable, 0 on timeout, -1 on error.                      */
/*--------------------------------------------------------------------------*/

int NET_PollReadable(int iFd, int iTimeoutMs);

/*--------------------------------------------------------------------------*/
/* NET_Close:                                                               */
/*                                                                          */
/* Closes a socket fd if it is valid (>= 0).  Safe to call with -1.         */
/*                                                                          */
/* Arguments:                                                               */
/*     iFd : socket fd to close, or -1 for a no-op.                         */
/*                                                                          */
/* Returns:                                                                 */
/*     void : releases the descriptor.                                      */
/*--------------------------------------------------------------------------*/

void NET_Close(int iFd);

/*--------------------------------------------------------------------------*/
/* NET_NowMs:                                                               */
/*                                                                          */
/* The monotonic clock in milliseconds: the time base for every deadline    */
/* in this program (read timeouts here, the backlight rule's black-frame    */
/* clock in the control loop).  Monotonic, so a wall-clock step on a device */
/* whose clock is unset until an NTP sync cannot stretch or collapse a      */
/* wait.                                                                    */
/*                                                                          */
/* Arguments:                                                               */
/*     None.                                                                */
/*                                                                          */
/* Returns:                                                                 */
/*     long long : milliseconds since an arbitrary fixed point in the past. */
/*--------------------------------------------------------------------------*/

long long NET_NowMs(void);

#endif // PRIMEVNC_NET_H
