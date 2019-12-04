/* SPDX-License-Identifier: MIT
 *
 * Copyright (C) 2019 WireGuard LLC. All Rights Reserved.
 */

#define _DEFAULT_SOURCE

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <libmnl/libmnl.h>
#include <linux/rtnetlink.h>

#include "common.h"
#include "dbg.h"

union kvalues {
	uint32_t u32;
	struct wg_combined_ip ip;
	char errmsg[256];
};

static void request_ip(enum wg_dynamic_key key, union kvalues kv, void **dest)
{
	struct wg_combined_ip *ip = &kv.ip;
	struct wg_dynamic_request_ip *r = (struct wg_dynamic_request_ip *)*dest;

	switch (key) {
	case WGKEY_REQUEST_IP:
		BUG_ON(*dest);
		*dest = calloc(1, sizeof(struct wg_dynamic_request_ip));
		if (!*dest)
			fatal("calloc()");

		break;
	case WGKEY_IPV4:
		memcpy(&r->ipv4, &ip->ip4, sizeof r->ipv4);
		r->cidrv4 = ip->cidr;
		r->has_ipv4 = true;
		break;
	case WGKEY_IPV6:
		memcpy(&r->ipv6, &ip->ip6, sizeof r->ipv6);
		r->cidrv6 = ip->cidr;
		r->has_ipv6 = true;
		break;
	case WGKEY_LEASESTART:
		r->start = kv.u32;
		break;
	case WGKEY_LEASETIME:
		r->leasetime = kv.u32;
		break;
	case WGKEY_ERRNO:
		r->wg_errno = kv.u32;
		break;
	case WGKEY_ERRMSG:
		r->errmsg = strdup(kv.errmsg);
		break;
	default:
		debug("Invalid key %d, aborting\n", key);
		BUG();
	}
}

static void (*const deserialize_fptr[])(enum wg_dynamic_key key,
					union kvalues kv, void **dest) = {
	NULL,
	NULL,
	request_ip,
};

static bool parse_ip_cidr(struct wg_combined_ip *ip, char *value)
{
	uintmax_t res;
	char *endptr;
	char *sep;

	if (value[0] == '\0') {
		memset(ip, 0, ip->family == AF_INET ? 4 : 16);
		ip->cidr = 0;
		return true;
	}

	sep = strchr(value, '/');
	if (!sep)
		return false;

	*sep = '\0';
	if (inet_pton(ip->family, value, ip) != 1)
		return false;

	res = strtoumax(sep + 1, &endptr, 10);
	if (res > UINT8_MAX || *endptr != '\0' || sep + 1 == endptr)
		return false;

	// TODO: validate cidr range depending on ip->family
	ip->cidr = (uint8_t)res;

	return true;
}

static bool parse_value(enum wg_dynamic_key key, char *str, union kvalues *kv)
{
	char *endptr;
	uintmax_t uresult;
	struct wg_combined_ip *ip;

	switch (key) {
	case WGKEY_IPV4:
	case WGKEY_IPV6:
		ip = &kv->ip;
		ip->family = (key == WGKEY_IPV4) ? AF_INET : AF_INET6;
		if (!parse_ip_cidr(ip, str))
			return false;

		break;
	case WGKEY_REQUEST_IP:
	case WGKEY_LEASESTART:
	case WGKEY_LEASETIME:
	case WGKEY_ERRNO:
		uresult = strtoumax(str, &endptr, 10);
		if (uresult > UINT32_MAX || *endptr != '\0')
			return false;

		kv->u32 = (uint32_t)uresult;
		break;
	case WGKEY_ERRMSG:
		strncpy(kv->errmsg, str, sizeof kv->errmsg);
		kv->errmsg[sizeof kv->errmsg - 1] = '\0';
		break;
	default:
		debug("Invalid key %d, aborting\n", key);
		BUG();
	}

	return true;
}

static enum wg_dynamic_key parse_key(char *key)
{
	for (enum wg_dynamic_key e = 2; e < ARRAY_SIZE(WG_DYNAMIC_KEY); ++e)
		if (!strcmp(key, WG_DYNAMIC_KEY[e]))
			return e;

	return WGKEY_UNKNOWN;
}

/* Consumes one full line from buf, or up to MAX_LINESIZE bytes if no newline
 * character was found. If less then MAX_LINESIZE bytes are available, a new
 * buffer will be allocated and req->buf and req->len set accordingly.
 *
 * Return values:
 *   > 0 : Amount of bytes consumed (<= MAX_LINESIZE)
 *   = 0 : Consumed len bytes; need more for a full line
 *   < 0 : Error
 */
static ssize_t parse_line(unsigned char *buf, size_t len,
			  struct wg_dynamic_request *req,
			  enum wg_dynamic_key *key, union kvalues *kv)
{
	unsigned char *line_end, *key_end;
	ssize_t line_len;

	line_end = memchr(buf, '\n', MIN(len, MAX_LINESIZE));
	if (!line_end) {
		if (len >= MAX_LINESIZE)
			return -E2BIG;

		req->len = len;
		req->buf = malloc(len);
		if (!req->buf)
			fatal("malloc()");

		memcpy(req->buf, buf, len);
		return 0;
	}

	if (line_end == buf) {
		*key = WGKEY_EOMSG;
		return 1;
	}

	*line_end = '\0';
	line_len = line_end - buf + 1;

	key_end = memchr(buf, '=', line_len - 1);
	if (!key_end || key_end == buf)
		return -EINVAL;

	*key_end = '\0';
	*key = parse_key((char *)buf);
	if (*key == WGKEY_UNKNOWN)
		return line_len;

	if (!parse_value(*key, (char *)key_end + 1, kv))
		return -EINVAL;

	return line_len;
}

static ssize_t parse_request(struct wg_dynamic_request *req, unsigned char *buf,
			     size_t len)
{
	ssize_t ret, offset = 0;
	size_t addlen = 0;
	enum wg_dynamic_key key;
	union kvalues kv;
	void (*deserialize)(enum wg_dynamic_key key, union kvalues kv,
			    void **dest);

	if (memchr(buf, '\0', len))
		return -EINVAL; /* don't allow null bytes */

	if (req->len > 0 && req->buf) {
		len += req->len;

		memmove(buf + req->len, buf, len);
		memcpy(buf, req->buf, req->len);
		addlen = req->len;
		free(req->buf);
		req->buf = NULL;
		req->len = 0;
	}

	if (req->cmd == WGKEY_UNKNOWN) {
		ret = parse_line(buf, len, req, &req->cmd, &kv);
		if (ret <= 0)
			return ret;

		req->version = kv.u32;
		if (req->cmd >= WGKEY_ENDCMD || req->cmd <= WGKEY_EOMSG ||
		    req->version != 1)
			return -EPROTONOSUPPORT;

		len -= ret;
		offset += ret;

		deserialize = deserialize_fptr[req->cmd];
		deserialize(req->cmd, kv, &req->result);
	} else {
		deserialize = deserialize_fptr[req->cmd];
	}

	while (len > 0) {
		ret = parse_line(buf + offset, len, req, &key, &kv);
		if (ret <= 0)
			return ret;

		len -= ret;
		offset += ret;

		if (key == WGKEY_EOMSG)
			return offset - addlen;
		else if (key == WGKEY_UNKNOWN)
			continue;
		else if (key <= WGKEY_ENDCMD)
			return -EINVAL;

		deserialize(key, kv, &req->result);
	}

	return 0;
}

int handle_request(int fd, struct wg_dynamic_request *req,
		   unsigned char buf[RECV_BUFSIZE + MAX_LINESIZE],
		   size_t *remaining)
{
	ssize_t bytes, processed;

	do {
		if (*remaining > 0)
			bytes = *remaining;
		else
			bytes = read(fd, buf, RECV_BUFSIZE);

		if (bytes < 0) {
			if (errno == EWOULDBLOCK || errno == EAGAIN ||
			    errno == EINTR)
				return 0;

			debug("Reading from socket %d failed: %s\n", fd,
			      strerror(errno));
			return -1;
		} else if (bytes == 0) {
			return -1;
		}

		processed = parse_request(req, buf, bytes);
		if (processed < 0)
			return processed; /* Parsing error */
	} while (processed == 0);

	*remaining = bytes - processed;
	memmove(buf, buf + processed, *remaining);

	return 1;
}

void free_wg_dynamic_request(struct wg_dynamic_request *req)
{
	BUG_ON(req->buf || req->len);

	req->cmd = WGKEY_UNKNOWN;
	req->version = 0;
	if (req->result) {
		free(((struct wg_dynamic_request_ip *)req->result)->errmsg);
		free(req->result);
		req->result = NULL;
	}
}

size_t serialize_request_ip(bool send, char *buf, size_t len,
			    struct wg_dynamic_request_ip *rip)
{
	size_t off = 0;
	char addrbuf[INET6_ADDRSTRLEN];

	if (send)
		print_to_buf(buf, len, &off, "request_ip=1\n");

	if (rip->has_ipv4) {
		if (!inet_ntop(AF_INET, &rip->ipv4, addrbuf, sizeof addrbuf))
			fatal("inet_ntop()");

		print_to_buf(buf, len, &off, "ipv4=%s/32\n", addrbuf);
	}

	if (rip->has_ipv6) {
		if (!inet_ntop(AF_INET6, &rip->ipv6, addrbuf, sizeof addrbuf))
			fatal("inet_ntop()");

		print_to_buf(buf, len, &off, "ipv6=%s/128\n", addrbuf);
	}

	if (rip->start && rip->leasetime)
		print_to_buf(buf, len, &off, "leasestart=%u\nleasetime=%u\n",
			     rip->start, rip->leasetime);

	if (rip->errmsg)
		print_to_buf(buf, len, &off, "errmsg=%s\n", rip->errmsg);

	if (!send)
		print_to_buf(buf, len, &off, "errno=%u\n", rip->wg_errno);

	print_to_buf(buf, len, &off, "\n");

	return off;
}

void print_to_buf(char *buf, size_t bufsize, size_t *offset, char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf + *offset, bufsize - *offset, fmt, ap);
	va_end(ap);

	if (n < 0) {
		fatal("vsnprintf()");
	} else if (n + *offset >= bufsize) {
		debug("Outbuffer too small: %d + %zu >= %zu\n", n, *offset,
		      bufsize);
		BUG();
	}

	*offset += n;
}

uint32_t current_time()
{
	struct timespec tp;
	if (clock_gettime(CLOCK_REALTIME, &tp))
		fatal("clock_gettime(CLOCK_REALTIME)");
	return tp.tv_sec;
}

bool is_link_local(unsigned char *addr)
{
	/* TODO: check if the remaining 54 bits are 0 */
	return IN6_IS_ADDR_LINKLOCAL(addr);
}

void iface_get_all_addrs(uint8_t family, mnl_cb_t data_cb, void *cb_data)
{
	struct mnl_socket *nl;
	char buf[MNL_SOCKET_BUFFER_SIZE];
	struct nlmsghdr *nlh;
	/* TODO: rtln-addr-dump from libmnl uses rtgenmsg here? */
	struct ifaddrmsg *ifaddr;
	int ret;
	unsigned int seq, portid;

	nl = mnl_socket_open(NETLINK_ROUTE);
	if (nl == NULL)
		fatal("mnl_socket_open");

	if (mnl_socket_bind(nl, 0, MNL_SOCKET_AUTOPID) < 0)
		fatal("mnl_socket_bind");

	/* You'd think that we could just request addresses from a specific
	 * interface, via NLM_F_MATCH or something, but we can't. See also:
	 * https://marc.info/?l=linux-netdev&m=132508164508217
	 */
	seq = time(NULL);
	portid = mnl_socket_get_portid(nl);
	nlh = mnl_nlmsg_put_header(buf);
	nlh->nlmsg_type = RTM_GETADDR;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	nlh->nlmsg_seq = seq;
	ifaddr = mnl_nlmsg_put_extra_header(nlh, sizeof(struct ifaddrmsg));
	ifaddr->ifa_family = family;

	if (mnl_socket_sendto(nl, nlh, nlh->nlmsg_len) < 0)
		fatal("mnl_socket_sendto");

	do {
		ret = mnl_socket_recvfrom(nl, buf, sizeof(buf));
		if (ret <= MNL_CB_STOP)
			break;
		ret = mnl_cb_run(buf, ret, seq, portid, data_cb, cb_data);
	} while (ret > 0);

	if (ret == -1)
		fatal("mnl_cb_run/mnl_socket_recvfrom");

	mnl_socket_close(nl);
}

int data_attr_cb(const struct nlattr *attr, void *data)
{
	const struct nlattr **tb = data;
	int type = mnl_attr_get_type(attr);

	/* skip unsupported attribute in user-space */
	if (mnl_attr_type_valid(attr, IFA_MAX) < 0)
		return MNL_CB_OK;

	switch (type) {
	case IFA_ADDRESS:
		if (mnl_attr_validate(attr, MNL_TYPE_BINARY) < 0) {
			perror("mnl_attr_validate");
			return MNL_CB_ERROR;
		}
		break;
	}
	tb[type] = attr;
	return MNL_CB_OK;
}
