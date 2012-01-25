/*
 * Copyright (C) 2002-2004 Sistina Software, Inc. All rights reserved.
 * Copyright (C) 2004-2011 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*

  CLVMD Cluster LVM daemon command processor.

  To add commands to the daemon simply add a processor in do_command and return
  and messages back in buf and the length in *retlen. The initial value of
  buflen is the maximum size of the buffer. if buf is not large enough then it
  may be reallocated by the functions in here to a suitable size bearing in
  mind that anything larger than the passed-in size will have to be returned
  using the system LV and so performance will suffer.

  The status return will be negated and passed back to the originating node.

  pre- and post- command routines are called only on the local node. The
  purpose is primarily to get and release locks, though the pre- routine should
  also do any other local setups required by the command (if any) and can
  return a failure code that prevents the command from being distributed around
  the cluster

  The pre- and post- routines are run in their own thread so can block as long
  they like, do_command is run in the main clvmd thread so should not block for
  too long. If the pre-command returns an error code (!=0) then the command
  will not be propogated around the cluster but the post-command WILL be called

  Also note that the pre and post routine are *always* called on the local
  node, even if the command to be executed was only requested to run on a
  remote node. It may peek inside the client structure to check the status of
  the command.

  The clients of the daemon must, naturally, understand the return messages and
  codes.

  Routines in here may only READ the values in the client structure passed in
  apart from client->private which they are free to do what they like with.

*/

#include "clvmd-common.h"

#include <pthread.h>

#include "clvmd-comms.h"
#include "clvm.h"
#include "clvmd.h"
#include "lvm-globals.h"
#include "lvm-functions.h"

#include "locking.h"

#include <sys/utsname.h>

extern struct cluster_ops *clops;
static int restart_clvmd(void);

/* This is where all the real work happens:
   NOTE: client will be NULL when this is executed on a remote node */
int do_command(struct local_client *client, struct clvm_header *msg, int msglen,
	       char **buf, int buflen, int *retlen)
{
	char *args = msg->node + strlen(msg->node) + 1;
	int arglen = msglen - sizeof(struct clvm_header) - strlen(msg->node);
	int status = 0;
	char *lockname;
	const char *locktype;
	struct utsname nodeinfo;
	unsigned char lock_cmd;
	unsigned char lock_flags;

	/* Reset test mode before we start */
	init_test(0);

	/* Do the command */
	switch (msg->cmd) {
		/* Just a test message */
	case CLVMD_CMD_TEST:
		if (arglen > buflen) {
			char *new_buf;
			buflen = arglen + 200;
			new_buf = realloc(*buf, buflen);
			if (new_buf == NULL) {
				status = errno;
				free (*buf);
			}
			*buf = new_buf;
		}
		if (*buf) {
			if (uname(&nodeinfo))
				memset(&nodeinfo, 0, sizeof(nodeinfo));

			*retlen = 1 + dm_snprintf(*buf, buflen,
						  "TEST from %s: %s v%s",
						  nodeinfo.nodename, args,
						  nodeinfo.release);
		}
		break;

	case CLVMD_CMD_LOCK_VG:
		lock_cmd = args[0];
		lock_flags = args[1];
		lockname = &args[2];
		/* Check to see if the VG is in use by LVM1 */
		status = do_check_lvm1(lockname);
		if (lock_flags & LCK_TEST_MODE)
			init_test(1);
		do_lock_vg(lock_cmd, lock_flags, lockname);
		break;

	case CLVMD_CMD_LOCK_LV:
		/* This is the biggie */
		lock_cmd = args[0];
		lock_flags = args[1];
		lockname = &args[2];
		if (lock_flags & LCK_TEST_MODE)
			init_test(1);
		status = do_lock_lv(lock_cmd, lock_flags, lockname);
		/* Replace EIO with something less scary */
		if (status == EIO) {
			*retlen = 1 + dm_snprintf(*buf, buflen, "%s",
						  get_last_lvm_error());
			return EIO;
		}
		break;

	case CLVMD_CMD_LOCK_QUERY:
		lockname = &args[2];
		if (buflen < 3)
			return EIO;
		if ((locktype = do_lock_query(lockname)))
			*retlen = 1 + dm_snprintf(*buf, buflen, "%s", locktype);
		break;

	case CLVMD_CMD_REFRESH:
		do_refresh_cache();
		break;

	case CLVMD_CMD_SYNC_NAMES:
		lvm_do_fs_unlock();
		break;

	case CLVMD_CMD_SET_DEBUG:
		clvmd_set_debug(args[0]);
		break;

	case CLVMD_CMD_RESTART:
		status = restart_clvmd();
		break;

	case CLVMD_CMD_GET_CLUSTERNAME:
		status = clops->get_cluster_name(*buf, buflen);
		if (!status)
			*retlen = strlen(*buf)+1;
		break;

	case CLVMD_CMD_VG_BACKUP:
		/*
		 * Do not run backup on local node, caller should do that.
		 */
		if (!client)
			lvm_do_backup(&args[2]);
		break;

	default:
		/* Won't get here because command is validated in pre_command */
		break;
	}

	/* Check the status of the command and return the error text */
	if (status) {
		*retlen = 1 + ((*buf) ? dm_snprintf(*buf, buflen, "%s",
						    strerror(status)) : -1);
	}

	return status;

}

static int lock_vg(struct local_client *client)
{
    struct dm_hash_table *lock_hash;
    struct clvm_header *header =
	(struct clvm_header *) client->bits.localsock.cmd;
    unsigned char lock_cmd;
    int lock_mode;
    char *args = header->node + strlen(header->node) + 1;
    int lkid;
    int status = 0;
    char *lockname;

    /* Keep a track of VG locks in our own hash table. In current
       practice there should only ever be more than two VGs locked
       if a user tries to merge lots of them at once */
    if (client->bits.localsock.private) {
	lock_hash = (struct dm_hash_table *)client->bits.localsock.private;
    }
    else {
	lock_hash = dm_hash_create(3);
	if (!lock_hash)
	    return ENOMEM;
	client->bits.localsock.private = (void *)lock_hash;
    }

    lock_cmd = args[0] & (LCK_NONBLOCK | LCK_HOLD | LCK_SCOPE_MASK | LCK_TYPE_MASK);
    lock_mode = ((int)lock_cmd & LCK_TYPE_MASK);
    /* lock_flags = args[1]; */
    lockname = &args[2];
    DEBUGLOG("doing PRE command LOCK_VG '%s' at %x (client=%p)\n", lockname, lock_cmd, client);

    if (lock_mode == LCK_UNLOCK) {

	lkid = (int)(long)dm_hash_lookup(lock_hash, lockname);
	if (lkid == 0)
	    return EINVAL;

	status = sync_unlock(lockname, lkid);
	if (status)
	    status = errno;
	else
	    dm_hash_remove(lock_hash, lockname);
    }
    else {
	/* Read locks need to be PR; other modes get passed through */
	if (lock_mode == LCK_READ)
	    lock_mode = LCK_PREAD;
	status = sync_lock(lockname, lock_mode, (lock_cmd & LCK_NONBLOCK) ? LCKF_NOQUEUE : 0, &lkid);
	if (status)
	    status = errno;
	else
	    dm_hash_insert(lock_hash, lockname, (void *)(long)lkid);
    }

    return status;
}


/* Pre-command is a good place to get locks that are needed only for the duration
   of the commands around the cluster (don't forget to free them in post-command),
   and to sanity check the command arguments */
int do_pre_command(struct local_client *client)
{
	struct clvm_header *header =
	    (struct clvm_header *) client->bits.localsock.cmd;
	unsigned char lock_cmd;
	unsigned char lock_flags;
	char *args = header->node + strlen(header->node) + 1;
	int lockid = 0;
	int status = 0;
	char *lockname;

	init_test(0);
	switch (header->cmd) {
	case CLVMD_CMD_TEST:
		status = sync_lock("CLVMD_TEST", LCK_EXCL, 0, &lockid);
		client->bits.localsock.private = (void *)(long)lockid;
		break;

	case CLVMD_CMD_LOCK_VG:
		lockname = &args[2];
		/* We take out a real lock unless LCK_CACHE was set */
		if (!strncmp(lockname, "V_", 2) ||
		    !strncmp(lockname, "P_#", 3))
			status = lock_vg(client);
		break;

	case CLVMD_CMD_LOCK_LV:
		lock_cmd = args[0];
		lock_flags = args[1];
		lockname = &args[2];
		if (lock_flags & LCK_TEST_MODE)
			init_test(1);
		status = pre_lock_lv(lock_cmd, lock_flags, lockname);
		break;

	case CLVMD_CMD_REFRESH:
	case CLVMD_CMD_GET_CLUSTERNAME:
	case CLVMD_CMD_SET_DEBUG:
	case CLVMD_CMD_VG_BACKUP:
	case CLVMD_CMD_SYNC_NAMES:
	case CLVMD_CMD_LOCK_QUERY:
	case CLVMD_CMD_RESTART:
		break;

	default:
		log_error("Unknown command %d received\n", header->cmd);
		status = EINVAL;
	}
	return status;
}

/* Note that the post-command routine is called even if the pre-command or the real command
   failed */
int do_post_command(struct local_client *client)
{
	struct clvm_header *header =
	    (struct clvm_header *) client->bits.localsock.cmd;
	int status = 0;
	unsigned char lock_cmd;
	unsigned char lock_flags;
	char *args = header->node + strlen(header->node) + 1;
	char *lockname;

	init_test(0);
	switch (header->cmd) {
	case CLVMD_CMD_TEST:
		status =
		    sync_unlock("CLVMD_TEST", (int) (long) client->bits.localsock.private);
		client->bits.localsock.private = 0;
		break;

	case CLVMD_CMD_LOCK_LV:
		lock_cmd = args[0];
		lock_flags = args[1];
		lockname = &args[2];
		if (lock_flags & LCK_TEST_MODE)
			init_test(1);
		status = post_lock_lv(lock_cmd, lock_flags, lockname);
		break;

	default:
		/* Nothing to do here */
		break;
	}
	return status;
}


/* Called when the client is about to be deleted */
void cmd_client_cleanup(struct local_client *client)
{
    if (client->bits.localsock.private) {

	struct dm_hash_node *v;
	struct dm_hash_table *lock_hash =
	    (struct dm_hash_table *)client->bits.localsock.private;

	dm_hash_iterate(v, lock_hash) {
		int lkid = (int)(long)dm_hash_get_data(lock_hash, v);
		char *lockname = dm_hash_get_key(lock_hash, v);

		DEBUGLOG("cleanup: Unlocking lock %s %x\n", lockname, lkid);
		sync_unlock(lockname, lkid);
	}

	dm_hash_destroy(lock_hash);
	client->bits.localsock.private = 0;
    }
}


static int restart_clvmd(void)
{
	const char **argv;
	char *lv_name;
	int argc = 0, max_locks = 0;
	struct dm_hash_node *hn = NULL;
	char debug_arg[16];
	const char *clvmd = getenv("LVM_CLVMD_BINARY") ? : CLVMD_PATH;

	DEBUGLOG("clvmd restart requested\n");

	/* Count exclusively-open LVs */
	do {
		hn = get_next_excl_lock(hn, &lv_name);
		if (lv_name) {
			max_locks++;
			if (!*lv_name)
				break; /* FIXME: Is this error ? */
		}
	} while (hn);

	/* clvmd + locks (-E uuid) + debug (-d X) + NULL */
	if (!(argv = malloc((max_locks * 2 + 5) * sizeof(*argv))))
		goto_out;

	/*
	 * Build the command-line
	 */
	argv[argc++] = "clvmd";

	/* Propogate debug options */
	if (clvmd_get_debug()) {
		if (dm_snprintf(debug_arg, sizeof(debug_arg), "-d%u", clvmd_get_debug()) < 0)
			goto_out;
		argv[argc++] = debug_arg;
	}

	argv[argc++] = "-I";
	argv[argc++] = clops->name;

	/* Now add the exclusively-open LVs */
	hn = NULL;
	do {
		hn = get_next_excl_lock(hn, &lv_name);
		if (lv_name) {
			if (!*lv_name)
				break; /* FIXME: Is this error ? */
			argv[argc++] = "-E";
			argv[argc++] = lv_name;
			DEBUGLOG("excl lock: %s\n", lv_name);
		}
	} while (hn);
	argv[argc] = NULL;

	/* Exec new clvmd */
	DEBUGLOG("--- Restarting %s ---\n", clvmd);
	for (argc = 1; argv[argc]; argc++) DEBUGLOG("--- %d: %s\n", argc, argv[argc]);

	/* NOTE: This will fail when downgrading! */
	execvp(clvmd, (char **)argv);
out:
	/* We failed */
	DEBUGLOG("Restart of clvmd failed.\n");

	free(argv);

	return EIO;
}
