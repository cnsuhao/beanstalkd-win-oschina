#include "dat_w32.h"

#include <stdint.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>

#include "dat.h"


#if defined WIN32_IOCP_MODE

#define WRITE_LOG(...)              \
    do {                            \
        if (verbose) {              \
            printf(__VA_ARGS__);    \
        }                           \
    }while(0);


static HANDLE iocph;

typedef struct {
    int     fd;
    Socket* sock;
} listen_sock_info_t;

static Socket* listens[100];
static int listens_size = 0;

static char buf0[512]; /* buffer of zeros */ 

/* Allocate disk space.
 * Expects fd's offset to be 0; may also reset fd's offset to 0.
 * Returns 0 on success, and a positive errno otherwise. */
int
rawfalloc(int fd, int len)
{
    int i, w;

    WRITE_LOG("rawfalloc len\n");

    for (i = 0; i < len; i += w) {
        w = write(fd, buf0, sizeof buf0);
        if (w == -1) return errno;
    }

    lseek(fd, 0, 0); /* do not care if this fails */

    return 0;
}


int
sockinit(void)
{
    WRITE_LOG("[sockinit] sockinit\n");

    /* create a single IOCP to be shared by all sockets */
    iocph = CreateIoCompletionPort(INVALID_HANDLE_VALUE,
        NULL,
        0,
        1);
    if (iocph == NULL) {
        return -1;
    }

    WRITE_LOG("[sockinit] done\n");

    return 0;
}

int
sockaccept(int fd, struct sockaddr *addr, int *addrlen) 
{
    Socket* s = NULL;
    int result = 0;
    SOCKADDR *plocalsa = NULL;
    SOCKADDR *premotesa = NULL;
    int locallen = 0;
    int remotelen = 0;
    int acceptfd = 0;
    int i;
    iocp_accept_t* acpt;
    
    for (i = 0; i < listens_size; ++i)
    {
        if (listens[i]->fd == fd) {
            s = listens[i];
            break;
        }
    }

    if (s == NULL) {
        return -1;
    }

    acpt = (iocp_accept_t*)(s->ovlp.data);
    acceptfd = acpt->fd;


    GetAcceptExSockaddrs(
        acpt->buf,
        0,
        sizeof(SOCKADDR),
        sizeof(SOCKADDR),
        &plocalsa, &locallen,
        &premotesa, &remotelen);


    if (addr != NULL) {
        if (remotelen > 0) {
            if (remotelen < *addrlen) {
                *addrlen = remotelen;
            }
            memcpy(addr, premotesa, *addrlen);
        } else {
            *addrlen = 0;
        }
    }

    /* queue another accept */
    if (sockqueueaccept(s) == -1) {
        return -1;
    }

    return acceptfd;
}


int
sockqueueaccept(Socket *s)
{
    iocp_accept_t* acpt;
    int result = 0;

    if (s->ovlp.data == NULL) {
        s->ovlp.data = new(sizeof(iocp_accept_t));
    }
    acpt = (iocp_accept_t*)(s->ovlp.data);
   
    acpt->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (acpt->fd == -1) {
        errno = WSAEINVAL;
        return -1;
    }
     
    

    if(!AcceptEx(s->fd, acpt->fd, acpt->buf, 0, 
        ACCEPT_LENGTH, ACCEPT_LENGTH, 0, (LPOVERLAPPED)&s->ovlp))
    {
        result = WSAGetLastError();
        if(result == WSA_IO_PENDING) {
            result = NO_ERROR;
        }else{
            result = -1;
        }
    }
    return result;
}

int
sockwant(Socket *s, int rw)
{
    WRITE_LOG("sockwant, fd:%d, want:%c\n", s->fd, (char)rw);
    
    if (s->bound == 0) {
        if (CreateIoCompletionPort((HANDLE) s->fd, iocph, (ULONG_PTR)s, 0) == NULL) {
            WRITE_LOG("[sockwant] CreateIoCompletionPort Fail!:fd:%d\n", s->fd);
            return -1;
        }
        if (s->type == SOCK_TYPE_LISTEN) {
            listens[listens_size] = s;
            listens_size++;
        }
        s->bound = 1;
    }

    if (s->added == 'r' && s->reading == 1) {
        s->added = 0;
        s->reading = 0;
        CancelIo((HANDLE)s->fd);
    }
    else if (s->added == 'w' && s->writing == 1) {
        s->added = 0;
        s->writing = 0;
        CancelIo((HANDLE)s->fd);
    }

    switch (rw) {
    case 'r':
        if (s->reading == 0) {
            if (s->type == SOCK_TYPE_LISTEN) {
                s->reading = 1;
                s->added = rw;
                s->ovlp.event = rw;
                return sockqueueaccept(s);

            } else {

                WSABUF             wsabuf;
                uint32             bytes, flags;
                int                rc;

                memset(&(s->ovlp), 0, sizeof(iocp_event_ovlp_t));
                s->ovlp.event = rw;
                wsabuf.buf = NULL;
                wsabuf.len = 0;
                flags = 0;
                bytes = 0;
                rc = WSARecv(s->fd, &wsabuf, 1, &bytes, &flags, (LPWSAOVERLAPPED)&(s->ovlp), NULL);
                if (rc == -1 && GetLastError() != ERROR_IO_PENDING) {
                    WRITE_LOG("[sockwant] WSARecv fail!:fd:%d, err:%d\n", s->fd, GetLastError());
                    return -1;
                }

                s->added = rw;
                s->reading = 1;
            }
        }
        break;
    case 'w':
        if (s->writing == 0) {

            WSABUF             wsabuf;
            uint32             bytes, flags;
            int                rc;

            memset(&(s->ovlp), 0, sizeof(iocp_event_ovlp_t));
            s->ovlp.event = rw;
            wsabuf.buf = NULL;
            wsabuf.len = 0;
            flags = 0;
            bytes = 0;
            rc = WSASend(s->fd, &wsabuf, 1, &bytes, flags, (LPWSAOVERLAPPED)&(s->ovlp), NULL);
            if (rc == -1 && GetLastError() != ERROR_IO_PENDING) {
                WRITE_LOG("[sockwant] WSASend fail!:fd:%d, err:%d\n", s->fd, GetLastError());
                return -1;
            }

            s->added = rw;
            s->writing = 1;
        }
        break;
    }



    WRITE_LOG("[sockwant] done:fd:%d\n", s->fd);
    return 0;
}

int
socknext(Socket **s, int64 timeout)
{
    OVERLAPPED_ENTRY entry;
    iocp_event_ovlp_t* ovlp;
    int rc  = 0;
    int err = 0;

    rc = GetQueuedCompletionStatus(iocph,
        &entry.dwNumberOfBytesTransferred,
        &entry.lpCompletionKey,
        &entry.lpOverlapped,
        timeout / 1000000);
    if (!rc && entry.lpOverlapped == NULL) {
        // timeout. Return.
        return 0;
    }

    *s = (Socket*)entry.lpCompletionKey;
    err = WSAGetLastError();

    if (err == ERROR_NETNAME_DELETED /* the socket was closed */
        || err == ERROR_OPERATION_ABORTED /* the operation was canceled */)
    {
        
        /*
         * the WSA_OPERATION_ABORTED completion notification
         * for a file descriptor that was closed
         */
        return 'h';
    }

    ovlp = (iocp_event_ovlp_t*)(entry.lpOverlapped);
    return ovlp->event;
}

#endif