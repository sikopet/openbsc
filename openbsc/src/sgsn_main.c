/* GPRS SGSN Implementation */

/* (C) 2010 by Harald Welte <laforge@gnumonks.org>
 * (C) 2010 by On Waves
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <errno.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <osmocore/talloc.h>
#include <osmocore/select.h>

#include <openbsc/signal.h>
#include <openbsc/debug.h>
#include <openbsc/telnet_interface.h>
#include <openbsc/vty.h>
#include <openbsc/sgsn.h>
#include <openbsc/gprs_ns.h>
#include <openbsc/gprs_bssgp.h>

#include "../bscconfig.h"

/* this is here for the vty... it will never be called */
void subscr_put() { abort(); }

#define _GNU_SOURCE
#include <getopt.h>

void *tall_bsc_ctx;

struct gprs_ns_inst *sgsn_nsi;

const char *openbsc_version = "Osmocom NSIP Proxy " PACKAGE_VERSION;
const char *openbsc_copyright =
	"Copyright (C) 2010 Harald Welte and On-Waves\n"
	"Contributions by Daniel Willmann, Jan Lübbe, Stefan Schmidt\n"
	"Dieter Spaar, Andreas Eversberg, Holger Freyther\n\n"
	"License GPLv2+: GNU GPL version 2 or later <http://gnu.org/licenses/gpl.html>\n"
	"This is free software: you are free to change and redistribute it.\n"
	"There is NO WARRANTY, to the extent permitted by law.\n";

static char *config_file = "osmo_sgsn.cfg";
static struct sgsn_config sgcfg;

/* call-back function for the NS protocol */
static int sgsn_ns_cb(enum gprs_ns_evt event, struct gprs_nsvc *nsvc,
		      struct msgb *msg, u_int16_t bvci)
{
	int rc = 0;

	switch (event) {
	case GPRS_NS_EVT_UNIT_DATA:
		/* hand the message into the BSSGP implementation */
		rc = gprs_bssgp_rcvmsg(msg);
		break;
	default:
		LOGP(DGPRS, LOGL_ERROR, "SGSN: Unknown event %u from NS\n", event);
		if (msg)
			talloc_free(msg);
		rc = -EIO;
		break;
	}
	return rc;
}

/* NSI that BSSGP uses when transmitting on NS */
extern struct gprs_ns_inst *bssgp_nsi;

int main(int argc, char **argv)
{
	struct gsm_network dummy_network;
	struct log_target *stderr_target;
	struct sockaddr_in sin;
	int rc;

	tall_bsc_ctx = talloc_named_const(NULL, 0, "osmo_sgsn");

	log_init(&log_info);
	stderr_target = log_target_create_stderr();
	log_add_target(stderr_target);
	log_set_all_filter(stderr_target, 1);

	telnet_init(&dummy_network, 4245);
	rc = sgsn_parse_config(config_file, &sgcfg);
	if (rc < 0) {
		LOGP(DGPRS, LOGL_FATAL, "Cannot parse config file\n");
		exit(2);
	}

	sgsn_nsi = gprs_ns_instantiate(&sgsn_ns_cb);
	if (!sgsn_nsi) {
		LOGP(DGPRS, LOGL_ERROR, "Unable to instantiate NS\n");
		exit(1);
	}
	bssgp_nsi = sgcfg.nsi = sgsn_nsi;
	nsip_listen(sgsn_nsi, sgcfg.nsip_listen_port);

	while (1) {
		rc = bsc_select_main(0);
		if (rc < 0)
			exit(3);
	}

	exit(0);
}

struct gsm_network;
int bsc_vty_init(struct gsm_network *dummy)
{
	cmd_init(1);
	vty_init();

	openbsc_vty_add_cmds();
        sgsn_vty_init();
	return 0;
}
