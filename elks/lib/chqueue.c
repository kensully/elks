/* lib/chqueue.c
 * (C) 1997 Chad Page
 *
 * (Based on the original character queue code by Alan Cox(?))
 *
 * Array queue handling for new tty drivers... rather generic routine, and
 * flexible to boot.  The character queue uses a data structure that
 * references a seperately set up char array, removing hard limits from the
 * queing code.
 *
 * I can't help but think there's a race condition somewhere in here :)
 *
 * 'warming' will be done by the tty driver itself when it copies data over
 *
 * 16 July 2001 : A divide is quite expensive on an 8086 so I changed the
 * code to use logical addition and not modulo. The only drawback is we
 * have to use only power of two buffer sizes. (Harry Kalogirou)
 */

#include <linuxmt/config.h>
#include <linuxmt/wait.h>
#include <linuxmt/chqueue.h>
#include <linuxmt/sched.h>
#include <linuxmt/types.h>
#include <linuxmt/debug.h>

void chq_erase(register struct ch_queue *q)
{
    q->len = q->tail = 0;
}

void chq_init(register struct ch_queue *q, unsigned char *buf, int size)
{
    debug3("CHQ: chq_init(%d, %d, %d)\n", q, buf, size);
    q->buf = (char *) buf;
    q->size = size;
    chq_erase(q);
}

/* Adds character c, waiting if wait=1 (or otherwise throws out new char) */
int chq_addch(register struct ch_queue *q, unsigned char c, int wait)
{
    unsigned int nhead;

    debug5("CHQ: chq_addch(%d, %c, %d) q->len=%d q->tail=%d\n", q, c, 0,
	   q->len, q->tail);

    if (q->len == q->size) {
	if (wait) {
	    debug("CHQ: addch sleeping\n");
	    interruptible_sleep_on(&q->wq);
	    debug("CHQ: addch waken up\n");
	}
    }

    if (q->len == q->size)
	return -1;

    nhead = (unsigned int) ((q->tail + q->len) & (q->size - 1));
    q->buf[nhead] = (char) c;
    q->len++;
    wake_up(&q->wq);

    return 0;
}

#if 0
/* Deletes last character in list */
int chq_delch(register struct ch_queue *q)
{
    if (q->len == q->size) {
	q->len--;
	return 1;
    }
    return 0;
}
#endif

/* Gets tail character, waiting for one if wait != 0 */
int chq_getch(register struct ch_queue *q, register unsigned char *c, int wait)
{
    int retval;

    debug6("CHQ: chq_getch(%d, %d, %d) q->len=%d q->tail=%d q->size=%d\n",
	   q, c, wait, q->len, q->tail, q->size);

    if (q->len == 0) {
	if (wait) {
	    debug("CHQ: getch sleeping\n");
	    interruptible_sleep_on(&q->wq);
	    debug("CHQ: getch wokeup\n");
	}
    }
    if (q->len == 0)
	return -1;

    retval = q->buf[q->tail];
    q->tail++;
    q->tail &= q->size - 1;
    q->len--;

    wake_up(&q->wq);
    if (c != 0)
	*c = (unsigned char) retval;

    return retval;
}

int chq_peekch(register struct ch_queue *q)
{
    return (q->len != 0);
}

int chq_full(register struct ch_queue *q)
{
    return (q->len == q->size);
}
