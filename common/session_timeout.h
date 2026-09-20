/* Cooperative, single-client network deadlines. The platform supplies
 * NET_DEADLINE(seconds) and NET_EXPIRED(deadline), using its uptime timer.
 * No timer runs while a command executes outside the network helpers.
 */
#ifndef AGENT_SESSION_TIMEOUT_H
#define AGENT_SESSION_TIMEOUT_H

#ifndef NET_AUTH_SECONDS
#define NET_AUTH_SECONDS 10UL
#endif
#ifndef NET_LINE_SECONDS
#define NET_LINE_SECONDS 60UL
#endif
#ifndef NET_IO_SECONDS
#define NET_IO_SECONDS 30UL
#endif

static int net_failed;
static int net_line_active;
static unsigned long net_line_deadline;
static unsigned long net_line_seconds;

static void net_reset(void) {
    net_failed = net_line_active = 0;
    net_line_seconds = NET_AUTH_SECONDS;
}

static int net_fail(void) {
    net_failed = 1;
    return -1;
}

static void net_begin_line(void) {
    net_line_deadline = NET_DEADLINE(net_line_seconds);
    net_line_active = 1;
}

static void net_end_line(void) {
    net_line_active = 0;
    net_line_seconds = NET_LINE_SECONDS;
}

/* A complete line has an absolute deadline: trickling bytes cannot renew
 * it. Binary I/O instead renews its idle deadline after positive progress.
 * Failure is sticky until the next accepted connection. */
static int net_expired(unsigned long deadline) {
    if (net_failed || (net_line_active ? NET_EXPIRED(net_line_deadline)
                                      : NET_EXPIRED(deadline))) {
        net_fail();
        return 1;
    }
    return 0;
}
#endif
