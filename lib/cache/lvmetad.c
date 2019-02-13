/*
 * Copyright (C) 2012 Red Hat, Inc.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "lib.h"
#include "toolcontext.h"
#include "metadata.h"
#include "device.h"
#include "lvmetad.h"
#include "lvmcache.h"
#include "lvmetad-client.h"
#include "format-text.h" // TODO for disk_locn, used as a DA representation
#include "crc.h"
#include "lvm-signal.h"
#include "lvmlockd.h"
#include "str_list.h"

#include <time.h>

static daemon_handle _lvmetad = { .error = 0 };
static int _lvmetad_use = 0;
static int _lvmetad_connected = 0;
static int _lvmetad_daemon_pid = 0;
static int _was_connected = 0;

static char *_lvmetad_token = NULL;
static const char *_lvmetad_socket = NULL;
static struct cmd_context *_lvmetad_cmd = NULL;
static int64_t _lvmetad_update_timeout;

static struct volume_group *_lvmetad_pvscan_vg(struct cmd_context *cmd, struct volume_group *vg, const char *vgid, struct format_type *fmt);

static uint64_t _monotonic_seconds(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
		return 0;
	return ts.tv_sec;
}

static int _log_debug_inequality(const char *name, struct dm_config_node *a, struct dm_config_node *b)
{
	int result = 0;
	int final_result = 0;

	if (a->v && b->v) {
		result = compare_value(a->v, b->v);
		if (result) {
			struct dm_config_value *av = a->v;
			struct dm_config_value *bv = b->v;

			if (!strcmp(a->key, b->key)) {
				if (a->v->type == DM_CFG_STRING && b->v->type == DM_CFG_STRING)
					log_debug_lvmetad("VG %s metadata inequality at %s / %s: %s / %s",
							  name, a->key, b->key, av->v.str, bv->v.str);
				else if (a->v->type == DM_CFG_INT && b->v->type == DM_CFG_INT)
					log_debug_lvmetad("VG %s metadata inequality at %s / %s: " FMTd64 " / " FMTd64,
							  name, a->key, b->key, av->v.i, bv->v.i);
				else
					log_debug_lvmetad("VG %s metadata inequality at %s / %s: type %d / type %d",
							  name, a->key, b->key, av->type, bv->type);
			} else {
				log_debug_lvmetad("VG %s metadata inequality at %s / %s", name, a->key, b->key);
			}
			final_result = result;
		}
	}

	if (a->v && !b->v) {
		log_debug_lvmetad("VG %s metadata inequality at %s / %s", name, a->key, b->key);
		final_result = 1;
	}

	if (!a->v && b->v) {
		log_debug_lvmetad("VG %s metadata inequality at %s / %s", name, a->key, b->key);
		final_result = -1;
	}

	if (a->child && b->child) {
		result = _log_debug_inequality(name, a->child, b->child);
		if (result)
			final_result = result;
	}

	if (a->sib && b->sib) {
		result = _log_debug_inequality(name, a->sib, b->sib);
		if (result)
			final_result = result;
	}
	

	if (a->sib && !b->sib) {
		log_debug_lvmetad("VG %s metadata inequality at %s / %s", name, a->key, b->key);
		final_result = 1;
	}

	if (!a->sib && b->sib) {
		log_debug_lvmetad("VG %s metadata inequality at %s / %s", name, a->key, b->key);
		final_result = -1;
	}

	return final_result;
}

void lvmetad_disconnect(void)
{
	if (_lvmetad_connected) {
		daemon_close(_lvmetad);
		_was_connected = 1;
	}

	_lvmetad_connected = 0;
	_lvmetad_use = 0;
	_lvmetad_cmd = NULL;
}

int lvmetad_connect(struct cmd_context *cmd)
{
	if (!lvmetad_socket_present()) {
		log_debug_lvmetad("Failed to connect to lvmetad: socket not present.");
		_lvmetad_connected = 0;
		_lvmetad_use = 0;
		_lvmetad_cmd = NULL;
		return 0;
	}

	_lvmetad_update_timeout = find_config_tree_int(cmd, global_lvmetad_update_wait_time_CFG, NULL);

	_lvmetad = lvmetad_open(_lvmetad_socket);

	if (_lvmetad.socket_fd >= 0 && !_lvmetad.error) {
		log_debug_lvmetad("Successfully connected to lvmetad on fd %d.",
				  _lvmetad.socket_fd);
		_lvmetad_connected = 1;
		_lvmetad_use = 1;
		_lvmetad_cmd = cmd;
		return 1;
	}

	log_debug_lvmetad("Failed to connect to lvmetad: %s", strerror(_lvmetad.error));
	_lvmetad_connected = 0;
	_lvmetad_use = 0;
	_lvmetad_cmd = NULL;

	return 0;
}

int lvmetad_used(void)
{
	return _lvmetad_use;
}

void lvmetad_make_unused(struct cmd_context *cmd)
{
	lvmetad_disconnect();

	if (cmd && !refresh_filters(cmd))
		stack;
}

int lvmetad_pidfile_present(void)
{
	const char *pidfile = getenv("LVM_LVMETAD_PIDFILE") ?: LVMETAD_PIDFILE;

	return !access(pidfile, F_OK);
}

int lvmetad_socket_present(void)
{
	const char *socket = _lvmetad_socket ?: LVMETAD_SOCKET;
	int r;

	if ((r = access(socket, F_OK)) && errno != ENOENT)
		log_sys_error("access", socket);

	return !r;
}

void lvmetad_set_socket(const char *sock)
{
	_lvmetad_socket = sock;
}

/*
 * Use a crc of the strings in the filter as the lvmetad token.
 */
void lvmetad_set_token(const struct dm_config_value *filter)
{
	int ft = 0;

	dm_free(_lvmetad_token);

	while (filter && filter->type == DM_CFG_STRING) {
		ft = calc_crc(ft, (const uint8_t *) filter->v.str, strlen(filter->v.str));
		filter = filter->next;
	}

	if (dm_asprintf(&_lvmetad_token, "filter:%u", ft) < 0)
		log_warn("WARNING: Failed to set lvmetad token. Out of memory?");
}

void lvmetad_release_token(void)
{
	dm_free(_lvmetad_token);
	_lvmetad_token = NULL;
}

/*
 * Check if lvmetad's token matches our token.  The token is a hash of the
 * global filter used to populate lvmetad.  The lvmetad token was set by the
 * last command to populate lvmetad, and it was set to the hash of the global
 * filter that command used when scanning to populate lvmetad.
 *
 * Our token is a hash of the global filter this command is using.
 *
 * If the lvmetad token is not set (or "none"), then lvmetad has not been
 * populated.  If the lvmetad token is "update in progress", then lvmetad is
 * currently being populated -- this should be temporary, so wait for a while
 * for the current update to finish and then compare our token with the new one
 * (hopefully it will match).  If the lvmetad token otherwise differs from
 * ours, then lvmetad was populated using a different global filter that we are
 * using.
 *
 * Return 1 if the lvmetad token matches ours.  We can use it as is.
 *
 * Return 0 if the lvmetad token does not match ours (lvmetad is empty or
 * populated using a different global filter).  The caller will repopulate
 * lvmetad (via lvmetad_pvscan_all_devs) before using lvmetad.
 *
 * If we time out waiting for an lvmetad update to finish, then disable this
 * command's use of lvmetad and return 0.
 */

int lvmetad_token_matches(struct cmd_context *cmd)
{
	daemon_reply reply;
	const char *daemon_token;
	unsigned int delay_usec = 0;
	unsigned int wait_sec = 0;
	uint64_t now = 0, wait_start = 0;
	int ret = 1;

	wait_sec = (unsigned int)_lvmetad_update_timeout;

retry:
	log_debug_lvmetad("Sending lvmetad get_global_info");

	reply = daemon_send_simple(_lvmetad, "get_global_info",
				   "token = %s", "skip",
				   "pid = " FMTd64, (int64_t)getpid(),
				   "cmd = %s", get_cmd_name(),
				   NULL);
	if (reply.error) {
		log_warn("WARNING: Not using lvmetad after send error (%d).", reply.error);
		goto fail;
	}

	if (strcmp(daemon_reply_str(reply, "response", ""), "OK")) {
		log_warn("WARNING: Not using lvmetad after response error.");
		goto fail;
	}

	if (!(daemon_token = daemon_reply_str(reply, "token", NULL))) {
		log_warn("WARNING: Not using lvmetad with older version."); 
		goto fail;
	}

	_lvmetad_daemon_pid = (int)daemon_reply_int(reply, "daemon_pid", 0);

	/*
	 * If lvmetad is being updated by another command, then sleep and retry
	 * until the token shows the update is done, and go on to the token
	 * comparison.
	 *
	 * Between retries, sleep for a random period between 1 and 2 seconds.
	 * Retry in this way for up to a configurable period of time.
	 */
	if (!strcmp(daemon_token, LVMETAD_TOKEN_UPDATE_IN_PROGRESS)) {
		if (!(now = _monotonic_seconds()))
			goto fail;

		if (!wait_start)
			wait_start = now;

		if (now - wait_start > wait_sec) {
			log_warn("WARNING: Not using lvmetad after %u sec lvmetad_update_wait_time.", wait_sec);
			goto fail;
		}

		log_warn("WARNING: lvmetad is being updated, retrying (setup) for %u more seconds.",
			 wait_sec - (unsigned int)(now - wait_start));

		/* Delay a random period between 1 and 2 seconds. */
		delay_usec = 1000000 + lvm_even_rand(&_lvmetad_cmd->rand_seed, 1000000);
		usleep(delay_usec);
		daemon_reply_destroy(reply);
		goto retry;
	}

	/*
	 * lvmetad is empty, not yet populated.
	 * The caller should do a disk scan to populate lvmetad.
	 */
	if (!strcmp(daemon_token, "none")) {
		log_debug_lvmetad("lvmetad initialization needed.");
		ret = 0;
		goto out;
	}

	/*
	 * lvmetad has an unmatching token; it was last populated using
	 * a different global filter.
	 * The caller should do a disk scan to populate lvmetad with
	 * our global filter.
	 */
	if (strcmp(daemon_token, _lvmetad_token)) {
		log_debug_lvmetad("lvmetad initialization needed for different filter.");
		ret = 0;
		goto out;
	}

	if (wait_start)
		log_debug_lvmetad("lvmetad initialized during wait.");
	else
		log_debug_lvmetad("lvmetad initialized previously.");

out:
	daemon_reply_destroy(reply);
	return ret;

fail:
	daemon_reply_destroy(reply);
	/* The command will not use lvmetad and will revert to scanning. */
	lvmetad_make_unused(cmd);
	return 0;
}

/*
 * Wait up to lvmetad_update_wait_time for the lvmetad updating state to be
 * finished.
 *
 * Return 0 if lvmetad is not updating or there's an error and we can't tell.
 * Return 1 if lvmetad is updating.
 */
static int _lvmetad_is_updating(struct cmd_context *cmd, int do_wait)
{
	daemon_reply reply;
	const char *daemon_token;
	unsigned int wait_sec = 0;
	uint64_t now = 0, wait_start = 0;
	int ret = 0;

	wait_sec = (unsigned int)_lvmetad_update_timeout;
retry:
	log_debug_lvmetad("Sending lvmetad get_global_info");

	reply = daemon_send_simple(_lvmetad, "get_global_info",
				   "token = %s", "skip",
				   "pid = " FMTd64, (int64_t)getpid(),
				   "cmd = %s", get_cmd_name(),
				   NULL);
	if (reply.error)
		goto out;

	if (strcmp(daemon_reply_str(reply, "response", ""), "OK"))
		goto out;

	if (!(daemon_token = daemon_reply_str(reply, "token", NULL)))
		goto out;

	if (!strcmp(daemon_token, LVMETAD_TOKEN_UPDATE_IN_PROGRESS)) {
		ret = 1;

		if (!do_wait)
			goto out;

		if (!(now = _monotonic_seconds()))
			goto out;

		if (!wait_start)
			wait_start = now;

		if (now - wait_start >= wait_sec)
			goto out;

		log_warn("WARNING: lvmetad is being updated, waiting for %u more seconds.",
			 wait_sec - (unsigned int)(now - wait_start));

		usleep(1000000);
		daemon_reply_destroy(reply);
		goto retry;
	} else {
		ret = 0;
	}

out:
	daemon_reply_destroy(reply);
	return ret;
}

static daemon_reply _lvmetad_send(struct cmd_context *cmd, const char *id, ...)
{
	va_list ap;
	daemon_reply reply = { 0 };
	daemon_request req;
	const char *token_expected;
	unsigned int delay_usec;
	unsigned int wait_sec = 0;
	uint64_t now = 0, wait_start = 0;
	int daemon_in_update;
	int we_are_in_update;

	if (!_lvmetad_connected || !_lvmetad_use) {
		reply.error = ECONNRESET;
		return reply;
	}

	wait_sec = (unsigned int)_lvmetad_update_timeout;
retry:
	req = daemon_request_make(id);

	if (!daemon_request_extend(req,
				   "token = %s", _lvmetad_token ?: "none",
				   "update_timeout = " FMTd64, (int64_t)wait_sec,
				   "pid = " FMTd64, (int64_t)getpid(),
				   "cmd = %s", get_cmd_name(),
				   NULL)) {
		reply.error = ENOMEM;
		return reply;
	}

	va_start(ap, id);
	daemon_request_extend_v(req, ap);
	va_end(ap);

	reply = daemon_send(_lvmetad, req);

	daemon_request_destroy(req);

	if (reply.error == ECONNRESET)
		log_warn("WARNING: lvmetad connection failed, cannot reconnect."); 

	/*
	 * For the "token_update" message, the result is handled entirely
	 * by the _token_update() function, so return the reply immediately.
	 */
	if (!strcmp(id, "token_update"))
		return reply;

	/*
	 * For other messages it may be useful to retry and resend the
	 * message, so check for that case before returning the reply.
	 * The reply will be checked further in lvmetad_handle_reply.
	 */

	if (reply.error)
		return reply;

	if (!strcmp(daemon_reply_str(reply, "response", ""), "token_mismatch")) {
		token_expected = daemon_reply_str(reply, "expected", "");
		daemon_in_update = !strcmp(token_expected, LVMETAD_TOKEN_UPDATE_IN_PROGRESS);
		we_are_in_update = !strcmp(_lvmetad_token, LVMETAD_TOKEN_UPDATE_IN_PROGRESS);

		if (daemon_in_update && !we_are_in_update) {
			/*
			 * Another command is updating lvmetad, and we cannot
			 * use lvmetad until the update is finished.  Retry our
			 * request for a while; the update should finish
			 * shortly.  This should not usually happen because
			 * this command already checked that the token is
			 * usable in lvmetad_token_matches(), but it's possible
			 * for another command's rescan to slip in between the
			 * time we call lvmetad_token_matches() and the time we
			 * get here to lvmetad_send().
			 */

			if (!(now = _monotonic_seconds()))
				goto out;

			if (!wait_start)
				wait_start = now;

			if (!wait_sec || (now - wait_start >= wait_sec)) {
				log_warn("WARNING: Cannot use lvmetad after %u sec lvmetad_update_wait_time.", wait_sec);
				goto out;
			}

			log_warn("WARNING: lvmetad is being updated, retrying (%s) for %u more seconds.",
				 id, wait_sec - (unsigned int)(now - wait_start));

			/* Delay a random period between 1 and 2 seconds. */
			delay_usec = 1000000 + lvm_even_rand(&_lvmetad_cmd->rand_seed, 1000000);
			usleep(delay_usec);
			daemon_reply_destroy(reply);
			goto retry;

		} else {
			/* See lvmetad_handle_reply for handling other cases. */
		}
	}
out:
	return reply;
}

/*
 * token_update happens when starting or ending an lvmetad update.
 * When starting we set the token to "update in progress".
 * When ending we set the token to our filter:<hash>.
 *
 * From the perspective of a command, the lvmetad state is one of:
 * "none" - the lvmetad cache is not populated and an update is required.
 * "filter:<matching_hash>" - the command with can use the lvmetad cache.
 * "filter:<unmatching_hash>" - the lvmetad cache must be updated to be used.
 * "update in progress" - a command is updating the lvmetad cache.
 *
 * . If none, the command will update (scan and populate lvmetad),
 *   then use the cache.
 *
 * . If filter is matching, the command will use the cache.
 *
 * . If filter is unmatching, the command will update (scan and
 *   populate lvmetad), then use the cache.
 *
 * . If update in progress, the command will wait for a while for the state
 *   to become non-updating.  If it changes, see above, if it doesn't change,
 *   then the command either reverts to not using lvmetad, or does an update
 *   (scan and populate lvmetad) and then uses the cache.
 *
 * A command that is explicitly intended to update the cache will always do
 * that (it may wait for a while first to allow a current update to complete).
 * A command that is not explicitly intended to update the cache may choose
 * to revert to scanning and not use lvmetad.
 *
 * Because two different updates from two commands can potentially overlap,
 * lvmetad saves the pid of the latest update to start, so it can reject messages
 * from preempted updates.  This prevents an invalid mix of two different updates.
 * (The command makes use of the update_pid to print more informative messages.)
 *
 * If lvmetad detects that a command doing an update is taking too long, it will
 * change the token from "update in progress" to "none", which means a new update
 * is required, causing the next command to do an update.  This effectively
 * cancels/preempts a slow/stuck update, and helps to automatically resolve
 * some failure cases.
 */

static int _token_update(int *replaced_update)
{
	daemon_reply reply;
	const char *token_expected;
	const char *prev_token;
	const char *reply_str;
	int update_pid;
	int ending_our_update;

	log_debug_lvmetad("Sending lvmetad token_update %s", _lvmetad_token);
	reply = _lvmetad_send(NULL, "token_update", NULL);

	if (replaced_update)
		*replaced_update = 0;

	if (reply.error) {
		log_warn("WARNING: lvmetad token update error: %s", strerror(reply.error));
		daemon_reply_destroy(reply);
		return 0;
	}

	update_pid = (int)daemon_reply_int(reply, "update_pid", 0);
	reply_str = daemon_reply_str(reply, "response", "");

	/*
	 * A mismatch can only happen when this command attempts to set the
	 * token to filter:<hash> at the end of its update, but the update has
	 * been preempted in lvmetad by a new one (from update_pid).
	 */
	if (!strcmp(reply_str, "token_mismatch")) {
		token_expected = daemon_reply_str(reply, "expected", "");

		ending_our_update = strcmp(_lvmetad_token, LVMETAD_TOKEN_UPDATE_IN_PROGRESS);

		log_debug_lvmetad("Received token update mismatch expected \"%s\" our token \"%s\" update_pid %d our pid %d",
				  token_expected, _lvmetad_token, update_pid, getpid());

		if (ending_our_update && (update_pid != getpid())) {
			log_warn("WARNING: lvmetad was updated by another command (pid %d).", update_pid);
		} else {
			/*
			 * Shouldn't happen.
			 * If we're ending our update and our pid matches the update_pid,
			 * then there would not be a mismatch.
			 * If we're starting a new update, lvmetad never returns a
			 * token mismatch.
			 * In any case, it doesn't hurt to just return an error here.
			 */
			log_error(INTERNAL_ERROR "lvmetad token update mismatch pid %d matches our own pid %d", update_pid, getpid());
		}

		daemon_reply_destroy(reply);
		return 0;
	}

	if (strcmp(reply_str, "OK")) {
		log_error("Failed response from lvmetad for token update.");
		daemon_reply_destroy(reply);
		return 0;
	}

	if ((prev_token = daemon_reply_str(reply, "prev_token", NULL))) {
		if (!strcmp(prev_token, LVMETAD_TOKEN_UPDATE_IN_PROGRESS))
			if (replaced_update && (update_pid != getpid()))
				*replaced_update = 1;
	}

	daemon_reply_destroy(reply);
	return 1;
}

/*
 * Helper; evaluate the reply from lvmetad, check for errors, print diagnostics
 * and return a summary success/failure exit code.
 *
 * If found is set, *found indicates whether or not device exists,
 * and missing device is not treated as an error.
 */
static int _lvmetad_handle_reply(daemon_reply reply, const char *id, const char *object, int *found)
{
	const char *token_expected;
	const char *action;
	const char *reply_str;
	int action_modifies = 0;
	int daemon_in_update;
	int we_are_in_update;
	int update_pid;

	if (!id)
		action = "<none>";
	else if (!strcmp(id, "pv_list"))
		action = "list PVs";
	else if (!strcmp(id, "vg_list"))
		action = "list VGs";
	else if (!strcmp(id, "vg_lookup"))
		action = "lookup VG";
	else if (!strcmp(id, "pv_lookup"))
		action = "lookup PV";
	else if (!strcmp(id, "pv_clear_all"))
		action = "clear info about all PVs";
	else if (!strcmp(id, "vg_clear_outdated_pvs"))
		action = "clear the list of outdated PVs";
	else if (!strcmp(id, "set_vg_info"))
		action = "set VG info";
	else if (!strcmp(id, "vg_update"))
		action = "update VG";
	else if (!strcmp(id, "vg_remove"))
		action = "remove VG";
	else if (!strcmp(id, "pv_found")) {
		action = "update PV";
		action_modifies = 1;
	} else if (!strcmp(id, "pv_gone")) {
		action = "drop PV";
		action_modifies = 1;
	} else {
		log_error(INTERNAL_ERROR "Unchecked lvmetad message %s.", id);
		action = "action unknown";
	}

	if (reply.error) {
		log_error("lvmetad cannot be used due to error: %s", strerror(reply.error));
		goto fail;
	}

	/*
	 * Errors related to token mismatch.
	 */
	reply_str = daemon_reply_str(reply, "response", "");
	if (!strcmp(reply_str, "token_mismatch")) {

		token_expected = daemon_reply_str(reply, "expected", "");
		update_pid = (int)daemon_reply_int(reply, "update_pid", 0);

		log_debug("lvmetad token mismatch, expected \"%s\" our token \"%s\"",
			  token_expected, _lvmetad_token);

		daemon_in_update = !strcmp(token_expected, LVMETAD_TOKEN_UPDATE_IN_PROGRESS);
		we_are_in_update = !strcmp(_lvmetad_token, LVMETAD_TOKEN_UPDATE_IN_PROGRESS);

		if (daemon_in_update && we_are_in_update) {

			/*
			 * When we do not match the update_pid, it means our
			 * update was cancelled and another process is now
			 * updating the cache.
			 */

			if (update_pid != getpid()) {
				log_warn("WARNING: lvmetad is being updated by another command (pid %d).", update_pid);
			} else {
				/* Shouldn't happen */
				log_error(INTERNAL_ERROR "lvmetad update by pid %d matches our own pid %d", update_pid, getpid());
			}
			/* We don't care if the action was modifying during a token update. */
			action_modifies = 0;
			goto fail;

		} else if (daemon_in_update && !we_are_in_update) {

			/*
			 * Another command is updating lvmetad, and we cannot
			 * use lvmetad until the update is finished.
			 * lvmetad_send resent this message up to the limit and
			 * eventually gave up.  The caller may choose to not
			 * use lvmetad at this point and revert to scanning.
			 */

			log_warn("WARNING: lvmetad is being updated and cannot be used.");
			goto fail;

		} else if (!daemon_in_update && we_are_in_update) {

			/*
			 * We are updating lvmetad after setting the token to
			 * "update in progress", but lvmetad has a non-update
			 * token and is rejecting our update messages.  This
			 * must mean that lvmetad cancelled our update (we were
			 * probably too slow, taking longer than the timeout),
			 * so another command completed an update and set the
			 * token based on its filter.  Here we've attempt to
			 * continue our cache update, and find we've been
			 * preempted, so we should just abort our failed
			 * update.
			 */

			log_warn("WARNING: lvmetad was updated by another command.");
			/* We don't care if the action was modifying during a token update. */
			action_modifies = 0;
			goto fail;

		} else if (!daemon_in_update && !we_are_in_update) {

			/*
			 * Another command has updated the lvmetad cache, and
			 * has done so using a different device filter from our
			 * own, which has made the lvmetad token and our token
			 * not match.  This should not usually happen because
			 * this command has already checked for a matching token
			 * in lvmetad_token_matches(), but it's possible for
			 * another command's rescan to slip in between the time
			 * we call lvmetad_token_matches() and the time we get
			 * here to lvmetad_send().  With a mismatched token
			 * (different set of devices), we cannot use the lvmetad
			 * cache.
			 *
			 * FIXME: it would be nice to have this command ignore
			 * lvmetad at this point and revert to disk scanning,
			 * but the layers above lvmetad_send are not yet able
			 * to switch modes in the middle of processing.
			 *
			 * (The advantage of lvmetad_check_token is that it
			 * can rescan to get the token in sync, or if that
			 * fails it can make the command revert to scanning
			 * from the start.)
			 */

			log_warn("WARNING: Cannot use lvmetad while it caches different devices.");
			goto fail;
		}
	}

	/*
	 * Non-token-mismatch related error checking.
	 */

	/* All OK? */
	if (!strcmp(reply_str, "OK")) {
		if (found)
			*found = 1;
		return 1;
	}

	/* Unknown device permitted? */
	if (found && !strcmp(reply_str, "unknown")) {
		log_very_verbose("Request to %s %s%sin lvmetad did not find any matching object.",
				 action, object, *object ? " " : "");
		*found = 0;
		return 1;
	}

	/* Multiple VGs with the same name were found. */
	if (found && !strcmp(reply_str, "multiple")) {
		log_very_verbose("Request to %s %s%sin lvmetad found multiple matching objects.",
				 action, object, *object ? " " : "");
		if (found)
			*found = 2;
		return 1;
	}

	/*
	 * Generic error message for error cases not specifically checked above.
	 */
	log_error("Request to %s %s%sin lvmetad gave response %s. Reason: %s",
		  action, object, *object ? " " : "", 
		  daemon_reply_str(reply, "response", "<missing>"),
		  daemon_reply_str(reply, "reason", "<missing>"));
fail:
	/*
	 * If the failed lvmetad message was updating lvmetad with new metadata
	 * that has been changed by this command, it is important to restart
	 * lvmetad (or at least rescan.)  (An lvmetad update that is just
	 * scanning disks to populate the cache is not a problem, so we try to
	 * avoid printing a "corruption" warning in that case.)
	 */

	if (action_modifies) {
		/*
		 * FIXME: experiment with killing the lvmetad process here, e.g.
		 * kill(_lvmetad_daemon_pid, SIGKILL);
		 */
		log_warn("WARNING: To avoid corruption, restart lvmetad (or disable with use_lvmetad=0).");
	}

	return 0;
}

static int _read_mda(struct lvmcache_info *info,
		     struct format_type *fmt,
		     const struct dm_config_node *cn)
{
	struct metadata_area_ops *ops;

	dm_list_iterate_items(ops, &fmt->mda_ops)
		if (ops->mda_import_text && ops->mda_import_text(info, cn))
			return 1;

	return 0;
}

static int _pv_populate_lvmcache(struct cmd_context *cmd,
				 struct dm_config_node *cn,
				 struct format_type *fmt, dev_t fallback)
{
	struct device *dev;
	struct id pvid, vgid;
	char mda_id[32];
	char da_id[32];
	int i = 0;
	struct dm_config_node *mda, *da;
	uint64_t offset, size;
	struct lvmcache_info *info;
	const char *pvid_txt = dm_config_find_str(cn->child, "id", NULL),
		   *vgid_txt = dm_config_find_str(cn->child, "vgid", NULL),
		   *vgname = dm_config_find_str(cn->child, "vgname", NULL),
		   *fmt_name = dm_config_find_str(cn->child, "format", NULL);
	dev_t devt = dm_config_find_int(cn->child, "device", 0);
	uint64_t devsize = dm_config_find_int64(cn->child, "dev_size", 0),
		 label_sector = dm_config_find_int64(cn->child, "label_sector", 0);
	uint32_t ext_flags = (uint32_t) dm_config_find_int64(cn->child, "ext_flags", 0);
	uint32_t ext_version = (uint32_t) dm_config_find_int64(cn->child, "ext_version", 0);

	if (!fmt && fmt_name)
		fmt = get_format_by_name(cmd, fmt_name);

	if (!fmt) {
		log_error("PV %s not recognised. Is the device missing?", pvid_txt);
		return 0;
	}

	dev = dev_cache_get_by_devt(devt, cmd->filter);
	if (!dev && fallback)
		dev = dev_cache_get_by_devt(fallback, cmd->filter);

	if (!dev) {
		log_warn("WARNING: Device for PV %s not found or rejected by a filter.", pvid_txt);
		return 0;
	}

	if (!pvid_txt || !id_read_format(&pvid, pvid_txt)) {
		log_error("Missing or ill-formatted PVID for PV: %s.", pvid_txt);
		return 0;
	}

	if (vgid_txt) {
		if (!id_read_format(&vgid, vgid_txt))
			return_0;
	} else
		/* NB uuid is short and NUL-terminated. */
		(void) dm_strncpy((char*)&vgid, fmt->orphan_vg_name, sizeof(vgid));

	if (!vgname)
		vgname = fmt->orphan_vg_name;

	if (!(info = lvmcache_add(fmt->labeller, (const char *)&pvid, dev,
				  vgname, (const char *)&vgid, 0)))
		return_0;

	lvmcache_get_label(info)->sector = label_sector;
	lvmcache_get_label(info)->dev = dev;
	lvmcache_set_device_size(info, devsize);
	lvmcache_del_das(info);
	lvmcache_del_mdas(info);
	lvmcache_del_bas(info);

	do {
		sprintf(mda_id, "mda%d", i);
		mda = dm_config_find_node(cn->child, mda_id);
		if (mda)
			_read_mda(info, fmt, mda);
		++i;
	} while (mda);

	i = 0;
	do {
		sprintf(da_id, "da%d", i);
		da = dm_config_find_node(cn->child, da_id);
		if (da) {
			if (!dm_config_get_uint64(da->child, "offset", &offset)) return_0;
			if (!dm_config_get_uint64(da->child, "size", &size)) return_0;
			lvmcache_add_da(info, offset, size);
		}
		++i;
	} while (da);

	i = 0;
	do {
		sprintf(da_id, "ba%d", i);
		da = dm_config_find_node(cn->child, da_id);
		if (da) {
			if (!dm_config_get_uint64(da->child, "offset", &offset)) return_0;
			if (!dm_config_get_uint64(da->child, "size", &size)) return_0;
			lvmcache_add_ba(info, offset, size);
		}
		++i;
	} while (da);

	lvmcache_set_ext_flags(info, ext_flags);
	lvmcache_set_ext_version(info, ext_version);

	return 1;
}

static int _pv_update_struct_pv(struct physical_volume *pv, struct format_instance *fid)
{
	struct lvmcache_info *info;

	if ((info = lvmcache_info_from_pvid((const char *)&pv->id, pv->dev, 0))) {
		pv->label_sector = lvmcache_get_label(info)->sector;
		pv->dev = lvmcache_device(info);
		if (!pv->dev)
			pv->status |= MISSING_PV;
		if (!lvmcache_fid_add_mdas_pv(info, fid))
			return_0;
		pv->fid = fid;
	} else
		pv->status |= MISSING_PV; /* probably missing */

	return 1;
}

struct volume_group *lvmetad_vg_lookup(struct cmd_context *cmd, const char *vgname, const char *vgid)
{
	struct volume_group *vg = NULL;
	struct volume_group *vg2 = NULL;
	daemon_reply reply;
	int found;
	char uuid[64];
	struct format_instance *fid = NULL;
	struct format_instance_ctx fic;
	struct dm_config_node *top;
	const char *name, *diag_name;
	const char *fmt_name;
	struct format_type *fmt;
	struct dm_config_node *pvcn;
	struct pv_list *pvl;
	int rescan = 0;

	if (!lvmetad_used())
		return NULL;

	if (vgid) {
		if (!id_write_format((const struct id*)vgid, uuid, sizeof(uuid)))
			return_NULL;
	}

	if (vgid && vgname) {
		log_debug_lvmetad("Asking lvmetad for VG %s %s", uuid, vgname);
		reply = _lvmetad_send(cmd, "vg_lookup",
				      "uuid = %s", uuid,
				      "name = %s", vgname,
				      NULL);
		diag_name = uuid;

	} else if (vgid) {
		log_debug_lvmetad("Asking lvmetad for VG vgid %s", uuid);
		reply = _lvmetad_send(cmd, "vg_lookup", "uuid = %s", uuid, NULL);
		diag_name = uuid;

	} else if (vgname) {
		log_debug_lvmetad("Asking lvmetad for VG %s", vgname);
		reply = _lvmetad_send(cmd, "vg_lookup", "name = %s", vgname, NULL);
		diag_name = vgname;

	} else {
		log_error(INTERNAL_ERROR "VG name required (VGID not available)");
		return NULL;
	}

	if (_lvmetad_handle_reply(reply, "vg_lookup", diag_name, &found) && found) {

		if ((found == 2) && vgname) {
			log_error("Multiple VGs found with the same name: %s.", vgname);
			log_error("See the --select option with VG UUID (vg_uuid).");
			goto out;
		}

		if (!(top = dm_config_find_node(reply.cft->root, "metadata"))) {
			log_error(INTERNAL_ERROR "metadata config node not found.");
			goto out;
		}

		name = daemon_reply_str(reply, "name", NULL);

		/* fall back to lvm2 if we don't know better */
		fmt_name = dm_config_find_str(top, "metadata/format", "lvm2");
		if (!(fmt = get_format_by_name(cmd, fmt_name))) {
			log_error(INTERNAL_ERROR
				  "We do not know the format (%s) reported by lvmetad.",
				  fmt_name);
			goto out;
		}

		fic.type = FMT_INSTANCE_MDAS | FMT_INSTANCE_AUX_MDAS;
		fic.context.vg_ref.vg_name = name;
		fic.context.vg_ref.vg_id = vgid;

		if (!(fid = fmt->ops->create_instance(fmt, &fic)))
			goto_out;

		if ((pvcn = dm_config_find_node(top, "metadata/physical_volumes")))
			for (pvcn = pvcn->child; pvcn; pvcn = pvcn->sib)
				_pv_populate_lvmcache(cmd, pvcn, fmt, 0);

		if ((pvcn = dm_config_find_node(top, "metadata/outdated_pvs")))
			for (pvcn = pvcn->child; pvcn; pvcn = pvcn->sib)
				_pv_populate_lvmcache(cmd, pvcn, fmt, 0);

		top->key = name;
		if (!(vg = import_vg_from_lvmetad_config_tree(reply.cft, fid)))
			goto_out;

		/*
		 * Read the VG from disk, ignoring the lvmetad copy in these
		 * cases:
		 *
		 * 1. The host is not using lvmlockd, but is reading lockd VGs
		 * using the --shared option.  The shared option is meant to
		 * let hosts not running lvmlockd look at lockd VGs, like the
		 * foreign option allows hosts to look at foreign VGs.  When
		 * --foreign is used, the code forces a rescan since the local
		 * lvmetad cache of foreign VGs is likely stale.  Similarly,
		 * for --shared, have the code reading the shared VGs below
		 * not use the cached copy from lvmetad but to rescan the VG.
		 *
		 * 2. The host failed to acquire the VG lock from lvmlockd for
		 * the lockd VG.  In this case, the usual mechanisms for
		 * updating the lvmetad copy of the VG have been missed.  Since
		 * we don't know if the cached copy is valid, assume it's not.
		 *
		 * 3. lvmetad has returned the "vg_invalid" flag, which is the
		 * usual mechanism used by lvmlockd/lvmetad to cause a host to
		 * reread a VG from disk that has been modified from another
		 * host.
		 */

		if (is_lockd_type(vg->lock_type) && cmd->include_shared_vgs) {
			log_debug_lvmetad("Rescan VG %s because including shared", vgname);
			rescan = 1;
		} else if (is_lockd_type(vg->lock_type) && cmd->lockd_vg_rescan) {
			log_debug_lvmetad("Rescan VG %s because no lvmlockd lock is held", vgname);
			rescan = 1;
		} else if (dm_config_find_node(reply.cft->root, "vg_invalid")) {
			if (!is_lockd_type(vg->lock_type)) {
				/* Can happen if a previous command failed/crashed without updating lvmetad. */
				log_warn("WARNING: Reading VG %s from disk because lvmetad metadata is invalid.", vgname);
			} else {
				/* This is normal when the VG was modified by another host. */
				log_debug_lvmetad("Rescan VG %s because lvmetad returned invalid", vgname);
			}
			rescan = 1;
		}

		/*
		 * locking may have detected a newer vg version and
		 * invalidated the cached vg.
		 */
		if (rescan) {
			if (!(vg2 = _lvmetad_pvscan_vg(cmd, vg, vgid, fmt))) {
				log_debug_lvmetad("VG %s from lvmetad not found during rescan.", vgname);
				fid = NULL;
				release_vg(vg);
				vg = NULL;
				goto out;
			}
			fid->ref_count++;
			release_vg(vg);
			fid->ref_count--;
			fmt->ops->destroy_instance(fid);
			vg = vg2;
			fid = vg2->fid;
		}

		dm_list_iterate_items(pvl, &vg->pvs) {
			if (!_pv_update_struct_pv(pvl->pv, fid)) {
				vg = NULL;
				goto_out;	/* FIXME: use an error path that disables lvmetad */
			}
		}

		dm_list_iterate_items(pvl, &vg->pvs_outdated) {
			if (!_pv_update_struct_pv(pvl->pv, fid)) {
				vg = NULL;
				goto_out;	/* FIXME: use an error path that disables lvmetad */
			}
		}

		lvmcache_update_vg(vg, 0);
		vg_mark_partial_lvs(vg, 1);
	}

out:
	if (!vg && fid)
		fid->fmt->ops->destroy_instance(fid);
	daemon_reply_destroy(reply);

	return vg;
}

struct _fixup_baton {
	int i;
	int find;
	int ignore;
};

static int _fixup_ignored(struct metadata_area *mda, void *baton) {
	struct _fixup_baton *b = baton;

	if (b->i == b->find)
		mda_set_ignored(mda, b->ignore);

	b->i ++;

	return 1;
}

/*
 * After the VG is written to disk, but before it's committed,
 * lvmetad is told the new seqno.  lvmetad sets the INVALID
 * flag on the cached VG and saves the new seqno.
 *
 * After the VG is committed on disk, the command sends the
 * new VG metadata, containing the new seqno.  lvmetad sees
 * that it has the updated metadata and clears the INVALID
 * flag on the cached VG.
 *
 * If the command fails after committing the metadata on disk
 * but before sending the new metadata to lvmetad, then the
 * next command that asks lvmetad for the metadata will get
 * back the INVALID flag.  That command will then read the
 * VG metadata from disk to use, and will send the latest
 * metadata from disk to lvmetad which will clear the
 * INVALID flag.
 */

int lvmetad_vg_update_pending(struct volume_group *vg)
{
	char uuid[64] __attribute__((aligned(8)));
	daemon_reply reply;

	if (!lvmetad_used() || test_mode())
		return 1; /* fake it */

	if (!id_write_format(&vg->id, uuid, sizeof(uuid)))
		return_0;

	log_debug_lvmetad("Sending lvmetad pending VG %s (seqno %" PRIu32 ")", vg->name, vg->seqno);
	reply = _lvmetad_send(vg->cmd, "set_vg_info",
			      "name = %s", vg->name,
			      "uuid = %s", uuid,
			      "version = %"PRId64, (int64_t)vg->seqno,
			      NULL);

	if (!_lvmetad_handle_reply(reply, "set_vg_info", vg->name, NULL)) {
		daemon_reply_destroy(reply);
		return_0;
	}

	vg->lvmetad_update_pending = 1;

	daemon_reply_destroy(reply);
	return 1;
}

int lvmetad_vg_update_finish(struct volume_group *vg)
{
	char uuid[64] __attribute__((aligned(8)));
	daemon_reply reply;
	struct dm_hash_node *n;
	struct metadata_area *mda;
	char mda_id[128], *num;
	struct volume_group *vgu;
	struct dm_config_tree *vgmeta;
	struct pv_list *pvl;
	struct lvmcache_info *info;
	struct _fixup_baton baton;

	if (!vg->lvmetad_update_pending)
		return 1;

	if (!(vg->fid->fmt->features & FMT_PRECOMMIT))
		return 1;

	if (!lvmetad_used() || test_mode())
		return 1; /* fake it */

	if (!id_write_format(&vg->id, uuid, sizeof(uuid)))
		return_0;

	/*
	 * vg->vg_committted is the state of the VG metadata when vg_commit()
	 * was called.  Since then, 'vg' may have been partially modified and
	 * not committed.  We only want to send committed metadata to lvmetad.
	 *
	 * lvmetad is sometimes updated in cases where the VG is not written
	 * (no vg_committed).  In those cases 'vg' has just been read from
	 * disk, and we can send 'vg' to lvmetad.  This happens when the
	 * command finds the lvmetad cache invalid, so the VG has been read
	 * from disk and is then sent to lvmetad.
	 */

	vgu = vg->vg_committed ? vg->vg_committed : vg;

	if (!(vgmeta = export_vg_to_config_tree(vgu))) {
		log_error("Failed to export VG to config tree.");
		return 0;
	}

	log_debug_lvmetad("Sending lvmetad updated VG %s (seqno %" PRIu32 ")", vg->name, vg->seqno);
	reply = _lvmetad_send(vg->cmd, "vg_update",
			      "vgname = %s", vg->name,
			      "metadata = %t", vgmeta,
			      NULL);

	dm_config_destroy(vgmeta);

	if (!_lvmetad_handle_reply(reply, "vg_update", vg->name, NULL)) {
		/*
		 * In this failure case, the VG cached in lvmetad remains in
		 * the INVALID state (from lvmetad_vg_update_pending).
		 * A subsequent command will see INVALID, ignore the cached
		 * copy, read the VG from disk, and update the cached copy.
		 */
		daemon_reply_destroy(reply);
		return 0;
	}

	daemon_reply_destroy(reply);

	n = (vgu->fid && vgu->fid->metadata_areas_index) ?
		dm_hash_get_first(vgu->fid->metadata_areas_index) : NULL;
	while (n) {
		mda = dm_hash_get_data(vgu->fid->metadata_areas_index, n);
		(void) dm_strncpy(mda_id, dm_hash_get_key(vgu->fid->metadata_areas_index, n), sizeof(mda_id));
		if ((num = strchr(mda_id, '_'))) {
			*num = 0;
			++num;
			if ((info = lvmcache_info_from_pvid(mda_id, NULL, 0))) {
				memset(&baton, 0, sizeof(baton));
				baton.find = atoi(num);
				baton.ignore = mda_is_ignored(mda);
				lvmcache_foreach_mda(info, _fixup_ignored, &baton);
			}
		}
		n = dm_hash_get_next(vgu->fid->metadata_areas_index, n);
	}

	dm_list_iterate_items(pvl, &vgu->pvs) {
		/* NB. the PV fmt pointer is sometimes wrong during vgconvert */
		if (pvl->pv->dev && !lvmetad_pv_found(vg->cmd, &pvl->pv->id, pvl->pv->dev,
						      vgu->fid ? vgu->fid->fmt : pvl->pv->fmt,
						      pvl->pv->label_sector, NULL, NULL, NULL))
			return_0;
	}

	vg->lvmetad_update_pending = 0;
	return 1;
}

int lvmetad_vg_remove_pending(struct volume_group *vg)
{
	char uuid[64] __attribute__((aligned(8)));
	daemon_reply reply;

	if (!lvmetad_used() || test_mode())
		return 1; /* fake it */

	if (!id_write_format(&vg->id, uuid, sizeof(uuid)))
		return_0;

	/* Sending version/seqno 0 in set_vg_info will set the INVALID flag. */

	log_debug_lvmetad("Sending lvmetad pending remove VG %s", vg->name);
	reply = _lvmetad_send(vg->cmd, "set_vg_info",
			      "name = %s", vg->name,
			      "uuid = %s", uuid,
			      "version = %"PRId64, (int64_t)0,
			      NULL);

	if (!_lvmetad_handle_reply(reply, "set_vg_info", vg->name, NULL)) {
		daemon_reply_destroy(reply);
		return_0;
	}

	daemon_reply_destroy(reply);
	return 1;
}

int lvmetad_vg_remove_finish(struct volume_group *vg)
{
	char uuid[64];
	daemon_reply reply;
	int result;

	if (!lvmetad_used() || test_mode())
		return 1; /* just fake it */

	vg->lvmetad_update_pending = 0;

	if (!id_write_format(&vg->id, uuid, sizeof(uuid)))
		return_0;

	log_debug_lvmetad("Telling lvmetad to remove VGID %s (%s)", uuid, vg->name);
	reply = _lvmetad_send(vg->cmd, "vg_remove", "uuid = %s", uuid, NULL);
	result = _lvmetad_handle_reply(reply, "vg_remove", vg->name, NULL);

	daemon_reply_destroy(reply);

	return result;
}

int lvmetad_pv_lookup(struct cmd_context *cmd, struct id pvid, int *found)
{
	char uuid[64];
	daemon_reply reply;
	int result = 0;
	struct dm_config_node *cn;

	if (!lvmetad_used())
		return_0;

	if (!id_write_format(&pvid, uuid, sizeof(uuid)))
		return_0;

	log_debug_lvmetad("Asking lvmetad for PV %s", uuid);
	reply = _lvmetad_send(cmd, "pv_lookup", "uuid = %s", uuid, NULL);
	if (!_lvmetad_handle_reply(reply, "pv_lookup", "", found))
		goto_out;

	if (found && !*found)
		goto out_success;

	if (!(cn = dm_config_find_node(reply.cft->root, "physical_volume")))
		goto_out;
        else if (!_pv_populate_lvmcache(cmd, cn, NULL, 0))
		goto_out;

out_success:
	result = 1;

out:
	daemon_reply_destroy(reply);

	return result;
}

int lvmetad_pv_lookup_by_dev(struct cmd_context *cmd, struct device *dev, int *found)
{
	int result = 0;
	daemon_reply reply;
	struct dm_config_node *cn;

	if (!lvmetad_used())
		return_0;

	log_debug_lvmetad("Asking lvmetad for PV on %s", dev_name(dev));
	reply = _lvmetad_send(cmd, "pv_lookup", "device = %" PRId64, (int64_t) dev->dev, NULL);
	if (!_lvmetad_handle_reply(reply, "pv_lookup", dev_name(dev), found))
		goto_out;

	if (found && !*found)
		goto out_success;

	cn = dm_config_find_node(reply.cft->root, "physical_volume");
	if (!cn || !_pv_populate_lvmcache(cmd, cn, NULL, dev->dev))
		goto_out;

out_success:
	result = 1;

out:
	daemon_reply_destroy(reply);

	return result;
}

int lvmetad_pv_list_to_lvmcache(struct cmd_context *cmd)
{
	daemon_reply reply;
	struct dm_config_node *cn;

	if (!lvmetad_used())
		return 1;

	log_debug_lvmetad("Asking lvmetad for complete list of known PVs");
	reply = _lvmetad_send(cmd, "pv_list", NULL);
	if (!_lvmetad_handle_reply(reply, "pv_list", "", NULL)) {
		daemon_reply_destroy(reply);
		return_0;
	}

	if ((cn = dm_config_find_node(reply.cft->root, "physical_volumes")))
		for (cn = cn->child; cn; cn = cn->sib)
			_pv_populate_lvmcache(cmd, cn, NULL, 0);

	daemon_reply_destroy(reply);

	return 1;
}

int lvmetad_get_vgnameids(struct cmd_context *cmd, struct dm_list *vgnameids)
{
	struct vgnameid_list *vgnl;
	struct id vgid;
	const char *vgid_txt;
	const char *vg_name;
	daemon_reply reply;
	struct dm_config_node *cn;

	log_debug_lvmetad("Asking lvmetad for complete list of known VG ids/names");
	reply = _lvmetad_send(cmd, "vg_list", NULL);
	if (!_lvmetad_handle_reply(reply, "vg_list", "", NULL)) {
		daemon_reply_destroy(reply);
		return_0;
	}

	if ((cn = dm_config_find_node(reply.cft->root, "volume_groups"))) {
		for (cn = cn->child; cn; cn = cn->sib) {
			vgid_txt = cn->key;
			if (!id_read_format(&vgid, vgid_txt)) {
				stack;
				continue;
			}

			if (!(vgnl = dm_pool_alloc(cmd->mem, sizeof(*vgnl)))) {
				log_error("vgnameid_list allocation failed.");
				return 0;
			}

			if (!(vg_name = dm_config_find_str(cn->child, "name", NULL))) {
				log_error("vg_list no name found.");
				return 0;
			}

			vgnl->vgid = dm_pool_strdup(cmd->mem, (char *)&vgid);
			vgnl->vg_name = dm_pool_strdup(cmd->mem, vg_name);

			if (!vgnl->vgid || !vgnl->vg_name) {
				log_error("vgnameid_list member allocation failed.");
				return 0;
			}

			dm_list_add(vgnameids, &vgnl->list);
		}
	}

	daemon_reply_destroy(reply);

	return 1;
}

int lvmetad_vg_list_to_lvmcache(struct cmd_context *cmd)
{
	struct volume_group *tmp;
	struct id vgid;
	const char *vgid_txt;
	daemon_reply reply;
	struct dm_config_node *cn;

	if (!lvmetad_used())
		return 1;

	log_debug_lvmetad("Asking lvmetad for complete list of known VGs");
	reply = _lvmetad_send(cmd, "vg_list", NULL);
	if (!_lvmetad_handle_reply(reply, "vg_list", "", NULL)) {
		daemon_reply_destroy(reply);
		return_0;
	}

	if ((cn = dm_config_find_node(reply.cft->root, "volume_groups")))
		for (cn = cn->child; cn; cn = cn->sib) {
			vgid_txt = cn->key;
			if (!id_read_format(&vgid, vgid_txt)) {
				stack;
				continue;
			}

			/* the call to lvmetad_vg_lookup will poke the VG into lvmcache */
			tmp = lvmetad_vg_lookup(cmd, NULL, (const char*)&vgid);
			release_vg(tmp);
		}

	daemon_reply_destroy(reply);

	return 1;
}

struct extract_dl_baton {
	int i;
	struct dm_config_tree *cft;
	struct dm_config_node *pre_sib;
};

static int _extract_mda(struct metadata_area *mda, void *baton)
{
	struct extract_dl_baton *b = baton;
	struct dm_config_node *cn;
	char id[32];

	if (!mda->ops->mda_export_text) /* do nothing */
		return 1;

	(void) dm_snprintf(id, 32, "mda%d", b->i);
	if (!(cn = make_config_node(b->cft, id, b->cft->root, b->pre_sib)))
		return 0;
	if (!mda->ops->mda_export_text(mda, b->cft, cn))
		return 0;

	b->i ++;
	b->pre_sib = cn; /* for efficiency */

	return 1;
}

static int _extract_disk_location(const char *name, struct disk_locn *dl, void *baton)
{
	struct extract_dl_baton *b = baton;
	struct dm_config_node *cn;
	char id[32];

	if (!dl)
		return 1;

	(void) dm_snprintf(id, 32, "%s%d", name, b->i);
	if (!(cn = make_config_node(b->cft, id, b->cft->root, b->pre_sib)))
		return 0;
	if (!config_make_nodes(b->cft, cn, NULL,
			       "offset = %"PRId64, (int64_t) dl->offset,
			       "size = %"PRId64, (int64_t) dl->size,
			       NULL))
		return 0;

	b->i ++;
	b->pre_sib = cn; /* for efficiency */

	return 1;
}

static int _extract_da(struct disk_locn *da, void *baton)
{
	return _extract_disk_location("da", da, baton);
}

static int _extract_ba(struct disk_locn *ba, void *baton)
{
	return _extract_disk_location("ba", ba, baton);
}

static int _extract_mdas(struct lvmcache_info *info, struct dm_config_tree *cft,
			 struct dm_config_node *pre_sib)
{
	struct extract_dl_baton baton = { .cft = cft };

	if (!lvmcache_foreach_mda(info, &_extract_mda, &baton))
		return 0;

	baton.i = 0;
	if (!lvmcache_foreach_da(info, &_extract_da, &baton))
		return 0;

	baton.i = 0;
	if (!lvmcache_foreach_ba(info, &_extract_ba, &baton))
		return 0;

	return 1;
}

int lvmetad_pv_found(struct cmd_context *cmd, const struct id *pvid, struct device *dev, const struct format_type *fmt,
		     uint64_t label_sector, struct volume_group *vg,
		     struct dm_list *found_vgnames,
		     struct dm_list *changed_vgnames)
{
	char uuid[64];
	daemon_reply reply;
	struct lvmcache_info *info;
	struct dm_config_tree *pvmeta, *vgmeta;
	const char *status = NULL, *vgname = NULL;
	int64_t changed = 0;
	int result, seqno_after;

	if (!lvmetad_used() || test_mode())
		return 1;

	if (!id_write_format(pvid, uuid, sizeof(uuid)))
                return_0;

	pvmeta = dm_config_create();
	if (!pvmeta)
		return_0;

	info = lvmcache_info_from_pvid((const char *)pvid, dev, 0);

	if (!(pvmeta->root = make_config_node(pvmeta, "pv", NULL, NULL))) {
		dm_config_destroy(pvmeta);
		return_0;
	}

	/* TODO: resolve what does it actually mean  'info == NULL'
	 *       missing info is likely an INTERNAL_ERROR */
	if (!config_make_nodes(pvmeta, pvmeta->root, NULL,
			       "device = %"PRId64, (int64_t) dev->dev,
			       "dev_size = %"PRId64, (int64_t) (info ? lvmcache_device_size(info) : 0),
			       "format = %s", fmt->name,
			       "label_sector = %"PRId64, (int64_t) label_sector,
			       "id = %s", uuid,
			       "ext_version = %"PRId64, (int64_t) (info ? lvmcache_ext_version(info) : 0),
			       "ext_flags = %"PRId64, (int64_t) (info ? lvmcache_ext_flags(info) : 0),
			       NULL))
	{
		dm_config_destroy(pvmeta);
		return_0;
	}

	if (info)
		/* FIXME A more direct route would be much preferable. */
		_extract_mdas(info, pvmeta, pvmeta->root);

	if (vg) {
		if (!(vgmeta = export_vg_to_config_tree(vg))) {
			dm_config_destroy(pvmeta);
			return_0;
		}

		log_debug_lvmetad("Telling lvmetad to store PV %s (%s) in VG %s", dev_name(dev), uuid, vg->name);
		reply = _lvmetad_send(cmd, "pv_found",
				      "pvmeta = %t", pvmeta,
				      "vgname = %s", vg->name,
				      "metadata = %t", vgmeta,
				      NULL);
		dm_config_destroy(vgmeta);
	} else {
		/*
		 * There is no VG metadata stored on this PV.
		 * It might or might not be an orphan.
		 */
		log_debug_lvmetad("Telling lvmetad to store PV %s (%s)", dev_name(dev), uuid);
		reply = _lvmetad_send(NULL, "pv_found", "pvmeta = %t", pvmeta, NULL);
	}

	dm_config_destroy(pvmeta);

	result = _lvmetad_handle_reply(reply, "pv_found", uuid, NULL);

	if (vg && result) {
		seqno_after = daemon_reply_int(reply, "seqno_after", -1);
		if ((seqno_after != (int) vg->seqno) ||
		    (seqno_after != daemon_reply_int(reply, "seqno_before", -1)))
			log_warn("WARNING: Inconsistent metadata found for VG %s", vg->name);
	}

	if (result && found_vgnames) {
		status = daemon_reply_str(reply, "status", NULL);
		vgname = daemon_reply_str(reply, "vgname", NULL);
		changed = daemon_reply_int(reply, "changed", 0);
	}

	/*
	 * If lvmetad now sees all PVs in the VG, it returned the
	 * "complete" status string.  Add this VG name to the list
	 * of found VGs so that the caller can do autoactivation.
	 *
	 * If there was a problem notifying lvmetad about the new
	 * PV, e.g. lvmetad was disabled due to a duplicate, then
	 * no autoactivation is attempted.
	 *
	 * FIXME: there was a previous fixme indicating that
	 * autoactivation might also be done for VGs with the
	 * "partial" status.
	 *
	 * If the VG has "changed" by finding the PV, lvmetad returns
	 * the "changed" flag.  The names of "changed" VGs are saved
	 * in the changed_vgnames lists, which is used during autoactivation.
	 * If a VG is changed, then autoactivation refreshes LVs in the VG.
	 */

	if (found_vgnames && vgname && status && !strcmp(status, "complete")) {
		log_debug("VG %s is complete in lvmetad with dev %s.", vgname, dev_name(dev));
		if (!str_list_add(cmd->mem, found_vgnames, dm_pool_strdup(cmd->mem, vgname)))
			log_error("str_list_add failed");

		if (changed_vgnames && changed) {
			log_debug("VG %s is changed in lvmetad.", vgname);
			if (!str_list_add(cmd->mem, changed_vgnames, dm_pool_strdup(cmd->mem, vgname)))
				log_error("str_list_add failed");
		}
	}

	daemon_reply_destroy(reply);

	return result;
}

int lvmetad_pv_gone(dev_t devno, const char *pv_name)
{
	daemon_reply reply;
	int result;
	int found;

	if (!lvmetad_used() || test_mode())
		return 1;

	/*
	 *  TODO: automatic volume deactivation takes place here *before*
	 *        all cached info is gone - call handler. Also, consider
	 *        integrating existing deactivation script  that deactivates
	 *        the whole stack from top to bottom (not yet upstream).
	 */

	log_debug_lvmetad("Telling lvmetad to forget any PV on %s", pv_name);
	reply = _lvmetad_send(NULL, "pv_gone", "device = %" PRId64, (int64_t) devno, NULL);

	result = _lvmetad_handle_reply(reply, "pv_gone", pv_name, &found);
	/* We don't care whether or not the daemon had the PV cached. */

	daemon_reply_destroy(reply);

	return result;
}

int lvmetad_pv_gone_by_dev(struct device *dev)
{
	return lvmetad_pv_gone(dev->dev, dev_name(dev));
}

/*
 * The following code implements pvscan --cache.
 */

struct _lvmetad_pvscan_baton {
	struct cmd_context *cmd;
	struct volume_group *vg;
	struct format_instance *fid;
};

static int _lvmetad_pvscan_single(struct metadata_area *mda, void *baton)
{
	struct _lvmetad_pvscan_baton *b = baton;
	struct volume_group *vg;

	if (mda_is_ignored(mda) ||
	    !(vg = mda->ops->vg_read(b->fid, "", mda, NULL, NULL)))
		return 1;

	/* FIXME Also ensure contents match etc. */
	if (!b->vg || vg->seqno > b->vg->seqno)
		b->vg = vg;
	else if (b->vg)
		release_vg(vg);

	return 1;
}

/*
 * FIXME: handle errors and do proper comparison of metadata from each area
 * like vg_read and fall back to real vg_read from disk if there's any problem.
 */

static int _lvmetad_pvscan_vg_single(struct metadata_area *mda, void *baton)
{
	struct _lvmetad_pvscan_baton *b = baton;
	struct volume_group *vg = NULL;

	if (mda_is_ignored(mda))
		return 1;

	if (!(vg = mda->ops->vg_read(b->fid, "", mda, NULL, NULL)))
		return 1;

	if (!b->vg)
		b->vg = vg;
	else if (vg->seqno > b->vg->seqno) {
		release_vg(b->vg);
		b->vg = vg;
	} else
		release_vg(vg);

	return 1;
}

/*
 * The lock manager may detect that the vg cached in lvmetad is out of date,
 * due to something like an lvcreate from another host.
 * This is limited to changes that only affect the vg (not global state like
 * orphan PVs), so we only need to reread mdas on the vg's existing pvs.
 * But, a previous PV in the VG may have been removed since we last read
 * the VG, and that PV may have been reused for another VG.
 */

static struct volume_group *_lvmetad_pvscan_vg(struct cmd_context *cmd, struct volume_group *vg,
					      const char *vgid, struct format_type *fmt)
{
	char pvid_s[ID_LEN + 1] __attribute__((aligned(8)));
	char uuid[64] __attribute__((aligned(8)));
	struct dm_config_tree *vgmeta;
	struct pv_list *pvl, *pvl_new;
	struct device_list *devl, *devlsafe;
	struct dm_list pvs_scan;
	struct dm_list pvs_drop;
	struct lvmcache_vginfo *vginfo = NULL;
	struct lvmcache_info *info = NULL;
	struct format_instance *fid;
	struct format_instance_ctx fic = { .type = 0 };
	struct _lvmetad_pvscan_baton baton;
	struct volume_group *save_vg;
	struct dm_config_tree *save_meta;
	struct device *save_dev = NULL;
	uint32_t save_seqno = 0;
	int found_new_pvs = 0;
	int retried_reads = 0;
	int found;

	save_vg = NULL;
	save_meta = NULL;
	save_dev = NULL;
	save_seqno = 0;

	dm_list_init(&pvs_scan);
	dm_list_init(&pvs_drop);

	log_debug_lvmetad("Rescan VG %s to update lvmetad (seqno %u).", vg->name, vg->seqno);

	/*
	 * Make sure this command knows about all PVs from lvmetad.
	 */
	lvmcache_seed_infos_from_lvmetad(cmd);

	/*
	 * Start with the list of PVs that we last saw in the VG.
	 * Some may now be gone, and some new PVs may have been added.
	 */
	dm_list_iterate_items(pvl, &vg->pvs) {
		if (!(devl = dm_pool_zalloc(cmd->mem, sizeof(*devl))))
			return_NULL;
		devl->dev = pvl->pv->dev;
		dm_list_add(&pvs_scan, &devl->list);
	}

	/*
	 * Rescan labels/metadata only from devs that we previously
	 * saw in the VG.  If we find below that there are new PVs
	 * in the VG, we'll have to rescan all devices to find which
	 * device(s) are now being used.
	 */
	log_debug_lvmetad("Rescan VG %s scanning data from devs in previous metadata.", vg->name);

	label_scan_devs(cmd, cmd->full_filter, &pvs_scan);

	/*
	 * Check if any pvs_scan entries are no longer PVs.
	 * In that case, label_read/_find_label_header will have
	 * found no label_header, and would have dropped the
	 * info struct for the device from lvmcache.  So, if
	 * we look up the info struct here and don't find it,
	 * we can infer it's no longer a PV.
	 *
	 * FIXME: we should record specific results from the
	 * label_read and then check specifically for whatever
	 * result means "no label was found", rather than going
	 * about this indirectly via the lvmcache side effects.
	 */
	dm_list_iterate_items_safe(devl, devlsafe, &pvs_scan) {
		if (!(info = lvmcache_info_from_pvid(devl->dev->pvid, devl->dev, 0))) {
			/* Another host removed this PV from the VG. */
			log_debug_lvmetad("Rescan VG %s from %s dropping dev (no label).",
					  vg->name, dev_name(devl->dev));
			dm_list_move(&pvs_drop, &devl->list);
		}
	}

	fic.type = FMT_INSTANCE_MDAS | FMT_INSTANCE_AUX_MDAS;
	fic.context.vg_ref.vg_name = vg->name;
	fic.context.vg_ref.vg_id = vgid;

 retry_reads:

	if (!(fid = fmt->ops->create_instance(fmt, &fic))) {
		/* FIXME: are there only internal reasons for failures here? */
		log_error("Reading VG %s failed to create format instance.", vg->name);
		return NULL;
	}

	/* FIXME: not sure if this is necessary */
	fid->ref_count++;

	baton.fid = fid;
	baton.cmd = cmd;

	/*
	 * FIXME: this vg_read path does not have the ability to repair
	 * any problems with the VG, e.g. VG on one dev has an older
	 * seqno.  When vg_read() is reworked, we need to fall back
	 * to using that from here (and vg_read's from lvmetad) when
	 * there is a problem.  Perhaps by disabling lvmetad when a
	 * VG problem is detected, causing commands to fully fall
	 * back to disk, which will repair the VG.  Then lvmetad can
	 * be repopulated and re-enabled (possibly automatically.)
	 */

	/*
	 * Do a low level vg_read on each dev, verify the vg returned
	 * from metadata on each device is for the VG being read
	 * (the PV may have been removed from the VG being read and
	 * added to a different one), and return this vg to the caller
	 * as the current vg to use.
	 *
	 * The label scan above will have saved in lvmcache which
	 * vg each device is used in, so we could figure that part
	 * out without doing the vg_read.
	 */
	dm_list_iterate_items_safe(devl, devlsafe, &pvs_scan) {
		if (!devl->dev)
			continue;

		log_debug_lvmetad("Rescan VG %s getting metadata from %s.",
				  vg->name, dev_name(devl->dev));

		/*
		 * The info struct for this dev knows what and where
		 * the mdas are for this dev (the label scan saved
		 * the mda locations for this dev on the lvmcache info struct).
		 */
		if (!(info = lvmcache_info_from_pvid(devl->dev->pvid, devl->dev, 0))) {
			log_debug_lvmetad("Rescan VG %s from %s dropping dev (no info).",
					  vg->name, dev_name(devl->dev));
			dm_list_move(&pvs_drop, &devl->list);
			continue;
		}

		baton.vg = NULL;

		/*
		 * Read VG metadata from this dev's mdas.
		 */
		lvmcache_foreach_mda(info, _lvmetad_pvscan_vg_single, &baton);

		/*
		 * The PV may have been removed from the VG by another host
		 * since we last read the VG.
		 */
		if (!baton.vg) {
			log_debug_lvmetad("Rescan VG %s from %s dropping dev (no metadata).",
					  vg->name, dev_name(devl->dev));
			dm_list_move(&pvs_drop, &devl->list);
			continue;
		}

		/*
		 * The PV may have been removed from the VG and used for a
		 * different VG since we last read the VG.
		 */
		if (strcmp(baton.vg->name, vg->name)) {
			log_debug_lvmetad("Rescan VG %s from %s dropping dev (other VG %s).",
					  vg->name, dev_name(devl->dev), baton.vg->name);
			release_vg(baton.vg);
			continue;
		}

		if (!(vgmeta = export_vg_to_config_tree(baton.vg))) {
			log_error("VG export to config tree failed");
			release_vg(baton.vg);
			continue;
		}

		/*
		 * The VG metadata read from each dev should match.  Save the
		 * metadata from the first dev, and compare it to the metadata
		 * read from each other dev.
		 */

		if (save_vg && (save_seqno != baton.vg->seqno)) {
			/* FIXME: fall back to vg_read to correct this. */
			log_warn("WARNING: inconsistent metadata for VG %s on devices %s seqno %u and %s seqno %u.",
				 vg->name, dev_name(save_dev), save_seqno,
				 dev_name(devl->dev), baton.vg->seqno);
			log_warn("WARNING: temporarily disable lvmetad to repair metadata.");

			/* Use the most recent */
			if (save_seqno < baton.vg->seqno) {
				release_vg(save_vg);
				dm_config_destroy(save_meta);
				save_vg = baton.vg;
				save_meta = vgmeta;
				save_seqno = baton.vg->seqno;
				save_dev = devl->dev;
			} else {
				release_vg(baton.vg);
				dm_config_destroy(vgmeta);
			}
			continue;
		}

		if (!save_vg) {
			save_vg = baton.vg;
			save_meta = vgmeta;
			save_seqno = baton.vg->seqno;
			save_dev = devl->dev;
		} else {
			struct dm_config_node *meta1 = save_meta->root;
			struct dm_config_node *meta2 = vgmeta->root;
			struct dm_config_node *sib1 = meta1->sib;
			struct dm_config_node *sib2 = meta2->sib;

			/*
			 * Do not compare the extraneous data that
			 * export_vg_to_config_tree() inserts next to the
			 * actual VG metadata.  This includes creation_time
			 * which may not match since it is generated separately
			 * for each call to create the config tree.
			 *
			 * We're saving the sibling pointer and restoring it
			 * after the compare because we're unsure if anything
			 * later might want it.
			 *
			 * FIXME: make it clearer what we're doing here, e.g.
			 * pass a parameter to export_vg_to_config_tree()
			 * telling it to skip the extraneous data, or something.
			 * It's very non-obvious that setting sib=NULL does that.
			 */
			meta1->sib = NULL;
			meta2->sib = NULL;

			if (compare_config(meta1, meta2)) {
				/* FIXME: fall back to vg_read to correct this. */
				log_warn("WARNING: inconsistent metadata for VG %s on devices %s seqno %u and %s seqno %u.",
					 vg->name, dev_name(save_dev), save_seqno,
					 dev_name(devl->dev), baton.vg->seqno);
				log_warn("WARNING: temporarily disable lvmetad to repair metadata.");
				log_error("VG %s metadata comparison failed for device %s vs %s",
					  vg->name, dev_name(devl->dev), save_dev ? dev_name(save_dev) : "none");
				_log_debug_inequality(vg->name, save_meta->root, vgmeta->root);

				meta1->sib = sib1;
				meta2->sib = sib2;

				/* no right choice, just use the previous copy */
				release_vg(baton.vg);
				dm_config_destroy(vgmeta);
			}
			meta1->sib = sib1;
			meta2->sib = sib2;
			release_vg(baton.vg);
			dm_config_destroy(vgmeta);
		}
	}

	/* FIXME: see above */
	fid->ref_count--;

	/*
	 * Look for any new PVs in the VG metadata that were not in our
	 * previous version of the VG.
	 *
	 * (Don't look for new PVs after a rescan and retry.)
	 */
	found_new_pvs = 0;

	if (save_vg && !retried_reads) {
		dm_list_iterate_items(pvl_new, &save_vg->pvs) {
			found = 0;
			dm_list_iterate_items(pvl, &vg->pvs) {
				if (pvl_new->pv->dev != pvl->pv->dev)
					continue;
				found = 1;
				break;
			}

			/*
			 * PV in new VG metadata not found in old VG metadata.
			 * There's a good chance we don't know about this new
			 * PV or what device it's on; a label scan is needed
			 * of all devices so we know which device the VG is
			 * now using.
			 */
			if (!found) {
				found_new_pvs++;
				strncpy(pvid_s, (char *) &pvl_new->pv->id, sizeof(pvid_s) - 1);
				if (!id_write_format((const struct id *)&pvid_s, uuid, sizeof(uuid)))
					stack;
				log_debug_lvmetad("Rescan VG %s found new PV %s.", vg->name, uuid);
			}
		}
	}

	if (!save_vg && retried_reads) {
		log_error("VG %s not found after rescanning devices.", vg->name);
		goto out;
	}

	/*
	 * Do a full rescan of devices, then look up which devices the
	 * scan found for this VG name, and select those devices to
	 * read metadata from in the loop above (rather than the list
	 * of devices we created from our last copy of the vg metadata.)
	 *
	 * Case 1: VG we knew is no longer on any of the devices we knew it
	 * to be on (save_vg is NULL, which means the metadata wasn't found
	 * when reading mdas on each of the initial pvs_scan devices).
	 * Rescan all devs and then retry reading metadata from the devs that
	 * the scan finds associated with this VG.
	 *
	 * Case 2: VG has new PVs but we don't know what devices they are
	 * so rescan all devs and then retry reading metadata from the devs
	 * that the scan finds associated with this VG.
	 *
	 * (N.B. after a retry, we don't check for found_new_pvs.)
	 */
	if (!save_vg || found_new_pvs) {
		if (!save_vg)
			log_debug_lvmetad("Rescan VG %s did not find VG on previous devs.", vg->name);
		if (found_new_pvs)
			log_debug_lvmetad("Rescan VG %s scanning all devs to find new PVs.", vg->name);

		label_scan(cmd);

		if (!(vginfo = lvmcache_vginfo_from_vgname(vg->name, NULL))) {
			log_error("VG %s vg info not found after rescanning devices.", vg->name);
			goto out;
		}

		/*
		 * Set pvs_scan to devs that the label scan found
		 * in the VG and retry the metadata reading loop.
		 */
		dm_list_init(&pvs_scan);

		if (!lvmcache_get_vg_devs(cmd, vginfo, &pvs_scan)) {
			log_error("VG %s info devs not found after rescanning devices.", vg->name);
			goto out;
		}

		log_debug_lvmetad("Rescan VG %s has %d PVs after label scan.",
				  vg->name, dm_list_size(&pvs_scan));

		if (save_vg)
			release_vg(save_vg);
		if (save_meta)
			dm_config_destroy(save_meta);
		save_vg = NULL;
		save_meta = NULL;
		save_dev = NULL;
		save_seqno = 0;
		found_new_pvs = 0;
		retried_reads = 1;
		goto retry_reads;
	}

	/*
	 * Remove pvs_drop entries from lvmetad.
	 */
	dm_list_iterate_items(devl, &pvs_drop) {
		if (!devl->dev)
			continue;
		log_debug_lvmetad("Rescan VG %s removing %s from lvmetad.", vg->name, dev_name(devl->dev));
		if (!lvmetad_pv_gone_by_dev(devl->dev)) {
			/* FIXME: use an error path that disables lvmetad */
			log_error("Failed to remove %s from lvmetad.", dev_name(devl->dev));
		}
	}

	/*
	 * Update lvmetad with the newly read version of the VG.
	 * When the seqno is unchanged the cached VG can be left.
	 */
	if (save_vg && (save_seqno != vg->seqno)) {
		dm_list_iterate_items(devl, &pvs_scan) {
			if (!devl->dev)
				continue;
			log_debug_lvmetad("Rescan VG %s removing %s from lvmetad to replace.",
					  vg->name, dev_name(devl->dev));
			if (!lvmetad_pv_gone_by_dev(devl->dev)) {
				/* FIXME: use an error path that disables lvmetad */
				log_error("Failed to remove %s from lvmetad.", dev_name(devl->dev));
			}
		}

		log_debug_lvmetad("Rescan VG %s updating lvmetad from seqno %u to seqno %u.",
				  vg->name, vg->seqno, save_seqno);

		/*
		 * If this vg_update fails the cached metadata in
		 * lvmetad will remain invalid.
		 */
		save_vg->lvmetad_update_pending = 1;
		if (!lvmetad_vg_update_finish(save_vg)) {
			/* FIXME: use an error path that disables lvmetad */
			log_error("Failed to update lvmetad with new VG meta");
		}
	}
out:
	if (!save_vg && fid)
		fmt->ops->destroy_instance(fid);
	if (save_meta)
		dm_config_destroy(save_meta);
	if (save_vg)
		log_debug_lvmetad("Rescan VG %s done (new seqno %u).", save_vg->name, save_vg->seqno);
	return save_vg;
}

int lvmetad_pvscan_single(struct cmd_context *cmd, struct device *dev,
			  struct dm_list *found_vgnames,
			  struct dm_list *changed_vgnames)
{
	struct label *label;
	struct lvmcache_info *info;
	struct _lvmetad_pvscan_baton baton;
	const struct format_type *fmt;
	/* Create a dummy instance. */
	struct format_instance_ctx fic = { .type = 0 };

	log_debug_lvmetad("Scan metadata from dev %s", dev_name(dev));

	if (!lvmetad_used()) {
		log_error("Cannot proceed since lvmetad is not active.");
		return 0;
	}

	if (udev_dev_is_mpath_component(dev)) {
		log_debug("Ignore multipath component for pvscan.");
		return 1;
	}

	if (!(info = lvmcache_info_from_pvid(dev->pvid, dev, 0))) {
		log_print_unless_silent("No PV info found on %s for PVID %s.", dev_name(dev), dev->pvid);
		if (!lvmetad_pv_gone_by_dev(dev))
			goto_bad;
		return 1;
	}

	if (!(label = lvmcache_get_label(info))) {
		log_print_unless_silent("No PV label found for %s.", dev_name(dev));
		if (!lvmetad_pv_gone_by_dev(dev))
			goto_bad;
		return 1;
	}

	fmt = lvmcache_fmt(info);

	baton.cmd = cmd;
	baton.vg = NULL;
	baton.fid = fmt->ops->create_instance(fmt, &fic);

	if (!baton.fid)
		goto_bad;

	lvmcache_foreach_mda(info, _lvmetad_pvscan_single, &baton);

	if (!baton.vg)
		fmt->ops->destroy_instance(baton.fid);

	if (!lvmetad_pv_found(cmd, (const struct id *) &dev->pvid, dev, fmt,
			      label->sector, baton.vg, found_vgnames, changed_vgnames)) {
		release_vg(baton.vg);
		goto_bad;
	}

	release_vg(baton.vg);
	return 1;

bad:
	return 0;
}

/*
 * Update the lvmetad cache: clear the current lvmetad cache, and scan all
 * devs, sending all info from the devs to lvmetad.
 *
 * We want only one command to be doing this at a time.  When do_wait is set,
 * this will first check if lvmetad is currently being updated by another
 * command, and if so it will delay until that update is finished, or until a
 * timeout, at which point it will go ahead and do the lvmetad update.
 *
 * Callers that have already checked and waited for the updating state, e.g. by
 * using lvmetad_token_matches(), will generaly set do_wait to 0.  Callers that
 * have not checked for the updating state yet will generally set do_wait to 1.
 *
 * If another command doing an update failed, it left lvmetad in the "update in
 * progess" state, so we can't just wait until that state has cleared, but have
 * to go ahead after a timeout.
 *
 * The _lvmetad_is_updating check avoids most races to update lvmetad from
 * multiple commands (which shouldn't generally happen anway) but does not
 * eliminate them.  If an update race happens, the second will see that the
 * previous token was "update in progress" when it calls _token_update().  It
 * will then fail, and the command calling lvmetad_pvscan_all_devs() will
 * generally revert disk scanning and not use lvmetad.
 */

int lvmetad_pvscan_all_devs(struct cmd_context *cmd, int do_wait)
{
	struct device_list *devl, *devl2;
	struct dm_list scan_devs;
	daemon_reply reply;
	char *future_token;
	const char *reason;
	int was_silent;
	int replacing_other_update = 0;
	int replaced_update = 0;
	int retries = 0;
	int ret = 1;

	if (!lvmetad_used()) {
		log_error("Cannot proceed since lvmetad is not active.");
		return 0;
	}

 retry:
	dm_list_init(&scan_devs);

	/*
	 * If another update is in progress, delay to allow it to finish,
	 * rather than interrupting it with our own update.
	 */
	if (do_wait && _lvmetad_is_updating(cmd, 1)) {
		log_warn("WARNING: lvmetad update is interrupting another update in progress.");
		replacing_other_update = 1;
	}

	future_token = _lvmetad_token;
	_lvmetad_token = (char *) LVMETAD_TOKEN_UPDATE_IN_PROGRESS;

	if (!_token_update(&replaced_update)) {
		log_error("Failed to start lvmetad update.");
		_lvmetad_token = future_token;
		return 0;
	}

	/*
	 * if _token_update() sets replaced_update to 1, it means that we set
	 * "update in progress" when the lvmetad was already set to "udpate in
	 * progress".  This detects a race between two commands doing updates
	 * at once.  The attempt above to avoid this race using
	 * _lvmetad_is_updating isn't perfect.
	 */
	if (!replacing_other_update && replaced_update) {
		if (do_wait && !retries) {
			retries = 1;
			log_warn("WARNING: lvmetad update in progress, retrying update.");
			_lvmetad_token = future_token;
			goto retry;
		}
		log_warn("WARNING: lvmetad update in progress, skipping update.");
		_lvmetad_token = future_token;
		return 0;
	}

	log_verbose("Scanning all devices to initialize lvmetad.");

	label_scan_pvscan_all(cmd, &scan_devs);

	log_debug_lvmetad("Telling lvmetad to clear its cache");
	reply = _lvmetad_send(cmd, "pv_clear_all", NULL);
	if (!_lvmetad_handle_reply(reply, "pv_clear_all", "", NULL))
		ret = 0;
	daemon_reply_destroy(reply);

	was_silent = silent_mode();
	init_silent(1);

	log_debug_lvmetad("Sending %d devices to lvmetad.", dm_list_size(&scan_devs));

	dm_list_iterate_items_safe(devl, devl2, &scan_devs) {
		if (sigint_caught()) {
			ret = 0;
			stack;
			break;
		}

		dm_list_del(&devl->list);

		ret = lvmetad_pvscan_single(cmd, devl->dev, NULL, NULL);

		label_scan_invalidate(devl->dev);

		dm_free(devl);

		if (!ret) {
			stack;
			break;
		}
	}

	init_silent(was_silent);

	_lvmetad_token = future_token;

	/*
	 * If we failed to fully and successfully populate lvmetad just leave
	 * the existing "update in progress" token in place so lvmetad will
	 * time out our update and force another command to do it.
	 * (We could try to set the token to empty here, but that doesn't
	 * help much.)
	 */
	if (!ret)
		return 0;

	if (!_token_update(NULL)) {
		log_error("Failed to update lvmetad token after device scan.");
		return 0;
	}

	/* This will disable lvmetad if label scan found duplicates. */
	lvmcache_pvscan_duplicate_check(cmd);
	if (lvmcache_found_duplicate_pvs()) {
		log_warn("WARNING: Scan found duplicate PVs.");
		return 0;
	}

	/*
	 * If lvmetad is disabled, and no duplicate PVs were seen, then re-enable lvmetad.
	 */
	if (lvmetad_is_disabled(cmd, &reason) && !lvmcache_found_duplicate_pvs()) {
		log_debug_lvmetad("Enabling lvmetad which was previously disabled.");
		lvmetad_clear_disabled(cmd);
	}

	return ret;
}

int lvmetad_vg_clear_outdated_pvs(struct volume_group *vg)
{
	char uuid[64];
	daemon_reply reply;
	int result;

	if (!id_write_format(&vg->id, uuid, sizeof(uuid)))
		return_0;

	log_debug_lvmetad("Sending lvmetad vg_clear_outdated_pvs");
	reply = _lvmetad_send(vg->cmd, "vg_clear_outdated_pvs", "vgid = %s", uuid, NULL);
	result = _lvmetad_handle_reply(reply, "vg_clear_outdated_pvs", vg->name, NULL);
	daemon_reply_destroy(reply);

	return result;
}

/*
 * Records the state of cached PVs in lvmetad so we can look for changes
 * after rescanning.
 */
struct pv_cache_list {
	struct dm_list list;
	dev_t devt;
	struct id pvid;
	const char *vgid;
	unsigned found : 1;
	unsigned update_udev : 1;
};

/*
 * Get the list of PVs known to lvmetad.
 */
static int _lvmetad_get_pv_cache_list(struct cmd_context *cmd, struct dm_list *pvc_list)
{
	daemon_reply reply;
	struct dm_config_node *cn;
	struct pv_cache_list *pvcl;
	const char *pvid_txt;
	const char *vgid;

	if (!lvmetad_used())
		return 1;

	log_debug_lvmetad("Asking lvmetad for complete list of known PVs");

	reply = _lvmetad_send(cmd, "pv_list", NULL);
	if (!_lvmetad_handle_reply(reply, "pv_list", "", NULL)) {
		daemon_reply_destroy(reply);
		return_0;
	}

	if ((cn = dm_config_find_node(reply.cft->root, "physical_volumes"))) {
		for (cn = cn->child; cn; cn = cn->sib) {
			if (!(pvcl = dm_pool_zalloc(cmd->mem, sizeof(*pvcl)))) {
				log_error("pv_cache_list allocation failed.");
				return 0;
			}

			pvid_txt = cn->key;
			if (!id_read_format(&pvcl->pvid, pvid_txt)) {
				stack;
				continue;
			}

			pvcl->devt = dm_config_find_int(cn->child, "device", 0);

			if ((vgid = dm_config_find_str(cn->child, "vgid", NULL)))
				pvcl->vgid = dm_pool_strdup(cmd->mem, vgid);

			dm_list_add(pvc_list, &pvcl->list);
		}
	}

	daemon_reply_destroy(reply);

	return 1;
}

/*
 * Opening the device RDWR should trigger a udev db update.
 * FIXME: is there a better way to update the udev db than
 * doing an open/close of the device? - For example writing
 * "change" to /sys/block/<device>/uevent?
 */
static void _update_pv_in_udev(struct cmd_context *cmd, dev_t devt)
{

	/*
	 * FIXME: this is diabled as part of removing dev_opens
	 * to integrate bcache.  If this is really needed, we
	 * can do a separate open/close here.
	 */
	log_debug_devs("SKIP device %d:%d open to update udev",
		       (int)MAJOR(devt), (int)MINOR(devt));

#if 0
	struct device *dev;

	if (!(dev = dev_cache_get_by_devt(devt, cmd->lvmetad_filter))) {
		log_error("_update_pv_in_udev no dev found");
		return;
	}

	if (!dev_open(dev)) {
		stack;
		return;
	}

	if (!dev_close(dev))
		stack;
#endif
}

/*
 * Compare before and after PV lists from before/after rescanning,
 * and update udev db for changes.
 *
 * For PVs that have changed pvid or vgid in lvmetad from rescanning,
 * there may be information in the udev database to update, so open
 * these devices to trigger a udev update.
 *
 * "before" refers to the list of pvs from lvmetad before rescanning
 * "after" refers to the list of pvs from lvmetad after rescanning
 *
 * Comparing both lists, we can see which PVs changed (pvid or vgid),
 * and trigger a udev db update for those.
 */
static void _update_changed_pvs_in_udev(struct cmd_context *cmd,
					struct dm_list *pvc_before,
					struct dm_list *pvc_after)
{
	struct pv_cache_list *before;
	struct pv_cache_list *after;
	char id_before[ID_LEN + 1];
	char id_after[ID_LEN + 1];
	int found;

	dm_list_iterate_items(before, pvc_before) {
		found = 0;

		dm_list_iterate_items(after, pvc_after) {
			if (after->found)
				continue;

			if (before->devt != after->devt)
				continue;

			if (!id_equal(&before->pvid, &after->pvid)) {
				(void) dm_strncpy(id_before, (char *) &before->pvid, sizeof(id_before));
				(void) dm_strncpy(id_after, (char *) &after->pvid, sizeof(id_after));

				log_debug_devs("device %d:%d changed pvid from %s to %s",
					       (int)MAJOR(before->devt), (int)MINOR(before->devt),
					       id_before, id_after);

				before->update_udev = 1;

			} else if ((before->vgid && !after->vgid) ||
				   (after->vgid && !before->vgid) ||
				   (before->vgid && after->vgid && strcmp(before->vgid, after->vgid))) {

				log_debug_devs("device %d:%d changed vg from %s to %s",
					       (int)MAJOR(before->devt), (int)MINOR(before->devt),
					       before->vgid ?: "none", after->vgid ?: "none");

				before->update_udev = 1;
			}

			after->found = 1;
			before->found = 1;
			found = 1;
			break;
		}

		if (!found) {
			(void) dm_strncpy(id_before, (char *) &before->pvid, sizeof(id_before));

			log_debug_devs("device %d:%d pvid %s vg %s is gone",
				       (int)MAJOR(before->devt), (int)MINOR(before->devt),
				       id_before, before->vgid ? before->vgid : "none");

			before->update_udev = 1;
		}
	}

	dm_list_iterate_items(before, pvc_before) {
		if (before->update_udev)
			_update_pv_in_udev(cmd, before->devt);
	}

	dm_list_iterate_items(after, pvc_after) {
		if (after->update_udev)
			_update_pv_in_udev(cmd, after->devt);
	}
}

/*
 * Before this command was run, some external entity may have
 * invalidated lvmetad's cache of global information, e.g. lvmlockd.
 *
 * The global information includes things like a new VG, a
 * VG that was removed, the assignment of a PV to a VG;
 * any change that is not isolated within a single VG.
 *
 * The external entity, like a lock manager, would invalidate
 * the lvmetad global cache if it detected that the global
 * information had been changed on disk by something other
 * than a local lvm command, e.g. an lvm command on another
 * host with access to the same devices.  (How it detects
 * the change is specific to lock manager or other entity.)
 *
 * The effect is that metadata on disk is newer than the metadata
 * in the local lvmetad daemon, and the local lvmetad's cache
 * should be updated from disk before this command uses it.
 *
 * So, using this function, a command checks if lvmetad's global
 * cache is valid.  If so, it does nothing.  If not, it rescans
 * devices to update the lvmetad cache, then it notifies lvmetad
 * that it's cache is valid again (consistent with what's on disk.)
 * This command can then go ahead and use the newly refreshed metadata.
 *
 * 1. Check if the lvmetad global cache is invalid.
 * 2. If so, reread metadata from all devices and update the lvmetad cache.
 * 3. Tell lvmetad that the global cache is now valid.
 */

void lvmetad_validate_global_cache(struct cmd_context *cmd, int force)
{
	struct dm_list pvc_before; /* pv_cache_list */
	struct dm_list pvc_after; /* pv_cache_list */
	const char *reason = NULL;
	daemon_reply reply;
	int global_invalid;

	dm_list_init(&pvc_before);
	dm_list_init(&pvc_after);

	if (!lvmlockd_use()) {
		log_error(INTERNAL_ERROR "validate global cache without lvmlockd");
		return;
	}

	if (!lvmetad_used())
		return;

	log_debug_lvmetad("Validating global lvmetad cache");

	if (force)
		goto do_scan;

	log_debug_lvmetad("lvmetad validate send get_global_info");

	reply = daemon_send_simple(_lvmetad, "get_global_info",
				   "token = %s", "skip",
				   "pid = " FMTd64, (int64_t)getpid(),
				   "cmd = %s", get_cmd_name(),
				   NULL);

	if (reply.error) {
		log_error("lvmetad_validate_global_cache get_global_info error %d", reply.error);
		goto do_scan;
	}

	if (strcmp(daemon_reply_str(reply, "response", ""), "OK")) {
		log_error("lvmetad_validate_global_cache get_global_info not ok");
		goto do_scan;
	}

	global_invalid = daemon_reply_int(reply, "global_invalid", -1);

	daemon_reply_destroy(reply);

	if (!global_invalid)
		return; /* cache is valid */

 do_scan:
	/*
	 * Save the current state of pvs from lvmetad so after devices are
	 * scanned, we can compare to the new state to see if pvs changed.
	 */
	_lvmetad_get_pv_cache_list(cmd, &pvc_before);

	log_debug_lvmetad("Rescan all devices to validate global cache.");

	/*
	 * Update the local lvmetad cache so it correctly reflects any
	 * changes made on remote hosts.  (It's possible that this command
	 * already refreshed the local lvmetad because of a token change,
	 * but we need to do it again here since we now hold the global
	 * lock.  Another host may have changed things between the time
	 * we rescanned for the token, and the time we acquired the global
	 * lock.)
	 */
	if (!lvmetad_pvscan_all_devs(cmd, 1)) {
		log_warn("WARNING: Not using lvmetad because cache update failed.");
		lvmetad_make_unused(cmd);
		return;
	}

	if (lvmetad_is_disabled(cmd, &reason)) {
		log_warn("WARNING: Not using lvmetad because %s.", reason);
		lvmetad_make_unused(cmd);
		return;
	}

	/*
	 * Clear the global_invalid flag in lvmetad.
	 * Subsequent local commands that read global state
	 * from lvmetad will not see global_invalid until
	 * another host makes another global change.
	 */
	log_debug_lvmetad("lvmetad validate send set_global_info");

	reply = daemon_send_simple(_lvmetad, "set_global_info",
				   "token = %s", "skip",
				   "global_invalid = " FMTd64, INT64_C(0),
				   "pid = " FMTd64, (int64_t)getpid(),
				   "cmd = %s", get_cmd_name(),
				   NULL);
	if (reply.error)
		log_error("lvmetad_validate_global_cache set_global_info error %d", reply.error);

	if (strcmp(daemon_reply_str(reply, "response", ""), "OK"))
		log_error("lvmetad_validate_global_cache set_global_info not ok");

	daemon_reply_destroy(reply);

	/*
	 * Populate this command's lvmcache structures from lvmetad.
	 */
	lvmcache_seed_infos_from_lvmetad(cmd);

	/*
	 * Update the local udev database to reflect PV changes from
	 * other hosts.
	 *
	 * Compare the before and after PV lists, and if a PV's
	 * pvid or vgid has changed, then open that device to trigger
	 * a uevent to update the udev db.
	 *
	 * This has no direct benefit to lvm, but is just a best effort
	 * attempt to keep the udev db updated and reflecting current
	 * lvm information.
	 *
	 * FIXME: lvmcache_seed_infos_from_lvmetad() and _lvmetad_get_pv_cache_list()
	 * each get pv_list from lvmetad, and they could share a single pv_list reply.
	 */
	if (!dm_list_empty(&pvc_before)) {
		_lvmetad_get_pv_cache_list(cmd, &pvc_after);
		_update_changed_pvs_in_udev(cmd, &pvc_before, &pvc_after);
	}

	log_debug_lvmetad("Rescanned all devices");
}

int lvmetad_vg_is_foreign(struct cmd_context *cmd, const char *vgname, const char *vgid)
{
	daemon_reply reply;
	struct dm_config_node *top;
	const char *system_id = NULL;
	char uuid[64];
	int ret;

	if (!id_write_format((const struct id*)vgid, uuid, sizeof(uuid)))
		return_0;

	log_debug_lvmetad("Sending lvmetad vg_clear_outdated_pvs");
	reply = _lvmetad_send(cmd, "vg_lookup",
			      "uuid = %s", uuid,
			      "name = %s", vgname,
			       NULL);

	if ((top = dm_config_find_node(reply.cft->root, "metadata")))
		system_id = dm_config_find_str(top, "metadata/system_id", NULL);

	ret = !is_system_id_allowed(cmd, system_id);

	daemon_reply_destroy(reply);

	return ret;
}

/*
 * lvmetad has a disabled state in which it continues running,
 * and returns the "disabled" flag in a get_global_info query.
 *
 * Case 1
 * ------
 * When "normal" commands start, (those not specifically
 * intended to rescan devs) they begin by checking lvmetad's
 * token and global info:
 *
 * - If the token doesn't match (should be uncommon), the
 * command first rescans devices to repopulate lvmetad with
 * the global_filter it is using.  After rescanning, the
 * lvmetad disabled state is set or cleared depending on
 * what the scan saw.
 *
 *     An unmatching token occurs when:
 *     . lvmetad was just started and has not been populated yet.
 *     . The global_filter has been changed in lvm.conf since the
 *       last command was run.
 *     . The global_filter is overriden on the command line.
 *       (There's little point in using lvmetad if global_filter
 *       is often changed/overridden.)
 *
 * - If the token does match (common case), the command and
 * lvmetad are using the same global_filter and the command
 * does not rescan devs to repopulate lvmetad, or change the
 * lvmetad disabled state.
 *
 * - After the token check/sync, the command checks if the
 * disabled flag is set in lvmetad.  If it is, the command will
 * not use the lvmetad cache and will revert to scanning, i.e.
 * it runs the same as if use_lvmetad=0.
 *
 * So, "normal" commands try to use the lvmetad cache to avoid
 * scanning devices.  In the uncommon case when the token doesn't
 * match, these commands will first rescan devs to repopulate the
 * lvmetad cache, and then attempt to use the lvmetad cache.
 * In the uncommon case where lvmetad is disabled (by a previous
 * command), the common commands do not rescan devs to repopulate
 * lvmetad, but revert the equivalent of use_lvmetad=0, reading
 * from disk instead of the cache.
 * The combination of those two uncommon cases means that a command
 * could begin by rescanning devs because of a token mismatch, then
 * disable lvmetad as a result of that scan, and continue without
 * using lvmetad.
 *
 * Case 2
 * ------
 * Commands that are meant to scan devices to repopulate the
 * lvmetad cache, e.g. pvscan --cache, will always rescan
 * devices and then set/clear the disabled state according to
 * what they found when scanning.  The global_filter is always
 * used when choosing which devices to scan to populate lvmetad.
 * The command-specific filter is never used when choosing
 * which devices to scan for repopulating the lvmetad cache.
 *
 * During a scan repopulating the lvmetad cache, a command looks
 * for PVs with lvm1 metadata, or duplicate PVs (two devices with
 * the same PVID).  If either of those are found during the scan,
 * the command sets the disabled state in lvmetad.  If none are
 * found, the command clears the disabled state in lvmetad.
 * (Other problems scanning may also cause the command to set the
 * disabled state.)
 *
 * Case 3
 * ------
 * The special command 'pvscan --cache <dev>' is meant to only
 * scan the specified device and send info from the dev to
 * lvmetad.  This single-dev pvscan will not detect duplicate PVs
 * since it only sees the one device.  If lvmetad already knows
 * about the same PV on another device, then lvmetad will be the
 * first to discover that a duplicate PV exists.  In this case,
 * lvmetad sets the disabled state for itself.
 *
 * Duplicates
 * ----------
 * The most common reasons for duplicate PVs to exist are:
 *
 * 1. Multipath.  When multipath is running, it creates a new
 * mpath device for the underlying "duplicate" devs.  lvm has
 * built in, automatic filtering that will hide the duplicate
 * devs of the underlying mpath dev, so the duplicates will
 * be skipping during scanning (multipath_component_detection).
 *
 * If multipath_component_detection=0, or if multipathd is not
 * running, or multipath is not set up to handle a particular
 * set of devs, then lvm will see the multipath paths as
 * duplicates.  lvm will choose one of them to use, consider
 * the other a duplicate, and disable lvmetad.  multipathd
 * should be configured and running to resolve these duplicates,
 * and multipath_component_detection enabled.
 *
 * 2. Cloning by copying.  One device is copied over another, e.g.
 * with dd.  This is a more concerning case because using the
 * wrong device could lead to corruption.  LVM will attempt to
 * choose the best device as the PV, but it may not always
 * be the right one.  In this case, lvmetad is disabled.
 * vgimportclone should be used on the new copy to resolve the
 * duplicates.
 *
 * 3. Cloning by hardware.  A LUN is cloned/snapshotted on
 * a hardware device.  The description here is the same as
 * cloning by copying.
 *
 * 4. Creating LVM snapshots of LVs being used as PVs.
 * If pvcreate is run on an LV, and lvcreate is used to
 * create a snapshot of that LV, then the two LVs will
 * appear to be duplicate PVs.
 *
 * Filtering duplicates
 * --------------------
 * 
 * If all but one copy of a PV is added to the global_filter,
 * then duplicates will not be seen when scanning to populate
 * the lvmetad cache.  Neither common commands nor scanning
 * commands will see the duplicates, and lvmetad will not be
 * disabled.
 *
 * If the global_filter is *not* used to hide duplicates,
 * then lvmetad will be disabled when they are scanned, but
 * common commands can use the command filter to hide the
 * duplicates and work with a selected instance of the PV.
 * The command will not use lvmetad in this case, but will
 * not see duplicate PVs itself because its command filter
 * is more restrictive than the global_filter and has hidden
 * the duplicates.
 */

/*
 * FIXME: if we fail to disable lvmetad, then other commands could
 * potentially use incorrect cache data from lvmetad.  Should we
 * do something more severe if the disable messages fails, like
 * sending SIGKILL to the lvmetad pid?
 *
 * FIXME: log something in syslog any time we disable lvmetad?
 * At a minimum if we fail to disable lvmetad.
 */
void lvmetad_set_disabled(struct cmd_context *cmd, const char *reason)
{
	daemon_handle tmph = { .error = 0 };
	daemon_reply reply;
	int tmp_con = 0;

	/*
	 * If we were using lvmetad at the start of the command, but are not
	 * now, then _was_connected should still be set.  In this case we
	 * want to make a temp connection just to disable it.
	 */
	if (!_lvmetad_use) {
		if (_was_connected) {
			/* Create a special temp connection just to send disable */
			tmph = lvmetad_open(_lvmetad_socket);
			if (tmph.socket_fd < 0 || tmph.error) {
				log_warn("Failed to connect to lvmetad to disable.");
				return;
			}
			tmp_con = 1;
		} else {
			/* We were never using lvmetad, don't start now. */
			return;
		}
	}

	log_debug_lvmetad("Sending lvmetad disabled %s", reason);

	if (tmp_con)
		reply = daemon_send_simple(tmph, "set_global_info",
				   "token = %s", "skip",
				   "global_disable = " FMTd64, (int64_t)1,
				   "disable_reason = %s", reason,
				   "pid = " FMTd64, (int64_t)getpid(),
				   "cmd = %s", get_cmd_name(),
				   NULL);
	else
		reply = daemon_send_simple(_lvmetad, "set_global_info",
				   "token = %s", "skip",
				   "global_disable = " FMTd64, (int64_t)1,
				   "disable_reason = %s", reason,
				   "pid = " FMTd64, (int64_t)getpid(),
				   "cmd = %s", get_cmd_name(),
				   NULL);

	if (reply.error)
		log_error("Failed to send message to lvmetad %d", reply.error);

	if (strcmp(daemon_reply_str(reply, "response", ""), "OK"))
		log_error("Failed response from lvmetad.");

	daemon_reply_destroy(reply);

	if (tmp_con)
		daemon_close(tmph);
}

void lvmetad_clear_disabled(struct cmd_context *cmd)
{
	daemon_reply reply;

	if (!_lvmetad_use)
		return;

	log_debug_lvmetad("Sending lvmetad disabled 0");

	reply = daemon_send_simple(_lvmetad, "set_global_info",
				   "token = %s", "skip",
				   "global_disable = " FMTd64, (int64_t)0,
				   "pid = " FMTd64, (int64_t)getpid(),
				   "cmd = %s", get_cmd_name(),
				   NULL);
	if (reply.error)
		log_error("Failed to send message to lvmetad %d", reply.error);

	if (strcmp(daemon_reply_str(reply, "response", ""), "OK"))
		log_error("Failed response from lvmetad.");

	daemon_reply_destroy(reply);
}

int lvmetad_is_disabled(struct cmd_context *cmd, const char **reason)
{
	daemon_reply reply;
	const char *reply_reason;
	int ret = 0;

	reply = daemon_send_simple(_lvmetad, "get_global_info",
				   "token = %s", "skip",
				   "pid = " FMTd64, (int64_t)getpid(),
				   "cmd = %s", get_cmd_name(),
				   NULL);

	if (reply.error) {
		*reason = "send error";
		ret = 1;
		goto out;
	}

	if (strcmp(daemon_reply_str(reply, "response", ""), "OK")) {
		*reason = "response error";
		ret = 1;
		goto out;
	}

	if (daemon_reply_int(reply, "global_disable", 0)) {
		ret = 1;

		reply_reason = daemon_reply_str(reply, "disable_reason", NULL);

		if (!reply_reason) {
			*reason = "<not set>";

		} else if (strstr(reply_reason, LVMETAD_DISABLE_REASON_DIRECT)) {
			*reason = "the disable flag was set directly";

		} else if (strstr(reply_reason, LVMETAD_DISABLE_REASON_REPAIR)) {
			*reason = "a repair command was run";

		} else if (strstr(reply_reason, LVMETAD_DISABLE_REASON_DUPLICATES)) {
			*reason = "duplicate PVs were found";

		} else if (strstr(reply_reason, LVMETAD_DISABLE_REASON_VGRESTORE)) {
			*reason = "vgcfgrestore is restoring VG metadata";

		} else {
			*reason = "<unknown>";
		}
	}
out:
	daemon_reply_destroy(reply);
	return ret;
}

