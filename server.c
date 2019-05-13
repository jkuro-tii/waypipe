/*
 * Copyright © 2019 Manuel Stoeckl
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#define _XOPEN_SOURCE 700

#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>

int run_server(const char *socket_path, int app_argc, char *const app_argv[])
{
	fprintf(stderr, "I'm a server on %s!\n", socket_path);
	fprintf(stderr, "Trying to run %d:", app_argc);
	for (int i = 0; i < app_argc; i++) {
		fprintf(stderr, " %s", app_argv[i]);
	}
	fprintf(stderr, "\n");

	// create another socketpair; one goes to display; one goes to child
	int csockpair[2];
	socketpair(AF_UNIX, SOCK_STREAM, 0, csockpair);
	int flags = fcntl(csockpair[0], F_GETFD);
	fcntl(csockpair[0], F_SETFD, flags | FD_CLOEXEC);

	pid_t pid = fork();
	if (pid == -1) {
		fprintf(stderr, "Fork failed\n");
		return EXIT_FAILURE;
	} else if (pid == 0) {
		char bufs2[16];
		sprintf(bufs2, "%d", csockpair[1]);

		// Provide the other socket in the pair to child application
		unsetenv("WAYLAND_DISPLAY");
		setenv("WAYLAND_SOCKET", bufs2, 0);

		execv(app_argv[0], app_argv);
		fprintf(stderr, "Failed to execv: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}

	struct wl_display *display = wl_display_create();
	if (wl_display_add_socket_fd(display, csockpair[0]) == -1) {
		fprintf(stderr, "Failed to add socket to display object\n");
		wl_display_destroy(display);
		return EXIT_FAILURE;
	}

	int status;

	struct sockaddr_un saddr;
	int fd;

	if (strlen(socket_path) >= sizeof(saddr.sun_path)) {
		fprintf(stderr, "Socket path is too long and would be truncated: %s\n",
				socket_path);
		return EXIT_FAILURE;
	}

	saddr.sun_family = AF_UNIX;
	strncpy(saddr.sun_path, socket_path, sizeof(saddr.sun_path) - 1);
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1) {
		fprintf(stderr, "Error creating socket: %s\n", strerror(errno));
		return EXIT_FAILURE;
	}
	if (connect(fd, (struct sockaddr *)&saddr, sizeof(saddr)) == -1) {
		fprintf(stderr, "Error connecting socket: %s\n",
				strerror(errno));
		close(fd);
		return EXIT_FAILURE;
	}

	// A connection has already been established
	int client_socket = csockpair[0];

	/** Main select loop:
	 * fd -> csockpair[0]
	 * csockpair[0] -> fd
	 * 1 second timer (poll waitpid) */
	struct timespec timeout = {.tv_sec = 0, .tv_nsec = 500000000L};
	fd_set readfds;
	int iter = 0;

	int maxmsg = 4096;
	char *buffer = calloc(1, maxmsg + 1);

	while (true) {
		iter++;
		if (iter > 10) {
			break;
		}
		FD_ZERO(&readfds);
		FD_SET(fd, &readfds);
		FD_SET(client_socket, &readfds);
		int maxfd = fd > client_socket ? fd : client_socket;
		int r = pselect(maxfd + 1, &readfds, NULL, NULL, &timeout,
				NULL);
		if (r < 0) {
			fprintf(stderr, "Error selecting fds: %s\n",
					strerror(errno));
			return EXIT_FAILURE;
		}
		if (r == 0) {
			// timeout!
			fprintf(stderr, "timeout,?? \n");
		} else {
			fprintf(stderr, "%d are set\n", r);
		}
		if (FD_ISSET(fd, &readfds)) {
			fprintf(stderr, "Readfd isset\n");
			int rc = iovec_read(fd, buffer, maxmsg, NULL, NULL);
			if (rc == -1) {
				fprintf(stderr, "FD Read failure %ld: %s\n", rc,
						strerror(errno));
				break;
			}
			fprintf(stderr, "Read from conn %d bytes\n", rc);
			if (rc > 0) {
				int wc = iovec_write(client_socket, buffer, rc,
						NULL, NULL);
				if (wc == -1) {
					fprintf(stderr, "FD Write  failure %ld: %s\n",
							wc, strerror(errno));
					break;
				}
			} else {
				fprintf(stderr, "the other side shut down\n");
				break;
			}
		}
		if (FD_ISSET(client_socket, &readfds)) {
			fprintf(stderr, "client socket isset\n");
			int rc = iovec_read(client_socket, buffer, maxmsg, NULL,
					NULL);
			if (rc == -1) {
				fprintf(stderr, "CS Read failure %ld: %s\n", rc,
						strerror(errno));
				break;
			}
			if (rc > 0) {
				int wc = iovec_write(
						fd, buffer, rc, NULL, NULL);
				if (wc == -1) {
					fprintf(stderr, "CS Write  failure %ld: %s\n",
							wc, strerror(errno));
					break;
				}
			} else {
				fprintf(stderr, "the client shut down\n");
				break;
			}
		}

		if (waitpid(pid, &status, WNOHANG) > 0) {
			break;
		}
	}
	close(fd);

	// todo: scope manipulation, to ensure all cleanups are done
	waitpid(pid, &status, 0);
	fprintf(stderr, "Program ended\n");
	return EXIT_SUCCESS;
}
