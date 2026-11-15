// SPDX-License-Identifier: MIT
/* The terminal layer of ucxtui; see term.h */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include "term.h"

struct cell {
	uint32_t ch, fg, bg;
	int attr;
};

static struct termios saved_termios;
static bool active, truecolor;
static int width, height;
static struct cell *back, *front;
static bool repaint;		/* the screen holds something else than @front */
static int clip_x0, clip_y0, clip_x1, clip_y1;
static int cursor_x = -1, cursor_y;
static int winch_pipe[2] = { -1, -1 };

/* Output is collected and written once per flush */
static char *out;
static size_t out_len, out_cap;

static void out_add(const char *data, size_t len)
{
	if (out_len + len > out_cap) {
		out_cap = (out_len + len) * 2;
		out = realloc(out, out_cap);
		if (!out)
			abort();
	}
	memcpy(out + out_len, data, len);
	out_len += len;
}

static void out_str(const char *s)
{
	out_add(s, strlen(s));
}

static void out_send(void)
{
	size_t done = 0;
	ssize_t n;

	while (done < out_len) {
		n = write(STDOUT_FILENO, out + done, out_len - done);
		if (n < 0 && errno != EINTR && errno != EAGAIN)
			break;
		if (n > 0)
			done += n;
	}
	out_len = 0;
}

/*
 * 1049: alternate screen. 1003, 1006: buttons and every motion of the mouse,
 * as SGR. 1004: focus changes. 2026: synchronised output
 */
static const char enter_seq[] = "\033[?1049h\033[?25l\033[?1003h\033[?1006h\033[?1004h";
static const char leave_seq[] = "\033[?1004l\033[?1006l\033[?1003l\033[0m\033[?25h\033[?1049l";

/* Async-signal-safe: also runs when the program dies of a signal */
static void restore(void)
{
	if (!active)
		return;
	active = false;
	if (write(STDOUT_FILENO, leave_seq, sizeof(leave_seq) - 1) < 0) {
		/* Nothing to be done about it */
	}
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_termios);
}

static void on_fatal(int sig)
{
	restore();
	signal(sig, SIG_DFL);
	raise(sig);
}

static void on_winch(int sig)
{
	int saved = errno;

	(void)sig;
	if (write(winch_pipe[1], "w", 1) < 0) {
		/* The pipe is full: a resize is already waiting */
	}
	errno = saved;
}

static void resize(void)
{
	struct winsize ws;
	int i;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) || !ws.ws_col || !ws.ws_row) {
		ws.ws_col = 80;
		ws.ws_row = 24;
	}
	width = ws.ws_col;
	height = ws.ws_row;
	back = realloc(back, sizeof(*back) * width * height);
	front = realloc(front, sizeof(*front) * width * height);
	if (!back || !front)
		abort();
	for (i = 0; i < width * height; i++)
		back[i] = (struct cell){ ' ', 0, 0, 0 };
	repaint = true;
	term_unclip();
}

int term_init(void)
{
	static const int fatal[] = { SIGTERM, SIGHUP, SIGINT, SIGQUIT, SIGSEGV, SIGABRT, SIGBUS,
				     SIGFPE, SIGILL };
	const char *colorterm = getenv("COLORTERM");
	struct sigaction sa = { 0 };
	struct termios raw;
	unsigned int i;

	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) || tcgetattr(STDIN_FILENO, &saved_termios))
		return -1;
	truecolor = colorterm && (strstr(colorterm, "truecolor") || strstr(colorterm, "24bit"));
	if (pipe(winch_pipe))
		return -1;
	for (i = 0; i < 2; i++) {
		fcntl(winch_pipe[i], F_SETFL, O_NONBLOCK);
		fcntl(winch_pipe[i], F_SETFD, FD_CLOEXEC);
	}

	raw = saved_termios;
	cfmakeraw(&raw);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw))
		return -1;
	active = true;
	atexit(restore);
	for (i = 0; i < sizeof(fatal) / sizeof(*fatal); i++)
		signal(fatal[i], on_fatal);
	sa.sa_handler = on_winch;
	sigaction(SIGWINCH, &sa, NULL);

	out_str(enter_seq);
	out_send();
	resize();
	return 0;
}

void term_exit(void)
{
	restore();
}

void term_size(int *w, int *h)
{
	*w = width;
	*h = height;
}

/* Drawing */

void term_clip(int x, int y, int w, int h)
{
	clip_x0 = x < 0 ? 0 : x;
	clip_y0 = y < 0 ? 0 : y;
	clip_x1 = x + w > width ? width : x + w;
	clip_y1 = y + h > height ? height : y + h;
}

void term_unclip(void)
{
	term_clip(0, 0, width, height);
}

void term_put(int x, int y, uint32_t ch, uint32_t fg, uint32_t bg, int attr)
{
	if (x < clip_x0 || x >= clip_x1 || y < clip_y0 || y >= clip_y1)
		return;
	back[y * width + x] = (struct cell){ ch, fg, bg, attr };
}

void term_fill(int x, int y, int w, int h, uint32_t ch, uint32_t fg, uint32_t bg)
{
	int i, j;

	for (j = y; j < y + h; j++)
		for (i = x; i < x + w; i++)
			term_put(i, j, ch, fg, bg, 0);
}

void term_tint(int x, int y, int w, int h, uint32_t bg)
{
	int i, j;

	for (j = y; j < y + h; j++)
		for (i = x; i < x + w; i++)
			if (i >= clip_x0 && i < clip_x1 && j >= clip_y0 && j < clip_y1)
				back[j * width + i].bg = bg;
}

/* The next code point of a UTF-8 string; bytes that are not UTF-8 come out as U+FFFD */
static uint32_t utf8_next(const char **text)
{
	const unsigned char *p = (const unsigned char *)*text;
	uint32_t ch = *p++;
	int more = ch >= 0xf0 ? 3 : ch >= 0xe0 ? 2 : ch >= 0xc0 ? 1 : 0;

	if (ch >= 0x80 && !more)
		ch = 0xfffd;
	else if (more)
		ch &= 0x3f >> more;
	while (more--) {
		if ((*p & 0xc0) != 0x80) {
			ch = 0xfffd;
			break;
		}
		ch = ch << 6 | (*p++ & 0x3f);
	}
	*text = (const char *)p;
	return ch;
}

int term_width(const char *text)
{
	int n = 0;

	while (*text) {
		utf8_next(&text);
		n++;
	}
	return n;
}

int term_text(int x, int y, int maxw, const char *text, uint32_t fg, uint32_t bg, int attr)
{
	int n = 0, total = term_width(text);

	while (*text && n < maxw) {
		uint32_t ch = utf8_next(&text);

		if (n == maxw - 1 && total > maxw)
			ch = 0x2026;	/* … */
		term_put(x + n++, y, ch < 0x20 ? ' ' : ch, fg, bg, attr);
	}
	return n;
}

void term_cursor(int x, int y)
{
	cursor_x = x;
	cursor_y = y;
}

static void out_utf8(uint32_t ch)
{
	char buf[4];
	int n = 0;

	if (ch < 0x80) {
		buf[n++] = ch;
	} else if (ch < 0x800) {
		buf[n++] = 0xc0 | ch >> 6;
		buf[n++] = 0x80 | (ch & 0x3f);
	} else if (ch < 0x10000) {
		buf[n++] = 0xe0 | ch >> 12;
		buf[n++] = 0x80 | (ch >> 6 & 0x3f);
		buf[n++] = 0x80 | (ch & 0x3f);
	} else {
		buf[n++] = 0xf0 | ch >> 18;
		buf[n++] = 0x80 | (ch >> 12 & 0x3f);
		buf[n++] = 0x80 | (ch >> 6 & 0x3f);
		buf[n++] = 0x80 | (ch & 0x3f);
	}
	out_add(buf, n);
}

/* The nearest entry of the 256-colour palette: the 6x6x6 cube or the grey ramp */
static int palette(uint32_t rgb)
{
	static const int levels[6] = { 0, 95, 135, 175, 215, 255 };
	int r = rgb >> 16 & 0xff, g = rgb >> 8 & 0xff, b = rgb & 0xff;
	int c[3] = { r, g, b }, idx[3], i, j, grey, gi, dc = 0, dg = 0;

	for (i = 0; i < 3; i++) {
		idx[i] = 0;
		for (j = 1; j < 6; j++)
			if (abs(levels[j] - c[i]) < abs(levels[idx[i]] - c[i]))
				idx[i] = j;
		dc += (levels[idx[i]] - c[i]) * (levels[idx[i]] - c[i]);
	}
	grey = (r + g + b) / 3;
	gi = grey < 8 ? 0 : grey > 238 ? 23 : (grey - 8) / 10;
	for (i = 0; i < 3; i++)
		dg += (8 + 10 * gi - c[i]) * (8 + 10 * gi - c[i]);
	return dg < dc ? 232 + gi : 16 + 36 * idx[0] + 6 * idx[1] + idx[2];
}

static void out_colour(bool bg, uint32_t rgb)
{
	char buf[32];

	if (truecolor)
		snprintf(buf, sizeof(buf), "\033[%d;2;%u;%u;%um", bg ? 48 : 38, rgb >> 16 & 0xff,
			 rgb >> 8 & 0xff, rgb & 0xff);
	else
		snprintf(buf, sizeof(buf), "\033[%d;5;%dm", bg ? 48 : 38, palette(rgb));
	out_str(buf);
}

void term_flush(void)
{
	struct cell pen = { 0, 0xffffffff, 0xffffffff, -1 };
	char buf[32];
	int x, y, at_x = -1, at_y = -1;

	out_str("\033[?2026h\033[?25l");
	for (y = 0; y < height; y++) {
		for (x = 0; x < width; x++) {
			struct cell *c = &back[y * width + x], *old = &front[y * width + x];

			if (!repaint && !memcmp(c, old, sizeof(*c)))
				continue;
			if (at_x != x || at_y != y) {
				snprintf(buf, sizeof(buf), "\033[%d;%dH", y + 1, x + 1);
				out_str(buf);
			}
			if (c->attr != pen.attr) {
				out_str("\033[0m");
				if (c->attr & ATTR_BOLD)
					out_str("\033[1m");
				if (c->attr & ATTR_ITALIC)
					out_str("\033[3m");
				if (c->attr & ATTR_UNDERLINE)
					out_str("\033[4m");
				pen = (struct cell){ 0, 0xffffffff, 0xffffffff, c->attr };
			}
			if (c->fg != pen.fg)
				out_colour(false, pen.fg = c->fg);
			if (c->bg != pen.bg)
				out_colour(true, pen.bg = c->bg);
			out_utf8(c->ch);
			*old = *c;
			at_x = x + 1;
			at_y = y;
		}
	}
	repaint = false;
	out_str("\033[0m");
	if (cursor_x >= 0 && cursor_x < width && cursor_y >= 0 && cursor_y < height) {
		snprintf(buf, sizeof(buf), "\033[%d;%dH\033[?25h", cursor_y + 1, cursor_x + 1);
		out_str(buf);
	}
	out_str("\033[?2026l");
	out_send();
}

/* Input */

static unsigned char in[256];
static int in_len;

static void in_drop(int n)
{
	memmove(in, in + n, in_len - n);
	in_len -= n;
}

/* Waits up to @timeout_ms for input; true if the window size changed instead */
static bool in_wait(int timeout_ms)
{
	struct pollfd fds[2] = { { STDIN_FILENO, POLLIN, 0 }, { winch_pipe[0], POLLIN, 0 } };
	char drain[16];
	ssize_t n;

	if (poll(fds, 2, timeout_ms) <= 0)
		return false;
	if (fds[1].revents & POLLIN) {
		while (read(winch_pipe[0], drain, sizeof(drain)) > 0)
			;
		return true;
	}
	if (fds[0].revents & (POLLIN | POLLHUP) && in_len < (int)sizeof(in)) {
		n = read(STDIN_FILENO, in + in_len, sizeof(in) - in_len);
		if (n > 0)
			in_len += n;
		else if (n == 0 || (errno != EINTR && errno != EAGAIN))
			exit(1);	/* the terminal is gone */
	}
	return false;
}

/* xterm's modifier parameter: 1 + shift 1, alt 2, control 4 */
static int mods_of(int param)
{
	return param > 1 ? (param - 1) & 7 : 0;
}

/*
 * A control sequence at the start of the input: ESC [ parameters final.
 * Returns the bytes it takes, 0 if more are needed, -1 if it is not one.
 */
static int parse_csi(struct event *ev)
{
	static const int tilde[] = {
		[1] = KEY_HOME, [3] = KEY_DELETE, [4] = KEY_END, [5] = KEY_PGUP, [6] = KEY_PGDN,
		[7] = KEY_HOME, [8] = KEY_END, [11] = KEY_F1, [12] = KEY_F1 + 1, [13] = KEY_F1 + 2,
		[14] = KEY_F1 + 3, [15] = KEY_F1 + 4, [17] = KEY_F1 + 5, [18] = KEY_F1 + 6,
		[19] = KEY_F1 + 7, [20] = KEY_F1 + 8, [21] = KEY_F1 + 9, [23] = KEY_F1 + 10,
		[24] = KEY_F12,
	};
	int params[4] = { 0 }, np = 0, i = 2;
	bool sgr_mouse = false;
	unsigned char final;

	if (i < in_len && in[i] == '<') {
		sgr_mouse = true;
		i++;
	}
	for (; i < in_len; i++) {
		if (in[i] >= '0' && in[i] <= '9') {
			if (np < 4)
				params[np] = params[np] * 10 + in[i] - '0';
		} else if (in[i] == ';') {
			np++;
		} else {
			break;
		}
	}
	if (i >= in_len)
		return in_len > 32 ? -1 : 0;
	final = in[i++];
	ev->mods = mods_of(params[1]);
	ev->key = KEY_NONE;
	if (sgr_mouse) {
		ev->x = params[1] - 1;
		ev->y = params[2] - 1;
		ev->mods = 0;
		if (params[0] & 64) {
			ev->key = KEY_WHEEL;
			ev->button = params[0] & 1 ? 1 : -1;
		} else {
			/* Button 3 is "none": plain motion */
			ev->button = params[0] & 3;
			ev->key = final == 'm' ? KEY_RELEASE : !(params[0] & 32) ? KEY_MOUSE :
				  ev->button == 3 ? KEY_MOVE : KEY_DRAG;
		}
		return i;
	}
	switch (final) {
	case 'A': ev->key = KEY_UP; break;
	case 'B': ev->key = KEY_DOWN; break;
	case 'C': ev->key = KEY_RIGHT; break;
	case 'D': ev->key = KEY_LEFT; break;
	case 'H': ev->key = KEY_HOME; break;
	case 'F': ev->key = KEY_END; break;
	case 'Z': ev->key = KEY_BACKTAB; break;
	case 'O': ev->key = KEY_FOCUS_OUT; break;
	case 'P': case 'Q': case 'R': case 'S':
		ev->key = KEY_F1 + final - 'P';
		break;
	case '~':
		if (params[0] < (int)(sizeof(tilde) / sizeof(*tilde)))
			ev->key = tilde[params[0]];
		break;
	}
	return i;
}

/* One event from the bytes read so far. 0: more bytes are needed */
static int parse(struct event *ev, bool final)
{
	static const int ss3[] = { ['A'] = KEY_UP, ['B'] = KEY_DOWN, ['C'] = KEY_RIGHT,
				   ['D'] = KEY_LEFT, ['H'] = KEY_HOME, ['F'] = KEY_END,
				   ['P'] = KEY_F1, ['Q'] = KEY_F1 + 1, ['R'] = KEY_F1 + 2,
				   ['S'] = KEY_F1 + 3 };
	unsigned char c = in[0];
	int n, more;

	*ev = (struct event){ 0 };
	if (c == 0x1b) {
		if (in_len == 1) {
			if (!final)
				return 0;
			ev->key = KEY_ESC;
			return 1;
		}
		if (in[1] == '[') {
			n = parse_csi(ev);
			if (n > 0 || (n == 0 && !final))
				return n;
			ev->key = KEY_NONE;
			return in_len;	/* a sequence that never ended: drop it */
		}
		if (in[1] == 'O') {
			if (in_len < 3)
				return final ? in_len : 0;
			ev->key = in[2] < sizeof(ss3) / sizeof(*ss3) ? ss3[in[2]] : KEY_NONE;
			return 3;
		}
		/* Alt with a key: ESC, then the key */
		in_drop(1);
		n = parse(ev, final);
		memmove(in + 1, in, in_len++);
		in[0] = 0x1b;
		if (n > 0)
			ev->mods |= MOD_ALT;
		return n ? n + 1 : 0;
	}
	if (c == '\r' || c == '\n') {
		ev->key = KEY_ENTER;
	} else if (c == '\t') {
		ev->key = KEY_TAB;
	} else if (c == 0x7f || c == 0x08) {
		ev->key = KEY_BACKSPACE;
	} else if (c < 0x20) {
		ev->key = c + 'a' - 1;
		ev->mods = MOD_CTRL;
	} else if (c < 0x80) {
		ev->key = c;
	} else {
		more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
		if (in_len < 1 + more)
			return final ? in_len : 0;
		ev->key = c & (0x3f >> more);
		for (n = 1; n <= more; n++)
			ev->key = ev->key << 6 | (in[n] & 0x3f);
		return 1 + more;
	}
	return 1;
}

bool term_poll(struct event *ev, int timeout_ms)
{
	int n;

	for (;;) {
		if (in_len) {
			n = parse(ev, false);
			if (!n) {
				/* Half a sequence: the rest follows at once, or it was Esc */
				if (in_wait(30))
					goto resized;
				n = parse(ev, false);
				if (!n)
					n = parse(ev, true);
			}
			in_drop(n);
			if (ev->key != KEY_NONE)
				return true;
			continue;
		}
		if (in_wait(timeout_ms))
			goto resized;
		if (!in_len)
			return false;
		timeout_ms = 0;
	}
resized:
	resize();
	*ev = (struct event){ .key = KEY_RESIZE };
	return true;
}
