/*
  Copyright (c) 2012-2013, Matthias Schiffer <mschiffer@universe-factory.net>
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

    1. Redistributions of source code must retain the above copyright notice,
       this list of conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright notice,
       this list of conditions and the following disclaimer in the documentation
       and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#include "fastd.h"
#include "peer.h"

#include <arpa/inet.h>


static int parse_log_level(fastd_context_t *ctx, const char *arg) {
	if (!strcmp(arg, "fatal"))
		return LOG_CRIT;
	else if (!strcmp(arg, "error"))
		return LOG_ERR;
	else if (!strcmp(arg, "warn"))
		return LOG_WARNING;
	else if (!strcmp(arg, "info"))
		return LOG_NOTICE;
	else if (!strcmp(arg, "verbose"))
		return LOG_INFO;
	else if (!strcmp(arg, "debug"))
		return LOG_DEBUG;
	else
		exit_error(ctx, "invalid log level `%s'", arg);
}


static void print_usage(const char *options, const char *message) {
	/* 28 spaces */
	static const char spaces[] = "                            ";

	int len = strlen(options);

	printf("%s", options);

	if (len < 28)
		printf("%s", spaces+len);
	else
		printf("\n%s", spaces);

	puts(message);
}

static void usage(fastd_context_t *ctx, fastd_config_t *conf) {
	puts("fastd (Fast and Secure Tunnelling Daemon) " FASTD_VERSION " usage:\n");

#define OR ", "
#define OPTION(func, options, message) print_usage("  " options, message)
#define OPTION_ARG(func, options, arg, message) print_usage("  " options " " arg, message)
#include "options.def.h"
#undef OR
#undef OPTION
#undef OPTION_ARG

	exit(0);
}

static void version(fastd_context_t *ctx, fastd_config_t *conf) {
	puts("fastd " FASTD_VERSION);
	exit(0);
}

static void option_user(fastd_context_t *ctx, fastd_config_t *conf, const char *arg) {
	free(conf->user);
	conf->user = strdup(arg);
}

static void option_group(fastd_context_t *ctx, fastd_config_t *conf, const char *arg) {
	free(conf->group);
	conf->group = strdup(arg);
}

static void option_log_level(fastd_context_t *ctx, fastd_config_t *conf, const char *arg) {
	conf->log_stderr_level = parse_log_level(ctx, arg);
}

static void option_syslog_level(fastd_context_t *ctx, fastd_config_t *conf, const char *arg) {
	conf->log_syslog_level = parse_log_level(ctx, arg);
}

static void option_syslog_ident(fastd_context_t *ctx, fastd_config_t *conf, const char *arg) {
	free(conf->log_syslog_ident);
	conf->log_syslog_ident = strdup(arg);
}

static void option_hide_ip_addresses(fastd_context_t *ctx, fastd_config_t *conf) {
	conf->hide_ip_addresses = true;
}

static void option_hide_mac_addresses(fastd_context_t *ctx, fastd_config_t *conf) {
	conf->hide_mac_addresses = true;
}

static void option_config(fastd_context_t *ctx, fastd_config_t *conf, const char *arg) {
	if (!strcmp(arg, "-"))
		arg = NULL;

	if (!fastd_read_config(ctx, conf, arg, false, 0))
		exit(1);
}

static void option_config_peer(fastd_context_t *ctx, fastd_config_t *conf, const char *arg) {
	fastd_peer_config_new(ctx, conf);

	if(!fastd_read_config(ctx, conf, arg, true, 0))
		exit(1);
}

static void option_config_peer_dir(fastd_context_t *ctx, fastd_config_t *conf, const char *arg) {
	fastd_add_peer_dir(ctx, conf, arg);
}

static void option_mode(fastd_context_t *ctx, fastd_config_t *conf, const char *arg) {
	if (!strcmp(arg, "tap"))
		conf->mode = MODE_TAP;
	else if (!strcmp(arg, "tun"))
		conf->mode = MODE_TUN;
	else
		exit_error(ctx, "invalid mode `%s'", arg);
}

static void option_interface(fastd_context_t *ctx, fastd_config_t *conf, const char *arg) {
	free(conf->ifname);
	conf->ifname = strdup(arg);
}

static void option_mtu(fastd_context_t *ctx, fastd_config_t *conf, const char *arg) {
	char *endptr;

	conf->mtu = strtol(arg, &endptr, 10);
	if (*endptr || conf->mtu < 576)
		exit_error(ctx, "invalid mtu `%s'", arg);
}

static void option_bind(fastd_context_t *ctx, fastd_config_t *conf, const char *arg) {
	long l;
	char *charptr;
	char *endptr;
	char *addrstr;

	if (arg[0] == '[') {
		charptr = strchr(arg, ']');
		if (!charptr || (charptr[1] != ':' && charptr[1] != '\0'))
			exit_error(ctx, "invalid bind address `%s'", arg);

		addrstr = strndup(arg+1, charptr-arg-1);

		if (charptr[1] == ':')
			charptr++;
		else
			charptr = NULL;
	}
	else {
		charptr = strchr(arg, ':');
		if (charptr) {
			addrstr = strndup(arg, charptr-arg);
		}
		else {
			addrstr = strdup(arg);
		}
	}

	if (charptr) {
		l = strtol(charptr+1, &endptr, 10);
		if (*endptr || l < 0 || l > 65535)
			exit_error(ctx, "invalid bind port `%s'", charptr+1);
	}
	else {
		l = 0;
	}

	fastd_peer_address_t addr = {};

	if (strcmp(addrstr, "any") == 0) {
		/* nothing to do */
	}
	else if (arg[0] == '[') {
		addr.in6.sin6_family = AF_INET6;
		addr.in6.sin6_port = htons(l);

		if (inet_pton(AF_INET6, addrstr, &addr.in6.sin6_addr) != 1)
			exit_error(ctx, "invalid bind address `%s'", addrstr);
	}
	else {
		addr.in.sin_family = AF_INET;
		addr.in.sin_port = htons(l);

		if (inet_pton(AF_INET, addrstr, &addr.in.sin_addr) != 1)
			exit_error(ctx, "invalid bind address `%s'", addrstr);
	}

	free(addrstr);

	fastd_config_bind_address(ctx, conf, &addr, NULL, false, false);
}

static void option_protocol(fastd_context_t *ctx, fastd_config_t *conf, const char *arg) {
	if (!fastd_config_protocol(ctx, conf, arg))
		exit_error(ctx, "invalid protocol `%s'", arg);
}

static void option_method(fastd_context_t *ctx, fastd_config_t *conf, const char *arg) {
	if (!fastd_config_method(ctx, conf, arg))
		exit_error(ctx, "invalid method `%s'", arg);
}

static void option_forward(fastd_context_t *ctx, fastd_config_t *conf) {
	conf->forward = true;
}

static void option_on_up(fastd_context_t *ctx, fastd_config_t *conf, const char *arg) {
	free(conf->on_up);
	free(conf->on_up_dir);

	conf->on_up = strdup(arg);
	conf->on_up_dir = get_current_dir_name();
}

static void option_on_down(fastd_context_t *ctx, fastd_config_t *conf, const char *arg) {
	free(conf->on_down);
	free(conf->on_down_dir);

	conf->on_down = strdup(arg);
	conf->on_down_dir = get_current_dir_name();
}

static void option_on_establish(fastd_context_t *ctx, fastd_config_t *conf, const char *arg) {
	free(conf->on_establish);
	free(conf->on_establish_dir);

	conf->on_establish = strdup(arg);
	conf->on_establish_dir = get_current_dir_name();
}

static void option_on_disestablish(fastd_context_t *ctx, fastd_config_t *conf, const char *arg) {
	free(conf->on_disestablish);
	free(conf->on_disestablish_dir);

	conf->on_disestablish = strdup(arg);
	conf->on_disestablish_dir = get_current_dir_name();
}

static void option_on_verify(fastd_context_t *ctx, fastd_config_t *conf, const char *arg) {
	free(conf->on_verify);
	free(conf->on_verify_dir);

	conf->on_verify = strdup(arg);
	conf->on_verify_dir = get_current_dir_name();
}

static void option_daemon(fastd_context_t *ctx, fastd_config_t *conf) {
	conf->daemon = true;
}

static void option_pid_file(fastd_context_t *ctx, fastd_config_t *conf, const char *arg) {
	free(conf->pid_file);
	conf->pid_file = strdup(arg);
}

static void option_generate_key(fastd_context_t *ctx, fastd_config_t *conf) {
	conf->generate_key = true;
	conf->show_key = false;
}

static void option_show_key(fastd_context_t *ctx, fastd_config_t *conf) {
	conf->generate_key = false;
	conf->show_key = true;
}

static void option_machine_readable(fastd_context_t *ctx, fastd_config_t *conf) {
	conf->machine_readable = true;
}


static bool config_match(const char *opt, ...) {
	va_list ap;
	bool match = false;
	const char *str;

	va_start(ap, opt);

	while((str = va_arg(ap, const char*)) != NULL) {
		if (strcmp(opt, str) == 0) {
			match = true;
			break;
		}
	}

	va_end(ap);

	return match;
}

void fastd_config_handle_options(fastd_context_t *ctx, fastd_config_t *conf, int argc, char *const argv[]) {
	int i = 1;

	while (i < argc) {
#define OR ,
#define OPTION(func, options, message)					\
		({							\
			if(config_match(argv[i], options, NULL)) {	\
				i++;					\
				func(ctx, conf);			\
				continue;				\
			}						\
		})
#define OPTION_ARG(func, options, arg, message)				\
		({							\
			if(config_match(argv[i], options, NULL)) {	\
				i+=2;					\
				if (i > argc)				\
					exit_error(ctx, "config error: option `%s' needs an argument; see --help for usage", argv[i-2]); \
				func(ctx, conf, argv[i-1]);		\
				continue;				\
			}						\
		})
#include "options.def.h"
#undef OR
#undef OPTION
#undef OPTION_ARG

		exit_error(ctx, "config error: unknown option `%s'; see --help for usage", argv[i]);
	}
}