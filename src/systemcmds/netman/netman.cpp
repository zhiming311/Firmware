/****************************************************************************
 *
 *   Copyright (c) 2020 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/
/**
 * @file netman.cpp
 * Network Manager driver.
 *
 * @author David Sidrane
 */

#include <px4_platform_common/px4_config.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/boardctl.h>

#include <parameters/param.h>
#include <px4_platform_common/module.h>
#include <px4_platform_common/getopt.h>
#include <px4_platform_common/log.h>
#include <arpa/inet.h>
#include <px4_platform_common/shutdown.h>


constexpr char DEFAULT_NETMAN_CONFIG[] = "/fs/microsd/net.cfg";

static void usage(const char *reason);
__BEGIN_DECLS
__EXPORT int  netman_main(int argc, char *argv[]);
__EXPORT int board_get_netconf(struct boardioc_netconf_s *netconf);
__END_DECLS

class net_params
{
private:

	param_t param_proto;
	param_t param_mask;
	param_t param_addr;
	param_t param_default_route;
	param_t param_dns;

	enum proto_e {
		DHCP   = 1,
		Static = 2,
		Both   = 3
	};


	class ipl
	{
		const char *myword;
	public:

		union {
			int32_t  l;
			uint32_t u;
			struct in_addr a;
			proto_e e;
		};


		ipl() {l = 0;}
		ipl(const char *w) : ipl()
		{ myword = w;}

		const char *to_str()
		{
			return inet_ntoa(a);
		}

		const char *protocol()
		{
			return e == Static ? "static" : (e  == DHCP) ? "dhcp" : "both";
		}

		const char *parseProtocol(const char *ps)
		{
			char *p = strstr(ps, "dhcp");

			if (p) {
				e = DHCP;

			} else {

				p = strstr(ps, "static");

				if (p) {
					e = Static;

				} else {

					p = strstr(ps, "both");

					if (p) {
						e = Both;
					}
				}
			}

			return ps;
		}


		const char *parse(const char *cp)
		{
			u = inet_addr(cp);
			return cp;
		}

		const char *parse(const char *buffer, const char *end)
		{
			char *ps = strstr(buffer, myword);

			if (ps) {
				int len = strlen(myword);

				if (ps + len < end) {
					ps += len;
					isalpha(*ps) ? parseProtocol(ps) : parse(ps);

				} else {
					ps = nullptr;
				}
			}

			return ps;
		}
	};


public:

	ipl proto{"proto "};
	ipl netmask{"netmask "};
	ipl ipaddr{"ipaddr "};
	ipl default_route{"draddr "};
	ipl dnsaddr{"dnsaddr"};


	net_params()
	{

		param_proto         = param_find("NET_I0_PROTO");
		param_mask          = param_find("NET_I0_MASK");
		param_addr          = param_find("NET_I0_IP");
		param_default_route = param_find("NET_I0_DR_IP");
		param_dns           = param_find("NET_I0_DNS_IP");

		param_get_cplusplus(param_proto, &proto.l);
		param_get_cplusplus(param_mask, &netmask.l);
		param_get_cplusplus(param_addr, &ipaddr.l);
		param_get_cplusplus(param_default_route, &default_route.l);
		param_get_cplusplus(param_dns, &dnsaddr.l);
	}

	~net_params()
	{
	}

	void hton()
	{
		/* Store them in network order */

		netmask.l = htonl(netmask.l);
		ipaddr.l = htonl(ipaddr.l);
		default_route.l = htonl(default_route.l);
		dnsaddr.l = htonl(dnsaddr.l);
	}

	void ntoh()
	{
		/* Store them in host order */

		netmask.l = ntohl(netmask.l);
		ipaddr.l = ntohl(ipaddr.l);
		default_route.l = ntohl(default_route.l);
		dnsaddr.l = ntohl(dnsaddr.l);

	}

	int save()
	{
		// Save the data in host order.
		ntoh();
		param_set(param_proto, &proto.l);
		param_set(param_mask, &netmask.l);
		param_set(param_addr, &ipaddr.l);
		param_set(param_default_route, &default_route.l);
		param_set(param_dns, &dnsaddr.l);
		param_save_default();
		return 0;
	}
};

int save(const char *path)
{
	net_params config;

	// For exporting addresses we need the data in network order

	config.hton();

	int rv = OK;
	constexpr int lsz = 80;
	char line[lsz + 1];
	int len;

	int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, PX4_O_MODE_666);

	if (fd < 0) {
		PX4_ERR("Can not create file %s", path);
		goto errout;
	}

	len = snprintf(line,  lsz, "proto %s\n", config.proto.protocol());

	if (len != write(fd, line, len)) {
		goto errout;
	}

	len = snprintf(line,  lsz, "netmask %s\n", config.netmask.to_str());

	if (len != write(fd, line, len)) {
		goto errout;
	}

	len = snprintf(line,  lsz, "ipaddr %s\n", config.ipaddr.to_str());

	if (len != write(fd, line, len)) {
		goto errout;
	}

	len = snprintf(line,  lsz, "draddr %s\n", config.default_route.to_str());

	if (len != write(fd, line, len)) {
		goto errout;
	}

	len = snprintf(line,  lsz, "dnsaddr %s\n", config.dnsaddr.to_str());

	if (len != write(fd, line, len)) {
		rv = -errno;

	} else {
		close(fd);
		return rv;
	}

errout: {
		rv = -errno;

		if (fd >= 0) {
			close(fd);
		}

		return rv;
	}
}


int update(const char *path)
{
	net_params config;
	struct stat sb;
	FAR char *lines = nullptr;
	int fd = -1;
	int rv = OK;

	/* For importing we get the user input data in network order from
	 * the network layer.
	 *
	 * So we must ensure any unchanged settings will be in network order
	 * so that the config.save(), that saves in host order, will be dealing
	 * only with network ordered data.
	 */

	config.hton();

	if (stat(path, &sb) < 0) {
		return 0;
	}

	lines = (char *) malloc(sb.st_size);

	if (!lines) {
		return -errno;
	}

	fd = open(path, O_RDONLY);

	if (fd < 0) {
		rv = -errno;
		goto errout;
	}

	if (read(fd, lines, sb.st_size) != sb.st_size) {
		rv = -errno;
		goto errout;
	}

	close(fd);
	fd = -1;

	config.proto.parse(lines, &lines[sb.st_size - 1]);
	config.netmask.parse(lines, &lines[sb.st_size - 1]);
	config.ipaddr.parse(lines, &lines[sb.st_size - 1]);
	config.default_route.parse(lines, &lines[sb.st_size - 1]);
	config.dnsaddr.parse(lines, &lines[sb.st_size - 1]);

	PX4_INFO("Network settings updated, rebooting....\n");
	config.save();
	unlink(path);

	// Ensure the message is seen.

	sleep(1);

	px4_reboot_request(false);

	while (1) { px4_usleep(1); } // this command should not return on success

errout:

	if (lines) {
		free(lines);
	}

	if (fd > 0) {
		close(fd);
	}

	return rv;
}

static void usage(const char *reason)
{
	if (reason != nullptr) {
		PX4_WARN("%s", reason);
	}

	PRINT_MODULE_DESCRIPTION(
		R"DESCR_STR(
  ### Description
  Network configuration  manager saves Network settings in parameters and feeds
  them to the OS on network initialization.

  The update option will check for the existence of net.cfg on the SD Card.
  It will update the parameters, delete the file and reboot the system.

  The save option will net.cfg on the SD Card.

  ### Examples
  $ netman -d         # display current settings.
  $ netman -u         # do an update
  $ netman -s [path]  # Save the parameters to the SD card.
)DESCR_STR");
    PRINT_MODULE_USAGE_NAME("netman", "system");
    PRINT_MODULE_USAGE_PARAM_FLAG('d', "Display the current network settings to the console.", true);
    PRINT_MODULE_USAGE_PARAM_FLAG('u', "Check SD card for network.cfg and update network parameters.", true);
    PRINT_MODULE_USAGE_PARAM_FLAG('s', "Save the current network parameters to the SD card.", true);
}

int netman_main(int argc, char *argv[])
{
  const char *path = DEFAULT_NETMAN_CONFIG;
  int ch;

  if (argc < 2) {
    usage(nullptr);
    return 1;
  }

  int myoptind = 1;
  const char *myoptarg = nullptr;

  while ((ch = px4_getopt(argc, argv, "usd", &myoptind, &myoptarg)) != EOF) {
    switch (ch) {

    case 'd':
        return save("/dev/console");

    case 's':
      return save(path);

    case 'u':
      return update(path);

    default:
      usage(nullptr);
      return 1;
    }
  }

  if (myoptind >= argc) {
    usage(nullptr);
    return 1;
  }
  return 0;
}
#ifdef CONFIG_BOARDCTL_NETCONF
__EXPORT int board_get_netconf(struct boardioc_netconf_s *netconf)
{
  net_params config;

  // N.B the nuttx netinit performs the HTONL

  netconf->flags          = (boardioc_netconf_e)config.proto.e;
  netconf->ipaddr         = config.ipaddr.u;
  netconf->netmask        = config.netmask.u;
  netconf->default_router = config.default_route.u;
  netconf->dnsaddr        = config.dnsaddr.u;;

  return OK;
}
#endif
