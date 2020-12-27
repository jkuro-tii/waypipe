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

#include "main.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static inline uint32_t conntoken_version(uint32_t header)
{
	return header >> 16;
}
static int check_conn_header(uint32_t header)
{
	if ((header >> 16) != WAYPIPE_PROTOCOL_VERSION) {
		wp_error("Rejecting connection header %08" PRIx32
			 ", protocol version (%u) does not match (%u).",
				header, conntoken_version(header),
				WAYPIPE_PROTOCOL_VERSION);
		wp_error("Check that Waypipe has the correct version (>=0.7.0 on both sides; this is %s)",
				WAYPIPE_VERSION);
		if ((header & CONN_FIXED_BIT) == 0 &&
				(header & CONN_UNSET_BIT) != 0) {
			wp_error("It is also possible that server endianness does not match client");
			return -1;
		}
		return -1;
	}
	return 0;
}
static inline bool key_match(
		const uint32_t key1[static 3], const uint32_t key2[static 3])
{
	return key1[0] == key2[0] && key1[1] == key2[1] && key1[2] == key2[2];
}
static int get_inherited_socket(void)
{
	const char *fd_no = getenv("WAYLAND_SOCKET");
	char *endptr = NULL;
	errno = 0;
	int fd = (int)strtol(fd_no, &endptr, 10);
	if (*endptr || errno) {
		wp_error("Failed to parse WAYLAND_SOCKET env variable with value \"%s\", exiting",
				fd_no);
		return -1;
	}
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags == -1 && errno == EBADF) {
		wp_error("The file descriptor WAYLAND_SOCKET=%d was invalid, exiting",
				fd);
		return -1;
	}
	return fd;
}

#define MAX_SOCKETPATH_LEN (int)sizeof(((struct sockaddr_un *)NULL)->sun_path)

static int get_display_path(char path[static MAX_SOCKETPATH_LEN])
{
	const char *display = getenv("WAYLAND_DISPLAY");
	if (!display) {
		wp_error("WAYLAND_DISPLAY is not set, exiting");
		return -1;
	}
	int len = 0;
	if (display[0] != '/') {
		const char *xdg_runtime_dir = getenv("XDG_RUNTIME_DIR");
		if (!xdg_runtime_dir) {
			wp_error("XDG_RUNTIME_DIR is not set, exiting");
			return -1;
		}
		len = snprintf(path, MAX_SOCKETPATH_LEN, "%s/%s",
				xdg_runtime_dir, display);
	} else {
		len = snprintf(path, MAX_SOCKETPATH_LEN, "%s", display);
	}
	if (len >= MAX_SOCKETPATH_LEN) {
		wp_error("Wayland display socket path is >=%d bytes, truncated to \"%s\", exiting",
				MAX_SOCKETPATH_LEN, path);
		return -1;
	}
	return 0;
}

static int run_single_client_reconnector(
		int channelsock, int linkfd, struct connection_token conn_id)
{
	int retcode = EXIT_SUCCESS;
	while (!shutdown_flag) {
		struct pollfd pf[2];
		pf[0].fd = channelsock;
		pf[0].events = POLLIN;
		pf[0].revents = 0;
		pf[1].fd = linkfd;
		pf[1].events = 0;
		pf[1].revents = 0;

		int r = poll(pf, 2, -1);
		if (r == -1 && errno == EINTR) {
			continue;
		} else if (r == -1) {
			retcode = EXIT_FAILURE;
			break;
		} else if (r == 0) {
			// Nothing to read
			continue;
		}

		if (pf[1].revents & POLLHUP) {
			/* Hang up, main thread has closed its link */
			break;
		}
		if (pf[0].revents & POLLIN) {
			int newclient = accept(channelsock, NULL, NULL);
			if (newclient == -1) {
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					// The wakeup may have been spurious
					continue;
				}
				wp_error("Connection failure: %s",
						strerror(errno));
				retcode = EXIT_FAILURE;
				break;
			} else {
				struct connection_token new_conn;
				memset(&new_conn, 0, sizeof(new_conn));
				if (read(newclient, &new_conn.header,
						    sizeof(new_conn.header)) !=
						sizeof(new_conn.header)) {
					wp_error("Failed to get connection id header");
					goto done;
				}
				if (check_conn_header(new_conn.header) < 0) {
					goto done;
				}
				if (read(newclient, &new_conn.key,
						    sizeof(new_conn.key)) !=
						sizeof(new_conn.key)) {
					wp_error("Failed to get connection id key");
					goto done;
				}
				if (!key_match(new_conn.key, conn_id.key)) {
					wp_error("Connection attempt with unmatched key");
					goto done;
				}
				bool update = new_conn.header &
					      CONN_RECONNECTABLE_BIT;
				if (!update) {
					wp_error("Connection token is missing update flag");
					goto done;
				}
				if (send_one_fd(linkfd, newclient) == -1) {
					wp_error("Failed to get connection id");
					retcode = EXIT_FAILURE;
					checked_close(newclient);
					break;
				}
			done:
				checked_close(newclient);
			}
		}
	}
	checked_close(channelsock);
	checked_close(linkfd);
	return retcode;
}

static int run_single_client(int channelsock, pid_t *eol_pid,
		const struct main_config *config, int disp_fd)
{
	/* To support reconnection attempts, this mode creates a child
	 * reconnection watcher process, linked via socketpair */
	int retcode = EXIT_SUCCESS;
	int chanclient = -1;
	struct connection_token conn_id;
	memset(&conn_id, 0, sizeof(conn_id));
	while (!shutdown_flag) {
		int status = -1;
		if (wait_for_pid_and_clean(eol_pid, &status, WNOHANG, NULL)) {
			eol_pid = 0; // < in case eol_pid is recycled

			wp_debug("Child (ssh) died, exiting");
			// Copy the exit code
			retcode = WEXITSTATUS(status);
			break;
		}

		struct pollfd cs;
		cs.fd = channelsock;
		cs.events = POLLIN;
		cs.revents = 0;
		int r = poll(&cs, 1, -1);
		if (r == -1) {
			if (errno == EINTR) {
				// If SIGCHLD, we will check the child.
				// If SIGINT, the loop ends
				continue;
			}
			retcode = EXIT_FAILURE;
			break;
		} else if (r == 0) {
			// Nothing to read
			continue;
		}

		chanclient = accept(channelsock, NULL, NULL);
		if (chanclient == -1) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				// The wakeup may have been spurious
				continue;
			}
			wp_error("Connection failure: %s", strerror(errno));
			retcode = EXIT_FAILURE;
			break;
		} else {
			if (read(chanclient, &conn_id.header,
					    sizeof(conn_id.header)) !=
					sizeof(conn_id.header)) {
				wp_error("Failed to get connection id header");
				goto fail_cc;
			}
			if (check_conn_header(conn_id.header) < 0) {
				goto fail_cc;
			}
			if (read(chanclient, &conn_id.key,
					    sizeof(conn_id.key)) !=
					sizeof(conn_id.key)) {
				wp_error("Failed to get connection id key");
				goto fail_cc;
			}
			break;
		fail_cc:
			retcode = EXIT_FAILURE;
			checked_close(chanclient);
			chanclient = -1;
			break;
		}
	}
	if (retcode == EXIT_FAILURE || shutdown_flag || chanclient == -1) {
		return retcode;
	}
	if (conn_id.header & CONN_UPDATE_BIT) {
		wp_error("Initial connection token had update flag set");
		return retcode;
	}

	/* Fork a reconnection handler, only if the connection is
	 * reconnectable/has a nonzero id */
	int linkfds[2] = {-1, -1};
	if (conn_id.header & CONN_RECONNECTABLE_BIT) {
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, linkfds) == -1) {
			wp_error("Failed to create socketpair: %s",
					strerror(errno));
			checked_close(chanclient);
			return EXIT_FAILURE;
		}

		pid_t reco_pid = fork();
		if (reco_pid == -1) {
			wp_debug("Fork failure");
			checked_close(chanclient);
			return EXIT_FAILURE;
		} else if (reco_pid == 0) {
			if (linkfds[0] != -1) {
				checked_close(linkfds[0]);
			}
			checked_close(chanclient);
			checked_close(disp_fd);
			int rc = run_single_client_reconnector(
					channelsock, linkfds[1], conn_id);
			exit(rc);
		}
		checked_close(linkfds[1]);
	}
	checked_close(channelsock);

	return main_interface_loop(
			chanclient, disp_fd, linkfds[0], config, true);
}

void send_new_connection_fd(
		struct conn_map *connmap, uint32_t key[static 3], int new_fd)
{
	for (int i = 0; i < connmap->count; i++) {
		if (key_match(connmap->data[i].token.key, key)) {
			if (send_one_fd(connmap->data[i].linkfd, new_fd) ==
					-1) {
				wp_error("Failed to send new connection fd to subprocess: %s",
						strerror(errno));
			}
			break;
		}
	}
}

static void handle_new_client_connection(struct pollfd *other_fds,
		int n_other_fds, int chanclient, struct conn_map *connmap,
		const struct main_config *config,
		const char disp_path[static MAX_SOCKETPATH_LEN],
		const struct connection_token *conn_id)
{
	bool reconnectable = conn_id->header & CONN_RECONNECTABLE_BIT;

	if (reconnectable && buf_ensure_size(connmap->count + 1,
					     sizeof(struct conn_addr),
					     &connmap->size,
					     (void **)&connmap->data) == -1) {
		wp_error("Failed to allocate space to track connection");
		goto fail_cc;
	}
	int linkfds[2] = {-1, -1};
	if (reconnectable) {
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, linkfds) == -1) {
			wp_error("Failed to create socketpair: %s",
					strerror(errno));
			goto fail_cc;
		}
	}
	pid_t npid = fork();
	if (npid == 0) {
		// Run forked process, with the only shared
		// state being the new channel socket
		for (int i = 0; i < n_other_fds; i++) {
			if (other_fds[i].fd != chanclient) {
				checked_close(other_fds[i].fd);
			}
		}
		if (reconnectable) {
			checked_close(linkfds[0]);
		}
		for (int i = 0; i < connmap->count; i++) {
			checked_close(connmap->data[i].linkfd);
		}

		int dfd = connect_to_socket(disp_path);
		if (dfd == -1) {
			exit(EXIT_FAILURE);
		}
		int rc = main_interface_loop(
				chanclient, dfd, linkfds[1], config, true);
		check_unclosed_fds();
		exit(rc);
	} else if (npid == -1) {
		wp_debug("Fork failure");
		goto fail_ps;
	}
	// Remove connection from this process

	if (reconnectable) {
		checked_close(linkfds[1]);
		connmap->data[connmap->count++] =
				(struct conn_addr){.linkfd = linkfds[0],
						.token = *conn_id,
						.pid = npid};
	}

	return;
fail_ps:
	checked_close(linkfds[0]);
fail_cc:
	checked_close(chanclient);
	return;
}
#define NUM_INCOMPLETE_CONNECTIONS 63

static void drop_incoming_connection(struct pollfd *fds,
		struct connection_token *tokens, uint8_t *bytes_read, int index,
		int incomplete)
{
	checked_close(fds[index].fd);
	if (index != incomplete - 1) {
		size_t shift = (size_t)(incomplete - 1 - index);
		memmove(fds + index, fds + index + 1,
				sizeof(struct pollfd) * shift);
		memmove(tokens + index, tokens + index + 1,
				sizeof(struct connection_token) * shift);
		memmove(bytes_read + index, bytes_read + index + 1,
				sizeof(uint8_t) * shift);
	}
	memset(&fds[incomplete - 1], 0, sizeof(struct pollfd));
	memset(&tokens[incomplete - 1], 0, sizeof(struct connection_token));
	bytes_read[incomplete - 1] = 0;
}

static int run_multi_client(int channelsock, pid_t *eol_pid,
		const struct main_config *config,
		const char disp_path[static MAX_SOCKETPATH_LEN])
{
	struct conn_map connmap = {.data = NULL, .count = 0, .size = 0};

	/* Keep track of the main socket, and all connections which have not
	 * yet fully provided their connection token. If we run out of space,
	 * the oldest incomplete connection gets dropped */
	struct pollfd fds[NUM_INCOMPLETE_CONNECTIONS + 1];
	struct connection_token tokens[NUM_INCOMPLETE_CONNECTIONS];
	uint8_t bytes_read[NUM_INCOMPLETE_CONNECTIONS];
	int incomplete = 0;
	memset(fds, 0, sizeof(fds));
	memset(tokens, 0, sizeof(tokens));
	memset(bytes_read, 0, sizeof(bytes_read));
	fds[0].fd = channelsock;
	fds[0].events = POLLIN;
	fds[0].revents = 0;

	int retcode = EXIT_SUCCESS;
	while (!shutdown_flag) {
		int status = -1;
		if (wait_for_pid_and_clean(
				    eol_pid, &status, WNOHANG, &connmap)) {
			wp_debug("Child (ssh) died, exiting");
			// Copy the exit code
			retcode = WEXITSTATUS(status);
			break;
		}

		int r = poll(fds, 1 + (nfds_t)incomplete, -1);
		if (r == -1) {
			if (errno == EINTR) {
				// If SIGCHLD, we will check the child.
				// If SIGINT, the loop ends
				continue;
			}
			retcode = EXIT_FAILURE;
			break;
		} else if (r == 0) {
			// Nothing to read
			continue;
		}

		for (int i = 0; i < incomplete; i++) {
			if (!(fds[i + 1].revents & POLLIN)) {
				continue;
			}
			int cur_fd = fds[i + 1].fd;
			char *dest = ((char *)&tokens[i]) + bytes_read[i];
			ssize_t s = read(cur_fd, dest, 16 - bytes_read[i]);
			if (s == -1) {
				wp_error("Failed to read from connection: %s",
						strerror(errno));
				drop_incoming_connection(fds + 1, tokens,
						bytes_read, i, incomplete);
				incomplete--;
				continue;
			} else if (s == 0) {
				/* connection closed */
				wp_error("Connection closed early");
				drop_incoming_connection(fds + 1, tokens,
						bytes_read, i, incomplete);
				incomplete--;
				continue;
			}
			bytes_read[i] += (uint8_t)s;
			if (bytes_read[i] - (uint8_t)s < 4 &&
					bytes_read[i] >= 4) {
				/* Validate connection token header */
				if (check_conn_header(tokens[i].header) < 0) {
					drop_incoming_connection(fds + 1,
							tokens, bytes_read, i,
							incomplete);
					incomplete--;
					continue;
				}
			}
			if (bytes_read[i] < 16) {
				continue;
			}
			/* Validate connection token key */
			if (tokens[i].header & CONN_UPDATE_BIT) {
				send_new_connection_fd(&connmap, tokens[i].key,
						cur_fd);
				drop_incoming_connection(fds + 1, tokens,
						bytes_read, i, incomplete);
				incomplete--;
				continue;
			}

			/* Failures here are logged, but should not
			 * affect this process' ability to e.g. handle
			 * reconnections. */
			handle_new_client_connection(fds, 1 + incomplete,
					cur_fd, &connmap, config, disp_path,
					&tokens[i]);
			drop_incoming_connection(fds + 1, tokens, bytes_read, i,
					incomplete);
			incomplete--;
		}

		/* Process new connections second, to give incomplete
		 * connections a chance to clear first */
		if (fds[0].revents & POLLIN) {
			int chanclient = accept(channelsock, NULL, NULL);
			if (chanclient == -1) {
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					// The wakeup may have been spurious
					continue;
				}
				// should errors like econnaborted exit?
				wp_error("Connection failure: %s",
						strerror(errno));
				retcode = EXIT_FAILURE;
				break;
			} else {
				if (set_nonblocking(chanclient) == -1) {
					wp_error("Error making new connection nonblocking: %s",
							strerror(errno));
					checked_close(chanclient);
					continue;
				}

				if (incomplete == NUM_INCOMPLETE_CONNECTIONS) {
					wp_error("Dropping oldest incomplete connection (out of %d)",
							NUM_INCOMPLETE_CONNECTIONS);
					drop_incoming_connection(fds + 1,
							tokens, bytes_read, 0,
							incomplete);
					incomplete--;
				}
				fds[1 + incomplete].fd = chanclient;
				fds[1 + incomplete].events = POLLIN;
				fds[1 + incomplete].revents = 0;
				memset(&tokens[incomplete], 0,
						sizeof(struct connection_token));
				bytes_read[incomplete] = 0;
				incomplete++;
			}
		}
	}
	for (int i = 0; i < incomplete; i++) {
		checked_close(fds[i + 1].fd);
	}

	for (int i = 0; i < connmap.count; i++) {
		checked_close(connmap.data[i].linkfd);
	}
	free(connmap.data);
	checked_close(channelsock);
	return retcode;
}

int run_client(const char *socket_path, const struct main_config *config,
		bool oneshot, bool via_socket, pid_t eol_pid)
{
	/* Connect to Wayland display. We don't use the wayland-client
	 * function here, because its errors aren't immediately useful,
	 * and older Wayland versions have edge cases */
	int dispfd = -1;
	char disp_path[MAX_SOCKETPATH_LEN];
	memset(disp_path, 0, sizeof(disp_path));

	if (via_socket) {
		dispfd = get_inherited_socket();
		if (dispfd == -1) {
			if (eol_pid) {
				waitpid(eol_pid, NULL, 0);
			}
			return EXIT_FAILURE;
		}
		/* This socket is inherited and meant to be closed by Waypipe */
		if (dispfd >= 0 && dispfd < 256) {
			inherited_fds[dispfd / 64] &= ~(1uLL << (dispfd % 64));
		}
	} else {
		if (get_display_path(disp_path) == -1) {
			if (eol_pid) {
				waitpid(eol_pid, NULL, 0);
			}
			return EXIT_FAILURE;
		}
	}

	if (oneshot) {
		if (!via_socket) {
			dispfd = connect_to_socket(disp_path);
		}
	} else {
		int test_conn = connect_to_socket(disp_path);
		if (test_conn == -1) {
			if (eol_pid) {
				waitpid(eol_pid, NULL, 0);
			}
			return EXIT_FAILURE;
		}
		checked_close(test_conn);
	}
	wp_debug("A wayland compositor is available. Proceeding.");

	int nmaxclients = oneshot ? 1 : 128;
	int channelsock = setup_nb_socket(socket_path, nmaxclients);
	if (channelsock == -1) {
		// Error messages already made
		if (eol_pid) {
			waitpid(eol_pid, NULL, 0);
		}
		if (dispfd != -1) {
			checked_close(dispfd);
		}
		return EXIT_FAILURE;
	}

	/* These handlers close the channelsock and dispfd */
	int retcode;
	if (oneshot) {
		retcode = run_single_client(
				channelsock, &eol_pid, config, dispfd);
	} else {
		retcode = run_multi_client(
				channelsock, &eol_pid, config, disp_path);
	}
	unlink(socket_path);
	int cleanup_type = shutdown_flag ? WNOHANG : 0;

	int status = -1;
	// Don't return until all child processes complete
	if (wait_for_pid_and_clean(&eol_pid, &status, cleanup_type, NULL)) {
		retcode = WEXITSTATUS(status);
	}
	return retcode;
}
