/*--------------------------------------------------------------------------*/
/* SPDX-License-Identifier: GPL-2.0-or-later                                */
/* Copyright (c) 2026 B. C. Services                                        */
/*                                                                          */
/* Licensed under the GNU General Public License version 2 or later --      */
/* see LICENSE beside this source.  GPL-2.0 full text in GPL-2.0.txt.       */
/*--------------------------------------------------------------------------*/
/* net.c:                                                                   */
/*                                                                          */
/* Implementation of the numeric-IPv4 TCP transport declared in net.h.      */
/* Everything here is standard POSIX sockets against glibc's own core (no   */
/* NSS, no getaddrinfo), so it links statically without the name-resolution */
/* hazard.  SIGPIPE is ignored once, on the first connect, so a write to a  */
/* dropped server surfaces as EPIPE at the call site instead of a signal.   */
/*--------------------------------------------------------------------------*/

#include "net.h"
#include "log.h"
#include "primevnc.h"

#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <fcntl.h>
#include <time.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

/*--------------------------------------------------------------------------*/
/* Connect bound.  A blocking connect() to a host that drops the SYN sits   */
/* in the kernel's retransmit schedule for about two minutes before         */
/* ETIMEDOUT, stalling the whole single-threaded client on a panel that     */
/* nothing else is driving meanwhile.  On a LAN a server that has not       */
/* answered in five seconds is not going to.                                */
/*--------------------------------------------------------------------------*/

#define NET_CONNECT_TIMEOUT_MS  5000         // bound on one connect attempt (LAN link)

/*---------------------------------------------------------------------------*/
/* NET_IgnoreSigpipeOnce:                                                    */
/*                                                                           */
/* Installs SIG_IGN for SIGPIPE exactly once per process.  Without it, the   */
/* first write to a peer that has closed would deliver SIGPIPE and terminate */
/* the client instead of returning EPIPE - which would defeat the graceful   */
/* server-vanished handling in the control loop.  Local to this file.        */
/*                                                                           */
/* Arguments:                                                                */
/*     None.                                                                 */
/*                                                                           */
/* Returns:                                                                  */
/*     void : sets the disposition on first call, no-ops thereafter.         */
/*---------------------------------------------------------------------------*/

static void NET_IgnoreSigpipeOnce(void)
{
    static int bDone = FALSE;                 // guards the one-time install

    if (bDone != FALSE)
    {   /* Already installed earlier in this process; nothing to do. */
        return;
    }

    /* Ignore SIGPIPE so a broken connection is an errno, not a death. */
    signal(SIGPIPE, SIG_IGN);
    bDone = TRUE;
}

/*--------------------------------------------------------------------------*/
/* NET_ConnectIp:                                                           */
/*                                                                          */
/* Numeric-only TCP connect, bounded by NET_CONNECT_TIMEOUT_MS, with        */
/* TCP_NODELAY on the result.  The connect itself runs non-blocking so a    */
/* dropped SYN fails in seconds instead of parking in the kernel's ~2 min   */
/* retransmit schedule; the socket handed back is BLOCKING again, which     */
/* is what every read and write in this module assumes.  See net.h for      */
/* the contract.                                                            */
/*                                                                          */
/* Arguments:                                                               */
/*     sIp   : dotted-quad IPv4 string.                                     */
/*     iPort : TCP port.                                                    */
/*                                                                          */
/* Returns:                                                                 */
/*     int : connected fd, or -1 on failure.                                */
/*--------------------------------------------------------------------------*/

int NET_ConnectIp(const char *sIp, int iPort)
{
    struct sockaddr_in addr;                  // IPv4 target address
    struct pollfd      pfd;                   // waits for the pending connect to finish
    socklen_t          iErrLen;               // size of the SO_ERROR result
    int                iFd;                   // the socket
    int                iOne;                  // setsockopt flag value
    int                iRc;                   // inet_pton / connect / poll result
    int                iFlags;                // descriptor flags, restored after the connect
    int                iSoErr;                // the connect's final outcome from SO_ERROR

    /* Make a broken pipe recoverable before we ever write. */
    NET_IgnoreSigpipeOnce();

    /* Build the target address from the numeric string only.  inet_pton       */
    /* returns 1 on a valid dotted-quad, 0 on a non-numeric string (which we   */
    /* reject rather than resolve), and -1 on an address-family error.         */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)iPort);

    iRc = inet_pton(AF_INET, sIp, &addr.sin_addr);

    if (iRc != 1)
    {   /* Not a numeric IPv4 address; by design we do not do DNS here. */
        LOG_Error("'%s' is not a numeric IPv4 address (name resolution is not built in)", sIp);
        return(-1);
    }

    /* Create the stream socket. */
    iFd = socket(AF_INET, SOCK_STREAM, 0);

    if (iFd < 0)
    {   /* Out of descriptors, or sockets unavailable. */
        LOG_Error("socket() failed: %s", strerror(errno));
        return(-1);
    }

    /* The connect runs NON-BLOCKING so it can be bounded: a blocking       */
    /* connect() to a host that drops the SYN sits in the kernel's          */
    /* retransmit schedule for about two minutes.  Read the current flags   */
    /* first so they can be restored exactly afterwards.                    */
    iFlags = fcntl(iFd, F_GETFL, 0);

    if (iFlags < 0)
    {   /* Cannot read the descriptor flags; nothing sensible can follow. */
        LOG_Error("fcntl(F_GETFL) failed: %s", strerror(errno));
        close(iFd);
        return(-1);
    }

    if (fcntl(iFd, F_SETFL, iFlags | O_NONBLOCK) != 0)
    {   /* Cannot enter non-blocking mode, so the connect could not be bounded. */
        LOG_Error("fcntl(F_SETFL, O_NONBLOCK) failed: %s", strerror(errno));
        close(iFd);
        return(-1);
    }

    /* Start the connect.  On a non-blocking socket the normal answer is -1   */
    /* with EINPROGRESS and the handshake completes in the background; an     */
    /* immediate 0 (loopback) is simply already connected.                    */
    iRc = connect(iFd, (struct sockaddr *)&addr, sizeof(addr));

    if (iRc != 0 && errno != EINPROGRESS)
    {   /* Failed outright: no route, or refused before the handshake began. */
        LOG_Error("connect to %s:%d failed: %s", sIp, iPort, strerror(errno));
        close(iFd);
        return(-1);
    }

    if (iRc != 0)
    {   /* In progress: wait a bounded time for the handshake to finish. */

        /* POLLOUT is how the kernel reports the connect has finished, with   */
        /* success or failure alike; SO_ERROR below says which.               */
        for (;;)
        {   /* Retry loop exists solely to swallow EINTR. */
            pfd.fd      = iFd;
            pfd.events  = POLLOUT;
            pfd.revents = 0;

            iRc = poll(&pfd, 1, NET_CONNECT_TIMEOUT_MS);

            if (iRc < 0 && errno == EINTR)
            {   /* Interrupted by a signal; poll again with the full bound. */
                continue;
            }

            break;
        }

        if (iRc == 0)
        {   /* Nothing answered within the bound: treat the host as dead. */
            LOG_Error("connect to %s:%d timed out after %d ms",
                      sIp, iPort, NET_CONNECT_TIMEOUT_MS);
            close(iFd);
            return(-1);
        }

        if (iRc < 0)
        {   /* poll itself failed for a reason other than a signal. */
            LOG_Error("poll() during connect to %s:%d failed: %s",
                      sIp, iPort, strerror(errno));
            close(iFd);
            return(-1);
        }

        /* Writable: the outcome lives in SO_ERROR, not in poll's result.  A   */
        /* refused connection surfaces here as ECONNREFUSED, a dead host as    */
        /* EHOSTUNREACH; 0 means the three-way handshake completed.            */
        iSoErr  = 0;
        iErrLen = sizeof(iSoErr);

        if (getsockopt(iFd, SOL_SOCKET, SO_ERROR, &iSoErr, &iErrLen) != 0)
        {   /* Could not read the result; assume the connect did not happen. */
            LOG_Error("getsockopt(SO_ERROR) failed: %s", strerror(errno));
            close(iFd);
            return(-1);
        }

        if (iSoErr != 0)
        {   /* The handshake ended in an error (refused, unreachable, reset). */
            LOG_Error("connect to %s:%d failed: %s", sIp, iPort, strerror(iSoErr));
            close(iFd);
            return(-1);
        }
    }

    /* Back to BLOCKING: every read and write in this module and above it   */
    /* polls first and then expects the call itself to complete.            */
    if (fcntl(iFd, F_SETFL, iFlags) != 0)
    {   /* Cannot restore blocking mode; the rest of the client would misread EAGAIN. */
        LOG_Error("fcntl(F_SETFL) restore failed: %s", strerror(errno));
        close(iFd);
        return(-1);
    }

    /* Disable Nagle: RFB control traffic is small and latency-sensitive. */
    iOne = 1;
    setsockopt(iFd, IPPROTO_TCP, TCP_NODELAY, &iOne, sizeof(iOne));

    LOG_Info("connected to %s:%d (fd %d)", sIp, iPort, iFd);
    /* Connected: hand the ready blocking socket back to the caller. */
    return(iFd);
}

/*---------------------------------------------------------------------------*/
/* NET_PollReadable:                                                         */
/*                                                                           */
/* poll() wrapper that retries EINTR.  A hang-up with data still pending     */
/* reports readable, not error, so the peer's final bytes drain.  See net.h. */
/*                                                                           */
/* Arguments:                                                                */
/*     iFd       : socket fd.                                                */
/*     iTimeoutMs: milliseconds to wait; <= 0 waits forever.                 */
/*                                                                           */
/* Returns:                                                                  */
/*     int : 1 readable, 0 timeout, -1 error.                                */
/*---------------------------------------------------------------------------*/

int NET_PollReadable(int iFd, int iTimeoutMs)
{
    struct pollfd pfd;                         // single-descriptor poll set
    int           iRc;                         // poll return

    for (;;)
    {   /* Retry loop exists solely to swallow EINTR. */
        pfd.fd      = iFd;
        pfd.events  = POLLIN;
        pfd.revents = 0;

        iRc = poll(&pfd, 1, (iTimeoutMs > 0) ? iTimeoutMs : -1);

        if (iRc < 0)
        {   /* A caught signal restarts the wait; anything else is an error. */
            if (errno == EINTR)
            {   /* Interrupted by a signal; poll again with the same timeout. */
                continue;
            }

            LOG_Error("poll() failed: %s", strerror(errno));
            return(-1);
        }

        if (iRc == 0)
        {   /* Timed out with no data. */
            return(0);
        }

        if ((pfd.revents & (POLLERR | POLLNVAL)) != 0)
        {   /* The descriptor went bad: an error for the caller. */
            return(-1);
        }

        if ((pfd.revents & POLLHUP) != 0 && (pfd.revents & POLLIN) == 0)
        {   /* The peer hung up and nothing is left to drain: treat as an error. */
            return(-1);
        }

        /* POLLIN set (a HUP beside it is fine): data, or the peer's FIN, is     */
        /* readable and read() will report which; the final bytes a peer sends   */
        /* just before closing are drained rather than dropped.                  */
        return(1);
    }
}

/*--------------------------------------------------------------------------*/
/* NET_NowMs:                                                               */
/*                                                                          */
/* Reads the monotonic clock in milliseconds.  Used to turn NET_ReadFull's  */
/* timeout into a DEADLINE, so the bound covers the whole read instead of   */
/* re-arming on every chunk (a trickling peer would never trip it), and by  */
/* the control loop for the backlight rule's black-frame clock.  Monotonic  */
/* so a wall-clock step cannot stretch or collapse a wait: the device's     */
/* clock may be unset and jump when an NTP sync finally succeeds.           */
/*                                                                          */
/* Arguments:                                                               */
/*     None.                                                                */
/*                                                                          */
/* Returns:                                                                 */
/*     long long : milliseconds since an arbitrary fixed point in the past. */
/*--------------------------------------------------------------------------*/

long long NET_NowMs(void)
{
    struct timespec ts;                         // monotonic clock reading

    /* CLOCK_MONOTONIC cannot fail for a valid clock id on Linux; the result   */
    /* is folded into one millisecond count.                                   */
    clock_gettime(CLOCK_MONOTONIC, &ts);

    /* Seconds scaled up, nanoseconds scaled down, summed. */
    return((long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L));
}

/*--------------------------------------------------------------------------*/
/* NET_ReadFull:                                                            */
/*                                                                          */
/* Reads exactly iLen bytes, retrying EINTR and short reads, under ONE      */
/* deadline fixed when the call starts: iTimeoutMs bounds the whole read,   */
/* not each chunk, so a trickling peer trips it as surely as a silent one.  */
/* See net.h.                                                               */
/*                                                                          */
/* Arguments:                                                               */
/*     iFd       : socket fd.                                               */
/*     pBuf      : destination buffer.                                      */
/*     iLen      : bytes to read.                                           */
/*     iTimeoutMs: overall deadline for the whole read; <= 0 waits forever. */
/*                                                                          */
/* Returns:                                                                 */
/*     int : SUCCESS if all bytes arrived, FAILURE otherwise.               */
/*--------------------------------------------------------------------------*/

int NET_ReadFull(int iFd, void *pBuf, size_t iLen, int iTimeoutMs)
{
    unsigned char *pDst;                        // walking write cursor
    size_t         iRemaining;                  // bytes still to read
    ssize_t        iGot;                        // bytes from one read()
    int            iReady;                      // NET_PollReadable result
    int            iWait;                       // this pass's poll bound, from the deadline
    long long      llDeadline;                  // monotonic ms by which the read must finish; 0 = none
    long long      llLeft;                      // ms left until llDeadline

    pDst       = (unsigned char *)pBuf;
    iRemaining = iLen;

    /* Fix the deadline ONCE so iTimeoutMs bounds the WHOLE read; <= 0 means   */
    /* no deadline, and every poll below then waits forever.                   */
    llDeadline = 0;

    if (iTimeoutMs > 0)
    {   /* A real bound: everything must have arrived by now plus the timeout. */
        llDeadline = NET_NowMs() + (long long)iTimeoutMs;
    }

    while (iRemaining > 0)
    {   /* Each pass fills as much of the remainder as one read() yields. */

        /* Wait for readability, bounded by what is LEFT of the deadline, so a   */
        /* stalled or trickling server trips the timeout rather than blocking    */
        /* the whole control loop indefinitely.                                  */
        iWait = -1;

        if (llDeadline != 0)
        {   /* Bounded read: spend only the remaining budget on this poll. */
            llLeft = llDeadline - NET_NowMs();

            if (llLeft <= 0)
            {   /* The deadline passed between chunks: the server is too slow or gone. */
                LOG_Warn("read timed out waiting for %lu of %lu bytes",
                         (unsigned long)iRemaining, (unsigned long)iLen);
                return(FAILURE);
            }

            iWait = (int)llLeft;
        }

        iReady = NET_PollReadable(iFd, iWait);

        if (iReady < 0)
        {   /* poll error or peer hang-up already reported. */
            return(FAILURE);
        }

        if (iReady == 0)
        {   /* Timeout: the server is too slow or gone. */
            LOG_Warn("read timed out waiting for %lu of %lu bytes",
                     (unsigned long)iRemaining, (unsigned long)iLen);
            return(FAILURE);
        }

        iGot = read(iFd, pDst, iRemaining);

        if (iGot < 0)
        {   /* A caught signal is retried; any other errno is fatal to the read. */
            if (errno == EINTR)
            {   /* Interrupted before any byte moved; try again. */
                continue;
            }

            LOG_Error("read() failed: %s", strerror(errno));
            return(FAILURE);
        }

        if (iGot == 0)
        {   /* Orderly shutdown by the peer: the server closed the connection. */
            LOG_Info("server closed the connection");
            return(FAILURE);
        }

        /* Advance past what we just stored. */
        pDst       += iGot;
        iRemaining -= (size_t)iGot;
    }

    /* Every requested byte was read. */
    return(SUCCESS);
}

/*--------------------------------------------------------------------------*/
/* NET_WriteFull:                                                           */
/*                                                                          */
/* Writes exactly iLen bytes, looping over short writes and retrying EINTR. */
/* See net.h.                                                               */
/*                                                                          */
/* Arguments:                                                               */
/*     iFd  : socket fd.                                                    */
/*     pBuf : source buffer.                                                */
/*     iLen : bytes to write.                                               */
/*                                                                          */
/* Returns:                                                                 */
/*     int : SUCCESS if all bytes were written, FAILURE otherwise.          */
/*--------------------------------------------------------------------------*/

int NET_WriteFull(int iFd, const void *pBuf, size_t iLen)
{
    const unsigned char *pSrc;        // walking read cursor
    size_t               iRemaining;  // bytes still to write
    ssize_t              iPut;        // bytes from one write()

    pSrc       = (const unsigned char *)pBuf;
    iRemaining = iLen;

    while (iRemaining > 0)
    {   /* Each pass drains as much of the remainder as one write() accepts. */
        iPut = write(iFd, pSrc, iRemaining);

        if (iPut < 0)
        {   /* Retry a signal; EPIPE and the rest are fatal (SIGPIPE is ignored). */
            if (errno == EINTR)
            {   /* Interrupted before any byte moved; try again. */
                continue;
            }

            LOG_Error("write() failed: %s", strerror(errno));
            return(FAILURE);
        }

        /* Advance past what the kernel accepted. */
        pSrc       += iPut;
        iRemaining -= (size_t)iPut;
    }

    /* Every requested byte was written. */
    return(SUCCESS);
}

/*--------------------------------------------------------------------------*/
/* NET_Close:                                                               */
/*                                                                          */
/* Closes a valid fd; ignores -1.  See net.h.                               */
/*                                                                          */
/* Arguments:                                                               */
/*     iFd : fd to close, or -1.                                            */
/*                                                                          */
/* Returns:                                                                 */
/*     void : releases the descriptor.                                      */
/*--------------------------------------------------------------------------*/

void NET_Close(int iFd)
{
    if (iFd >= 0)
    {   /* Only a real descriptor is closed; -1 is a deliberate no-op. */
        close(iFd);
    }
}
