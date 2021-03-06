/** 
 * @file net.cpp
 * @brief Cross-platform routines for sending and receiving packets.
 *
 * $LicenseInfo:firstyear=2000&license=viewergpl$
 * 
 * Copyright (c) 2000-2009, Linden Research, Inc.
 * 
 * Second Life Viewer Source Code
 * The source code in this file ("Source Code") is provided by Linden Lab
 * to you under the terms of the GNU General Public License, version 2.0
 * ("GPL"), unless you have obtained a separate licensing agreement
 * ("Other License"), formally executed by you and Linden Lab.  Terms of
 * the GPL can be found in doc/GPL-license.txt in this distribution, or
 * online at http://secondlifegrid.net/programs/open_source/licensing/gplv2
 * 
 * There are special exceptions to the terms and conditions of the GPL as
 * it is applied to this Source Code. View the full text of the exception
 * in the file doc/FLOSS-exception.txt in this software distribution, or
 * online at
 * http://secondlifegrid.net/programs/open_source/licensing/flossexception
 * 
 * By copying, modifying or distributing this software, you acknowledge
 * that you have read and understood your obligations described above,
 * and agree to abide by those obligations.
 * 
 * ALL LINDEN LAB SOURCE CODE IS PROVIDED "AS IS." LINDEN LAB MAKES NO
 * WARRANTIES, EXPRESS, IMPLIED OR OTHERWISE, REGARDING ITS ACCURACY,
 * COMPLETENESS OR PERFORMANCE.
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "net.h"

// system library includes
#include <stdexcept>

#if LL_WINDOWS
	#define WIN32_LEAN_AND_MEAN
	#include <winsock2.h>
	#include <windows.h>
#else
	#include <sys/types.h>
	#include <sys/socket.h>
	#include <netinet/in.h>
	#include <arpa/inet.h>
	#include <fcntl.h>
	#include <errno.h>
#endif

// linden library includes
#include "llerror.h"
#include "llhost.h"
#include "lltimer.h"
#include "indra_constants.h"

#include "llsocks5.h"

// Globals
#if LL_WINDOWS

SOCKADDR_IN stDstAddr;
SOCKADDR_IN stSrcAddr;
SOCKADDR_IN stLclAddr;
static WSADATA stWSAData;

#else

struct sockaddr_in stDstAddr;
struct sockaddr_in stSrcAddr;
struct sockaddr_in stLclAddr;

#if LL_DARWIN
#ifndef _SOCKLEN_T
#define _SOCKLEN_T
typedef int socklen_t;
#endif
#endif

#endif

static U32 gsnReceivingIFAddr = INVALID_HOST_IP_ADDRESS; // Address to which datagram was sent

const char* LOOPBACK_ADDRESS_STRING = "127.0.0.1";

#if LL_DARWIN
	// Mac OS X returns an error when trying to set these to 400000.  Smaller values succeed.
	const int	SEND_BUFFER_SIZE	= 200000;
	const int	RECEIVE_BUFFER_SIZE	= 200000;
#else // LL_DARWIN
	const int	SEND_BUFFER_SIZE	= 400000;
	const int	RECEIVE_BUFFER_SIZE	= 400000;
#endif // LL_DARWIN

// universal functions (cross-platform)

LLHost get_sender()
{
	return LLHost(stSrcAddr.sin_addr.s_addr, ntohs(stSrcAddr.sin_port));
}

U32 get_sender_ip(void) 
{
	return stSrcAddr.sin_addr.s_addr;
}

U32 get_sender_port() 
{
	return ntohs(stSrcAddr.sin_port);
}

LLHost get_receiving_interface()
{
	return LLHost(gsnReceivingIFAddr, INVALID_PORT);
}

U32 get_receiving_interface_ip(void)
{
	return gsnReceivingIFAddr;
}

const char* u32_to_ip_string(U32 ip)
{
	static char buffer[MAXADDRSTR];	 /* Flawfinder: ignore */ 

	// Convert the IP address into a string
	in_addr in;
	in.s_addr = ip;
	char* result = inet_ntoa(in);

	// NULL indicates error in conversion
	if (result != NULL)
	{
		strncpy( buffer, result, MAXADDRSTR );	 /* Flawfinder: ignore */ 
		buffer[MAXADDRSTR-1] = '\0';
		return buffer;
	}
	else
	{
		return "(bad IP addr)";
	}
}


// Returns ip_string if successful, NULL if not.  Copies into ip_string
char *u32_to_ip_string(U32 ip, char *ip_string)
{
	char *result;
	in_addr in;

	// Convert the IP address into a string
	in.s_addr = ip;
	result = inet_ntoa(in);

	// NULL indicates error in conversion
	if (result != NULL)
	{
		//the function signature needs to change to pass in the lengfth of first and last.
		strcpy(ip_string, result);	/*Flawfinder: ignore*/
		return ip_string;
	}
	else
	{
		return NULL;
	}
}


// Wrapper for inet_addr()
U32 ip_string_to_u32(const char* ip_string)
{
	return inet_addr(ip_string);
}


//////////////////////////////////////////////////////////////////////////////////////////
// Windows Versions
//////////////////////////////////////////////////////////////////////////////////////////

#if LL_WINDOWS

int tcp_handshake(S32 handle, char * dataout, int outlen, char * datain, int maxinlen)
{
	int result;
	result = send(handle, dataout, outlen, 0);
	if (result != outlen)
	{
		S32 err = WSAGetLastError();
		llwarns << "Error sending data to proxy control channel, number of bytes sent were " << result << " error code was " << err << llendl;
		return -1;
	}

	result = recv(handle, datain, maxinlen, 0);
	if (result != maxinlen)
	{
		S32 err = WSAGetLastError();
		llwarns << "Error receiving data from proxy control channel, number of bytes received were " << result << " error code was " << err << llendl;
		return -1;
	}

	return 0;
}

S32 tcp_open_channel(LLHost host)
{
	// Open a TCP channel
	// Jump through some hoops to ensure that if the request hosts is down
	// or not reachable connect() does not block

	S32 handle;
	handle = socket(AF_INET, SOCK_STREAM, 0);
	if (!handle)
	{
		llwarns << "Error opening TCP control socket, socket() returned " << handle << llendl;
		return -1;
	}

	struct sockaddr_in address;
	address.sin_port        = htons(host.getPort());
	address.sin_family      = AF_INET;
	address.sin_addr.s_addr = host.getAddress();

	// Non blocking 
	WSAEVENT hEvent=WSACreateEvent();
	WSAEventSelect(handle, hEvent, FD_CONNECT) ;
	connect(handle, (struct sockaddr*)&address, sizeof(address)) ;
	// Wait fot 5 seconds, if we can't get a TCP channel open in this
	// time frame then there is something badly wrong.
	WaitForSingleObject(hEvent, 1000*5); // 5 seconds time out

	WSANETWORKEVENTS netevents;
	WSAEnumNetworkEvents(handle,hEvent,&netevents);

	// Check the async event status to see if we connected
	if ((netevents.lNetworkEvents & FD_CONNECT) == FD_CONNECT)
	{
		if (netevents.iErrorCode[FD_CONNECT_BIT] != 0)
		{
			llwarns << "Unable to open TCP channel, WSA returned an error code of " << netevents.iErrorCode[FD_CONNECT_BIT] << llendl;
			WSACloseEvent(hEvent);
			return -1;
		}

		// Now we are connected disable non blocking
		// we don't need support an async interface as
		// currently our only consumer (socks5) will make one round
		// of packets then just hold the connection open
		WSAEventSelect(handle, hEvent, NULL) ;
		unsigned long NonBlock = 0;
		ioctlsocket(handle, FIONBIO, &NonBlock);

		return handle;
	}

	llwarns << "Unable to open TCP channel, Timeout is the host up?" << netevents.iErrorCode[FD_CONNECT_BIT] << llendl;
	return -1;
}

void tcp_close_channel(S32 handle)
{
	llinfos << "Closing TCP channel" << llendl;
	shutdown(handle, SD_BOTH);
	closesocket(handle);
}

S32 start_net(S32& socket_out, int& nPort) 
{			
	// Create socket, make non-blocking
    // Init WinSock 
	int nRet;
	int hSocket;

	int snd_size = SEND_BUFFER_SIZE;
	int rec_size = RECEIVE_BUFFER_SIZE;
	int buff_size = 4;
 
	// Initialize windows specific stuff
	if(WSAStartup(0x0202, &stWSAData))
	{
		S32 err = WSAGetLastError();
		WSACleanup();
		LL_WARNS("AppInit") << "Windows Sockets initialization failed, err " << err << LL_ENDL;
		return 1;
	}

	// Get a datagram socket
    hSocket = (int)socket(AF_INET, SOCK_DGRAM, 0);
    if (hSocket == INVALID_SOCKET)
	{
		S32 err = WSAGetLastError();
		WSACleanup();
		LL_WARNS("AppInit") << "socket() failed, err " << err << LL_ENDL;
		return 2;
	}

	// Name the socket (assign the local port number to receive on)
	stLclAddr.sin_family      = AF_INET;
	stLclAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	stLclAddr.sin_port        = htons(nPort);

	S32 attempt_port = nPort;
	LL_DEBUGS("AppInit") << "attempting to connect on port " << attempt_port << LL_ENDL;
	nRet = bind(hSocket, (struct sockaddr*) &stLclAddr, sizeof(stLclAddr));

	if (nRet == SOCKET_ERROR)
	{
		// If we got an address in use error...
		if (WSAGetLastError() == WSAEADDRINUSE)
		{
			// Try all ports from PORT_DISCOVERY_RANGE_MIN to PORT_DISCOVERY_RANGE_MAX
			for(attempt_port = PORT_DISCOVERY_RANGE_MIN;
				attempt_port <= PORT_DISCOVERY_RANGE_MAX;
				attempt_port++)
			{
				stLclAddr.sin_port = htons(attempt_port);
				LL_DEBUGS("AppInit") << "trying port " << attempt_port << LL_ENDL;
				nRet = bind(hSocket, (struct sockaddr*) &stLclAddr, sizeof(stLclAddr));

				if (!(nRet == SOCKET_ERROR && 
					WSAGetLastError() == WSAEADDRINUSE))
				{
					break;
				}
			}

			if (nRet == SOCKET_ERROR)
			{
				LL_WARNS("AppInit") << "startNet() : Couldn't find available network port." << LL_ENDL;
				// Fail gracefully here in release
				return 3;
			}
		}
		else
		// Some other socket error
		{
			LL_WARNS("AppInit") << llformat("bind() port: %d failed, Err: %d\n", nPort, WSAGetLastError()) << LL_ENDL;
			// Fail gracefully in release.
			return 4;
		}
	}

	sockaddr_in socket_address;
	S32 socket_address_size = sizeof(socket_address);
	getsockname(hSocket, (SOCKADDR*) &socket_address, &socket_address_size);
	attempt_port = ntohs(socket_address.sin_port);

	LL_INFOS("AppInit") << "connected on port " << attempt_port << LL_ENDL;
	nPort = attempt_port;
	
	// Set socket to be non-blocking
	unsigned long argp = 1;
	nRet = ioctlsocket (hSocket, FIONBIO, &argp);
	if (nRet == SOCKET_ERROR) 
	{
		printf("Failed to set socket non-blocking, Err: %d\n", 
		WSAGetLastError());
	}

	// set a large receive buffer
	nRet = setsockopt(hSocket, SOL_SOCKET, SO_RCVBUF, (char *)&rec_size, buff_size);
	if (nRet)
	{
		LL_INFOS("AppInit") << "Can't set receive buffer size!" << LL_ENDL;
	}

	nRet = setsockopt(hSocket, SOL_SOCKET, SO_SNDBUF, (char *)&snd_size, buff_size);
	if (nRet)
	{
		LL_INFOS("AppInit") << "Can't set send buffer size!" << LL_ENDL;
	}

	getsockopt(hSocket, SOL_SOCKET, SO_RCVBUF, (char *)&rec_size, &buff_size);
	getsockopt(hSocket, SOL_SOCKET, SO_SNDBUF, (char *)&snd_size, &buff_size);

	LL_DEBUGS("AppInit") << "startNet - receive buffer size : " << rec_size << LL_ENDL;
	LL_DEBUGS("AppInit") << "startNet - send buffer size    : " << snd_size << LL_ENDL;

	//  Setup a destination address
	char achMCAddr[MAXADDRSTR] = " ";	/* Flawfinder: ignore */ 
	stDstAddr.sin_family =      AF_INET;
    stDstAddr.sin_addr.s_addr = inet_addr(achMCAddr);
    stDstAddr.sin_port =        htons(nPort);

	socket_out = hSocket;
	return 0;
}

void end_net(S32& socket_out)
{
	if (socket_out >= 0)
	{
		shutdown(socket_out, SD_BOTH);
		closesocket(socket_out);
	}
	WSACleanup();
}

S32 receive_packet(int hSocket, char * receiveBuffer)
{
	//  Receives data asynchronously from the socket set by initNet().
	//  Returns the number of bytes received into dataReceived, or zero
	//  if there is no data received.
	int nRet;
	int addr_size = sizeof(struct sockaddr_in);

	nRet = recvfrom(hSocket, receiveBuffer, NET_BUFFER_SIZE, 0, (struct sockaddr*)&stSrcAddr, &addr_size);
	if (nRet == SOCKET_ERROR ) 
	{
		if (WSAEWOULDBLOCK == WSAGetLastError())
			return 0;
		if (WSAECONNRESET == WSAGetLastError())
			return 0;
		llinfos << "receivePacket() failed, Error: " << WSAGetLastError() << llendl;
	}
	
	return nRet;
}

// Returns TRUE on success.
BOOL send_packet(int hSocket, const char *sendBuffer, int size, U32 recipient, int nPort)
{

	//  Sends a packet to the address set in initNet
	//  
	int nRet = 0;
	U32 last_error = 0;

	stDstAddr.sin_addr.s_addr = recipient;
	stDstAddr.sin_port = htons(nPort);
	do
	{
		nRet = sendto(hSocket, sendBuffer, size, 0, (struct sockaddr*)&stDstAddr, sizeof(stDstAddr));					

		if (nRet == SOCKET_ERROR ) 
		{
			last_error = WSAGetLastError();
			if (last_error != WSAEWOULDBLOCK)
			{
				// WSAECONNRESET - I think this is caused by an ICMP "connection refused"
				// message being sent back from a Linux box...  I'm not finding helpful
				// documentation or web pages on this.  The question is whether the packet
				// actually got sent or not.  Based on the structure of this code, I would
				// assume it is.  JNC 2002.01.18
				if (WSAECONNRESET == WSAGetLastError())
				{
					return TRUE;
				}
				llinfos << "sendto() failed to " << u32_to_ip_string(recipient) << ":" << nPort 
					<< ", Error " << last_error << llendl;
			}
		}
	} while (  (nRet == SOCKET_ERROR)
			 &&(last_error == WSAEWOULDBLOCK));

	return (nRet != SOCKET_ERROR);
}

//////////////////////////////////////////////////////////////////////////////////////////
// Linux Versions
//////////////////////////////////////////////////////////////////////////////////////////

#else


int tcp_handshake(S32 handle, char * dataout, int outlen, char * datain, int maxinlen)
{
	if (send(handle, dataout, outlen, 0) != outlen)
	{
		llwarns << "Error sending data to proxy control channel" << llendl;
		return -1;
	}

	if (recv(handle, datain, maxinlen, 0) != maxinlen)
	{
		llwarns << "Error receiving data to proxy control channel" << llendl;		
		return -1;
	}

	return 0;
}

S32 tcp_open_channel(LLHost host)
{
	S32 handle;
	handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (!handle)
	{
		llwarns << "Error opening TCP control socket, socket() returned " << handle << llendl;
		return -1;
	}

	struct sockaddr_in address;
	address.sin_port        = htons(host.getPort());
	address.sin_family      = AF_INET;
	address.sin_addr.s_addr = host.getAddress();

	// Set the socket to non blocking for the connect()
	int flags = fcntl(handle, F_GETFL, 0);
	fcntl(handle, F_SETFL, flags | O_NONBLOCK);

	S32 error = connect(handle, (sockaddr*)&address, sizeof(address));
	if (error && (errno != EINPROGRESS))
	{
			llwarns << "Unable to open TCP channel, error code: " << errno << llendl;
			return -1;
	}

	struct timeval timeout;
	timeout.tv_sec  = 5; // Maximum time to wait for the connect() to complete
	timeout.tv_usec = 0;
    fd_set fds;
	FD_ZERO(&fds);
	FD_SET(handle, &fds);

	// See if we have connectde or time out after 5 seconds
	U32 rc = select(sizeof(fds)*8, NULL, &fds, NULL, &timeout);	
	
	if (rc != 1) // we require exactly one descriptor to be set
	{
			llwarns << "Unable to open TCP channel" << llendl;
			return -1;
	}

	// Return the socket to blocking operations
	fcntl(handle, F_SETFL, flags);

	return handle;
}

void tcp_close_channel(S32 handle)
{
		close(handle);
}

//  Create socket, make non-blocking
S32 start_net(S32& socket_out, int& nPort)
{
	int hSocket, nRet;
	int snd_size = SEND_BUFFER_SIZE;
	int rec_size = RECEIVE_BUFFER_SIZE;

	socklen_t buff_size = 4;
    
	//  Create socket
    hSocket = socket(AF_INET, SOCK_DGRAM, 0);
    if (hSocket < 0)
	{
		llwarns << "socket() failed" << llendl;
		return 1;
	}

	if (NET_USE_OS_ASSIGNED_PORT == nPort)
	{
		// Although bind is not required it will tell us which port we were
		// assigned to.
		stLclAddr.sin_family      = AF_INET;
		stLclAddr.sin_addr.s_addr = htonl(INADDR_ANY);
		stLclAddr.sin_port        = htons(0);
		llinfos << "attempting to connect on OS assigned port" << llendl;
		nRet = bind(hSocket, (struct sockaddr*) &stLclAddr, sizeof(stLclAddr));
		if (nRet < 0)
		{
			llwarns << "Failed to bind on an OS assigned port error: "
					<< nRet << llendl;
		}
		else
		{
			sockaddr_in socket_info;
			socklen_t len = sizeof(sockaddr_in);
			int err = getsockname(hSocket, (sockaddr*)&socket_info, &len);
			llinfos << "Get socket returned: " << err << " length " << len << llendl;
			nPort = ntohs(socket_info.sin_port);
			llinfos << "Assigned port: " << nPort << llendl;
			
		}
	}
	else
	{
	    // Name the socket (assign the local port number to receive on)
		stLclAddr.sin_family      = AF_INET;
		stLclAddr.sin_addr.s_addr = htonl(INADDR_ANY);
		stLclAddr.sin_port        = htons(nPort);
		U32 attempt_port = nPort;
		llinfos << "attempting to connect on port " << attempt_port << llendl;

		nRet = bind(hSocket, (struct sockaddr*) &stLclAddr, sizeof(stLclAddr));
		if (nRet < 0)
		{
			// If we got an address in use error...
			if (errno == EADDRINUSE)
			{
				// Try all ports from PORT_DISCOVERY_RANGE_MIN to PORT_DISCOVERY_RANGE_MAX
				for(attempt_port = PORT_DISCOVERY_RANGE_MIN;
					attempt_port <= PORT_DISCOVERY_RANGE_MAX;
					attempt_port++)
				{
					stLclAddr.sin_port = htons(attempt_port);
					llinfos << "trying port " << attempt_port << llendl;
					nRet = bind(hSocket, (struct sockaddr*) &stLclAddr, sizeof(stLclAddr));
					if (!((nRet < 0) && (errno == EADDRINUSE)))
					{
						break;
					}
				}
				if (nRet < 0)
				{
					llwarns << "startNet() : Couldn't find available network port." << llendl;
					// Fail gracefully in release.
					return 3;
				}
			}
			// Some other socket error
			else
			{
				llwarns << llformat ("bind() port: %d failed, Err: %s\n", nPort, strerror(errno)) << llendl;
				// Fail gracefully in release.
				return 4;
			}
		}
		llinfos << "connected on port " << attempt_port << llendl;
		nPort = attempt_port;
	}
	// Set socket to be non-blocking
 	fcntl(hSocket, F_SETFL, O_NONBLOCK);
	// set a large receive buffer
	nRet = setsockopt(hSocket, SOL_SOCKET, SO_RCVBUF, (char *)&rec_size, buff_size);
	if (nRet)
	{
		llinfos << "Can't set receive size!" << llendl;
	}
	nRet = setsockopt(hSocket, SOL_SOCKET, SO_SNDBUF, (char *)&snd_size, buff_size);
	if (nRet)
	{
		llinfos << "Can't set send size!" << llendl;
	}
	getsockopt(hSocket, SOL_SOCKET, SO_RCVBUF, (char *)&rec_size, &buff_size);
	getsockopt(hSocket, SOL_SOCKET, SO_SNDBUF, (char *)&snd_size, &buff_size);

	llinfos << "startNet - receive buffer size : " << rec_size << llendl;
	llinfos << "startNet - send buffer size    : " << snd_size << llendl;

#if LL_LINUX
	// Turn on recipient address tracking
	{
		int use_pktinfo = 1;
		if( setsockopt( hSocket, SOL_IP, IP_PKTINFO, &use_pktinfo, sizeof(use_pktinfo) ) == -1 )
		{
			llwarns << "No IP_PKTINFO available" << llendl;
		}
		else
		{
			llinfos << "IP_PKKTINFO enabled" << llendl;
		}
	}
#endif

	//  Setup a destination address
	char achMCAddr[MAXADDRSTR] = "127.0.0.1";	/* Flawfinder: ignore */ 
	stDstAddr.sin_family =      AF_INET;
        stDstAddr.sin_addr.s_addr = inet_addr(achMCAddr);
        stDstAddr.sin_port =        htons(nPort);

	socket_out = hSocket;
	return 0;
}

void end_net(S32& socket_out)
{
	if (socket_out >= 0)
	{
		close(socket_out);
	}
}

#if LL_LINUX
static int recvfrom_destip( int socket, void *buf, int len, struct sockaddr *from, socklen_t *fromlen, U32 *dstip )
{
	int size;
	struct iovec iov[1];
	char cmsg[CMSG_SPACE(sizeof(struct in_pktinfo))];
	struct cmsghdr *cmsgptr;
	struct msghdr msg = {0};

	iov[0].iov_base = buf;
	iov[0].iov_len = len;

	memset( &msg, 0, sizeof msg );
	msg.msg_name = from;
	msg.msg_namelen = *fromlen;
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;
	msg.msg_control = &cmsg;
	msg.msg_controllen = sizeof(cmsg);

	size = recvmsg( socket, &msg, 0 );

	if( size == -1 )
	{
		return -1;
	}

	for( cmsgptr = CMSG_FIRSTHDR(&msg); cmsgptr != NULL; cmsgptr = CMSG_NXTHDR( &msg, cmsgptr ) )
	{
		if( cmsgptr->cmsg_level == SOL_IP && cmsgptr->cmsg_type == IP_PKTINFO )
		{
			in_pktinfo *pktinfo = (in_pktinfo *)CMSG_DATA(cmsgptr);
			if( pktinfo )
			{
				// Two choices. routed and specified. ipi_addr is routed, ipi_spec_dst is
				// routed. We should stay with specified until we go to multiple
				// interfaces
				*dstip = pktinfo->ipi_spec_dst.s_addr;
			}
		}
	}

	return size;
}
#endif

int receive_packet(int hSocket, char * receiveBuffer)
{
	//  Receives data asynchronously from the socket set by initNet().
	//  Returns the number of bytes received into dataReceived, or zero
	//  if there is no data received.
	// or -1 if an error occured!
	int nRet;
	socklen_t addr_size = sizeof(struct sockaddr_in);

	gsnReceivingIFAddr = INVALID_HOST_IP_ADDRESS;

#if LL_LINUX
	nRet = recvfrom_destip(hSocket, receiveBuffer, NET_BUFFER_SIZE, (struct sockaddr*)&stSrcAddr, &addr_size, &gsnReceivingIFAddr);
#else	
	int recv_flags = 0;
	nRet = recvfrom(hSocket, receiveBuffer, NET_BUFFER_SIZE, recv_flags, (struct sockaddr*)&stSrcAddr, &addr_size);
#endif

	if (nRet == -1)
	{
		// To maintain consistency with the Windows implementation, return a zero for size on error.
		return 0;
	}

	// Uncomment for testing if/when implementing for Mac or Windows:
	// llinfos << "Received datagram to in addr " << u32_to_ip_string(get_receiving_interface_ip()) << llendl;

	return nRet;
}

BOOL send_packet(int hSocket, const char * sendBuffer, int size, U32 recipient, int nPort)
{
	int		ret;
	BOOL	success;
	BOOL	resend;
	S32		send_attempts = 0;

	stDstAddr.sin_addr.s_addr = recipient;
	stDstAddr.sin_port = htons(nPort);

	do
	{
		ret = sendto(hSocket, sendBuffer, size, 0,	(struct sockaddr*)&stDstAddr, sizeof(stDstAddr));
		send_attempts++;

		if (ret >= 0)
		{
			// successful send
			success = TRUE;
			resend = FALSE;
		}
		else
		{
			// send failed, check to see if we should resend
			success = FALSE;

			if (errno == EAGAIN)
			{
				// say nothing, just repeat send
				llinfos << "sendto() reported buffer full, resending (attempt " << send_attempts << ")" << llendl;
				llinfos << inet_ntoa(stDstAddr.sin_addr) << ":" << nPort << llendl;
				resend = TRUE;
			}
			else if (errno == ECONNREFUSED)
			{
				// response to ICMP connection refused message on earlier send
				llinfos << "sendto() reported connection refused, resending (attempt " << send_attempts << ")" << llendl;
				llinfos << inet_ntoa(stDstAddr.sin_addr) << ":" << nPort << llendl;
				resend = TRUE;
			}
			else
			{
				// some other error
				llinfos << "sendto() failed: " << errno << ", " << strerror(errno) << llendl;
				llinfos << inet_ntoa(stDstAddr.sin_addr) << ":" << nPort << llendl;
				resend = FALSE;
			}
		}
	}
	while ( resend && send_attempts < 3);

	if (send_attempts >= 3)
	{
		llinfos << "sendPacket() bailed out of send!" << llendl;
		return FALSE;
	}

	return success;
}

#endif

//EOF
