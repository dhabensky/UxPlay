/* Unit test for netlink_addr_watch_is_addr_change() -- depends on nothing
 * beyond the function under test. Exit 0 = PASS. */
#include <assert.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "../lib/netlink_addr_watch.h"

static void put_nlmsg(unsigned char *buf, __u16 type) {
    struct nlmsghdr nlh;
    memset(&nlh, 0, sizeof(nlh));
    nlh.nlmsg_len = NLMSG_LENGTH(0);
    nlh.nlmsg_type = type;
    memcpy(buf, &nlh, sizeof(nlh));
}

int main(void) {
    unsigned char buf[256];

    /* A real address-change event must be recognized. */
    put_nlmsg(buf, RTM_NEWADDR);
    assert(netlink_addr_watch_is_addr_change(buf, NLMSG_LENGTH(0)) == true);

    put_nlmsg(buf, RTM_DELADDR);
    assert(netlink_addr_watch_is_addr_change(buf, NLMSG_LENGTH(0)) == true);

    /* An unrelated event (e.g. link up/down) must NOT trigger a refresh. */
    put_nlmsg(buf, RTM_NEWLINK);
    assert(netlink_addr_watch_is_addr_change(buf, NLMSG_LENGTH(0)) == false);

    /* A batch with an irrelevant message followed by a real one must
     * still be recognized -- the parser must walk the whole batch. */
    {
        size_t off = 0;
        put_nlmsg(buf + off, RTM_NEWLINK);
        off += NLMSG_ALIGN(NLMSG_LENGTH(0));
        put_nlmsg(buf + off, RTM_NEWADDR);
        off += NLMSG_ALIGN(NLMSG_LENGTH(0));
        assert(netlink_addr_watch_is_addr_change(buf, off) == true);
    }

    /* No data -- nothing to report. */
    assert(netlink_addr_watch_is_addr_change(buf, 0) == false);

    /* Smoke-test socket creation; not asserted against a specific value
     * since sandboxed/unprivileged CI containers can plausibly lack
     * netlink -- callers already treat -1 as "fall back to polling". */
    int fd = netlink_addr_watch_open();
    if (fd >= 0) {
        close(fd);
    }

    printf("PASS: netlink_addr_watch_is_addr_change all cases correct\n");
    return 0;
}
