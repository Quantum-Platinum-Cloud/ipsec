/* $Id: grabmyaddr.c,v 1.23.4.2 2005/07/16 04:41:01 monas Exp $ */

/*
 * Copyright (C) 1995, 1996, 1997, and 1998 WIDE Project.
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "config.h"

#include <sys/types.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#include <net/if.h>
#if defined(__FreeBSD__) && __FreeBSD__ >= 3
#include <net/if_var.h>
#endif
#if defined(__NetBSD__) || defined(__FreeBSD__) || defined(__APPLE__)
#include <netinet/in.h>
#include <netinet6/in6_var.h>
#endif
#include <net/route.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <netdb.h>
#ifdef HAVE_GETIFADDRS
#include <ifaddrs.h>
#include <net/if.h>
#endif 

#include "var.h"
#include "misc.h"
#include "vmbuf.h"
#include "plog.h"
#include "sockmisc.h"
#include "debug.h"

#include "localconf.h"
#include "handler.h"
#include "grabmyaddr.h"
#include "sockmisc.h"
#include "isakmp_var.h"
#include "gcmalloc.h"
#include "nattraversal.h"

#ifdef __linux__
#include <linux/types.h>
#include <linux/rtnetlink.h>
#ifndef HAVE_GETIFADDRS
#define HAVE_GETIFADDRS
#define NEED_LINUX_GETIFADDRS
#endif
#endif

#ifndef HAVE_GETIFADDRS
static unsigned int if_maxindex __P((void));
#endif
#ifdef __APPLE__
static struct myaddrs *find_myaddr __P((struct myaddrs *, struct sockaddr *, int));
#else
static struct myaddrs *find_myaddr __P((struct myaddrs *, struct myaddrs *));
#endif
static int suitable_ifaddr __P((const char *, const struct sockaddr *));
#ifdef INET6
static int suitable_ifaddr6 __P((const char *, const struct sockaddr *));
#endif

#ifdef NEED_LINUX_GETIFADDRS

/* We could do this _much_ better. kame racoon in its current form
 * will esentially die at frequent changes of address configuration.
 */

struct ifaddrs
{
	struct ifaddrs *ifa_next;
	char		ifa_name[16];
	int		ifa_ifindex;
	struct sockaddr *ifa_addr;
	struct sockaddr_storage ifa_addrbuf;
};

static int parse_rtattr(struct rtattr *tb[], int max, struct rtattr *rta, int len)
{
	while (RTA_OK(rta, len)) {
		if (rta->rta_type <= max)
			tb[rta->rta_type] = rta;
		rta = RTA_NEXT(rta,len);
	}
	return 0;
}

static void recvaddrs(int fd, struct ifaddrs **ifa, __u32 seq)
{
	char	buf[8192];
	struct sockaddr_nl nladdr;
	struct iovec iov = { buf, sizeof(buf) };
	struct ifaddrmsg *m;
	struct rtattr * rta_tb[IFA_MAX+1];
	struct ifaddrs *I;

	while (1) {
		int status;
		struct nlmsghdr *h;

		struct msghdr msg = {
			(void*)&nladdr, sizeof(nladdr),
			&iov,	1,
			NULL,	0,
			0
		};

		status = recvmsg(fd, &msg, 0);

		if (status < 0)
			continue;

		if (status == 0)
			return;

		if (nladdr.nl_pid) /* Message not from kernel */
			continue;

		h = (struct nlmsghdr*)buf;
		while (NLMSG_OK(h, status)) {
			if (h->nlmsg_seq != seq)
				goto skip_it;

			if (h->nlmsg_type == NLMSG_DONE)
				return;

			if (h->nlmsg_type == NLMSG_ERROR)
				return;

			if (h->nlmsg_type != RTM_NEWADDR)
				goto skip_it;

			m = NLMSG_DATA(h);

			if (m->ifa_family != AF_INET &&
			    m->ifa_family != AF_INET6)
				goto skip_it;

			if (m->ifa_flags&IFA_F_TENTATIVE)
				goto skip_it;

			memset(rta_tb, 0, sizeof(rta_tb));
			parse_rtattr(rta_tb, IFA_MAX, IFA_RTA(m), h->nlmsg_len - NLMSG_LENGTH(sizeof(*m)));

			if (rta_tb[IFA_LOCAL] == NULL)
				rta_tb[IFA_LOCAL] = rta_tb[IFA_ADDRESS];
			if (rta_tb[IFA_LOCAL] == NULL)
				goto skip_it;
			
			I = malloc(sizeof(struct ifaddrs));
			if (!I)
				return;
			memset(I, 0, sizeof(*I));

			I->ifa_ifindex = m->ifa_index;
			I->ifa_addr = (struct sockaddr*)&I->ifa_addrbuf;
			I->ifa_addr->sa_family = m->ifa_family;
			if (m->ifa_family == AF_INET) {
				struct sockaddr_in *sin = (void*)I->ifa_addr;
				memcpy(&sin->sin_addr, RTA_DATA(rta_tb[IFA_LOCAL]), 4);
			} else {
				struct sockaddr_in6 *sin = (void*)I->ifa_addr;
				memcpy(&sin->sin6_addr, RTA_DATA(rta_tb[IFA_LOCAL]), 16);
				if (IN6_IS_ADDR_LINKLOCAL(&sin->sin6_addr))
					sin->sin6_scope_id = I->ifa_ifindex;
			}
			I->ifa_next = *ifa;
			*ifa = I;

skip_it:
			h = NLMSG_NEXT(h, status);
		}
		if (msg.msg_flags & MSG_TRUNC)
			continue;
	}
	return;
}

static int getifaddrs(struct ifaddrs **ifa0)
{
	struct {
		struct nlmsghdr nlh;
		struct rtgenmsg g;
	} req;
	struct sockaddr_nl nladdr;
	static __u32 seq;
	struct ifaddrs *i;
	int fd;

	fd = socket(PF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
	if (fd < 0)
		return -1;

	memset(&nladdr, 0, sizeof(nladdr));
	nladdr.nl_family = AF_NETLINK;

	req.nlh.nlmsg_len = sizeof(req);
	req.nlh.nlmsg_type = RTM_GETADDR;
	req.nlh.nlmsg_flags = NLM_F_ROOT|NLM_F_MATCH|NLM_F_REQUEST;
	req.nlh.nlmsg_pid = 0;
	req.nlh.nlmsg_seq = ++seq;
	req.g.rtgen_family = AF_UNSPEC;

	if (sendto(fd, (void*)&req, sizeof(req), 0, (struct sockaddr*)&nladdr, sizeof(nladdr)) < 0) {
		close(fd);
		return -1;
	}

	*ifa0 = NULL;

	recvaddrs(fd, ifa0, seq);

	close(fd);

	fd = socket(AF_INET, SOCK_DGRAM, 0);

	for (i=*ifa0; i; i = i->ifa_next) {
		struct ifreq ifr;
		ifr.ifr_ifindex = i->ifa_ifindex;
		ioctl(fd, SIOCGIFNAME, (void*)&ifr);
		memcpy(i->ifa_name, ifr.ifr_name, 16);
	}
	close(fd);

	return 0;
}

static void freeifaddrs(struct ifaddrs *ifa0)
{
        struct ifaddrs *i;

        while (ifa0) {
                i = ifa0;
                ifa0 = i->ifa_next;
                free(i);
        }
}

#endif

#ifndef HAVE_GETIFADDRS
static unsigned int
if_maxindex()
{
	struct if_nameindex *p, *p0;
	unsigned int max = 0;

	p0 = if_nameindex();
	for (p = p0; p && p->if_index && p->if_name; p++) {
		if (max < p->if_index)
			max = p->if_index;
	}
	if_freenameindex(p0);
	return max;
}
#endif


void
clear_myaddr()
{
	struct myaddrs *p, *next;

	for (p = lcconf->myaddrs; p; p = next) {
		next = p->next;

		if (p->addr)
			racoon_free(p->addr);
		racoon_free(p);
	}

	lcconf->myaddrs = NULL;

}


static struct myaddrs *
find_myaddr(db, addr, udp_encap)
	struct myaddrs *db;
	struct sockaddr *addr;
	int udp_encap;
{
	struct myaddrs *q;
	char h1[NI_MAXHOST], h2[NI_MAXHOST];

	if (getnameinfo(addr, sysdep_sa_len(addr), h1, sizeof(h1), NULL, 0,
	    NI_NUMERICHOST | niflags) != 0)
		return NULL;

	for (q = db; q; q = q->next) {
		if (!q->addr)
			continue;
		if (q->udp_encap && !udp_encap
			|| !q->udp_encap && udp_encap)
			continue;
		if (addr->sa_family != q->addr->sa_family)
			continue;
		if (getnameinfo(q->addr, sysdep_sa_len(q->addr), h2, sizeof(h2),
		    NULL, 0, NI_NUMERICHOST | niflags) != 0)
			return NULL;
		if (strcmp(h1, h2) == 0)
			return q;
	}

	return NULL;
}


// modified to avoid closing and opening sockets for
// all interfaces each time an interface change occurs.
// on return: 	addrcount = zero indicates address no longer used
//				sock = -1 indicates a new address - no socket opened yet.
void
grab_myaddrs()
{
#ifdef HAVE_GETIFADDRS
	struct myaddrs *p, *q;
	struct ifaddrs *ifa0, *ifap;
#ifdef INET6
	struct sockaddr_in6 *sin6;
#endif

	char addr1[NI_MAXHOST];

	if (getifaddrs(&ifa0)) {
		plog(LLV_ERROR2, LOCATION, NULL,
			"getifaddrs failed: %s\n", strerror(errno));
		exit(1);
		/*NOTREACHED*/
	}

	// clear the in_use flag for each address in the list
	for (p = lcconf->myaddrs; p; p = p->next)
		p->in_use = 0;

	for (ifap = ifa0; ifap; ifap = ifap->ifa_next) {

		if (ifap->ifa_addr->sa_family != AF_INET
#ifdef INET6
		 && ifap->ifa_addr->sa_family != AF_INET6
#endif
		)
			continue;

		if (!suitable_ifaddr(ifap->ifa_name, ifap->ifa_addr)) {
			plog(LLV_ERROR, LOCATION, NULL,
				"unsuitable address: %s %s\n",
				ifap->ifa_name,
				saddrwop2str(ifap->ifa_addr));
			continue;
		}

		p = find_myaddr(lcconf->myaddrs, ifap->ifa_addr, 0);
		if (p) {
			p->in_use = 1;
#ifdef ENABLE_NATT
			q = find_myaddr(lcconf->myaddrs, ifap->ifa_addr, 1);
			if (q)
				q->in_use = 1;
#endif				
		} else {	
			p = newmyaddr();
			if (p == NULL) {
				plog(LLV_ERROR2, LOCATION, NULL,
					"unable to allocate space for addr.\n");
				exit(1);
				/*NOTREACHED*/
			}
			p->addr = dupsaddr(ifap->ifa_addr);
			if (p->addr == NULL) {
				plog(LLV_ERROR2, LOCATION, NULL,
					"unable to duplicate addr.\n");
				exit(1);
				/*NOTREACHED*/
			}
			p->sock = -1;
			p->in_use = 1;

			if (getnameinfo(p->addr, p->addr->sa_len,
					addr1, sizeof(addr1),
					NULL, 0,
					NI_NUMERICHOST | niflags))
				strlcpy(addr1, "(invalid)", sizeof(addr1));
			plog(LLV_DEBUG, LOCATION, NULL,
				"my interface: %s (%s)\n",
				addr1, ifap->ifa_name);
		
			p->next = lcconf->myaddrs;
			lcconf->myaddrs = p;
			
#ifdef ENABLE_NATT
			if (natt_enabled_in_rmconf ()) {
				q = dupmyaddr(p);
				if (q == NULL) {
					plog(LLV_ERROR2, LOCATION, NULL,
						"unable to allocate space for natt addr.\n");
					exit(1);
				}
				q->udp_encap = 1;
			}
#endif

		}
	}

	freeifaddrs(ifa0);


#else /*!HAVE_GETIFADDRS*/
#error "NOT SUPPORTED"
#endif /*HAVE_GETIFADDRS*/
}


/*
 * check the interface is suitable or not
 */
static int
suitable_ifaddr(ifname, ifaddr)
	const char *ifname;
	const struct sockaddr *ifaddr;
{
#ifdef ENABLE_HYBRID
	/* Exclude any address we got through ISAKMP mode config */
	if (exclude_cfg_addr(ifaddr) == 0)
		return 0;
#endif
	switch(ifaddr->sa_family) {
	case AF_INET:
		return 1;
#ifdef INET6
	case AF_INET6:
		return suitable_ifaddr6(ifname, ifaddr);
#endif
	default:
		return 0;
	}
	/*NOTREACHED*/
}

#ifdef INET6
static int
suitable_ifaddr6(ifname, ifaddr)
	const char *ifname;
	const struct sockaddr *ifaddr;
{
#ifndef __linux__
	struct in6_ifreq ifr6;
	int s;
#endif

	if (ifaddr->sa_family != AF_INET6)
		return 0;

#ifndef __linux__
	s = socket(PF_INET6, SOCK_DGRAM, 0);
	if (s == -1) {
		plog(LLV_ERROR, LOCATION, NULL,
			"socket(SOCK_DGRAM) failed:%s\n", strerror(errno));
		return 0;
	}

	memset(&ifr6, 0, sizeof(ifr6));
	strncpy(ifr6.ifr_name, ifname, strlen(ifname));

	ifr6.ifr_addr = *(const struct sockaddr_in6 *)ifaddr;

	if (ioctl(s, SIOCGIFAFLAG_IN6, &ifr6) < 0) {
		plog(LLV_ERROR, LOCATION, NULL,
			"ioctl(SIOCGIFAFLAG_IN6) failed:%s\n", strerror(errno));
		close(s);
		return 0;
	}

	close(s);

	if (ifr6.ifr_ifru.ifru_flags6 & IN6_IFF_DUPLICATED
	 || ifr6.ifr_ifru.ifru_flags6 & IN6_IFF_DETACHED
	 || ifr6.ifr_ifru.ifru_flags6 & IN6_IFF_ANYCAST)
		return 0;
#endif

	/* suitable */
	return 1;
}
#endif

int
update_myaddrs()
{
#ifdef __linux__
	char msg[BUFSIZ];
	int len;
	struct nlmsghdr *h = (void*)msg;
	len = read(lcconf->rtsock, msg, sizeof(msg));
	if (len < 0)
		return errno == ENOBUFS;
	if (len < sizeof(*h))
		return 0;
	if (h->nlmsg_pid) /* not from kernel! */
		return 0;
	if (h->nlmsg_type == RTM_NEWLINK)
		return 0;
	plog(LLV_DEBUG, LOCATION, NULL,
		"netlink signals update interface address list\n");
	return 1;
#else
	char msg[BUFSIZ];
	int len;
	struct rt_msghdr *rtm;

	len = read(lcconf->rtsock, msg, sizeof(msg));
	if (len < 0) {
		plog(LLV_ERROR, LOCATION, NULL,
			"read(PF_ROUTE) failed: %s\n",
			strerror(errno));
		return 0;
	}
	rtm = (struct rt_msghdr *)msg;
	if (len < rtm->rtm_msglen) {
		plog(LLV_ERROR, LOCATION, NULL,
			"read(PF_ROUTE) short read\n");
		return 0;
	}
	if (rtm->rtm_version != RTM_VERSION) {
		plog(LLV_ERROR, LOCATION, NULL,
			"routing socket version mismatch\n");
		close(lcconf->rtsock);
		lcconf->rtsock = -1;
		return 0;
	}
	switch (rtm->rtm_type) {
	case RTM_NEWADDR:
	case RTM_DELADDR:
	case RTM_DELETE:
	case RTM_IFINFO:
		break;
	case RTM_MISS:
		/* ignore this message silently */
		return 0;
	default:
		plog(LLV_DEBUG, LOCATION, NULL,
			"msg %d not interesting\n", rtm->rtm_type);
		return 0;
	}
	/* XXX more filters here? */

	plog(LLV_DEBUG, LOCATION, NULL,
		"caught rtm:%d, need update interface address list\n",
		rtm->rtm_type);

	return 1;
#endif /* __linux__ */
}

/*
 * initialize default port for ISAKMP to send, if no "listen"
 * directive is specified in config file.
 *
 * DO NOT listen to wildcard addresses.  if you receive packets to
 * wildcard address, you'll be in trouble (DoS attack possible by
 * broadcast storm).
 */
int
autoconf_myaddrsport()
{
	struct myaddrs *p;
	int n;

	plog(LLV_DEBUG, LOCATION, NULL,
		"configuring default isakmp port.\n");

	for (p = lcconf->myaddrs, n = 0; p; p = p->next, n++) {
		set_port (p->addr, p->udp_encap ? lcconf->port_isakmp_natt : lcconf->port_isakmp);
	}
	plog(LLV_DEBUG, LOCATION, NULL,
		"%d addrs are configured successfully\n", n);

	return 0;
}

/*
 * get a port number to which racoon binded.
 * NOTE: network byte order returned.
 */
u_short
getmyaddrsport(local)
	struct sockaddr *local;
{
	struct myaddrs *p, *bestmatch = NULL;
	u_short bestmatch_port = PORT_ISAKMP;

	/* get a relative port */
	for (p = lcconf->myaddrs; p; p = p->next) {
		if (!p->addr)
			continue;
		if (!cmpsaddrwop(local, p->addr)) {
			if (! bestmatch) {
				bestmatch = p;
				continue;
			}
			
			switch (p->addr->sa_family) {
			case AF_INET:
				if (((struct sockaddr_in *)p->addr)->sin_port == PORT_ISAKMP) {
					bestmatch = p;
					bestmatch_port = ((struct sockaddr_in *)p->addr)->sin_port;
					break;
				}
				break;
#ifdef INET6
			case AF_INET6:
				if (((struct sockaddr_in6 *)p->addr)->sin6_port == PORT_ISAKMP) {
					bestmatch = p;
					bestmatch_port = ((struct sockaddr_in6 *)p->addr)->sin6_port;
					break;
				}
				break;
#endif
			default:
				plog(LLV_ERROR, LOCATION, NULL,
				     "unsupported AF %d\n", p->addr->sa_family);
				continue;
			}
		}
	}

	return htons(bestmatch_port);
}

struct myaddrs *
newmyaddr()
{
	struct myaddrs *new;

	new = racoon_calloc(1, sizeof(*new));
	if (new == NULL) {
		plog(LLV_ERROR, LOCATION, NULL,
			"failed to allocate buffer for myaddrs.\n");
		return NULL;
	}

	new->next = NULL;
	new->addr = NULL;

	return new;
}

struct myaddrs *
dupmyaddr(struct myaddrs *old)
{
	struct myaddrs *new;

	new = racoon_calloc(1, sizeof(*new));
	if (new == NULL) {
		plog(LLV_ERROR, LOCATION, NULL,
			"failed to allocate buffer for myaddrs.\n");
		return NULL;
	}

	/* Copy the whole structure and set the differences.  */
	memcpy (new, old, sizeof (*new));
	new->addr = dupsaddr (old->addr);
	if (new->addr == NULL) {
		plog(LLV_ERROR, LOCATION, NULL,
			"failed to allocate buffer for duplicate addr.\n");
		racoon_free(new);
		return NULL;
	}
	new->next = old->next;
	old->next = new;

	return new;
}

void
insmyaddr(new, head)
	struct myaddrs *new;
	struct myaddrs **head;
{
	new->next = *head;
	*head = new;
}

void
delmyaddr(myaddr)
	struct myaddrs *myaddr;
{
	if (myaddr->addr)
		racoon_free(myaddr->addr);
	racoon_free(myaddr);
}

int
initmyaddr()
{
	/* initialize routing socket */
	lcconf->rtsock = socket(PF_ROUTE, SOCK_RAW, PF_UNSPEC);
	if (lcconf->rtsock < 0) {
		plog(LLV_ERROR, LOCATION, NULL,
			"socket(PF_ROUTE) failed: %s",
			strerror(errno));
		return -1;
	}

#ifdef __linux__
   {
	struct sockaddr_nl nl;
	u_int addr_len;

	memset(&nl, 0, sizeof(nl));
	nl.nl_family = AF_NETLINK;
	nl.nl_groups = RTMGRP_IPV4_IFADDR|RTMGRP_LINK;

	if (bind(lcconf->rtsock, (struct sockaddr*)&nl, sizeof(nl)) < 0) {
		plog(LLV_ERROR, LOCATION, NULL,
		     "bind(PF_NETLINK) failed: %s\n",
		     strerror(errno));
		return -1;
	}
	addr_len = sizeof(nl);
	if (getsockname(lcconf->rtsock, (struct sockaddr*)&nl, &addr_len) < 0) {
		plog(LLV_ERROR, LOCATION, NULL,
		     "getsockname(PF_NETLINK) failed: %s\n",
		     strerror(errno));
		return -1;
	}
   }
#endif

	if (lcconf->myaddrs == NULL && lcconf->autograbaddr == 1) {
		grab_myaddrs();

		if (autoconf_myaddrsport() < 0)
			return -1;
	}

	return 0;
}

/* select the socket to be sent */
/* should implement other method. */
int
getsockmyaddr(my)
	struct sockaddr *my;
{
	struct myaddrs *p, *lastresort = NULL;
#if defined(INET6) && defined(__linux__)
	struct myaddrs *match_wo_scope_id = NULL;
	int check_wo_scope_id = (my->sa_family == AF_INET6) && 
		IN6_IS_ADDR_LINKLOCAL(&((struct sockaddr_in6 *)my)->sin6_addr);
#endif

	for (p = lcconf->myaddrs; p; p = p->next) {
		if (p->addr == NULL)
			continue;
		if (my->sa_family == p->addr->sa_family) {
			lastresort = p;
		} else continue;
		if (sysdep_sa_len(my) == sysdep_sa_len(p->addr)
		 && memcmp(my, p->addr, sysdep_sa_len(my)) == 0) {
			break;
		}
#if defined(INET6) && defined(__linux__)
		if (check_wo_scope_id && IN6_IS_ADDR_LINKLOCAL(&((struct sockaddr_in6 *)p->addr)->sin6_addr) &&
			/* XXX: this depends on sin6_scope_id to be last
			 * item in struct sockaddr_in6 */
			memcmp(my, p->addr, 
				sysdep_sa_len(my) - sizeof(uint32_t)) == 0) {
			match_wo_scope_id = p;
		}
#endif
	}
#if defined(INET6) && defined(__linux__)
	if (!p)
		p = match_wo_scope_id;
#endif
	if (!p)
		p = lastresort;
	if (!p) {
		plog(LLV_ERROR, LOCATION, NULL,
			"no socket matches address family %d\n",
			my->sa_family);
		return -1;
	}

	return p->sock;
}
