/*
 * teredo.c - Common Teredo helper functions
 * $Id$
 *
 * See "Teredo: Tunneling IPv6 over UDP through NATs"
 * for more information
 */

/***********************************************************************
 *  Copyright (C) 2004-2005 Remi Denis-Courmont.                       *
 *  This program is free software; you can redistribute and/or modify  *
 *  it under the terms of the GNU General Public License as published  *
 *  by the Free Software Foundation; version 2 of the license.         *
 *                                                                     *
 *  This program is distributed in the hope that it will be useful,    *
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of     *
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.               *
 *  See the GNU General Public License for more details.               *
 *                                                                     *
 *  You should have received a copy of the GNU General Public License  *
 *  along with this program; if not, you can get it from:              *
 *  http://www.gnu.org/copyleft/gpl.html                               *
 ***********************************************************************/

#if HAVE_CONFIG_H
# include <config.h>
#endif

#include <string.h> // memcpy()

#if HAVE_STDINT_H
# include <stdint.h> /* Mac OS X needs that */
#endif
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/ip6.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <errno.h>

#ifndef SOL_IP
# define SOL_IP IPPROTO_IP
#endif

#include "teredo.h"
#include "teredo-udp.h"

/*
 * Teredo addresses
 */
const struct in6_addr teredo_restrict =
	{ { { 0xfe, 0x80, 0, 0, 0, 0, 0, 0,
		    0, 0, 'T', 'E', 'R', 'E', 'D', 'O' } } };
const struct in6_addr teredo_cone =
	{ { { 0xfe, 0x80, 0, 0, 0, 0, 0, 0,
		    0x80, 0, 'T', 'E', 'R', 'E', 'D', 'O' } } };

/*
 * Opens a Teredo UDP/IPv4 socket.
 */
int teredo_socket (uint32_t bind_ip, uint16_t port)
{
	struct sockaddr_in myaddr = { };
	int fd, flags;

	myaddr.sin_family = AF_INET;
	myaddr.sin_port = port;
	myaddr.sin_addr.s_addr = bind_ip;
#ifdef HAVE_SA_LEN
	myaddr.sin_len = sizeof (myaddr);
#endif

	fd = socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (fd == -1)
		return -1; // failure

	flags = fcntl (fd, F_GETFL, 0);
	if (flags != -1)
		fcntl (fd, F_SETFL, O_NONBLOCK | flags);

	if (bind (fd, (struct sockaddr *)&myaddr, sizeof (myaddr)))
		return -1;

	flags = 1;
	setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &flags, sizeof (flags));
#ifdef IP_PMTUDISC_DONT
	/* 
	 * This tells the (Linux) kernel not to set the Don't Fragment flags
	 * on UDP packets we send. This is recommended by the Teredo
	 * specifiation.
	 */
	flags = IP_PMTUDISC_DONT;
	setsockopt (fd, SOL_IP, IP_MTU_DISCOVER, &flags, sizeof (flags));
#endif
	/*
	 * Teredo multicast packets always have a TTL of 1.
	 */
	setsockopt (fd, SOL_IP, IP_MULTICAST_TTL, &flags, sizeof (flags));
	return fd;
}


int teredo_sendv (int fd, const struct iovec *iov, size_t count,
                  uint32_t dest_ip, uint16_t dest_port)
{
	struct msghdr msg;
	struct sockaddr_in addr = { };
	int res, tries;

	addr.sin_family = AF_INET;
	addr.sin_port = dest_port;
	addr.sin_addr.s_addr = dest_ip;
#ifdef HAVE_SA_LEN
	addr.sin_len = sizeof (addr);
#endif

	msg.msg_name = &addr;
	msg.msg_namelen = sizeof (addr);
	msg.msg_iov = (struct iovec *)iov;
	msg.msg_iovlen = count;
	msg.msg_control = NULL;
	msg.msg_controllen = 0;
	msg.msg_flags = 0;

	for (tries = 0; tries < 10; tries++)
	{
		res = sendmsg (fd, &msg, 0);
		if (res != -1)
			return res;
		/*
		 * NOTE:
		 * We must ignore ICMP errors returned by sendto() because they are
		 * asynchronous, so that in most case they refer to a packet which was
		 * sent earlier already, most likely to another destination.
		 * That means we also ignore EHOSTUNREACH when it is generated by the
		 * kernel routing table (meaning we are not attached to the network);
		 * while it would have been a good idea to handle that case properly,
		 * it's never been implemented in Miredo, and it turns out the ICMP
		 * errors issue prevents any future implementation.
		 *
		 * NOTE 2:
		 * To prevent an infinite loop in case of a really unreachable
		 * destination, we must have a limit on the number of sendto()
		 * attempts.
		 */
		switch (errno)
		{
			/*case EMSGSIZE:*/ /* ICMP fragmentation needed *
			 * given we don't ensure that the length is below 65507, we
			 * must not ignore that error */
			case ENETUNREACH: /* ICMP address unreachable */
			case EHOSTUNREACH: /* ICMP destination unreachable */
			case ENOPROTOOPT: /* ICMP protocol unreachable */
			case ECONNREFUSED: /* ICMP port unreachable */
			case EOPNOTSUPP: /* ICMP source route failed
							- should not happen */
			case EHOSTDOWN: /* ICMP host unknown */
			case ENONET: /* ICMP host isolated */
				continue;
	
			default:
				return -1; /* hard error */
		}
	}

	return -1;
}


int teredo_send (int fd, const void *packet, size_t plen,
                 uint32_t dest_ip, uint16_t dest_port)
{
	struct iovec iov = { (void *)packet, plen };
	return teredo_sendv (fd, &iov, 1, dest_ip, dest_port);
}


/**
 * Receives and parses a Teredo packet from a socket.
 * Blocks if the socket is blocking, don't block if not.
 *
 * @param fd socket file descriptor
 * @param p teredo_packet receive buffer
 *
 * @return 0 on success, -1 in error.
 * Errors might be caused by :
 *  - lower level network I/O,
 *  - malformatted packets,
 *  - no data pending while using a non-blocking socket.
 */
int teredo_recv (int fd, struct teredo_packet *p)
{
	uint8_t *ptr;
	int length;

	// Receive a UDP packet
	{
		struct sockaddr_in ad;
		socklen_t alen = sizeof (ad);

		length = recvfrom (fd, p->buf, sizeof (p->buf), 0,
						   (struct sockaddr *)&ad, &alen);

		if (length < 0)
			return -1;

		p->source_ipv4 = ad.sin_addr.s_addr;
		p->source_port = ad.sin_port;
	}

	// Check type of Teredo header:
	ptr = p->buf;
	p->orig = NULL;
	p->nonce = NULL;

	// Parse Teredo headers
	if (length < 40)
		return -1; // too small

	// Teredo Authentication header
	if ((ptr[0] == 0) && (ptr[1] == teredo_auth_hdr))
	{
		uint8_t id_len, au_len;

		ptr += 2;
		length -= 13;
		if (length < 0)
			return -1; // too small

		/* ID and Auth */
		id_len = *ptr++;
		au_len = *ptr++;

		length -= id_len + au_len;
		if (length < 0)
			return -1;

		/* TODO: secure qualification */
		ptr += id_len + au_len;

		/* Nonce + confirmation byte */
		p->nonce = ptr;
		ptr += 9;
	}

	/* Teredo Origin Indication */
	if ((ptr[0] == 0) && (ptr[1] == teredo_orig_ind))
	{
		length -= sizeof (p->orig_buf);
		if (length < 0)
			return -1; /* too small */

		memcpy (&p->orig_buf, ptr, sizeof (p->orig_buf));
		p->orig = &p->orig_buf;
		ptr += sizeof (p->orig_buf);
	}

	/* length <= 65507 = sizeof(buf) */
	p->ip6_len = length;
	p->ip6 = ptr;

	return 0;
}


/**
 * Waits for, receives and parses a Teredo packet from a socket.
 *
 * @param fd socket file descriptor
 * @param p teredo_packet receive buffer
 *
 * @return 0 on success, -1 in error.
 * Errors might be caused by :
 *  - lower level network I/O,
 *  - malformatted packets,
 *  - a race condition if two thread are waiting on the same
 *    non-blocking socket for receiving.
 */
int teredo_wait_recv (int fd, struct teredo_packet *p)
{
	fd_set readset;
	int val;

	FD_ZERO (&readset);
	FD_SET (fd, &readset);
	val = select (fd + 1, &readset, NULL, NULL, NULL);
	return (val == 1) ? teredo_recv (fd, p) : -1;
}
