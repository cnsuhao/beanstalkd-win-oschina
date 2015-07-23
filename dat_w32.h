#ifndef __DAT_W32_H__
#define __DAT_W32_H__

#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <WinError.h>

#pragma comment(lib, "ws2_32.lib")

#define EWOULDBLOCK WSAEWOULDBLOCK

#ifndef __attribute__
#define __attribute__(x)
#endif

#ifndef __func__
#define __func__ __FUNCDNAME__
#endif

#if !defined WIN32
#define net_read  read
#define net_write write
#define net_close close
#else
#define net_read(s, b, c)  recv(s, b, c, 0)
#define net_write(s, b, c) send(s, b, c, 0)
#define net_close closesocket
#endif

#if !defined snprintf
#define snprintf _snprintf
#endif

#endif