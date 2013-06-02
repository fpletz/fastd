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


#define _GNU_SOURCE

#include "fastd.h"
#include "peer.h"
#include <config.ll.h>
#include <config.yy.h>

#include <dirent.h>
#include <grp.h>
#include <libgen.h>
#include <pwd.h>
#include <stdarg.h>
#include <strings.h>

#include <arpa/inet.h>

#include <sys/stat.h>
#include <sys/types.h>


extern const fastd_protocol_t fastd_protocol_ec25519_fhmqvc;

extern const fastd_method_t fastd_method_null;

#ifdef WITH_METHOD_XSALSA20_POLY1305
extern const fastd_method_t fastd_method_xsalsa20_poly1305;
#endif
#ifdef WITH_METHOD_AES128_GCM
extern const fastd_method_t fastd_method_aes128_gcm;
#endif


#ifdef USE_CRYPTO_AES128CTR
#ifdef WITH_CRYPTO_AES128CTR_NACL
extern const fastd_crypto_aes128ctr_t fastd_crypto_aes128ctr_nacl;
#endif
#ifdef WITH_CRYPTO_AES128CTR_LINUX
extern const fastd_crypto_aes128ctr_t fastd_crypto_aes128ctr_linux;
#endif

#ifdef WITH_CRYPTO_AES128CTR_NACL
static const fastd_crypto_aes128ctr_t *fastd_crypto_aes128ctr_default = &fastd_crypto_aes128ctr_nacl;
#else
static const fastd_crypto_aes128ctr_t *fastd_crypto_aes128ctr_default = &fastd_crypto_aes128ctr_linux;
#endif

#endif

#ifdef USE_CRYPTO_GHASH
#ifdef WITH_CRYPTO_GHASH_BUILTIN
extern const fastd_crypto_ghash_t fastd_crypto_ghash_builtin;
#endif
#ifdef WITH_CRYPTO_GHASH_LINUX
extern const fastd_crypto_ghash_t fastd_crypto_ghash_linux;
#endif

#ifdef WITH_CRYPTO_GHASH_BUILTIN
static const fastd_crypto_ghash_t *fastd_crypto_ghash_default = &fastd_crypto_ghash_builtin;
#else
static const fastd_crypto_ghash_t *fastd_crypto_ghash_default = &fastd_crypto_ghash_linux;
#endif

#endif

static void default_config(fastd_config_t *conf) {
	memset(conf, 0, sizeof(fastd_config_t));

	conf->log_stderr_level = -1;
	conf->log_syslog_level = -1;
	conf->log_syslog_ident = strdup("fastd");

	conf->keepalive_interval = 20;
	conf->peer_stale_time = 90;
	conf->eth_addr_stale_time = 300;

	conf->reorder_count = 64;
	conf->reorder_time = 10;

	conf->min_handshake_interval = 15;
	conf->min_resolve_interval = 15;

	conf->mtu = 1500;
	conf->mode = MODE_TAP;

	conf->drop_caps = DROP_CAPS_ON;

	conf->protocol = &fastd_protocol_ec25519_fhmqvc;
	conf->method_default = &fastd_method_null;
	conf->key_valid = 3600;		/* 60 minutes */
	conf->key_refresh = 3300;	/* 55 minutes */
	conf->key_refresh_splay = 300;	/* 5 minutes */

#ifdef USE_CRYPTO_AES128CTR
	conf->crypto_aes128ctr = fastd_crypto_aes128ctr_default;
#endif
#ifdef USE_CRYPTO_GHASH
	conf->crypto_ghash = fastd_crypto_ghash_default;
#endif

	conf->peer_group = calloc(1, sizeof(fastd_peer_group_config_t));
	conf->peer_group->name = strdup("default");
	conf->peer_group->max_connections = -1;
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

bool fastd_config_protocol(fastd_context_t *ctx, fastd_config_t *conf, const char *name) {
	if (!strcmp(name, "ec25519-fhmqvc"))
		conf->protocol = &fastd_protocol_ec25519_fhmqvc;
	else
		return false;

	return true;
}

static inline const fastd_method_t* parse_method_name(const char *name) {
	if (!strcmp(name, "null"))
		return &fastd_method_null;
#ifdef WITH_METHOD_XSALSA20_POLY1305
	else if (!strcmp(name, "xsalsa20-poly1305"))
		return &fastd_method_xsalsa20_poly1305;
#endif
#ifdef WITH_METHOD_AES128_GCM
	else if (!strcmp(name, "aes128-gcm"))
		return &fastd_method_aes128_gcm;
#endif
	else
		return NULL;
}

bool fastd_config_method(fastd_context_t *ctx, fastd_config_t *conf, const char *name) {
	const fastd_method_t *method = parse_method_name(name);

	if (!method)
		return false;

	conf->method_default = method;

	int i;
	for (i = 0; i < MAX_METHODS; i++) {
		if (conf->methods[i] == method)
			return true;

		if (conf->methods[i] == NULL) {
			conf->methods[i] = method;
			return true;
		}
	}

	exit_bug(ctx, "MAX_METHODS too low");
}

bool fastd_config_crypto(fastd_context_t *ctx, fastd_config_t *conf, const char *alg, const char *impl) {
#ifdef USE_CRYPTO_AES128CTR
	if (!strcasecmp(alg, "aes128-ctr") || !strcasecmp(alg, "aes128") || !strcasecmp(alg, "aes-ctr") || !strcasecmp(alg, "aes")) {
		if (!strcasecmp(impl, "default"))
			conf->crypto_aes128ctr = fastd_crypto_aes128ctr_default;
#ifdef WITH_CRYPTO_AES128CTR_NACL
		else if (!strcasecmp(impl, "nacl"))
			conf->crypto_aes128ctr = &fastd_crypto_aes128ctr_nacl;
#endif
#ifdef WITH_CRYPTO_AES128CTR_LINUX
		else if (!strcasecmp(impl, "linux"))
			conf->crypto_aes128ctr = &fastd_crypto_aes128ctr_linux;
#endif
		else
			return false;

		return true;
	}
	else
#endif
#ifdef USE_CRYPTO_GHASH
	if (!strcasecmp(alg, "ghash")) {
		if (!strcasecmp(impl, "default"))
			conf->crypto_ghash = fastd_crypto_ghash_default;
#ifdef WITH_CRYPTO_GHASH_BUILTIN
		else if (!strcasecmp(impl, "builtin"))
			conf->crypto_ghash = &fastd_crypto_ghash_builtin;
#endif
#ifdef WITH_CRYPTO_GHASH_LINUX
		else if (!strcasecmp(impl, "linux"))
			conf->crypto_ghash = &fastd_crypto_ghash_linux;
#endif
		else
			return false;

		return true;
	}
	else
#endif
	return false;
}

void fastd_config_bind_address(fastd_context_t *ctx, fastd_config_t *conf, const fastd_peer_address_t *address, const char *bindtodev, bool default_v4, bool default_v6) {
	fastd_bind_address_t *addr = malloc(sizeof(fastd_bind_address_t));
	addr->next = conf->bind_addrs;
	conf->bind_addrs = addr;
	conf->n_bind_addrs++;

	addr->addr = *address;
	addr->bindtodev = bindtodev ? strdup(bindtodev) : NULL;

	fastd_peer_address_simplify(&addr->addr);

	if (addr->addr.sa.sa_family != AF_INET6 && (default_v4 || !conf->bind_addr_default_v4))
		conf->bind_addr_default_v4 = addr;

	if (addr->addr.sa.sa_family != AF_INET && (default_v6 || !conf->bind_addr_default_v6))
		conf->bind_addr_default_v6 = addr;
}

void fastd_config_peer_group_push(fastd_context_t *ctx, fastd_config_t *conf, const char *name) {
	fastd_peer_group_config_t *group = calloc(1, sizeof(fastd_peer_group_config_t));
	group->name = strdup(name);
	group->max_connections = -1;

	group->parent = conf->peer_group;
	group->next = group->parent->children;

	group->parent->children = group;

	conf->peer_group = group;
}

void fastd_config_peer_group_pop(fastd_context_t *ctx, fastd_config_t *conf) {
	conf->peer_group = conf->peer_group->parent;
}

static void free_peer_group(fastd_peer_group_config_t *group) {
	while (group->children) {
		fastd_peer_group_config_t *next = group->children->next;
		free_peer_group(group->children);
		group->children = next;
	}

	fastd_string_stack_free(group->peer_dirs);
	free(group->name);
	free(group);
}

static bool has_peer_group_peer_dirs(const fastd_peer_group_config_t *group) {
	if (group->peer_dirs)
		return true;

	const fastd_peer_group_config_t *child;
	for (child = group->children; child; child = child->next) {
		if (has_peer_group_peer_dirs(child))
			return true;
	}

	return false;
}

bool fastd_config_add_log_file(fastd_context_t *ctx, fastd_config_t *conf, const char *name, int level) {
	char *name2 = strdup(name);
	char *name3 = strdup(name);

	char *dir = dirname(name2);
	char *base = basename(name3);

	char *oldcwd = get_current_dir_name();

	if (!chdir(dir)) {
		char *logdir = get_current_dir_name();

		fastd_log_file_t *file = malloc(sizeof(fastd_log_file_t));
		file->filename = malloc(strlen(logdir) + 1 + strlen(base) + 1);

		strcpy(file->filename, logdir);
		strcat(file->filename, "/");
		strcat(file->filename, base);

		file->level = level;

		file->next = conf->log_files;
		conf->log_files = file;

		if(chdir(oldcwd))
			pr_error(ctx, "can't chdir to `%s': %s", oldcwd, strerror(errno));

		free(logdir);
	}
	else {
		pr_error(ctx, "change from directory `%s' to `%s' failed: %s", oldcwd, dir, strerror(errno));
	}

	free(oldcwd);
	free(name2);
	free(name3);

	return true;
}

static void read_peer_dir(fastd_context_t *ctx, fastd_config_t *conf, const char *dir) {
	DIR *dirh = opendir(".");

	if (dirh) {
		while (true) {
			struct dirent entry, *result;
			int ret;

			ret = readdir_r(dirh, &entry, &result);
			if (ret) {
				pr_error(ctx, "readdir_r: %s", strerror(ret));
				break;
			}

			if (!result)
				break;
			if (result->d_name[0] == '.')
				continue;

			if (result->d_name[strlen(result->d_name)-1] == '~') {
				pr_verbose(ctx, "ignoring file `%s' as it seems to be a backup file", result->d_name);
				continue;
			}

			struct stat statbuf;
			if (stat(result->d_name, &statbuf)) {
				pr_warn(ctx, "ignoring file `%s': stat failed: %s", result->d_name, strerror(errno));
				continue;
			}
			if ((statbuf.st_mode & S_IFMT) != S_IFREG) {
				pr_info(ctx, "ignoring file `%s': no regular file", result->d_name);
				continue;
			}

			fastd_peer_config_new(ctx, conf);
			conf->peers->name = strdup(result->d_name);
			conf->peers->config_source_dir = dir;

			if (!fastd_read_config(ctx, conf, result->d_name, true, 0)) {
				pr_warn(ctx, "peer config `%s' will be ignored", result->d_name);
				fastd_peer_config_delete(ctx, conf);
			}
		}

		closedir(dirh);
	}
	else {
		pr_error(ctx, "opendir for `%s' failed: %s", dir, strerror(errno));
	}
}

static void read_peer_dirs(fastd_context_t *ctx, fastd_config_t *conf) {
	char *oldcwd = get_current_dir_name();

	fastd_string_stack_t *dir;
	for (dir = conf->peer_group->peer_dirs; dir; dir = dir->next) {
		if (!chdir(dir->str))
			read_peer_dir(ctx, conf, dir->str);
		else
			pr_error(ctx, "change from directory `%s' to `%s' failed: %s", oldcwd, dir->str, strerror(errno));
	}

	if (chdir(oldcwd))
		pr_error(ctx, "can't chdir to `%s': %s", oldcwd, strerror(errno));

	free(oldcwd);
}

void fastd_add_peer_dir(fastd_context_t *ctx, fastd_config_t *conf, const char *dir) {
	char *oldcwd = get_current_dir_name();

	if (!chdir(dir)) {
		char *newdir = get_current_dir_name();
		conf->peer_group->peer_dirs = fastd_string_stack_push(conf->peer_group->peer_dirs, newdir);
		free(newdir);

		if(chdir(oldcwd))
			pr_error(ctx, "can't chdir to `%s': %s", oldcwd, strerror(errno));
	}
	else {
		pr_error(ctx, "change from directory `%s' to `%s' failed: %s", oldcwd, dir, strerror(errno));
	}

	free(oldcwd);
}

bool fastd_read_config(fastd_context_t *ctx, fastd_config_t *conf, const char *filename, bool peer_config, int depth) {
	if (depth >= MAX_CONFIG_DEPTH)
		exit_error(ctx, "maximum config include depth exceeded");

	bool ret = true;
	char *oldcwd = get_current_dir_name();
	char *filename2 = NULL;
	char *dir = NULL;
	FILE *file;
	yyscan_t scanner;
	fastd_config_pstate *ps;
	fastd_string_stack_t *strings = NULL;

	fastd_config_yylex_init(&scanner);
	ps = fastd_config_pstate_new();

	if (!filename) {
		file = stdin;
	}
	else {
		file = fopen(filename, "r");
		if (!file) {
			pr_error(ctx, "can't open config file `%s': %s", filename, strerror(errno));
			ret = false;
			goto end_free;
		}
	}

	fastd_config_yyset_in(file, scanner);

	if (filename) {
		filename2 = strdup(filename);
		dir = dirname(filename2);

		if (chdir(dir)) {
			pr_error(ctx, "change from directory `%s' to `%s' failed", oldcwd, dir);
			ret = false;
			goto end_free;
		}
	}

	int token;
	YYSTYPE token_val;
	YYLTYPE loc = {1, 0, 1, 0};

	if (peer_config)
		token = START_PEER_CONFIG;
	else
		token = conf->peer_group->parent ? START_PEER_GROUP_CONFIG : START_CONFIG;

	int parse_ret = fastd_config_push_parse(ps, token, &token_val, &loc, ctx, conf, filename, depth+1);

	while(parse_ret == YYPUSH_MORE) {
		token = fastd_config_yylex(&token_val, &loc, scanner);

		if (token < 0) {
			pr_error(ctx, "config error: %s at %s:%i:%i", token_val.error, filename, loc.first_line, loc.first_column);
			ret = false;
			goto end_free;
		}

		if (token == TOK_STRING) {
			token_val.str->next = strings;
			strings = token_val.str;
		}

		parse_ret = fastd_config_push_parse(ps, token, &token_val, &loc, ctx, conf, filename, depth+1);
	}

	if (parse_ret)
		ret = false;

 end_free:
	fastd_string_stack_free(strings);

	fastd_config_pstate_delete(ps);
	fastd_config_yylex_destroy(scanner);

	if(chdir(oldcwd))
		pr_error(ctx, "can't chdir to `%s': %s", oldcwd, strerror(errno));

	free(filename2);
	free(oldcwd);

	if (filename && file)
		fclose(file);

	return ret;
}

static void count_peers(fastd_context_t *ctx, fastd_config_t *conf) {
	conf->n_floating = 0;
	conf->n_v4 = 0;
	conf->n_v6 = 0;
	conf->n_dynamic = 0;
	conf->n_dynamic_v4 = 0;
	conf->n_dynamic_v6 = 0;

	fastd_peer_config_t *peer;
	for (peer = conf->peers; peer; peer = peer->next) {
		switch (peer->address.sa.sa_family) {
		case AF_UNSPEC:
			if (peer->hostname)
				conf->n_dynamic++;
			else
				conf->n_floating++;
			break;

		case AF_INET:
			if (peer->hostname)
				conf->n_dynamic_v4++;
			else
				conf->n_v4++;
			break;

		case AF_INET6:
			if (peer->hostname)
				conf->n_dynamic_v6++;
			else
				conf->n_v6++;
			break;

		default:
			exit_bug(ctx, "invalid peer address family");
		}
	}
}


#define OPTIONS \
	OPTION(usage, "--help" OR "-h", "Shows this help text") \
	OPTION(version, "--version" OR "-v", "Shows the fastd version") \
	OPTION(option_daemon, "--daemon" OR "-d", "Runs fastd in the background") \
	OPTION_ARG(option_user, "--user", "<user>", "Sets the user to run fastd as") \
	OPTION_ARG(option_group, "--group", "<group>", "Sets the group to run fastd as") \
	OPTION_ARG(option_pid_file, "--pid-file", "<filename>", "Writes fastd's PID to the specified file") \
	OPTION_ARG(option_log_level, "--log-level", "error|warn|info|verbose|debug", "Sets the stderr log level; default is info, if no alternative log destination is configured") \
	OPTION_ARG(option_syslog_level, "--syslog-level", "error|warn|info|verbose|debug", "Sets the log level for syslog output; default is not to use syslog") \
	OPTION_ARG(option_syslog_ident, "--syslog-ident", "<ident>", "Sets the syslog identification; default is 'fastd'") \
	OPTION(option_hide_ip_addresses, "--hide-ip-addresses", "Hides IP addresses in log output") \
	OPTION(option_hide_mac_addresses, "--hide-mac-addresses", "Hides MAC addresses in log output") \
	OPTION_ARG(option_config, "--config" OR "-c", "<filename>", "Loads a config file") \
	OPTION_ARG(option_config_peer, "--config-peer", "<filename>", "Loads a config file for a single peer") \
	OPTION_ARG(option_config_peer_dir, "--config-peer-dir", "<dir>", "Loads all files from a directory as peer configs") \
	OPTION_ARG(option_mode, "--mode" OR "-m", "tap|tun", "Sets the mode of the interface") \
	OPTION_ARG(option_interface, "--interface" OR "-i", "<name>", "Sets the name of the TUN/TAP interface to use") \
	OPTION_ARG(option_mtu, "--mtu" OR "-M", "<mtu>", "Sets the MTU; must be at least 576") \
	OPTION_ARG(option_bind, "--bind" OR "-b", "<address>:<port>", "Sets the bind address") \
	OPTION_ARG(option_protocol, "--protocol" OR "-p", "<protocol>", "Sets the protocol") \
	OPTION_ARG(option_method, "--method", "<method>", "Sets the encryption method") \
	OPTION(option_forward, "--forward", "Enables forwarding of packets between clients; read the documentation before use!") \
	OPTION_ARG(option_on_up, "--on-up", "<command>", "Sets a shell command to execute after interface creation") \
	OPTION_ARG(option_on_down, "--on-down", "<command>", "Sets a shell command to execute before interface destruction") \
	OPTION_ARG(option_on_establish, "--on-establish", "<command>", "Sets a shell command to execute when a new connection is established") \
	OPTION_ARG(option_on_disestablish, "--on-disestablish", "<command>", "Sets a shell command to execute when a connection is lost") \
	OPTION_ARG(option_on_verify, "--on-verify", "<command>", "Sets a shell command to execute to check an connection attempt by an unknown peer") \
	OPTION(option_generate_key, "--generate-key", "Generates a new keypair") \
	OPTION(option_show_key, "--show-key", "Shows the public key corresponding to the configured secret") \
	OPTION(option_machine_readable, "--machine-readable", "Suppresses output of explaining text in the --show-key and --generate-key commands")


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
#define OR ", "
#define OPTION(func, options, message) print_usage("  " options, message);
#define OPTION_ARG(func, options, arg, message) print_usage("  " options " " arg, message);

	puts("fastd (Fast and Secure Tunnelling Daemon) " FASTD_VERSION " usage:\n");

	OPTIONS
	exit(0);

#undef OR
#undef OPTION
#undef OPTION_ARG
}

static void version(fastd_context_t *ctx, fastd_config_t *conf) {
	puts("fastd " FASTD_VERSION);
	exit(0);
}

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


static void configure_user(fastd_context_t *ctx, fastd_config_t *conf) {
	conf->uid = getuid();
	conf->gid = getgid();

	if (conf->user) {
		struct passwd pwd, *pwdr;
		size_t bufspace = 1024;
		int error;

		do {
			char buf[bufspace];
			error = getpwnam_r(conf->user, &pwd, buf, bufspace, &pwdr);
			bufspace *= 2;
		} while(error == ERANGE);

		if (error)
			exit_errno(ctx, "getpwnam_r");

		if (!pwdr)
			exit_error(ctx, "Unable to find user `%s'.", conf->user);

		conf->uid = pwdr->pw_uid;
		conf->gid = pwdr->pw_gid;
	}

	if (conf->group) {
		struct group grp, *grpr;
		size_t bufspace = 1024;
		int error;

		do {
			char buf[bufspace];
			error = getgrnam_r(conf->group, &grp, buf, bufspace, &grpr);
			bufspace *= 2;
		} while(error == ERANGE);

		if (error)
			exit_errno(ctx, "getgrnam_r");

		if (!grpr)
			exit_error(ctx, "Unable to find group `%s'.", conf->group);

		conf->gid = grpr->gr_gid;
	}

	if (conf->user) {
		int ngroups = 0;
		if (getgrouplist(conf->user, conf->gid, NULL, &ngroups) < 0) {
			/* the user has supplementary groups */

			conf->groups = calloc(ngroups, sizeof(gid_t));
			if (getgrouplist(conf->user, conf->gid, conf->groups, &ngroups) < 0)
				exit_errno(ctx, "getgrouplist");

			conf->n_groups = ngroups;
		}
	}

}

void fastd_configure(fastd_context_t *ctx, fastd_config_t *conf, int argc, char *const argv[]) {
#define OR ,
#define OPTION(func, options, message) \
	if(config_match(argv[i], options, NULL)) {	\
		i++;					\
		func(ctx, conf);			\
		continue;				\
	}
#define OPTION_ARG(func, options, arg, message) 	\
	if(config_match(argv[i], options, NULL)) {	\
		i+=2;					\
		if (i > argc)				\
			exit_error(ctx, "config error: option `%s' needs an argument; see --help for usage", argv[i-2]); \
		func(ctx, conf, argv[i-1]);		\
		continue;				\
	}

	default_config(conf);

	int i = 1;
	while (i < argc) {
		OPTIONS

		exit_error(ctx, "config error: unknown option `%s'; see --help for usage", argv[i]);
	}

	if (conf->log_stderr_level < 0 && conf->log_syslog_level < 0 && !conf->log_files)
		conf->log_stderr_level = FASTD_DEFAULT_LOG_LEVEL;

	if (!conf->methods[0])
		conf->methods[0] = conf->method_default;

	if (conf->generate_key || conf->show_key)
		return;

	if (conf->mode == MODE_TUN) {
		if (!conf->peers || conf->peers->next)
			exit_error(ctx, "config error: for tun mode exactly one peer must be configured");
		if (has_peer_group_peer_dirs(conf->peer_group))
			exit_error(ctx, "config error: for tun mode peer directories can't be used");
	}

	if (!conf->peers && !has_peer_group_peer_dirs(conf->peer_group))
		exit_error(ctx, "config error: neither fixed peers nor peer dirs have been configured");

	configure_user(ctx, conf);

#undef OR
#undef OPTION
#undef OPTION_ARG
}

static void peer_dirs_read_peer_group(fastd_context_t *ctx, fastd_config_t *new_conf) {
	read_peer_dirs(ctx, new_conf);

	fastd_peer_group_config_t *group;
	for (group = new_conf->peer_group->children; group; group = group->next) {
		new_conf->peer_group = group;
		peer_dirs_read_peer_group(ctx, new_conf);
	}
}

static void peer_dirs_handle_old_peers(fastd_context_t *ctx, fastd_peer_config_t **old_peers, fastd_peer_config_t **new_peers) {
	fastd_peer_config_t **peer, **next, **new_peer, **new_next;
	for (peer = old_peers; *peer; peer = next) {
		next = &(*peer)->next;

		/* don't touch statically configured peers */
		if (!(*peer)->config_source_dir)
			continue;

		/* search for each peer in the list of new peers */
		for (new_peer = new_peers; *new_peer; new_peer = new_next) {
			new_next = &(*new_peer)->next;

			if (((*peer)->config_source_dir == (*new_peer)->config_source_dir) && strequal((*peer)->name, (*new_peer)->name)) {
				if (fastd_peer_config_equal(*peer, *new_peer)) {
					pr_verbose(ctx, "peer `%s' unchanged", (*peer)->name);

					fastd_peer_config_t *free_peer = *new_peer;
					*new_peer = *new_next;
					fastd_peer_config_free(free_peer);
					peer = NULL;
				}
				else {
					pr_verbose(ctx, "peer `%s' changed, resetting", (*peer)->name);
					new_peer = NULL;
				}

				break;
			}
		}

		/* no new peer was found, or the old one has changed */
		if (peer && (!new_peer || !*new_peer)) {
			pr_verbose(ctx, "removing peer `%s'", (*peer)->name);

			fastd_peer_config_t *free_peer = *peer;
			*peer = *next;
			next = peer;

			fastd_peer_config_purge(ctx, free_peer);
		}
	}
}

static void peer_dirs_handle_new_peers(fastd_context_t *ctx, fastd_peer_config_t **peers, fastd_peer_config_t *new_peers) {
	fastd_peer_config_t *peer;
	for (peer = new_peers; peer; peer = peer->next) {
		if (peer->next)
			continue;

		peer->next = *peers;
		*peers = new_peers;
		return;
	}
}

void fastd_config_load_peer_dirs(fastd_context_t *ctx, fastd_config_t *conf) {
	fastd_config_t temp_conf;
	temp_conf.peer_group = conf->peer_group;
	temp_conf.peers = NULL;

	peer_dirs_read_peer_group(ctx, &temp_conf);
	peer_dirs_handle_old_peers(ctx, &conf->peers, &temp_conf.peers);
	peer_dirs_handle_new_peers(ctx, &conf->peers, temp_conf.peers);

	count_peers(ctx, conf);
}

void fastd_config_release(fastd_context_t *ctx, fastd_config_t *conf) {
	while (conf->peers)
		fastd_peer_config_delete(ctx, conf);

	while (conf->log_files) {
		fastd_log_file_t *next = conf->log_files->next;
		free(conf->log_files->filename);
		free(conf->log_files);
		conf->log_files = next;
	}

	while (conf->bind_addrs) {
		fastd_bind_address_t *next = conf->bind_addrs->next;
		free(conf->bind_addrs->bindtodev);
		free(conf->bind_addrs);
		conf->bind_addrs = next;
	}

	free_peer_group(conf->peer_group);

	free(conf->user);
	free(conf->group);
	free(conf->groups);
	free(conf->ifname);
	free(conf->secret);
	free(conf->on_up);
	free(conf->on_up_dir);
	free(conf->on_down);
	free(conf->on_down_dir);
	free(conf->on_establish);
	free(conf->on_establish_dir);
	free(conf->on_disestablish);
	free(conf->on_disestablish_dir);
	free(conf->on_verify);
	free(conf->on_verify_dir);
	free(conf->protocol_config);
	free(conf->log_syslog_ident);
}
