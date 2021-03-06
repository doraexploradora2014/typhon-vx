/* Main method of the layer2/3 stack */

/* (C) 2010 by Holger Hans Peter Freyther
 * (C) 2010 by Harald Welte <laforge@gnumonks.org>
 *
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

#include <osmocom/bb/common/osmocom_data.h>
#include <osmocom/bb/common/l1ctl.h>
#include <osmocom/bb/common/l1l2_interface.h>
#include <osmocom/bb/common/sap_interface.h>
#include <osmocom/bb/misc/layer3.h>
#include <osmocom/bb/common/logging.h>
#include <osmocom/bb/common/l23_app.h>

#include <osmocom/core/msgb.h>
#include <osmocom/core/talloc.h>
#include <osmocom/core/select.h>
#include <osmocom/core/linuxlist.h>
#include <osmocom/core/gsmtap_util.h>
#include <osmocom/core/gsmtap.h>

#include <arpa/inet.h>

#define _GNU_SOURCE
#include <getopt.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <limits.h>
#include <sys/stat.h>

#include "common.h"

struct log_target *stderr_target;

void *l23_ctx = NULL;
static char *layer2_socket_path = "/tmp/osmocom_l2";
static char *sap_socket_path = "/tmp/osmocom_sap";
char pool_pipe[PATH_MAX] = DEF_PIPE_PATH;

struct llist_head ms_list;
static struct osmocom_ms *ms = NULL;
static char *gsmtap_ip = NULL;
struct gsmtap_inst *gsmtap_inst;
unsigned short vty_port = 4247;
int (*l23_app_work) (struct osmocom_ms *ms) = NULL;
int (*l23_app_exit) (struct osmocom_ms *ms) = NULL;
int quit = 0;
int mode = MODE_STANDALONE;
/*  - standalone
    - master
    - slave  */

// operator & bts identification for current burst
// first 3 digits: country code , last 2 digits: operator code
uint32_t b_mcnc = 0;
uint32_t b_cellid = 0;

const char *openbsc_copyright =
	"Copyright (C) 2008-2010 ...\n"
	"Contributions by ...\n\n"
	"License GPLv2+: GNU GPL version 2 or later "
		"<http://gnu.org/licenses/gpl.html>\n"
	"This is free software: you are free to change and redistribute it.\n"
	"There is NO WARRANTY, to the extent permitted by law.\n";

static void print_usage(const char *app)
{
	printf("Usage: %s\n", app);
}

static void print_help()
{
	printf(" Some help...\n");
	printf("  -h --help		this text\n");
	printf("  -s --socket		/tmp/osmocom_l2. Path to the unix "
		"domain socket (l2)\n");
	printf("  -S --sap		/tmp/osmocom_sap. Path to the unix "
		"domain socket (BTSAP)\n");
	printf("  -p --pool-pipe	Path to pool pipe (default %s)\n",DEF_PIPE_PATH);
	printf("  --master		Run in the master mode. (send jump commands to pool)\n");
	printf("  --cellid		Set cellid for master (master only)\n");
	printf("  --mcnc		Set country&operator code for master (master only)\n");
	printf("  --slave		Run in the slave mode. (read jump command as pool member)\n");
	printf("  -a --arfcn NR		The ARFCN to be used for layer2.\n");
	printf("  -i --gsmtap-ip	The destination IP used for GSMTAP.\n");
	printf("  -v --vty-port		The VTY port number to telnet to. "
		"(default %u)\n", vty_port);
	printf("  -d --debug		Change debug flags.\n");
}

static void handle_options(int argc, char **argv)
{
	struct sockaddr_in gsmtap;
	while (1) {
		int option_index = 0, c;
		static struct option long_options[] = {
			{"help", 0, 0, 'h'},
			{"socket", 1, 0, 's'},
			{"sap", 1, 0, 'S'},
			{"arfcn", 1, 0, 'a'},
			{"gsmtap-ip", 1, 0, 'i'},
			{"vty-port", 1, 0, 'v'},
			{"debug", 1, 0, 'd'},
			{"tmsi", 1, 0, 't'},
			{"key", 1, 0, 'k'},
			{"pool-pipe", 1, 0, 'p'},
			{"master", 0, 0, 'm'},
			{"slave", 0, 0, 214},
			{"cellid", 1, 0, 216},
			{"mcnc", 1, 0, 215},
			{0, 0, 0, 0},
		};

		c = getopt_long(argc, argv, "hs:S:a:i:v:d:t:k:mp:",
				long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case 'h':
			print_usage(argv[0]);
			print_help();
			exit(0);
			break;
		case 's':
			layer2_socket_path = talloc_strdup(l23_ctx, optarg);
			break;
		case 'S':
			sap_socket_path = talloc_strdup(l23_ctx, optarg);
			break;
		case 'p':
			strncpy(pool_pipe,optarg,PATH_MAX);
			break;
		case 'a':
			ms->test_arfcn = atoi(optarg);
			break;
		case 'i':
			gsmtap_ip = optarg;
			break;
		case 'v':
			vty_port = atoi(optarg);
			break;
		case 'd':
			log_parse_category_mask(stderr_target, optarg);
			break;
		case 'k':
			set_key(optarg);
			break;
		case 't':
			set_tmsi(optarg);
			break;
		case 'm':
			mode=MODE_MASTER;
			printf("mode: master\n");
			break;
		case 214:
			mode=MODE_SLAVE;
			printf("mode: slave\n");
			break;
		case 215:
//			printf("mcnc\n");
			b_mcnc=atoi(optarg);
			break;
		case 216:
//			printf("cellid\n");
			b_cellid=atoi(optarg);
			break;

		default:
			break;
		}
	}
}

void sighandler(int sigset)
{
	int rc = 0;

	if (sigset == SIGHUP || sigset == SIGPIPE)
		return;

	fprintf(stderr, "Signal %d recevied.\n", sigset);
	if (l23_app_exit)
		rc = l23_app_exit(ms);

	if (rc != -EBUSY)
		exit (0);
}

int main(int argc, char **argv)
{
	int rc;

	printf("%s\n", openbsc_copyright);

	INIT_LLIST_HEAD(&ms_list);
	log_init(&log_info, NULL);
	stderr_target = log_target_create_stderr();
	log_add_target(stderr_target);
	log_set_all_filter(stderr_target, 0); //1
	log_set_log_level(stderr_target,0); 
	// XXX


	l23_ctx = talloc_named_const(NULL, 1, "layer2 context");

	ms = talloc_zero(l23_ctx, struct osmocom_ms);
	if (!ms) {
		fprintf(stderr, "Failed to allocate MS\n");
		exit(1);
	}
	llist_add_tail(&ms->entity, &ms_list);

	sprintf(ms->name, "1");

	ms->test_arfcn = 871;

	handle_options(argc, argv);

	rc = layer2_open(ms, layer2_socket_path);
	if (rc < 0) {
		fprintf(stderr, "Failed during layer2_open()\n");
		exit(1);
	}

	rc = sap_open(ms, sap_socket_path);
	if (rc < 0)
		fprintf(stderr, "Failed during sap_open(), no SIM reader\n");



	if (mode != MODE_STANDALONE) {
		// create pool pipe if does exist
		struct stat info;

		if(lstat(pool_pipe,&info) != 0) {
			if(errno == ENOENT) {
				//  doesn't exist
				if (mkfifo(pool_pipe,S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP) != 0) {
					fprintf(stderr, "Failed to create %s, exiting.\n");
					return 142;
				}
			} else if(errno == EACCES) {
			// we don't have permission to know if
			//  the path/file exists.. impossible to tell
				fprintf(stderr, "Permission denied when looking for pool pipe, exiting\n");
				return 143;

			}
		} else {
			if(!(S_ISFIFO(info.st_mode))) {
			  //it's not a named pipe 
				fprintf(stderr, "The pool pipe path exists, but it is not a pipe, exiting\n");
				return 144;
			}
		}
	}

//	lapdm_init(&ms->l2_entity.lapdm_dcch, ms);
//	lapdm_init(&ms->l2_entity.lapdm_acch, ms);
	lapdm_channel_init(&ms->lapdm_channel, LAPDM_MODE_MS);

	if (gsmtap_ip) {
	gsmtap_inst = gsmtap_source_init(gsmtap_ip, GSMTAP_UDP_PORT, 1);
		if (!gsmtap_inst) {
			fprintf(stderr, "Failed during gsmtap_init()\n");
			exit(1);
		}
		gsmtap_source_add_sink(gsmtap_inst);
	}

	signal(SIGINT, sighandler);
	signal(SIGHUP, sighandler);
	signal(SIGTERM, sighandler);
	signal(SIGPIPE, sighandler);

	rc = l23_app_init(ms);
	if (rc < 0)
		exit(1);

	// main loop
	while (!quit) {
		if (l23_app_work)
			l23_app_work(ms);
		osmo_select_main(0);
	}

	return 0;
}
