/*
 * $Id: hncp_io.c $
 *
 * Author: Markus Stenberg <mstenber@cisco.com>
 *
 * Copyright (c) 2013 cisco Systems, Inc.
 *
 * Created:       Mon Nov 25 14:00:10 2013 mstenber
 * Last modified: Wed May 27 10:39:23 2015 mstenber
 * Edit time:     371 min
 *
 */

/* This module implements I/O needs of hncp. Notably, it has both
 * functionality that deals with sockets, and bit more abstract ones
 * that just deal with buffers for input and output (thereby
 * facilitating unit testing without using real sockets). */

#include "hncp_i.h"
#undef __unused
/* In linux, fcntl.h includes something with __unused. Argh. */
#include <fcntl.h>
#define __unused __attribute__((unused))
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <net/if.h>
#include <net/ethernet.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <libubox/usock.h>
#include <ifaddrs.h>

#ifdef __linux__
#define AF_LINK AF_PACKET
#include <linux/if_packet.h>
#endif /* __linux__ */

static int
_get_hwaddrs(dncp_ext ext __unused, unsigned char *buf, int buf_left)
{
  struct ifaddrs *ia, *p;
  int r = getifaddrs(&ia);
  void *a1 = buf, *a2 = buf + ETHER_ADDR_LEN;
  int addrs = 0;
  unsigned char zeroed_addr[] = {0, 0, 0, 0, 0, 0};

  if (buf_left < ETHER_ADDR_LEN * 2)
    return 0;
  memset(buf, 0, ETHER_ADDR_LEN * 2);
  if (r)
    return 0;
  for (p = ia ; p ; p = p->ifa_next)
    if (p->ifa_addr && p->ifa_addr->sa_family == AF_LINK)
      {
        void *a;
#ifdef __linux__
        struct sockaddr_ll *sll = (struct sockaddr_ll *) p->ifa_addr;
        a = sll->sll_addr;
#else
        a = &p->ifa_addr->sa_data[0];
#endif /* __linux__ */
        if (memcmp(a, zeroed_addr, sizeof(zeroed_addr)) == 0)
          continue;
        if (!addrs || memcmp(a1, a, ETHER_ADDR_LEN) < 0)
          memcpy(a1, a, ETHER_ADDR_LEN);
        if (!addrs || memcmp(a2, a, ETHER_ADDR_LEN) > 0)
          memcpy(a2, a, ETHER_ADDR_LEN);
        addrs++;
      }
  L_INFO("_get_hwaddrs => %s", HEX_REPR(buf, ETHER_ADDR_LEN * 2));
  freeifaddrs(ia);
  if (!addrs)
    {
      L_ERR("_get_hwaddrs failed - no AF_LINK addresses");
      return 0;
    }
  return ETHER_ADDR_LEN * 2;
}

static void _timeout(struct uloop_timeout *t)
{
  hncp h = container_of(t, hncp_s, timeout);
  dncp_ext_timeout(h->dncp);
}

bool
hncp_io_set_ifname_enabled(hncp h, const char *ifname, bool enabled)
{
  struct ipv6_mreq val;

  val.ipv6mr_multiaddr = h->multicast_address;
  L_DEBUG("_set_ifname_enabled %s %s",
          ifname, enabled ? "enabled" : "disabled");
  uint32_t ifindex = 0;
  if (!(ifindex = if_nametoindex(ifname)))
    {
      L_DEBUG("unable to enable on %s - if_nametoindex: %s",
              ifname, strerror(errno));
      return false;
    }
  val.ipv6mr_interface = ifindex;
  int fd6;
  udp46_get_fds(h->u46_server, NULL, &fd6);
  if (setsockopt(fd6, IPPROTO_IPV6,
                 enabled ? IPV6_ADD_MEMBERSHIP : IPV6_DROP_MEMBERSHIP,
                 (char *) &val, sizeof(val)) < 0)
    {
      L_ERR("unable to enable on %s - setsockopt:%s", ifname, strerror(errno));
      return false;
    }
  /* Yay. It succeeded(?). */
  dncp_ext_ep_ready(dncp_ep_find_by_name(h->dncp, ifname), enabled);
  return true;
}

static void _schedule_timeout(dncp_ext ext, int msecs)
{
  hncp h = container_of(ext, hncp_s, ext);

  //1ms timeout was weird in VirtualBox env (causing less than 1ms
  //to). Also we do not really want too many timeouts anyway; if it
  //is not instant timeout, we might as well wait 10ms.
  if (msecs && msecs < 10)
    msecs = 10;
  uloop_timeout_set(&h->timeout, msecs);
}

static ssize_t
_recv(dncp_ext ext,
      dncp_ep *ep,
      struct sockaddr_in6 **src_store,
      struct sockaddr_in6 **dst_store,
      void *buf, size_t len)
{
  hncp h = container_of(ext, hncp_s, ext);
  ssize_t l = -1;
  char ifname[IFNAMSIZ];

  while (l < 0)
    {
#ifdef DTLS
      if (h->d)
        {
          l = dtls_recv(h->d, src_store, dst_store, buf, len);
          if (l > 0)
            {
              /* Ignore non-linklocal dtls for now. */
              if (src_store && !IN6_IS_ADDR_LINKLOCAL(&(*src_store)->sin6_addr))
                continue;
            }
        }
#endif /* DTLS */
      if (l < 0)
        {
          static struct sockaddr_in6 src, dst;
          l = udp46_recv(h->u46_server, &src, &dst, buf, len);
#ifdef DTLS
          if (h->d && !IN6_IS_ADDR_MULTICAST(&dst.sin6_addr))
            {
              L_ERR("plaintext unicast received when in dtls mode - skip");
              continue;
            }
#endif /* DTLS */
          *src_store = &src;
          *dst_store = &dst;
        }
      if (l < 0)
        continue;
      if (!*src_store)
        {
          L_DEBUG("no source..?");
          continue;
        }
      if (!(*src_store)->sin6_scope_id)
        {
          L_DEBUG("no scope id..?");
          continue;
        }
      if (!if_indextoname((*src_store)->sin6_scope_id, ifname))
        {
          L_ERR("unable to receive - if_indextoname:%s",
                strerror(errno));
          continue;
        }
      *ep = dncp_ep_find_by_name(h->dncp, ifname);
      break;
    }
  return l;
}

static void
_send(dncp_ext ext, dncp_ep ep,
      struct sockaddr_in6 *src,
      struct sockaddr_in6 *dst,
      void *buf, size_t len)
{
  hncp h = container_of(ext, hncp_s, ext);
  struct sockaddr_in6 rdst;
  ssize_t r;

  if (!dst)
    {
      rdst.sin6_port = htons(HNCP_PORT);
      rdst.sin6_addr = h->multicast_address;
    }
  else
    rdst = *dst;
  rdst.sin6_scope_id = if_nametoindex(ep->ifname);
#ifdef DTLS
  if (h->d && !IN6_IS_ADDR_MULTICAST(&dst->sin6_addr))
    {
      /* Change destination port to DTLS server port too if it is the
       * default port. Otherwise answer on the different port (which
       * is presumably already DTLS protected due to protection in
       * input path).*/
      if (rdst.sin6_port == htons(HNCP_PORT))
        rdst.sin6_port = htons(HNCP_DTLS_SERVER_PORT);
      r = dtls_send(h->d, src, &rdst, buf, len);
      if (r > 0 && (size_t) r != len)
        L_ERR("short dtls send?!?");
    }
  else
#endif /* DTLS */
    {
      r = udp46_send(h->u46_server, src, &rdst, buf, len);
      if (r > 0 && (size_t) r != len)
        L_ERR("short udp46_send?!?");
    }
}

static hnetd_time_t _get_time(dncp_ext ext __unused)
{
  return hnetd_time();
}

#ifdef DTLS

static void _dtls_readable_callback(dtls d __unused, void *context)
{
  hncp h = context;

  dncp_ext_readable(h->dncp);
}


void hncp_set_dtls(hncp h, dtls d)
{
  h->d = d;
  dtls_set_readable_callback(d, _dtls_readable_callback, h);
}

#endif /* DTLS */

void _udp46_readable_callback(udp46 s __unused, void *context)
{
  hncp h = context;

  dncp_ext_readable(h->dncp);
}

pid_t hncp_run(char *argv[])
{
  pid_t pid = fork();

  if (pid == 0)
    {
      execv(argv[0], argv);
      _exit(128);
    }
  L_DEBUG("hncp_run %s", argv[0]);
  for (int i = 1 ; argv[i] ; i++)
    L_DEBUG(" %s", argv[i]);
  return pid;
}

bool hncp_io_init(hncp h)
{
  if (!(h->u46_server = udp46_create(h->udp_port)))
    return false;
  h->timeout.cb = _timeout;
  h->ext.cb.recv = _recv;
  h->ext.cb.send = _send;
  h->ext.cb.get_hwaddrs = _get_hwaddrs;
  h->ext.cb.get_time = _get_time;
  h->ext.cb.schedule_timeout = _schedule_timeout;
  udp46_set_readable_callback(h->u46_server, _udp46_readable_callback, h);
  return true;
}

void hncp_io_uninit(hncp h)
{
  if (h->u46_server)
    udp46_destroy(h->u46_server);
  /* clear the timer from uloop. */
  uloop_timeout_cancel(&h->timeout);
}
