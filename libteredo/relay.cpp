/*
 * relay.cpp - Teredo relay peers list definition
 * $Id$
 *
 * See "Teredo: Tunneling IPv6 over UDP through NATs"
 * for more information
 */

/***********************************************************************
 *  Copyright (C) 2004 Remi Denis-Courmont.                            *
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

#include <string.h>
#include <time.h> // TODO: use gettimeofday
#include <inttypes.h>

#include <sys/types.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/ip6.h> // struct ip6_hdr
#include <syslog.h>

#ifdef USE_OPENSSL
# include <openssl/rand.h>
# include <openssl/err.h>
#endif

#include "teredo.h"
#include <v4global.h> // is_ipv4_global_unicast()
#include "teredo-udp.h"

#include "packets.h"
#include "relay.h"

#define TEREDO_TIMEOUT 30 // seconds


#define EXPIRED( date, now ) ((((unsigned)now) - (unsigned)date) > 30)
#define ENTRY_EXPIRED( peer, now ) (peer->flags.flags.replied \
					? EXPIRED (peer->last_rx, now) \
					: EXPIRED (peer->last_xmit, now))

// is_valid_teredo_prefix (PREFIX_UNSET) MUST return false
# define PREFIX_UNSET 0xffffffff

struct __TeredoRelay_peer
{
	struct __TeredoRelay_peer *next;

	struct in6_addr addr;
	uint32_t mapped_addr;
	uint16_t mapped_port;
	union
	{
		struct
		{
			unsigned trusted:1;
			unsigned replied:1;
			unsigned bubbles:2;
			unsigned nonce:1; // mapped_* unset, nonce set
		} flags;
		uint16_t all_flags;
	} flags;
	// TODO: nonce and mapped_* could be union-ed
	uint8_t nonce[8]; /* only for client toward non-client */
	time_t last_rx;
	time_t last_xmit;

	uint8_t *queue;
	size_t queuelen;

	// TODO: implement incoming queue
};

#define PROBE_CONE	1
#define PROBE_RESTRICT	2
#define PROBE_SYMMETRIC	3

#define QUALIFIED	0


TeredoRelay::TeredoRelay (uint32_t pref, uint16_t port, bool cone)
	: head (NULL)
{
	addr.teredo.prefix = pref;
	addr.teredo.server_ip = 0;
	addr.teredo.flags = cone ? htons (TEREDO_FLAG_CONE) : 0;
	addr.teredo.client_ip = 0;
	addr.teredo.client_port = 0;
	probe.state = QUALIFIED;

	sock.ListenPort (port);
}


TeredoRelay::TeredoRelay (uint32_t server_ip, uint16_t port)
	: head (NULL)
{
	addr.teredo.prefix = PREFIX_UNSET;
	addr.teredo.server_ip = server_ip;
	addr.teredo.flags = htons (TEREDO_FLAG_CONE);
	addr.teredo.client_ip = 0;
	addr.teredo.client_port = 0;

	memset (probe.nonce, 0, 8);
#ifdef USE_OPENSSL
	if (!RAND_bytes (probe.nonce, 8))
	{
		char buf[120];

		syslog (LOG_ERR, _("Lack of entropy: %s"),
			ERR_error_string (ERR_get_error (), buf));
	}
	else
#endif
	if (sock.ListenPort (port) == 0)
	{
		probe.state = PROBE_CONE;
		probe.count = 0;
		gettimeofday (&probe.next, NULL);
		Process ();
	}
}


/* Releases peers list entries */
TeredoRelay::~TeredoRelay (void)
{
	struct __TeredoRelay_peer *p = head;

	while (p != NULL)
	{
		struct __TeredoRelay_peer *buf = p->next;
		if (p->queue != NULL)
			delete p->queue;
		delete p;
		p = buf;
	}
}


int TeredoRelay::NotifyUp (const struct in6_addr *addr)
{
	return 0;
}


int TeredoRelay::NotifyDown (void)
{
	return 0;
}


/* 
 * Allocates a peer entry. It is up to the caller to fill informations
 * correctly.
 *
 * FIXME: number of entry should be bound
 * FIXME: move to another file
 */
struct __TeredoRelay_peer *TeredoRelay::AllocatePeer (void)
{
	time_t now;
	time (&now);

	/* Tries to recycle a timed-out peer entry */
	for (struct __TeredoRelay_peer *p = head; p != NULL; p = p->next)
		if (ENTRY_EXPIRED (p, now))
		{
			if (p->queue != NULL)
				delete p->queue;
			return p;
		}

	/* Otherwise allocates a new peer entry */
	struct __TeredoRelay_peer *p;
	try
	{
		p = new struct __TeredoRelay_peer;
	}
	catch (...)
	{
		return NULL;
	}

	/* Puts new entry at the head of the list */
	p->next = head;
	head = p;
	return p;
}


/*
 * Returns a pointer to the first peer entry matching <addr>,
 * or NULL if none were found.
 */
struct __TeredoRelay_peer *TeredoRelay::FindPeer (const struct in6_addr *addr)
{
	time_t now;

	time(&now);

	for (struct __TeredoRelay_peer *p = head; p != NULL; p = p->next)
		if (memcmp (&p->addr, addr, sizeof (struct in6_addr)) == 0)
			if (!ENTRY_EXPIRED (p, now))
				return p; // found!
	
	return NULL;
}




/*
 * Returs true if the packet whose header is passed as a parameter looks
 * like a Teredo bubble.
 */
inline bool IsBubble (const struct ip6_hdr *hdr)
{
	return (hdr->ip6_plen == 0) && (hdr->ip6_nxt == IPPROTO_NONE);
}


/*
 * Handles a packet coming from the IPv6 Internet, toward a Teredo node
 * (as specified per paragraph 5.4.1). That's what the specification calls
 * "Packet transmission".
 * Returns 0 on success, -1 on error.
 */
int TeredoRelay::SendPacket (const void *packet, size_t length)
{
	/* Makes sure we are qualified properly */
	if (!IsRunning ())
		return -1; // TODO: send ICMPv6 error?

	struct ip6_hdr ip6;
	if ((length < sizeof (ip6)) || (length > 65507))
		return 0;

	memcpy (&ip6, packet, sizeof (ip6_hdr));

	// Sanity check (should we trust the kernel?):
	// It's no use emitting such a broken packet because the other side
	// will drop it anyway.
	if (((ip6.ip6_vfc >> 4) != 6)
	 || ((sizeof (ip6) + ntohs (ip6.ip6_plen)) != length))
		return 0; // invalid IPv6 packet

	const union teredo_addr *dst = (union teredo_addr *)&ip6.ip6_dst,
				*src = (union teredo_addr *)&ip6.ip6_src;

	if (dst->teredo.prefix != GetPrefix ()
	 && src->teredo.prefix != GetPrefix ())
		/*
		 * Routing packets not from a Teredo client,
		 * neither toward a Teredo client is NOT allowed through a
		 * Teredo tunnel. The Teredo server will reject the packet.
		 *
		 * We also drop link-local unicast and multicast packets as
		 * they can't be routed through Teredo properly.
		 */
		// TODO: maybe, send a ICMP adminstrative error
		return 0;


	struct __TeredoRelay_peer *p = FindPeer (&ip6.ip6_dst);
#ifdef DEBUG
	{
		struct in_addr a;
		a.s_addr = ~addr.teredo.client_ip;
		syslog (LOG_DEBUG, "DEBUG: packet for %s:%hu\n", inet_ntoa (a),
				~addr.teredo.client_port);
	}
#endif

	if (p != NULL)
	{
		/* Case 1 (paragraphs 5.2.4 & 5.4.1): trusted peer */
		if (p->flags.flags.trusted)
		{
			/* Already known -valid- peer */
			time (&p->last_xmit);
			return sock.SendPacket (packet, length,
						p->mapped_addr,
						p->mapped_port);
		}
	}
	
	/* Unknown or untrusted peer */
	if (dst->teredo.prefix != GetPrefix ())
	{
		/* Unkown or untrusted non-Teredo node */

		/*
		 * If we are not a qualified client, ie. we have no server
		 * IPv4 address to contact for direct IPv6 connectivity, we
		 * cannot route packets toward non-Teredo IPv6 addresses, and
		 * we are not allowed to do it by the specification either.
		 *
		 * TODO:
		 * The specification mandates silently ignoring such
		 * packets. However, this only happens in case of
		 * misconfiguration, so I believe it could be better to
		 * notify the user. An alternative might be to send an
		 * ICMPv6 error back to the kernel.
		 */
		if (IsRelay ())
			return 0;
			
		/* Client case 2: direct IPv6 connectivity test */
		// TODO: avoid code duplication
		if (p == NULL)
		{
			p = AllocatePeer ();
			if (p == NULL)
				return -1; // memory error
			memcpy (&p->addr, &ip6.ip6_dst, sizeof (struct in6_addr));
			p->mapped_port = 0;
			p->mapped_addr = 0;
			p->flags.all_flags = 0;
			time (&p->last_xmit);
			p->queue = NULL;
		}


		// FIXME: queue packet
		// FIXME: re-send echo request if no response
		if (!p->flags.flags.nonce)
		{
			p->flags.flags.nonce = 1;
			memset (p->nonce, 0, 8);
#ifdef USE_OPENSSL
			if (!RAND_pseudo_bytes (p->nonce, 8))
			{
				char buf[120];

				syslog (LOG_WARNING,
					_("Possibly predictable nonce: %s"),
					ERR_error_string (ERR_get_error (),
								buf));
			}
#endif
		}
		return SendPing (sock, &addr, &dst->ip6, p->nonce);
	}

	/* Unknown or untrusted Teredo client */

	// Ignores Teredo clients with incorrect server IPv4
	if (!is_ipv4_global_unicast (IN6_TEREDO_SERVER (&ip6.ip6_dst))
	 || (IN6_TEREDO_SERVER (&ip6.ip6_dst) == 0))
		return 0;
		
	/* Client case 3: TODO: implement local discovery */

	if (p == NULL)
	{
		/* Unknown Teredo clients */

		// Creates a new entry
		p = AllocatePeer ();
		if (p == NULL)
			return -1; // insufficient memory
		memcpy (&p->addr, &ip6.ip6_dst, sizeof (struct in6_addr));
		p->mapped_port = IN6_TEREDO_PORT (dst);
		p->mapped_addr = IN6_TEREDO_IPV4 (dst);
		p->flags.all_flags = 0;
		time (&p->last_xmit);
		p->queue = NULL;
	
		/* Client case 4 & relay case 2: new cone peer */
		if (IN6_IS_TEREDO_ADDR_CONE (&ip6.ip6_dst))
		{
			p->flags.flags.trusted = 1;
			return sock.SendPacket (packet, length,
						p->mapped_addr,
						p->mapped_port);
		}
	}

	/* Client case 5 & relay case 3: untrusted non-cone peer */
	/* TODO: enqueue more than one packet 
	 * (and do this in separate functions) */
	if (p->queue == NULL)
	{
		try
		{
			p->queue = new uint8_t[length];
		}
		catch (...)
		{
			p->queue = NULL; // memory error
		}

		memcpy (p->queue, packet, length);
		p->queuelen = length;
	}
#ifdef DEBUG
	else
		syslog (LOG_DEBUG, _("FIXME: packet not queued\n"));
#endif

	// Sends no more than one bubble every 2 seconds,
	// and 3 bubbles every 30 secondes
	if (p->flags.flags.bubbles < 3)
	{
		time_t now;
		time (&now);

		if (!p->flags.flags.bubbles || ((now - p->last_xmit) >= 2))
		{
			p->flags.flags.bubbles ++;
			memcpy (&p->last_xmit, &now, sizeof (p->last_xmit));

			/*
			 * Open the return path if we are behind a
			 * restricted NAT.
			 */
			if (!IsCone ()
			 && SendBubble (sock, &ip6.ip6_dst, IsCone (), false))
				return -1;

			return SendBubble (sock, &ip6.ip6_dst, IsCone ());
		}
	}

	// Too many bubbles already sent
	return 0;
}


/*
 * Handles a packet coming from the Teredo tunnel
 * (as specified per paragraph 5.4.2). That's called "Packet reception".
 * Returns 0 on success, -1 on error.
 */
// seconds to wait before considering that we've lost contact with the server
#define SERVER_LOSS_DELAY 35
#define SERVER_PING_DELAY 30
#define RESTART_DELAY 300
#define PROBE_DELAY 4

int TeredoRelay::ReceivePacket (const fd_set *readset)
{
	TeredoPacket packet;

	if (sock.ReceivePacket (readset, packet))
		return -1;

	size_t length;
	const struct ip6_hdr *buf = packet.GetIPv6Header (length);
	struct ip6_hdr ip6;

	// Checks packet
	if ((length < sizeof (ip6)) || (length > 65507))
		return 0; // invalid packet

	memcpy (&ip6, buf, sizeof (ip6));
	if (((ip6.ip6_vfc >> 4) != 6)
	 || ((ntohs (ip6.ip6_plen) + sizeof (ip6)) != length))
		return 0; // malformatted IPv6 packet

	if (!IsRunning ())
	{
		/* Handle router advertisement for qualification */
		/*
		 * We don't accept router advertisement without nonce.
		 * It is far too easy to spoof such packets.
		 *
		 * We don't check the source address (which may be the
		 * server's secondary address, nor the source port)
		 * TODO: Maybe we should check that too
		 */
		const uint8_t *s_nonce = packet.GetAuthNonce ();
		if ((s_nonce == NULL) || memcmp (s_nonce, probe.nonce, 8))
			return 0;
		if (packet.GetConfByte ())
		{
			syslog (LOG_ERR,
				_("Authentication refused by server."));
			return 0;
		}

		union teredo_addr newaddr;

		newaddr.teredo.server_ip = GetServerIP ();
		if (!ParseRA (packet, &newaddr, probe.state == PROBE_CONE))
			return 0;

		/* Correct router advertisement! */
		gettimeofday (&probe.serv, NULL);
		probe.serv.tv_sec += SERVER_LOSS_DELAY;

		if (probe.state == PROBE_RESTRICT)
		{
			probe.state = PROBE_SYMMETRIC;
			SendRS (sock, GetServerIP (), probe.nonce,
				false, false);

			gettimeofday (&probe.next, NULL);
			probe.next.tv_sec += PROBE_DELAY;
			memcpy (&addr, &newaddr, sizeof (addr));
		}
		else
		if ((probe.state == PROBE_SYMMETRIC)
		 && ((addr.teredo.client_port != newaddr.teredo.client_port)
		  || (addr.teredo.client_ip != newaddr.teredo.client_ip)))
		{
			syslog (LOG_ERR,
				_("Unsupported symmetric NAT detected."));

			/* Resets state, will retry in 5 minutes */
			addr.teredo.prefix = PREFIX_UNSET;
			probe.state = PROBE_CONE;
			probe.count = 0;
			return 0;
		}
		else
		{
			syslog (LOG_INFO, _("Qualified (NAT type: %s)"),
				gettext (probe.state == PROBE_CONE
				? N_("cone") : N_("restricted")));
			probe.state = QUALIFIED;
			probe.next.tv_sec += SERVER_PING_DELAY - PROBE_DELAY;

			// call memcpy before NotifyUp for re-entrancy
			memcpy (&addr, &newaddr, sizeof (addr));
			NotifyUp (&newaddr.ip6);
		}

		return 0;
	}

	// Checks source IPv6 address
	/*
	 * TODO:
	 * The specification says we "should" check that the packet
	 * destination address is ours, if we are a client. The kernel
	 * will do this for us if we are a client. In the relay's case, we
	 * "should" check that the destination is in the "range of IPv6
	 * adresses served by the relay", which may be a run-time option (?).
	 *
	 * NOTE:
	 * The specification specifies that the relay MUST look up the peer in
	 * the list and update last reception date even if the destination is
	 * incorrect.
	 */
#if 0
	/*
	 * Ensures that the packet destination has an IPv6 Internet scope
	 * (ie 2000::/3)
	 * That should be done just before calling SendIPv6Packet().
	 */
	if ((ip6.ip6_dst.s6_addr[0] & 0xe0) != 0x20)
		return 0; // must be discarded, or ICMPv6 error (?)
#endif

	if (IsClient () && (packet.GetClientIP () == GetServerIP ())
	 && (packet.GetClientPort () == htons (IPPORT_TEREDO)))
	{
		// TODO: refresh interval randomisation
		gettimeofday (&probe.serv, NULL);
		probe.serv.tv_sec += SERVER_LOSS_DELAY;

		// Make sure our Teredo address did not change:
		union teredo_addr newaddr;
		newaddr.teredo.server_ip = GetServerIP ();

		if (ParseRA (packet, &newaddr, IsCone ())
		 && memcmp (&addr, &newaddr, sizeof (addr)))
		{
			memcpy (&addr, &newaddr, sizeof (addr));
			syslog (LOG_NOTICE, _("Teredo address changed"));
			NotifyUp (&newaddr.ip6);
			return 0;
		}

		const struct teredo_orig_ind *ind = packet.GetOrigInd ();
		if (ind != NULL)
			return SendBubble (sock, ~ind->orig_addr,
						~ind->orig_port,
						&ip6.ip6_dst, &ip6.ip6_src);

		/*
		 * Normal reception of packet must only occur if it does not
		 * come from the server, as specified.
		 */
		return 0;
	}

	/* Actual packet reception, either as a relay or a client */

	// Look up peer in the list:
	struct __TeredoRelay_peer *p = FindPeer (&ip6.ip6_src);

	if (p != NULL)
	{
		// Client case 1 (trusted node or (trusted) Teredo client):
		if (p->flags.flags.trusted
		 && (packet.GetClientIP () == p->mapped_addr)
		 && (packet.GetClientPort () == p->mapped_port))
		{
			p->flags.flags.replied = 1;

			time (&p->last_rx);
			return SendIPv6Packet (buf, length);
		}

		// Client case 2 (untrusted non-Teredo node):
		// FIXME: or maybe untrusted non-cone Teredo client
		if ((!p->flags.flags.trusted) && p->flags.flags.nonce
		 && CheckPing (packet, p->nonce))
		{
			p->flags.flags.trusted = p->flags.flags.replied = 1;
			p->flags.flags.nonce = 0;

			p->mapped_port = packet.GetClientPort ();
			p->mapped_addr = packet.GetClientIP ();
			time (&p->last_rx);

			// FIXME: dequeue incoming and outgoing packets
			/*
			 * NOTE:
			 * This implies the kernel will see Echo replies sent
			 * for Teredo tunneling maintenance. It's not really
			 * an issue, as IPv6 stacks ignore them.
			 */
			return SendIPv6Packet (buf, length);
		}
	}

	/*
	 * At this point, we have either a trusted mapping mismatch or an
	 * unlisted peer.
	 */

	if (IN6_TEREDO_PREFIX (&ip6.ip6_src) == GetPrefix ())
	{
		// Client case 3 (unknown matching Teredo client):
		if (IN6_MATCHES_TEREDO_CLIENT (&ip6.ip6_src,
						packet.GetClientIP (),
						packet.GetClientPort ()))
		{
			if (p == NULL)
			{
				/*
				 * Relays We are explicitly allowed to drop
				 * packets from unknown peers and it is surely
				 * much better. It prevents routing of packet
				 * through the wrong relay.
				 */
				if (IsRelay ())
					return 0;

				// TODO: do not duplicate this code
				p = AllocatePeer ();
				if (p == NULL)
					return -1; // insufficient memory
				memcpy (&p->addr, &ip6.ip6_dst,
					sizeof (struct in6_addr));
				
				p->mapped_port =
					IN6_TEREDO_PORT (&ip6.ip6_dst);
				p->mapped_addr =
					IN6_TEREDO_IPV4 (&ip6.ip6_dst);
				p->flags.all_flags = 0;
				p->queue = NULL;
			}
			else
			if (p->queue != NULL)
			{
				sock.SendPacket (p->queue, p->queuelen,
							p->mapped_addr,
							p->mapped_port);
				delete p->queue;
				p->queue = NULL;
			}

			p->flags.flags.trusted = p->flags.flags.replied = 1;
			time (&p->last_rx);

			if (IsBubble (&ip6))
				return 0; // discard Teredo bubble
			return SendIPv6Packet (buf, length);
		}
	}

	// Relays only accept packet from Teredo clients;
	if (IsRelay ())
		return 0;

	// TODO: implement client cases 4 & 5 for local Teredo

	/*
	 * Default: Client case 6:
	 * (unknown non-Teredo node or Tereco client with incorrect mapping):
	 * We should be cautious when accepting packets there, all the
	 * more as we don't know if we are a really client or just a
	 * qualified relay (ie. whether the host's default route is
	 * actually the Teredo tunnel).
	 */

	// TODO: avoid code duplication (direct IPv6 connectivity test)
	if (p == NULL)
	{
		p = AllocatePeer ();
		if (p == NULL)
			return -1; // memory error
		memcpy (&p->addr, &ip6.ip6_src, sizeof (struct in6_addr));
		p->mapped_port = 0;
		p->mapped_addr = 0;
		p->flags.all_flags = 0;
		time (&p->last_rx);
		time (&p->last_xmit);
		p->queue = NULL;
	}

	p->flags.flags.nonce = 1;

	// FIXME: queue packet in incoming queue
	// FIXME: re-send echo request if no response
#ifdef USE_OPENSSL
	if (!p->flags.flags.nonce)
	{
		p->flags.flags.nonce = 1;

		if (!RAND_pseudo_bytes (p->nonce, 8))
		{
			char buf[120];

			syslog (LOG_WARNING,
				_("Possibly predictable nonce: %s"),
				ERR_error_string (ERR_get_error (), buf));
		}
	}
#else
	return -1;
#endif
	return SendPing (sock, &addr, &ip6.ip6_src, p->nonce);
}


int TeredoRelay::Process (void)
{
	struct timeval now;

	gettimeofday (&now, NULL);

	if (IsRelay ())
		return 0;

	/* Qualification or server refresh (only for client) */
	if (((signed)(now.tv_sec - probe.next.tv_sec) > 0)
	 || ((now.tv_sec == probe.next.tv_sec)
	  && ((signed)(now.tv_usec - probe.next.tv_usec) > 0)))
	{
		unsigned delay;
		bool down = false;

		if (probe.state == QUALIFIED)
		{
			// TODO: randomize refresh interval
			delay = SERVER_PING_DELAY;

			if (((signed)(now.tv_sec - probe.serv.tv_sec) > 0)
			 || ((now.tv_sec == probe.serv.tv_sec)
			  && ((signed)(now.tv_usec - probe.serv.tv_usec) > 0)))
			{
				// connectivity with server lost
				probe.count = 1;
				probe.state = IsCone () ? PROBE_CONE
							: PROBE_RESTRICT;
				down = true;
			}
		}
		else
		{
			delay = PROBE_DELAY;

			if (probe.state == PROBE_CONE)
			{
				if (probe.count == 4) // already 4 attempts?
				{
					// Cone qualification failed
					probe.state = PROBE_RESTRICT;
					probe.count = 0;
				}
			}
			else
			{
				if (probe.state == PROBE_SYMMETRIC)
					/*
					 * Second half of restricted
					 * qualification failed: re-trying
					 * restricted qualifcation
					 */
					probe.state = PROBE_RESTRICT;

				if (probe.count == 4)
					/*
					 * Restricted qualification failed.
					 * Restarting from zero.
					 */
					probe.state = PROBE_CONE;
				else
				if (probe.count == 3)
					/*
					 * Last restricted qualification
					 * attempt before declaring failure.
					 * Defer new attempts for 300 seconds.
					 */
					delay = RESTART_DELAY;
			}

			probe.count ++;
		}

		SendRS (sock, GetServerIP (), probe.nonce,
			probe.state == PROBE_CONE /* cone */,
			probe.state == PROBE_RESTRICT /* secondary */);

		gettimeofday (&probe.next, NULL);
		probe.next.tv_sec += delay;

		if (down)
		{
			syslog (LOG_NOTICE, _("Lost Teredo connectivity"));
			// do this at the end to allow re-entrancy
			NotifyDown ();
		}
	}

	return 0;
}
