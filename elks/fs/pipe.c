/*
 *  elks/fs/pipe.c
 *
 * Copyright (C) 1991, 1992 Linux Torvalds
 * 		 1998 Alistair Riddoch
 */

#include <linuxmt/config.h>
#include <linuxmt/types.h>
#include <linuxmt/sched.h>
#include <linuxmt/kernel.h>
#include <linuxmt/major.h>
#include <linuxmt/stat.h>
#include <linuxmt/errno.h>
#include <linuxmt/fcntl.h>
#include <linuxmt/string.h>
#include <linuxmt/time.h>
#include <linuxmt/locks.h>
#include <linuxmt/mm.h>
#include <linuxmt/fs.h>
#include <linuxmt/kdev_t.h>
#include <linuxmt/wait.h>
#include <linuxmt/debug.h>

#include <arch/system.h>
#include <arch/segment.h>
#include <arch/bitops.h>

#define MAX_PIPES 8

int get_unused_fd(struct file *f)
{
    register char *pfd = 0;
    register struct file_struct *cfs = &current->files;

    do {
	if (!cfs->fd[(unsigned int) pfd]) {
	    cfs->fd[(unsigned int) pfd] = f;
	    clear_bit((unsigned int) pfd,
			     &cfs->close_on_exec);
	    return (int) pfd;
	}
    } while (((int)(++pfd)) < NR_OPEN);

    return -EMFILE;
}

loff_t pipe_lseek(struct inode *inode, struct file *file, loff_t offset,
		  int orig)
{
    debug("PIPE: lseek called.\n");
    return -ESPIPE;
}

#ifdef CONFIG_PIPE

/*
 *	FIXME - we should pull pipes off the root (or pipe ?) fs as per
 *	V7, and they should be buffers
 */

static char pipe_base[MAX_PIPES][PIPE_BUF];

static int pipe_in_use[(MAX_PIPES + 15)/16];

static char *get_pipe_mem(void)
{
    int i = 0;

    i = find_first_zero_bit(pipe_in_use, MAX_PIPES);
    if(i < MAX_PIPES) {
	set_bit(i, pipe_in_use);
	return pipe_base[i];
    }

    debug("PIPE: No more buffers.\n");		/* FIXME */
    return NULL;
}

static void free_pipe_mem(char *buf)
{
    int i;

    i = ((unsigned int)pipe_base - (unsigned int)buf)/PIPE_BUF;
    if(i < MAX_PIPES)
	clear_bit(i, pipe_in_use);
}

static size_t pipe_read(register struct inode *inode, struct file *filp,
		     char *buf, size_t count)
{
    size_t read = 0;
    register char *chars;

    debug("PIPE: read called.\n");
    while(!(inode->u.pipe_i.len) || (inode->u.pipe_i.lock)) {
	if(!(inode->u.pipe_i.lock) && !(inode->u.pipe_i.writers)) {
	    return 0;
	}
	if(filp->f_flags & O_NONBLOCK) {
	    return -EAGAIN;
	}
	if(current->signal)
	    return -ERESTARTSYS;
	interruptible_sleep_on(&(inode->u.pipe_i.wait));
    }
    (inode->u.pipe_i.lock)++;
    while (count > 0 && inode->u.pipe_i.len) {
	chars = (char *)(PIPE_BUF - (inode->u.pipe_i.start));
	if ((size_t)chars > count)
	    chars = (char *)count;
	if ((size_t)chars > inode->u.pipe_i.len)
	    chars = (char *)(inode->u.pipe_i.len);
	memcpy_tofs(buf, (inode->u.pipe_i.base+inode->u.pipe_i.start), (size_t)chars);
	buf += (size_t)chars;
        inode->u.pipe_i.start = (inode->u.pipe_i.start + (size_t)chars)&(PIPE_BUF-1);
	(inode->u.pipe_i.len) -= (size_t)chars;
	read += (size_t)chars;
	count -= (size_t)chars;
    }
    (inode->u.pipe_i.lock)--;
    wake_up_interruptible(&(inode->u.pipe_i.wait));
    if (read) {
	inode->i_atime = CURRENT_TIME;
	return (int) read;
    }
    if ((inode->u.pipe_i.writers))
	return -EAGAIN;
    return 0;
}

static size_t pipe_write(register struct inode *inode, struct file *filp,
		      char *buf, size_t count)
{
    size_t free, tmp, written = 0;
    register char *chars;

    debug("PIPE: write called.\n");
    if (!(inode->u.pipe_i.readers)) {
	send_sig(SIGPIPE, current, 0);
	return -EPIPE;
    }

    free = (count <= PIPE_BUF) ? count : 1;
    while (count > 0) {
	while (((PIPE_BUF - (inode->u.pipe_i.len)) < free)
	       || (inode->u.pipe_i.lock)) {
	    if (!(inode->u.pipe_i.readers)) {
		send_sig(SIGPIPE, current, 0);
		return written ? (int) written : -EPIPE;
	    }
	    if (current->signal)
		return written ? (int) written : -ERESTARTSYS;
	    if (filp->f_flags & O_NONBLOCK)
		return written ? (int) written : -EAGAIN;
	    interruptible_sleep_on(&(inode->u.pipe_i.wait));
	}
	(inode->u.pipe_i.lock)++;
	while (count > 0 && (free = (PIPE_BUF - inode->u.pipe_i.len))) {

            tmp = (inode->u.pipe_i.start + inode->u.pipe_i.len)&(PIPE_BUF-1);
	    chars = (char *)(PIPE_BUF - tmp);
	    if ((size_t)chars > count)
		chars = (char *) count;
	    if ((size_t)chars > free)
		chars = (char *)free;

	    memcpy_fromfs((inode->u.pipe_i.base + tmp), buf, (size_t)chars);
	    buf += (size_t)chars;
	    (inode->u.pipe_i.len) += (size_t)chars;
	    written += (size_t)chars;
	    count -= (size_t)chars;
	}
	(inode->u.pipe_i.lock)--;
	wake_up_interruptible(&(inode->u.pipe_i.wait));
	free = 1;
    }
    inode->i_ctime = inode->i_mtime = CURRENT_TIME;

    return written;
}

#ifdef STRICT_PIPES
static void pipe_read_release(register struct inode *inode, struct file *filp)
{
    debug("PIPE: read_release called.\n");
    (inode->u.pipe_i.readers)--;
    wake_up_interruptible(&(inode->u.pipe_i.wait));
}

static void pipe_write_release(register struct inode *inode, struct file *filp)
{
    debug("PIPE: write_release called.\n");
    (inode->u.pipe_i.writers)--;
    wake_up_interruptible(&(inode->u.pipe_i.wait));
}
#endif

static void pipe_rdwr_release(register struct inode *inode,
			      register struct file *filp)
{
    debug("PIPE: rdwr_release called.\n");

    if (filp->f_mode & FMODE_READ)
	(inode->u.pipe_i.readers)--;

    if (filp->f_mode & FMODE_WRITE)
	(inode->u.pipe_i.writers)--;

    if(!(inode->u.pipe_i.readers + inode->u.pipe_i.writers)) {
	if(inode->u.pipe_i.base) {
	/* Free up any memory allocated to the pipe */
	    free_pipe_mem(inode->u.pipe_i.base);
	    inode->u.pipe_i.base = NULL;
	}
    }
    else
	wake_up_interruptible(&(inode->u.pipe_i.wait));
}

#ifdef STRICT_PIPES
static int pipe_read_open(struct inode *inode, struct file *filp)
{
    debug("PIPE: read_open called.\n");
    (inode->u.pipe_i.readers)++;

    return 0;
}

static int pipe_write_open(struct inode *inode, struct file *filp)
{
    debug("PIPE: write_open called.\n");
    (inode->u.pipe_i.writers)++;

    return 0;
}
#endif

static int pipe_rdwr_open(register struct inode *inode,
			  register struct file *filp)
{
    debug("PIPE: rdwr called.\n");

    if(!PIPE_BASE(*inode)) {
	if(!(PIPE_BASE(*inode) = get_pipe_mem()))
	    return -ENOMEM;
#if 0
	/* next fields already set to zero by get_empty_inode() */
	PIPE_START(*inode) = PIPE_LEN(*inode) = 0;
	PIPE_RD_OPENERS(*inode) = PIPE_WR_OPENERS(*inode) = 0;
	PIPE_READERS(*inode) = PIPE_WRITERS(*inode) = 0;
	PIPE_LOCK(*inode) = 0;
#endif
    }
    if (filp->f_mode & FMODE_READ) {
	(inode->u.pipe_i.readers)++;
	if(inode->u.pipe_i.writers > 0) {
	    if(inode->u.pipe_i.readers < 2)
		wake_up_interruptible(&(inode->u.pipe_i.wait));
	}
	else {
	    if(!(filp->f_flags & O_NONBLOCK) && (inode->i_sb))
		while(!(inode->u.pipe_i.writers))
		    interruptible_sleep_on(&(inode->u.pipe_i.wait));
	}
    }

    if (filp->f_mode & FMODE_WRITE) {
	(inode->u.pipe_i.writers)++;
	if(inode->u.pipe_i.readers > 0) {
	    if(inode->u.pipe_i.writers < 2)
		wake_up_interruptible(&(inode->u.pipe_i.wait));
	}
	else {
	    if(filp->f_flags & O_NONBLOCK)
		return -ENXIO;
	    while(!(inode->u.pipe_i.readers))
		interruptible_sleep_on(&(inode->u.pipe_i.wait));
	}
    }
    return 0;
}

#ifdef STRICT_PIPES
static size_t bad_pipe_rw(struct inode *inode, struct file *filp,
		       char *buf, int count)
{
    debug("PIPE: bad rw called.\n");

    return -EBADF;
}

/*@-type@*/

struct file_operations read_pipe_fops = {
    pipe_lseek, pipe_read, bad_pipe_rw, NULL,	/* no readdir */
    NULL,			/* select */
    NULL,			/* ioctl */
    pipe_read_open, pipe_read_release,
};

struct file_operations write_pipe_fops = {
    pipe_lseek,
    bad_pipe_rw,
    pipe_write,
    NULL,			/* no readdir */
    NULL,			/* select */
    NULL,			/* ioctl */
    pipe_write_open,
    pipe_write_release,
};
#endif

struct file_operations rdwr_pipe_fops = {
    pipe_lseek,
    pipe_read,
    pipe_write,
    NULL,			/* no readdir */
    NULL,			/* select */
    NULL,			/* ioctl */
    pipe_rdwr_open,
    pipe_rdwr_release,
};

struct inode_operations pipe_inode_operations = {
    &rdwr_pipe_fops, NULL,	/* create */
    NULL,			/* lookup */
    NULL,			/* link */
    NULL,			/* unlink */
    NULL,			/* symlink */
    NULL,			/* mkdir */
    NULL,			/* rmdir */
    NULL,			/* mknod */
    NULL,			/* readlink */
    NULL,			/* follow_link */
#ifdef BLOAT_FS
    NULL,			/* bmap */
#endif
    NULL,			/* truncate */
#ifdef BLOAT_FS
    NULL			/* permission */
#endif
};

/*@+type@*/

static int do_pipe(int *fd)
{
    register struct inode *inode;
    struct file *f1;
    struct file *f2;
    int error = -ENOMEM;
    int i;

    if(!(inode = new_inode(NULL, S_IFIFO | S_IRUSR | S_IWUSR)))	/* Create inode */
	goto no_inodes;

    /* read file */
    if((error = open_filp(O_RDONLY, inode, &f1)))
	goto no_files;

    if ((error = get_unused_fd(f1)) < 0)
	goto close_f1;
    fd[0] = error;
    i = error;

    (inode->i_count)++;		/* Increase inode usage count */
    /* write file */
    if((error = open_filp(O_WRONLY, inode, &f2)))
	goto close_f1_i;

    if ((error = get_unused_fd(f2)) < 0)
	goto close_f12;
    fd[1] = error;

    return 0;

  close_f12:
    close_filp(inode, f2);

  close_f1_i:
    current->files.fd[i] = NULL;
    inode->i_count--;

  close_f1:
    close_filp(inode, f1);

  no_files:
    iput(inode);

  no_inodes:
    return error;
}

int sys_pipe(unsigned int *filedes)
{
    int fd[2];
    int error;

    debug("PIPE: called.\n");

    if ((error = do_pipe(fd)))
	return error;

    debug2("PIPE: Returned %d %d.\n", fd[0], fd[1]);

    return verified_memcpy_tofs(filedes, fd, 2 * sizeof(int));
}

#else

int sys_pipe(unsigned int *filedes)
{
    return -ENOSYS;
}

#endif
