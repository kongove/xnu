/*	$FreeBSD: src/sys/netinet6/ipcomp_input.c,v 1.1.2.2 2001/07/03 11:01:54 ume Exp $	*/
/*	$KAME: ipcomp_input.c,v 1.25 2001/03/01 09:12:09 itojun Exp $	*/

/*
 * Copyright (C) 1999 WIDE Project.
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

/*
 * RFC2393 IP payload compression protocol (IPComp).
 */


#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/mbuf.h>
#include <sys/domain.h>
#include <sys/protosw.h>
#include <sys/socket.h>
#include <sys/errno.h>
#include <sys/time.h>
#include <sys/kernel.h>
#include <sys/syslog.h>

#include <net/if.h>
#include <net/route.h>
#include <libkern/zlib.h>
#include <kern/cpu_number.h>
#include <kern/locks.h>

#include <netinet/in.h>
#include <netinet/in_systm.h>
#include <netinet/in_var.h>
#include <netinet/ip.h>
#include <netinet/ip_var.h>
#include <netinet/ip_ecn.h>
#include <netinet/kpi_ipfilter_var.h>

#if INET6
#include <netinet/ip6.h>
#include <netinet6/ip6_var.h>
#endif
#include <netinet6/ipcomp.h>
#if INET6
#include <netinet6/ipcomp6.h>
#endif

#include <netinet6/ipsec.h>
#if INET6
#include <netinet6/ipsec6.h>
#endif
#include <netkey/key.h>
#include <netkey/keydb.h>

#include <net/net_osdep.h>
#include <mach/sdt.h>

#define IPLEN_FLIPPED

void
ipcomp4_input(struct mbuf *m, int off)
{
	struct mbuf *md;
	struct ip *ip;
	struct ipcomp *ipcomp;
	const struct ipcomp_algorithm *algo;
	u_int16_t cpi;	/* host order */
	u_int16_t nxt;
	size_t hlen;
	int error;
	size_t newlen, olen;
	struct secasvar *sav = NULL;


	if (m->m_pkthdr.len < off + sizeof(struct ipcomp)) {
		ipseclog((LOG_DEBUG, "IPv4 IPComp input: assumption failed "
		    "(packet too short)\n"));
		IPSEC_STAT_INCREMENT(ipsecstat.in_inval);
		goto fail;
	}

	md = m_pulldown(m, off, sizeof(*ipcomp), NULL);
	if (!md) {
		m = NULL;	/*already freed*/
		ipseclog((LOG_DEBUG, "IPv4 IPComp input: assumption failed "
		    "(pulldown failure)\n"));
		IPSEC_STAT_INCREMENT(ipsecstat.in_inval);
		goto fail;
	}
	ipcomp = mtod(md, struct ipcomp *);
	ip = mtod(m, struct ip *);
	nxt = ipcomp->comp_nxt;
#ifdef _IP_VHL
	hlen = IP_VHL_HL(ip->ip_vhl) << 2;
#else
	hlen = ip->ip_hl << 2;
#endif

	cpi = ntohs(ipcomp->comp_cpi);

	if (cpi >= IPCOMP_CPI_NEGOTIATE_MIN) {
		sav = key_allocsa(AF_INET, (caddr_t)&ip->ip_src,
			(caddr_t)&ip->ip_dst, IPPROTO_IPCOMP, htonl(cpi));
		if (sav != NULL
		 && (sav->state == SADB_SASTATE_MATURE
		  || sav->state == SADB_SASTATE_DYING)) {
			cpi = sav->alg_enc;	/*XXX*/
			/* other parameters to look at? */
		}
	}
	algo = ipcomp_algorithm_lookup(cpi);
	if (!algo) {
		ipseclog((LOG_WARNING, "IPv4 IPComp input: unknown cpi %u\n",
			cpi));
		IPSEC_STAT_INCREMENT(ipsecstat.in_nosa);
		goto fail;
	}

	/* chop ipcomp header */
	ipcomp = NULL;
	md->m_data += sizeof(struct ipcomp);
	md->m_len -= sizeof(struct ipcomp);
	m->m_pkthdr.len -= sizeof(struct ipcomp);
#ifdef IPLEN_FLIPPED
	ip->ip_len -= sizeof(struct ipcomp);
#else
	ip->ip_len = htons(ntohs(ip->ip_len) - sizeof(struct ipcomp));
#endif

	olen = m->m_pkthdr.len;
	newlen = m->m_pkthdr.len - off;
	error = (*algo->decompress)(m, m->m_next, &newlen);
	if (error != 0) {
		if (error == EINVAL) {
			IPSEC_STAT_INCREMENT(ipsecstat.in_inval);
		} else if (error == ENOBUFS)
			IPSEC_STAT_INCREMENT(ipsecstat.in_nomem);
		m = NULL;
		goto fail;
	}
	IPSEC_STAT_INCREMENT(ipsecstat.in_comphist[cpi]);

	/*
	 * returning decompressed packet onto icmp is meaningless.
	 * mark it decrypted to prevent icmp from attaching original packet.
	 */
	m->m_flags |= M_DECRYPTED;

	m->m_pkthdr.len = off + newlen;
	ip = mtod(m, struct ip *);
    {
	size_t len;
#ifdef IPLEN_FLIPPED
	len = ip->ip_len;
#else
	len = ntohs(ip->ip_len);
#endif
	/*
	 * be careful about underflow.  also, do not assign exact value
	 * as ip_len is manipulated differently on *BSDs.
	 */
	len += m->m_pkthdr.len;
	len -= olen;
	if (len & ~0xffff) {
		/* packet too big after decompress */
		IPSEC_STAT_INCREMENT(ipsecstat.in_inval);
		goto fail;
	}
#ifdef IPLEN_FLIPPED
	ip->ip_len = len & 0xffff;
#else
	ip->ip_len = htons(len & 0xffff);
#endif
	ip->ip_p = nxt;
    }

	if (sav) {
		key_sa_recordxfer(sav, m);
		if (ipsec_addhist(m, IPPROTO_IPCOMP, (u_int32_t)cpi) != 0) {
			IPSEC_STAT_INCREMENT(ipsecstat.in_nomem);
			goto fail;
		}
		key_freesav(sav, KEY_SADB_UNLOCKED);
		sav = NULL;
	}

	if (nxt != IPPROTO_DONE) {
		if ((ip_protox[nxt]->pr_flags & PR_LASTHDR) != 0 &&
		    ipsec4_in_reject(m, NULL)) {
			IPSEC_STAT_INCREMENT(ipsecstat.in_polvio);
			goto fail;
		}

		DTRACE_IP6(receive, struct mbuf *, m, struct inpcb *, NULL,
                        struct ip *, ip, struct ifnet *, m->m_pkthdr.rcvif,
                        struct ip *, ip, struct ip6_hdr *, NULL);

		ip_proto_dispatch_in(m, off, nxt, 0);
	} else
		m_freem(m);
	m = NULL;

	IPSEC_STAT_INCREMENT(ipsecstat.in_success);
	return;

fail:
	if (sav)
		key_freesav(sav, KEY_SADB_UNLOCKED);

	if (m)
		m_freem(m);
	return;
}

#if INET6
int
ipcomp6_input(struct mbuf **mp, int *offp, int proto)
{
#pragma unused(proto)
	struct mbuf *m, *md;
	int off;
	struct ip6_hdr *ip6;
	struct ipcomp *ipcomp;
	const struct ipcomp_algorithm *algo;
	u_int16_t cpi;	/* host order */
	u_int16_t nxt;
	int error;
	size_t newlen;
	struct secasvar *sav = NULL;
	char *prvnxtp;

	m = *mp;
	off = *offp;

	md = m_pulldown(m, off, sizeof(*ipcomp), NULL);
	if (!md) {
		m = NULL;	/*already freed*/
		ipseclog((LOG_DEBUG, "IPv6 IPComp input: assumption failed "
		    "(pulldown failure)\n"));
		IPSEC_STAT_INCREMENT(ipsec6stat.in_inval);
		goto fail;
	}
	ipcomp = mtod(md, struct ipcomp *);
	ip6 = mtod(m, struct ip6_hdr *);
	nxt = ipcomp->comp_nxt;

	cpi = ntohs(ipcomp->comp_cpi);

	if (cpi >= IPCOMP_CPI_NEGOTIATE_MIN) {
		sav = key_allocsa(AF_INET6, (caddr_t)&ip6->ip6_src,
			(caddr_t)&ip6->ip6_dst, IPPROTO_IPCOMP, htonl(cpi));
		if (sav != NULL
		 && (sav->state == SADB_SASTATE_MATURE
		  || sav->state == SADB_SASTATE_DYING)) {
			cpi = sav->alg_enc;	/*XXX*/
			/* other parameters to look at? */
		}
	}
	algo = ipcomp_algorithm_lookup(cpi);
	if (!algo) {
		ipseclog((LOG_WARNING, "IPv6 IPComp input: unknown cpi %u; "
			"dropping the packet for simplicity\n", cpi));
		IPSEC_STAT_INCREMENT(ipsec6stat.in_nosa);
		goto fail;
	}

	/* chop ipcomp header */
	ipcomp = NULL;
	md->m_data += sizeof(struct ipcomp);
	md->m_len -= sizeof(struct ipcomp);
	m->m_pkthdr.len -= sizeof(struct ipcomp);

	newlen = m->m_pkthdr.len - off;
	error = (*algo->decompress)(m, md, &newlen);
	if (error != 0) {
		if (error == EINVAL) {
			IPSEC_STAT_INCREMENT(ipsec6stat.in_inval);
		} else if (error == ENOBUFS)
			IPSEC_STAT_INCREMENT(ipsec6stat.in_nomem);
		m = NULL;
		goto fail;
	}
	IPSEC_STAT_INCREMENT(ipsec6stat.in_comphist[cpi]);
	m->m_pkthdr.len = off + newlen;

	/*
	 * returning decompressed packet onto icmp is meaningless.
	 * mark it decrypted to prevent icmp from attaching original packet.
	 */
	m->m_flags |= M_DECRYPTED;

	/* update next header field */
	prvnxtp = ip6_get_prevhdr(m, off);
	*prvnxtp = nxt;

	/*
	 * no need to adjust payload length, as all the IPv6 protocols
	 * look at m->m_pkthdr.len
	 */

	if (sav) {
		key_sa_recordxfer(sav, m);
		if (ipsec_addhist(m, IPPROTO_IPCOMP, (u_int32_t)cpi) != 0) {
			IPSEC_STAT_INCREMENT(ipsec6stat.in_nomem);
			goto fail;
		}
		key_freesav(sav, KEY_SADB_UNLOCKED);
		sav = NULL;
	}
	*offp = off;
	*mp = m;
	IPSEC_STAT_INCREMENT(ipsec6stat.in_success);
	return nxt;

fail:
	if (m)
		m_freem(m);
	if (sav)
		key_freesav(sav, KEY_SADB_UNLOCKED);
	return IPPROTO_DONE;
}
#endif /* INET6 */
