/*-
 * Copyright (c) 2016, Babak Farrokhi
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <sys/cdefs.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/time.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>

#include <err.h>
#include <errno.h>

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>

#if (__STDC_VERSION__ >= 199901L)
#include <stdint.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>
#define SYSLOG_NAMES
#include <syslog.h>
#include <pthread.h>
#include <pidutil.h>

#include "logfile.h"

#define	PORT 5060
#define BACKLOG 1024

#ifndef IPV6_BINDV6ONLY			/* Linux does not have IPV6_BINDV6ONLY */
#define IPV6_BINDV6ONLY IPV6_V6ONLY
#endif					/* IPV6_BINDV6ONLY */

/* bzero is deprecated */
#define bzero(b,len) (memset((b), '\0', (len)), (void) 0)

/*
 * Globals
 */
log_t  *lfh;
struct pidfh *pfh;
char   *pidfilename = NULL;
bool	use_syslog = false;
char   *logfilename = NULL;
int	syslog_pri = -1;

struct sockaddr_in t_sa, u_sa;
int	t_sockfd, u_sockfd;

#ifdef PF_INET6
struct sockaddr_in6 t6_sa, u6_sa;
int	t6_sockfd, u6_sockfd;

#endif					/* PF_INET6 */

/*
 * trim string from whitespace characters
 */
size_t
chomp(char *s)
{
	int i;

	/* trim leading spaces */
	while (isspace(*s))
		s++;

	/* All spaces? */
	if (*s == 0)
		return 0;

	/* trim trailing spaces */
	i = strlen(s);

	while ((i > 0) && (isspace(s[i - 1])))
		i--;

	s[i] = '\0';

	return i;
}

/*
 * search and replace function
 * http://creativeandcritical.net/str-replace-c
 */
char *repl_str(const char *str, const char *from, const char *to) {

	/* Adjust each of the below values to suit your needs. */

	/* Increment positions cache size initially by this number. */
	size_t cache_sz_inc = 16;
	/* Thereafter, each time capacity needs to be increased,
	 * multiply the increment by this factor. */
	const size_t cache_sz_inc_factor = 3;
	/* But never increment capacity by more than this number. */
	const size_t cache_sz_inc_max = 1048576;

	char *pret, *ret = NULL;
	const char *pstr2, *pstr = str;
	size_t i, count = 0;
	#if (__STDC_VERSION__ >= 199901L)
	uintptr_t *pos_cache_tmp, *pos_cache = NULL;
	#else
	ptrdiff_t *pos_cache_tmp, *pos_cache = NULL;
	#endif
	size_t cache_sz = 0;
	size_t cpylen, orglen, retlen, tolen, fromlen = strlen(from);

	/* Find all matches and cache their positions. */
	while ((pstr2 = strstr(pstr, from)) != NULL) {
		count++;

		/* Increase the cache size when necessary. */
		if (cache_sz < count) {
			cache_sz += cache_sz_inc;
			pos_cache_tmp = realloc(pos_cache, sizeof(*pos_cache) * cache_sz);
			if (pos_cache_tmp == NULL) {
				goto end_repl_str;
			} else pos_cache = pos_cache_tmp;
			cache_sz_inc *= cache_sz_inc_factor;
			if (cache_sz_inc > cache_sz_inc_max) {
				cache_sz_inc = cache_sz_inc_max;
			}
		}

		pos_cache[count-1] = pstr2 - str;
		pstr = pstr2 + fromlen;
	}

	orglen = pstr - str + strlen(pstr);

	/* Allocate memory for the post-replacement string. */
	if (count > 0) {
		tolen = strlen(to);
		retlen = orglen + (tolen - fromlen) * count;
	} else	retlen = orglen;
	ret = malloc(retlen + 1);
	if (ret == NULL) {
		goto end_repl_str;
	}

	if (count == 0) {
		/* If no matches, then just duplicate the string. */
		strcpy(ret, str);
	} else {
		/* Otherwise, duplicate the string whilst performing
		 * the replacements using the position cache. */
		pret = ret;
		memcpy(pret, str, pos_cache[0]);
		pret += pos_cache[0];
		for (i = 0; i < count; i++) {
			memcpy(pret, to, tolen);
			pret += tolen;
			pstr = str + pos_cache[i] + fromlen;
			cpylen = (i == count-1 ? orglen : pos_cache[i+1]) - pos_cache[i] - fromlen;
			memcpy(pret, pstr, cpylen);
			pret += cpylen;
		}
		ret[retlen] = '\0';
	}

end_repl_str:
	/* Free the cache and return the post-replacement string,
	 * which will be NULL in the event of an error. */
	free(pos_cache);
	return ret;
}

/*
 * Prepare for a clean shutdown
 */
void
daemon_shutdown()
{
	pidfile_remove(pfh);
	if (pidfilename != NULL)
		free(pidfilename);

	if (!use_syslog) {
		log_close(lfh);
		free(logfilename);
	}
}

/*
 * Act upon receiving signals
 */
void
signal_handler(int sig)
{
	switch (sig) {

	case SIGHUP:
		if (!use_syslog)
			log_reopen(&lfh);	/* necessary for log file
						 * rotation */
		break;
	case SIGINT:
	case SIGTERM:
		daemon_shutdown();
		exit(EXIT_SUCCESS);
		break;
	default:
		break;
	}
}

void
process_request(int af, struct sockaddr *src, int proto, char *str)
{
	char *logmsg = NULL;
	char *p_names[] = {"TCP", "UDP", "RAW", "UNKNOWN"};
	char *pname;
	u_int port;
	char addr_str[46];
	struct sockaddr_in *s_in;

#ifdef PF_INET6
	struct sockaddr_in6 *s_in6;

#endif					/* PF_INET6 */

	switch (proto) {
	case SOCK_STREAM:
		pname = p_names[0];
		break;
	case SOCK_DGRAM:
		pname = p_names[1];
		break;
	case SOCK_RAW:
		pname = p_names[2];
		break;
	default:
		pname = p_names[3];;
	}

	chomp(str);

	logmsg = repl_str(str, "\r\n", "|");    /* sipvicious uses \r\n */
	logmsg = repl_str(logmsg, "\n", "|");   /* CiscoUCSM uses \n */
	if (logmsg == NULL)
		return;
	
#ifdef PF_INET6
	switch (af) {
	case AF_INET6:
		s_in6 = (struct sockaddr_in6 *)src;
		inet_ntop(af, &s_in6->sin6_addr, addr_str, sizeof(addr_str));
		port = ntohs(s_in6->sin6_port);
		if (use_syslog) {
			syslog(syslog_pri, "saddr: %s; sport: %d; proto: %s6; content: %s", addr_str, port, pname, logmsg);
		} else {
			log_printf(lfh, "%ld,%s6,%s,%d,%s", time(NULL), pname, addr_str, port, logmsg);
		}
		break;
	case AF_INET:
		s_in = (struct sockaddr_in *)src;
		if (use_syslog) {
			syslog(syslog_pri, "saddr: %s; sport: %d; proto: %s4; content: %s", inet_ntoa(s_in->sin_addr), ntohs(s_in->sin_port), pname, logmsg);
		} else {
			log_printf(lfh, "%ld,%s4,%s,%d,%s", time(NULL), pname, inet_ntoa(s_in->sin_addr), ntohs(s_in->sin_port), logmsg);
		}
		break;
	}
#else
	s_in = (struct sockaddr_in *)src;
	if (use_syslog) {
		syslog(syslog_pri, "saddr: %s; sport: :%d; proto: %s4; content: %s", inet_ntoa(s_in->sin_addr), ntohs(s_in->sin_port), pname, logmsg);
	} else {
		log_printf(lfh, "%ld,%s4,%s,%d,%s", time(NULL), pname, inet_ntoa(s_in->sin_addr), ntohs(s_in->sin_port), logmsg);
	}
#endif

	free(logmsg);
}

/*
 * setup TCP listener socket
 */
int
init_tcp()
{

#ifdef PF_INET6
	/* Setup TCP6 Listener */
	bzero(&t6_sa, sizeof(t6_sa));
	t6_sa.sin6_port = htons(PORT);
	t6_sa.sin6_family = AF_INET6;
	t6_sa.sin6_addr = in6addr_any;
	t6_sa.sin6_scope_id = 0;
	if ((t6_sockfd = socket(PF_INET6, SOCK_STREAM, 0)) < 0) {
		perror("tcp6 socket()");
		return (EXIT_FAILURE);
	}
	int on = 1;

	setsockopt(t6_sockfd, IPPROTO_IPV6, IPV6_BINDV6ONLY, (char *)&on, sizeof(on));

	if (bind(t6_sockfd, (struct sockaddr *)&t6_sa, sizeof(t6_sa)) < 0) {
		perror("tcp6 bind()");
		return (EXIT_FAILURE);
	}
#endif					/* PF6_INET */

	/* Setup TCP4 Listener */
	bzero(&t_sa, sizeof(t_sa));
	t_sa.sin_port = htons(PORT);
	t_sa.sin_family = AF_INET;
	t_sa.sin_addr.s_addr = htonl(INADDR_ANY);
	if ((t_sockfd = socket(PF_INET, SOCK_STREAM, 0)) < 0) {
		perror("tcp4 socket()");
		return (EXIT_FAILURE);
	}
	if (bind(t_sockfd, (struct sockaddr *)&t_sa, sizeof(t_sa)) < 0) {
		perror("tcp4 bind()");
		return (EXIT_FAILURE);
	}
	return (EXIT_SUCCESS);
}

/*
 * setup UDP listener socket
 */
int
init_udp()
{

#ifdef PF_INET6

	/* Setup UDP6 Listener */
	bzero(&u6_sa, sizeof(u6_sa));
	u6_sa.sin6_port = htons(PORT);
	u6_sa.sin6_family = AF_INET6;
	u6_sa.sin6_addr = in6addr_any;
	u6_sa.sin6_scope_id = 0;
	if ((u6_sockfd = socket(PF_INET6, SOCK_DGRAM, 0)) < 0) {
		perror("udp6 socket()");
		return (EXIT_FAILURE);
	}
	int on = 1;

	setsockopt(u6_sockfd, IPPROTO_IPV6, IPV6_BINDV6ONLY, (char *)&on, sizeof(on));

	if (bind(u6_sockfd, (struct sockaddr *)&u6_sa, sizeof(u6_sa)) < 0) {
		perror("udp6 bind()");
		return (EXIT_FAILURE);
	}
#endif					/* PF_INET6 */

	/* Setup UDP4 Listener */
	bzero(&u_sa, sizeof(u_sa));
	u_sa.sin_port = htons(PORT);
	u_sa.sin_family = AF_INET;
	u_sa.sin_addr.s_addr = htonl(INADDR_ANY);
	if ((u_sockfd = socket(PF_INET, SOCK_DGRAM, 0)) < 0) {
		perror("udp4 socket()");
		return (EXIT_FAILURE);
	}
	if (bind(u_sockfd, (struct sockaddr *)&u_sa, sizeof(u_sa)) < 0) {
		perror("udp4 bind()");
		return (EXIT_FAILURE);
	}
	return (EXIT_SUCCESS);
}

void   *
tcp4_handler(void *args)
{
	int c;
	struct sockaddr_in t_other;
	FILE *client;
	char str[8192];
	socklen_t sa_len;

	listen(t_sockfd, BACKLOG);

	while (1) {
		sa_len = sizeof(t_sa);
		if ((c = accept(t_sockfd, (struct sockaddr *)&t_other, &sa_len)) < 0) {
			perror("tcp accept()");
			pthread_exit(NULL);
		}
		if ((client = fdopen(c, "r")) == NULL) {
			perror("tcp fdopen()");
			pthread_exit(NULL);
		}
		bzero(str, sizeof(str));/* just in case */
		fgets(str, sizeof(str), client);
		process_request(t_other.sin_family, (struct sockaddr *)&t_other, SOCK_STREAM, str);
		fclose(client);
	}
	return (args);			/* suppress compiler warning */
}

void   *
udp4_handler(void *args)
{
	char str[8192];
	struct sockaddr_in u_other;
	socklen_t sa_len;
	ssize_t len;

	sa_len = sizeof(u_other);
	while (1) {
		bzero(str, sizeof(str));/* just in case */
		if ((len = recvfrom(u_sockfd, str, sizeof(str), 0, (struct sockaddr *)&u_other, &sa_len)) > 0) {
			process_request(u_other.sin_family, (struct sockaddr *)&u_other, SOCK_DGRAM, str);
		}
	}

	return (args);			/* suppress compiler warning */
}

#ifdef PF_INET6

void   *
tcp6_handler(void *args)
{
	int c;
	struct sockaddr_in6 t_other;
	FILE *client;
	char str[8192];
	socklen_t sa_len;

	listen(t6_sockfd, BACKLOG);

	while (1) {
		sa_len = sizeof(t6_sa);
		if ((c = accept(t6_sockfd, (struct sockaddr *)&t_other, &sa_len)) < 0) {
			perror("tcp6 accept()");
			pthread_exit(NULL);
		}
		if ((client = fdopen(c, "r")) == NULL) {
			perror("tcp6 fdopen()");
			pthread_exit(NULL);
		}
		bzero(str, sizeof(str));/* just in case */
		fgets(str, sizeof(str), client);
		process_request(t_other.sin6_family, (struct sockaddr *)&t_other, SOCK_STREAM, str);
		fclose(client);
	}
	return (args);			/* suppress compiler warning */
}

void   *
udp6_handler(void *args)
{
	char str[8192];
	struct sockaddr_in6 u_other;
	socklen_t sa_len;
	ssize_t len;

	sa_len = sizeof(u_other);
	while (1) {
		bzero(str, sizeof(str));/* just in case */
		if ((len = recvfrom(u6_sockfd, str, sizeof(str), 0, (struct sockaddr *)&u_other, &sa_len)) > 0) {
			process_request(u_other.sin6_family, (struct sockaddr *)&u_other, SOCK_DGRAM, str);
		}
	}

	return (args);			/* suppress compiler warning */
}

#endif					/* PF_INET6 */

void
init_logger()
{
	if (use_syslog) {
		/* initialize facility and level parameters */
		if (syslog_pri == -1)	/* not specidied by user, use default */
			syslog_pri = LOG_USER | LOG_NOTICE | LOG_PID;
	} else {
		/* open a log file in current directory */
		if (logfilename == NULL)
			logfilename = strdup("fsipd.log");
		if ((lfh = log_open(logfilename, 0644)) == NULL)
			err(EXIT_FAILURE, "Cannot open log file \"%s\"", logfilename);
	}
}

/*
 * Daemonize and persist pid
 */
int
daemon_start()
{
	struct sigaction sig_action;
	sigset_t sig_set;
	pid_t otherpid;
	int curPID;
	pthread_t tcp4_thread, udp4_thread;
	pthread_t tcp6_thread, udp6_thread;

	/* Check if we can acquire the pid file */
	pfh = pidfile_open(pidfilename, 0644, &otherpid);

	if (pfh == NULL) {
		if (errno == EEXIST) {
			errx(EXIT_FAILURE, "Daemon already running, pid: %jd.", (intmax_t)otherpid);
		}
		err(EXIT_FAILURE, "Cannot open or create pidfile");
	}
	init_logger();

	/* Initialize TCP46 and UDP46 sockets */
	if (init_tcp() == EXIT_FAILURE)
		return (EXIT_FAILURE);
	if (init_udp() == EXIT_FAILURE)
		return (EXIT_FAILURE);

	/* start daemonizing */
	curPID = fork();

	switch (curPID) {
	case 0:			/* This process is the child */
		break;
	case -1:			/* fork() failed, should exit */
		perror("fork");
		return (EXIT_FAILURE);
	default:			/* fork() successful, should exit
					 * (parent) */
		return (EXIT_SUCCESS);
	}

	/* we are the child, complete the daemonization */

	/* Close standard IO */
	fclose(stdin);
	fclose(stdout);
	fclose(stderr);

	/* Block unnecessary signals */
	sigemptyset(&sig_set);
	sigaddset(&sig_set, SIGCHLD);	/* ignore child - i.e. we don't need
					 * to wait for it */
	sigaddset(&sig_set, SIGTSTP);	/* ignore tty stop signals */
	sigaddset(&sig_set, SIGTTOU);	/* ignore tty background writes */
	sigaddset(&sig_set, SIGTTIN);	/* ignore tty background reads */
	sigprocmask(SIG_BLOCK, &sig_set, NULL);	/* Block the above specified
						 * signals */

	/* Catch necessary signals */
	sig_action.sa_handler = signal_handler;
	sigemptyset(&sig_action.sa_mask);
	sig_action.sa_flags = 0;

	sigaction(SIGTERM, &sig_action, NULL);
	sigaction(SIGHUP, &sig_action, NULL);
	sigaction(SIGINT, &sig_action, NULL);

	/* create new session and process group */
	setsid();

	/* persist pid */
	pidfile_write(pfh);

	/* Create TCP and UDP listener threads */
	pthread_create(&tcp4_thread, NULL, tcp4_handler, NULL);
	pthread_create(&udp4_thread, NULL, udp4_handler, NULL);
#ifdef PF_INET6
	pthread_create(&tcp6_thread, NULL, tcp6_handler, NULL);
	pthread_create(&udp6_thread, NULL, udp6_handler, NULL);
#endif

	/*
	 * Wait for threads to terminate, which normally shouldn't ever
	 * happen
	 */
	pthread_join(tcp4_thread, NULL);
	pthread_join(udp4_thread, NULL);
#ifdef PF_INET6
	pthread_join(tcp6_thread, NULL);
	pthread_join(udp6_thread, NULL);
#endif

	return (EXIT_SUCCESS);
}

void
usage()
{
	printf("usage: fsipd [-h] [-g pidfile] [-l logfile] [-s] [-p priority] \n");
	printf("\t-h: this message\n");
	printf("\t-g: specify pid filename (default: /var/run/fsipd.pid\n");
	printf("\t-l: specify output log filename (default: fsipd.log)\n");
	printf("\t-s: use syslog instead of local log file\n");
	printf("\t-p: syslog priority and level (default: user.notice)\n");
}

static int
decode(char *name, const CODE * codetab)
{
	const CODE *c;

	if (isdigit(*name))
		return (atoi(name));

	for (c = codetab; c->c_name; c++)
		if (!strcasecmp(name, c->c_name))
			return (c->c_val);

	return (-1);
}

static int
decodepri(char *s)
{
	char *save;
	int fac, lev;

	for (save = s; *s && *s != '.'; ++s);
	if (*s) {
		*s = '\0';
		fac = decode(save, facilitynames);
		if (fac < 0)
			errx(1, "unknown facility name: %s", save);
		*s++ = '.';
	} else {
		fac = 0;
		s = save;
	}
	lev = decode(s, prioritynames);
	if (lev < 0)
		errx(1, "unknown priority name: %s", save);
	return ((lev & LOG_PRIMASK) | (fac & LOG_FACMASK));
}

int
main(int argc, char *argv[])
{
	int opt;

	while ((opt = getopt(argc, argv, "g:hl:sp:")) != -1) {
		switch (opt) {
		case 's':
			use_syslog = true;
			break;
		case 'p':
			if (use_syslog) {
				syslog_pri = decodepri(optarg) | LOG_PID;
			} else {
				errx(EX_USAGE, "you need to specify \"-s\".");
			}
			break;
		case 'l':
			logfilename = strdup(optarg);
			break;
		case 'g':
			pidfilename = strdup(optarg);
			break;
		case 'h':
			usage();
			exit(0);
			break;
		default:
			printf("invalid option: %c\n", opt);
			usage();
			exit(1);
		}
	}

	return (daemon_start());
}
