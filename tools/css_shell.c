/*
 * css_shell - CSS debug console client via COMA "debug" service
 *
 * protocol (reverse engineered from CSS firmware):
 *   send: payload[0]=0x00, payload[1..n]=input chars (can be a whole line)
 *   recv: payload[0]=0x02, payload[1]=output char (one char per message)
 *   ping: payload[0]=0xAA, CSS echoes it back
 *
 * usage:
 *   css_shell                    # interactive mode
 *   css_shell -c "tdm stats 0"  # execute single command, print output, exit
 *   css_shell -i "text set 1 32 1"  # execute command(s), then go interactive
 *                                    # (multiple -i flags allowed)
 */

#include <comatose/coma.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <termios.h>
#include <signal.h>
#include <sys/socket.h>

static volatile int running = 1;

static void sighandler(int sig)
{
	(void)sig;
	running = 0;
}

static int send_char(coma_conn_t *conn, char c)
{
	/* css-term sends one character at a time: {0x00, char} */
	uint8_t buf[2] = { 0x00, (uint8_t)c };
	ssize_t n = coma_send(conn, buf, 2);
	return (n < 0) ? -1 : 0;
}

static int send_input(coma_conn_t *conn, const char *str, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (send_char(conn, str[i]) < 0)
			return -1;
	}
	return 0;
}

static int recv_output(coma_conn_t *conn, int timeout_ms)
{
	struct pollfd pfd = { .fd = coma_fd(conn), .events = POLLIN };
	int count = 0;

	while (1) {
		int ret = poll(&pfd, 1, timeout_ms);
		if (ret <= 0)
			break;

		uint8_t buf[256];
		ssize_t n = coma_recv(conn, buf, sizeof(buf));
		if (n <= 0)
			break;

		/* output messages: type=0x02, then character data */
		if (n >= 2 && buf[0] == 0x02) {
			for (ssize_t i = 1; i < n; i++) {
				putchar(buf[i]);
				count++;
			}
			fflush(stdout);
		}
		/* use short timeout for subsequent chars in burst */
		timeout_ms = 50;
	}

	return count;
}

static void activate_coma_io(coma_conn_t *conn);

static void interactive_mode(coma_conn_t *conn)
{
	struct termios old_term, raw_term;
	int has_term = (tcgetattr(STDIN_FILENO, &old_term) == 0);

	if (has_term) {
		raw_term = old_term;
		raw_term.c_lflag &= ~(ICANON | ECHO);
		raw_term.c_cc[VMIN] = 0;
		raw_term.c_cc[VTIME] = 1;
		tcsetattr(STDIN_FILENO, TCSANOW, &raw_term);
	}

	fprintf(stderr, "[css_shell: connected. Ctrl+C to exit]\n");

	activate_coma_io(conn);

	while (running) {
		struct pollfd fds[2] = {
			{ .fd = STDIN_FILENO, .events = POLLIN },
			{ .fd = coma_fd(conn), .events = POLLIN },
		};

		int ret = poll(fds, 2, 100);
		if (ret < 0)
			break;

		/* stdin → CSS */
		if (fds[0].revents & POLLIN) {
			char c;
			ssize_t n = read(STDIN_FILENO, &c, 1);
			if (n == 1) {
				send_input(conn, &c, 1);
			}
		}

		/* CSS → stdout */
		if (fds[1].revents & POLLIN) {
			recv_output(conn, 50);
		}
	}

	if (has_term)
		tcsetattr(STDIN_FILENO, TCSANOW, &old_term);

	fprintf(stderr, "\n[css_shell: disconnected]\n");
}

static void activate_coma_io(coma_conn_t *conn)
{
	/* send 0xAA ping to activate the COMA I/O backend on the CSS.
	 * the CSS debug service echoes this back and may switch shell I/O. */
	uint8_t ping = 0xAA;
	coma_send(conn, &ping, 1);
	recv_output(conn, 300);

	/* also try sending a bare newline to trigger prompt */
	send_input(conn, "\n", 1);
	recv_output(conn, 500);
}

static void command_mode(coma_conn_t *conn, const char *cmd)
{
	activate_coma_io(conn);

	/* send the command + newline */
	send_input(conn, cmd, strlen(cmd));
	send_input(conn, "\n", 1);

	/* collect output — use longer timeout for initial response,
	 * shorter for subsequent chars in burst */
	recv_output(conn, 3000);
	/* keep draining until no more output */
	while (recv_output(conn, 500) > 0)
		;
	printf("\n");
}

int main(int argc, char *argv[])
{
	const char *cmd = NULL;
	const char *init_cmds[16];
	int num_init = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
			cmd = argv[++i];
		} else if (strcmp(argv[i], "-i") == 0 && i + 1 < argc) {
			if (num_init < 16)
				init_cmds[num_init++] = argv[++i];
		}
	}

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	coma_conn_t *conn = coma_connect("debug");
	if (!conn) {
		fprintf(stderr, "css_shell: failed to connect to debug service\n");
		return 1;
	}

	if (cmd) {
		command_mode(conn, cmd);
	} else {
		/* send init commands then go interactive */
		if (num_init > 0) {
			activate_coma_io(conn);
			for (int i = 0; i < num_init; i++) {
				fprintf(stderr, "[init] %s\n", init_cmds[i]);
				send_input(conn, init_cmds[i], strlen(init_cmds[i]));
				send_input(conn, "\n", 1);
				recv_output(conn, 1000);
			}
		}
		interactive_mode(conn);
	}

	coma_close(conn);
	return 0;
}
