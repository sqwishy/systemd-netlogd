/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2016 Susant Sahani

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <stdio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <getopt.h>
#include <systemd/sd-daemon.h>

#include "util.h"
#include "build.h"
#include "mkdir.h"
#include "path-util.h"
#include "fd-util.h"
#include "fs-util.h"
#include "user-util.h"
#include "fileio.h"
#include "network-util.h"
#include "netlog-conf.h"
#include "netlog-manager.h"

#define STATE_FILE "/var/lib/systemd/journal-netlogd/state"

static const char *arg_cursor = NULL;
static const char *arg_save_state = STATE_FILE;

static void help(void) {
        printf("%s ..\n\n"
               "Forwards messages from the journal to other hosts over the network using the syslog\n"
               "RFC 5424 format in both unicast and multicast addresses.\n\n"
               "  -h --help                 Show this help\n"
               "     --version              Show package version\n"
               "     --cursor=CURSOR        Start at the specified cursor\n"
               "     --save-state[=FILE]    Save uploaded cursors (default \n"
               "                            " STATE_FILE ")\n"
               "  -h --help                 Show this help and exit\n"
               "     --version              Print version string and exit\n"
               , program_invocation_short_name);
}

static int parse_argv(int argc, char *argv[]) {
        enum {
                ARG_VERSION = 0x100,
                ARG_CURSOR,
                ARG_SAVE_STATE,
        };

        static const struct option options[] = {
                { "help",         no_argument,       NULL, 'h'                },
                { "version",      no_argument,       NULL, ARG_VERSION        },
                { "cursor",       required_argument, NULL, ARG_CURSOR         },
                { "save-state",   optional_argument, NULL, ARG_SAVE_STATE     },
                {}
        };

        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "h", options, NULL)) >= 0)
                switch(c) {
                case 'h':
                        help();
                        return 0;

                case ARG_VERSION:
                        puts(PACKAGE_STRING);
                        puts(SYSTEMD_FEATURES);
                        return 0;

                case ARG_CURSOR:
                        if (arg_cursor) {
                                log_error("cannot use more than one --cursor/--after-cursor");
                                return -EINVAL;
                        }

                        arg_cursor = optarg;
                        break;
                case ARG_SAVE_STATE:
                        arg_save_state = optarg ?: STATE_FILE;
                        break;

                case '?':
                        log_error("Unknown option %s.", argv[optind-1]);
                        return -EINVAL;

                case ':':
                        log_error("Missing argument to %s.", argv[optind-1]);
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option code.");
                }


        if (optind < argc) {
                log_error("Input arguments make no sense with journal input.");
                return -EINVAL;
        }

        return 1;
}

int main(int argc, char **argv) {
        _cleanup_(manager_freep) Manager *m = NULL;
        const char *user = "systemd-journal-netlog";
        uid_t uid;
        gid_t gid;
        int r;

        log_show_color(true);
        log_set_target(LOG_TARGET_AUTO);
        log_parse_environment();
        log_open();

        r = parse_argv(argc, argv);
        if (r <= 0)
                goto finish;

        umask(0022);

        r = get_user_creds(&user, &uid, &gid, NULL, NULL);
        if (r < 0) {
                log_error_errno(r, "Cannot resolve user name %s: %m", user);
                goto finish;
        }

        r = manager_new(&m, arg_save_state, arg_cursor);
        if (r < 0) {
                log_error_errno(r, "Failed to allocate manager: %m");
                goto finish;
        }

        r = manager_parse_config_file(m);
        if (r < 0) {
                log_error_errno(r, "Failed to parse configuration file: %m");
                goto finish;
        }

        log_debug("%s running as pid "PID_FMT,
                  program_invocation_short_name, getpid());

        sd_notify(false,
                  "READY=1\n"
                  "STATUS=Processing input...");

        if (network_is_online()) {
                r = manager_connect(m);
                if (r < 0)
                        goto cleanup;
        }

        r = sd_event_loop(m->event);
        if (r < 0) {
                log_error_errno(r, "Failed to run event loop: %m");
                goto cleanup;
        }

        sd_event_get_exit_code(m->event, &r);

 cleanup:
        sd_notify(false,
                  "STOPPING=1\n"
                  "STATUS=Shutting down...");

 finish:
        return r >= 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
