/*
 *   Copyright 1996, 1997, 1998, 1999, 2000, 2001, 2002, 2003 Michiel Boland.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or
 *   without modification, are permitted provided that the following
 *   conditions are met:
 *
 *   1. Redistributions of source code must retain the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer.
 *
 *   2. Redistributions in binary form must reproduce the above
 *      copyright notice, this list of conditions and the following
 *      disclaimer in the documentation and/or other materials
 *      provided with the distribution.
 *
 *   3. The name of the author may not be used to endorse or promote
 *      products derived from this software without specific prior
 *      written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY
 *   EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 *   THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 *   PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR
 *   BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 *   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 *   TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *   ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 *   LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 *   IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 *   THE POSSIBILITY OF SUCH DAMAGE.
 */

/* Once Around */

static const char rcsid[] = "$Id$";

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include "mathopd.h"

const char server_version[] = "Mathopd/1.5b10";

volatile sig_atomic_t gotsigterm;
volatile sig_atomic_t gotsighup;
volatile sig_atomic_t gotsigusr1;
volatile sig_atomic_t gotsigusr2;
volatile sig_atomic_t gotsigchld;
volatile sig_atomic_t gotsigquit;
int numchildren;
int debug;
unsigned long fcm; /* should be mode_t */
int stayroot;
int my_pid;

static int am_daemon;
static char *progname;

static const char devnull[] = "/dev/null";

static int mysignal(int sig, void(*f)(int))
{
	struct sigaction act;

	act.sa_handler = f;
	sigemptyset(&act.sa_mask);
	act.sa_flags = 0;
	return sigaction(sig, &act, 0);
}

static void die(const char *t, const char *fmt, ...)
{
	va_list ap;

	if (fmt) {
		fprintf(stderr, "%s: ", progname);
		va_start(ap, fmt);
		vfprintf(stderr, fmt, ap);
		fprintf(stderr, "\n");
		va_end(ap);
	}
	if (t)
		perror(t);
	exit(1);
}

static void startup_server(struct server *s)
{
	int onoff;
	struct sockaddr_in sa;

	s->fd = socket(AF_INET, SOCK_STREAM, 0);
	if (s->fd == -1)
		die("socket", 0);
	onoff = 1;
	if (setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, (char *) &onoff, sizeof onoff) == -1)
		die("setsockopt", "cannot set re-use flag");
	fcntl(s->fd, F_SETFD, FD_CLOEXEC);
	fcntl(s->fd, F_SETFL, O_NONBLOCK);
	memset((char *) &sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr = s->addr;
	sa.sin_port = htons(s->port);
	if (bind(s->fd, (struct sockaddr *) &sa, sizeof sa) == -1)
		die("bind", "cannot start up server at %s port %lu", inet_ntoa(s->addr), s->port);
	if (listen(s->fd, s->backlog) == -1)
		die("listen", 0);
}

static void sighandler(int sig)
{
	switch (sig) {
	case SIGTERM:
	case SIGINT:
		gotsigterm = 1;
		break;
	case SIGHUP:
		gotsighup = 1;
		break;
	case SIGUSR1:
		gotsigusr1 = 1;
		break;
	case SIGUSR2:
		gotsigusr2 = 1;
		break;
	case SIGCHLD:
		gotsigchld = 1;
		break;
	case SIGQUIT:
		gotsigquit = 1;
		break;
	}
}

int main(int argc, char *argv[])
{
	int c, i, n, version, pid_fd, null_fd, tee;
	struct server *s;
	char buf[10];
	struct rlimit rl;
	const char *message;
	const char *config_filename;

	my_pid = getpid();
	progname = argv[0];
	am_daemon = 1;
	version = 0;
	config_filename = 0;
	tee = 0;
	while ((c = getopt(argc, argv, "ndvf:t")) != EOF) {
		switch(c) {
		case 'n':
			am_daemon = 0;
			break;
		case 'd':
			debug = 1;
			break;
		case 'v':
			version = 1;
			break;
		case 'f':
			if (config_filename == 0)
				config_filename = optarg;
			else
				die(0, "You may not specify more than one configuration file.");
			break;
		case 't':
			tee = 1;
			break;
		default:
			die(0, "usage: %s [ -ndvt ] [ -f configuration_file ]", progname);
			break;
		}
	}
	if (version) {
		fprintf(stderr, "%s\n", server_version);
		return 0;
	}
	if (getrlimit(RLIMIT_NOFILE, &rl) == -1)
		die("getrlimit", 0);
	n = rl.rlim_cur = rl.rlim_max;
	setrlimit(RLIMIT_NOFILE, &rl);
	if (am_daemon)
		for (i = 3; i < n; i++)
			close(i);
	null_fd = open(devnull, O_RDWR);
	if (null_fd == -1)
		die("open", "Cannot open %s", devnull);
	while (null_fd < 3) {
		null_fd = dup(null_fd);
		if (null_fd == -1)
			die("dup", 0);
	}
	message = config(config_filename);
	if (message)
		die(0, "%s", message);
	s = servers;
	while (s) {
		startup_server(s);
		s = s->next;
	}
	if (rootdir) {
		if (chroot(rootdir) == -1)
			die("chroot", 0);
		if (chdir("/") == -1)
			die("chdir", 0);
	}
	setuid(geteuid());
	if (geteuid() == 0) {
		if (server_uid == 0)
			die(0, "No user specified.");
		if (setgroups(0, 0) == -1)
			if (setgroups(1, &server_gid) == -1)
				die("setgroups", 0);
		if (setgid(server_gid) == -1)
			die("setgid", 0);
		if (stayroot) {
			if (seteuid(server_uid) == -1)
				die("seteuid", 0);
		} else {
			if (setuid(server_uid) == -1)
				die("setuid", 0);
		}
	}
	if (getrlimit(RLIMIT_CORE, &rl) == -1)
		die("getrlimit", 0);
	if (coredir) {
		rl.rlim_cur = rl.rlim_max;
		if (chdir(coredir) == -1)
			die("chdir", 0);
	} else {
		rl.rlim_cur = 0;
		chdir("/");
	}
	setrlimit(RLIMIT_CORE, &rl);
	umask(fcm);
	if (pid_filename) {
		pid_fd = open(pid_filename, O_WRONLY | O_CREAT, 0666);
		if (pid_fd == -1)
			die("open", "Cannot open PID file");
	} else
		pid_fd = -1;
	if (init_logs(tee) == -1)
		die("open", "Cannot open log files");
	dup2(null_fd, 0);
	dup2(null_fd, 1);
	dup2(null_fd, 2);
	close(null_fd);
	if (am_daemon) {
		if (fork())
			_exit(0);
		setsid();
		if (fork())
			_exit(0);
	}
	mysignal(SIGCHLD, sighandler);
	mysignal(SIGHUP, sighandler);
	mysignal(SIGTERM, sighandler);
	mysignal(SIGINT, sighandler);
	mysignal(SIGQUIT, sighandler);
	mysignal(SIGUSR1, sighandler);
	mysignal(SIGUSR2, sighandler);
	mysignal(SIGPIPE, SIG_IGN);
	my_pid = getpid();
	if (pid_fd != -1) {
		ftruncate(pid_fd, 0);
		sprintf(buf, "%d\n", my_pid);
		write(pid_fd, buf, strlen(buf));
		close(pid_fd);
	}
	if (init_buffers() == -1)
		return 1;
	httpd_main();
	return 0;
}

pid_t spawn(const char *program, char *const argv[], char *const envp[], int fd, int efd, uid_t u, gid_t g, const char *curdir)
{
	pid_t pid;
	sigset_t set, oset;

	sigfillset(&set);
	sigprocmask(SIG_SETMASK, &set, &oset);
#ifdef HAVE_VFORK
	pid = vfork();
#else
	pid = fork();
#endif
	switch (pid) {
	default:
		setpgid(pid, pid);
	case -1:
		sigprocmask(SIG_SETMASK, &oset, 0);
		return pid;
	case 0:
		setpgid(0, getpid());
		mysignal(SIGCHLD, SIG_DFL);
		mysignal(SIGHUP, SIG_DFL);
		mysignal(SIGTERM, SIG_DFL);
		mysignal(SIGINT, SIG_DFL);
		mysignal(SIGQUIT, SIG_DFL);
		mysignal(SIGUSR1, SIG_DFL);
		mysignal(SIGUSR2, SIG_DFL);
		mysignal(SIGPIPE, SIG_DFL);
		sigprocmask(SIG_SETMASK, &oset, 0);
		dup2(fd, 0);
		dup2(fd, 1);
		if (efd != -1)
			dup2(efd, 2);
		if (u) {
			if (setuid(0) == -1)
				_exit(1);
			if (setgid(g) == -1)
				_exit(2);
			if (setuid(u) == -1)
				_exit(3);
		}
		if (getuid() == 0 || geteuid() == 0)
			_exit(4);
		if (chdir(curdir) == -1)
			_exit(5);
		execve(program, argv, envp);
		_exit(6);
	}
	return -1; /* not reached */
}
