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

#define _GNU_SOURCE
#include "util.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <pthread.h>

struct copy_setup {
	int conn;
	int wayl;
	bool is_display_side;
	struct main_config *mc;
};

void *start_looper(void *data)
{
	struct copy_setup *setup = (struct copy_setup *)data;
	main_interface_loop(setup->conn, setup->wayl, setup->mc,
			setup->is_display_side);
	return NULL;
}

static void atomic_logger(const char *file, int line, enum log_level level,
		const char *fmt, ...)
{
	pthread_t tid = pthread_self();
	char msg[1024];
	int nwri = 0;
	nwri += sprintf(msg + nwri, "%lx [%s:%3d] ", (long)tid, file, line);

	va_list args;
	va_start(args, fmt);
	nwri += vsnprintf(msg + nwri, (size_t)(1022 - nwri), fmt, args);
	va_end(args);

	msg[nwri++] = '\n';
	msg[nwri] = 0;

	write(STDOUT_FILENO, msg, (size_t)nwri);
	(void)level;
}

log_handler_func_t log_funcs[2] = {NULL, NULL};
int main(int argc, char **argv)
{
	if (argc == 1 || !strcmp(argv[1], "--help")) {
		printf("Usage: ./fuzz_hook [--log] {input_file}\n");
		printf("A program to run and control inputs for a linked client/server pair, from a file.\n");
		return EXIT_FAILURE;
	}
	if (argc > 1 && !strcmp(argv[1], "--log")) {
		log_funcs[0] = atomic_logger;
		log_funcs[1] = atomic_logger;
		argc--;
		argv++;
	}

	int fd = open(argv[1], O_RDONLY);
	if (fd == -1) {
		printf("Failed to open '%s'", argv[1]);
		return EXIT_FAILURE;
	}
	long len = lseek(fd, 0, SEEK_END);
	if (len == 0) {
		close(fd);
		return EXIT_SUCCESS;
	}
	lseek(fd, 0, SEEK_SET);
	char *buf = malloc(len);
	read(fd, buf, len);
	close(fd);
	printf("Loaded %ld bytes\n", len);

	int srv_fds[2], cli_fds[2], conn_fds[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, srv_fds) == -1 ||
			socketpair(AF_UNIX, SOCK_STREAM, 0, cli_fds) == -1 ||
			socketpair(AF_UNIX, SOCK_STREAM, 0, conn_fds) == -1) {
		printf("Socketpair failed\n");
		return EXIT_FAILURE;
	}

	struct main_config config = {.drm_node = NULL,
			.n_worker_threads = 1,
			.compression = COMP_NONE,
			.no_gpu = true, /* until we can construct dmabufs here
					 */
			.linear_dmabuf = false,
			.video_if_possible = true};

	pthread_t thread_a, thread_b;
	struct copy_setup server_conf = {.conn = conn_fds[0],
			.wayl = srv_fds[1],
			.is_display_side = true,
			.mc = &config};
	struct copy_setup client_conf = {.conn = conn_fds[1],
			.wayl = cli_fds[1],
			.is_display_side = false,
			.mc = &config};
	if (pthread_create(&thread_a, NULL, start_looper, &server_conf) == -1) {
		printf("Thread failed\n");
	}
	if (pthread_create(&thread_b, NULL, start_looper, &client_conf) == -1) {
		printf("Thread failed\n");
	}

	char *ignore_buf = malloc(65536);

	/* Main loop: RW from socketpairs with sendmsg, with short wait */
	long file_nwords = len / 4;
	long cursor = 0;
	uint32_t *data = (uint32_t *)buf;
	char template[256];
	while (cursor < file_nwords) {
		uint32_t header = data[cursor++];
		bool to_server = header & 0x1;
		bool add_file = header & 0x2;
		int new_fileno = -1;

		if (add_file && cursor < file_nwords) {
			uint32_t fsize = data[cursor++];
			if (fsize == 0) {
				/* 'copy' sink */
				new_fileno = open("/dev/null", O_WRONLY);
				if (new_fileno == -1) {
					wp_log(WP_ERROR,
							"Failed to open /dev/null");
				}
			} else {
				/* avoid buffer overflow */
				fsize = fsize > 1000000 ? 1000000 : fsize;
				/* This should be a tmpfs */
#if defined(__linux__)
				sprintf(template, "%x:%x", (uint32_t)cursor,
						fsize);
				new_fileno = memfd_create(template, 0);
#else
				/* WARNING: this can be rather file-system
				 * intensive */
				strcpy(template, "/tmp/fuzz_hook_XXXXXX");
				new_fileno = mkstemp(template);
				unlink(template);
#endif
				if (new_fileno == -1) {
					wp_log(WP_ERROR, "Failed to mkstemp");
				} else if (ftruncate(new_fileno, fsize) == -1) {
					wp_log(WP_ERROR,
							"Failed to resize tempfile");
					close(new_fileno);
					new_fileno = -1;
				}
			}
		}

		uint32_t packet_size = header >> 2;
		if ((long)packet_size > file_nwords - cursor) {
			packet_size = (uint32_t)(file_nwords - cursor);
		}
		if (packet_size > 2048) {
			packet_size = 2048;
		}
		/* 2 msec max delay for 8KB of data, assuming no system
		 * interference, should be easily attainable */
		int max_write_delay_ms = 1;
		int max_read_delay_ms = 2;

		int send_fd = to_server ? srv_fds[0] : cli_fds[0];
		/* Write packet to stream */
		struct pollfd write_pfd;
		write_pfd.fd = send_fd;
		write_pfd.events = POLLOUT;
		int nw;
	retry_poll:
		nw = poll(&write_pfd, 1, max_write_delay_ms);
		if (nw == -1) {
			if (new_fileno != -1) {
				close(new_fileno);
			}

			if (errno == EINTR) {
				goto retry_poll;
			}
			printf("Poll error\n");
			break;
		} else if (nw == 1) {
			/* Send message */
			struct iovec the_iovec;
			the_iovec.iov_len = packet_size * 4;
			the_iovec.iov_base = (char *)&data[cursor];
			struct msghdr msg;
			msg.msg_name = NULL;
			msg.msg_namelen = 0;
			msg.msg_iov = &the_iovec;
			msg.msg_iovlen = 1;
			msg.msg_control = NULL;
			msg.msg_controllen = 0;
			msg.msg_flags = 0;

			union {
				char buf[CMSG_SPACE(sizeof(int))];
				struct cmsghdr align;
			} uc;
			memset(uc.buf, 0, sizeof(uc.buf));

			if (new_fileno != -1) {
				msg.msg_control = uc.buf;
				msg.msg_controllen = sizeof(uc.buf);
				struct cmsghdr *frst = CMSG_FIRSTHDR(&msg);
				frst->cmsg_level = SOL_SOCKET;
				frst->cmsg_type = SCM_RIGHTS;
				memcpy(CMSG_DATA(frst), &new_fileno,
						sizeof(int));
				frst->cmsg_len = CMSG_LEN(sizeof(int));
				msg.msg_controllen = CMSG_SPACE(sizeof(int));
			}

			int target_fd = to_server ? srv_fds[0] : cli_fds[0];
			ssize_t ret = sendmsg(target_fd, &msg, 0);
			if (ret == -1) {
				wp_log(WP_ERROR, "Error in sendmsg");
				break;
			}
		} else {
			wp_log(WP_ERROR,
					"Failed to send message before timeout");
		}
		if (new_fileno != -1) {
			close(new_fileno);
		}

		/* Wait up to max_delay for a response. Almost all packets
		 * should be passed on unmodified; a very small fraction
		 * are dropped */
		struct pollfd read_pfds[2];
		read_pfds[0].fd = srv_fds[0];
		read_pfds[1].fd = cli_fds[0];
		read_pfds[0].events = POLLIN;
		read_pfds[1].events = POLLIN;
		int nr = poll(read_pfds, 2,
				packet_size > 0 ? max_read_delay_ms : 0);
		if (nr == -1) {
			if (errno == EINTR) {
				continue;
			}
			printf("Poll error\n");
			break;
		} else if (nr == 0) {
			wp_log(WP_DEBUG, "No reply to sent packet %d",
					packet_size);
		}
		for (int i = 0; i < 2; i++) {
			if (read_pfds[i].revents & POLLIN) {
				char cmsgdata[(CMSG_LEN(28 * sizeof(int32_t)))];
				struct iovec the_iovec;
				the_iovec.iov_len = 65536;
				the_iovec.iov_base = ignore_buf;
				struct msghdr msg;
				msg.msg_name = NULL;
				msg.msg_namelen = 0;
				msg.msg_iov = &the_iovec;
				msg.msg_iovlen = 1;
				msg.msg_control = &cmsgdata;
				msg.msg_controllen = sizeof(cmsgdata);
				msg.msg_flags = 0;
				ssize_t ret = recvmsg(read_pfds[i].fd, &msg, 0);
				if (ret == -1) {
					wp_log(WP_ERROR, "Error in recvmsg");
				}
			}
		}

		cursor += packet_size;
	}
	close(srv_fds[0]);
	close(cli_fds[0]);

	pthread_join(thread_a, NULL);
	pthread_join(thread_b, NULL);

	free(buf);
	free(ignore_buf);
	return EXIT_SUCCESS;
}