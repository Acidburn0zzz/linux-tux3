/*
 * A daemon to flush dirty data to disk taking consistency into account.
 *
 * Copyright (c) 2008-2014 OGAWA Hirofumi
 */

#if TUX3_FLUSHER == TUX3_FLUSHER_SYNC
#include "tux3.h"

static void __tux3_init_flusher(struct sb *sb)
{
#ifdef __KERNEL__
	/* Disable writeback task to control inode reclaim by dirty flags */
	vfs_sb(sb)->s_bdi = &noop_backing_dev_info;
#endif
}

int tux3_init_flusher(struct sb *sb)
{
	__tux3_init_flusher(sb);
	return 0;
}

void tux3_exit_flusher(struct sb *sb)
{
}

static void schedule_flush_delta(struct sb *sb)
{
	/* Wake up waiters for pending delta staging */
	wake_up_all(&sb->delta_event_wq);
}

static int flush_pending_delta(struct sb *sb)
{
	int err = 0;

	if (!test_bit(TUX3_COMMIT_PENDING_BIT, &sb->backend_state))
		goto out;

	if (test_and_clear_bit(TUX3_COMMIT_PENDING_BIT, &sb->backend_state))
		err = flush_delta(sb);
out:
	return err;
}

/* Try delta transition */
static void try_delta_transition(struct sb *sb)
{
	trace("stage %u, backend_state %lx",
	      sb->staging_delta, sb->backend_state);
	if (!test_and_set_bit(TUX3_COMMIT_RUNNING_BIT, &sb->backend_state))
		delta_transition(sb);
}

/* Do the delta transition until specified delta */
static int try_delta_transition_until_delta(struct sb *sb, unsigned delta)
{
	trace("delta %u, stage %u, backend_state %lx",
	      delta, sb->staging_delta, sb->backend_state);

	/* Already delta transition was started for delta */
	if (delta_after_eq(sb->staging_delta, delta))
		return 1;

	if (!test_and_set_bit(TUX3_COMMIT_RUNNING_BIT, &sb->backend_state)) {
		/* Recheck after grabed TUX3_COMMIT_RUNNING_BIT */
		if (delta_after_eq(sb->staging_delta, delta)) {
			clear_bit(TUX3_COMMIT_RUNNING_BIT, &sb->backend_state);
			return 1;
		}

		delta_transition(sb);
	}

	return delta_after_eq(sb->staging_delta, delta);
}

/* Advance delta transition until specified delta */
static int wait_for_transition(struct sb *sb, unsigned delta)
{
	return wait_event_killable(sb->delta_event_wq,
				   try_delta_transition_until_delta(sb, delta));
}

static int try_flush_pending_until_delta(struct sb *sb, unsigned delta)
{
	trace("delta %u, committed %u, backend_state %lx",
	      delta, sb->committed_delta, sb->backend_state);

	if (!delta_after_eq(sb->committed_delta, delta))
		flush_pending_delta(sb);

	return delta_after_eq(sb->committed_delta, delta);
}

static int wait_for_commit(struct sb *sb, unsigned delta)
{
	return wait_event_killable(sb->delta_event_wq,
				   try_flush_pending_until_delta(sb, delta));
}

static int sync_current_delta(struct sb *sb, enum unify_flags unify_flag)
{
	struct delta_ref *delta_ref;
	unsigned delta;
	int err = 0;

	down_write(&sb->delta_lock);
	/* Get delta that have to write */
	delta_ref = delta_get(sb);
#ifdef UNIFY_DEBUG
	delta_ref->unify_flag = unify_flag;
#endif
	delta = delta_ref->delta;
	delta_put(sb, delta_ref);

	trace("delta %u", delta);

	/* Make sure the delta transition was done for current delta */
	err = wait_for_transition(sb, delta);
	if (err)
		return err;
	assert(delta_after_eq(sb->staging_delta, delta));

	/* Wait until committing the current delta */
	err = wait_for_commit(sb, delta);
	assert(err || delta_after_eq(sb->committed_delta, delta));
	up_write(&sb->delta_lock);
	return err;
}

#else /* TUX3_FLUSHER == TUX3_FLUSHER_ASYNC */

static void try_delta_transition(struct sb *sb)
{
#if 0
	trace("stage %u, backend_state %lx",
	      sb->staging_delta, sb->backend_state);
	sync_inodes_sb(vfs_sb(sb));
#endif
}

/* Do the delta transition until specified delta */
static int try_delta_transition_until_delta(struct sb *sb, unsigned delta)
{
	trace("delta %u, stage %u, backend_state %lx",
	      delta, sb->staging_delta, sb->backend_state);

	/* Already delta transition was started for delta */
	if (delta_after_eq(sb->staging_delta, delta))
		return 1;

	if (!test_and_set_bit(TUX3_COMMIT_RUNNING_BIT, &sb->backend_state)) {
		/* Recheck after grabed TUX3_COMMIT_RUNNING_BIT */
		if (delta_after_eq(sb->staging_delta, delta)) {
			clear_bit(TUX3_COMMIT_RUNNING_BIT, &sb->backend_state);
			return 1;
		}

		delta_transition(sb);
	}

	return delta_after_eq(sb->staging_delta, delta);
}

/* Advance delta transition until specified delta */
static int wait_for_transition(struct sb *sb, unsigned delta)
{
	return wait_event_killable(sb->delta_event_wq,
		try_delta_transition_until_delta(sb, delta));
}

long tux3_writeback(struct super_block *super, struct writeback_control *wbc, long *nr_pages)
{
	struct sb *sb = tux_sb(super);
	struct delta_ref *delta_ref;
	unsigned delta;
	int err;

	/* If we didn't finish replay yet, don't flush. */
	if (!(super->s_flags & MS_ACTIVE))
		return 0;

	/* Get delta that have to write */
	delta_ref = delta_get(sb);
#ifdef UNIFY_DEBUG
	/* NO_UNIFY and FORCE_UNIFY are not supported for now */
	delta_ref->unify_flag = ALLOW_UNIFY;
#endif
	delta = delta_ref->delta;
	delta_put(sb, delta_ref);

	/* Make sure the delta transition was done for current delta */
	err = wait_for_transition(sb, delta);
	if (err)
		return err;
	assert(delta_after_eq(sb->staging_delta, delta));

	/* Wait for last referencer of delta was gone */
	wait_event(sb->delta_event_wq,
		   test_bit(TUX3_COMMIT_PENDING_BIT, &sb->backend_state));

	if (test_bit(TUX3_COMMIT_PENDING_BIT, &sb->backend_state)) {
		clear_bit(TUX3_COMMIT_PENDING_BIT, &sb->backend_state);

		err = flush_delta(sb);
		/* FIXME: error handling */
#if 0
		/* wb_update_bandwidth() is not exported to module */
		wb_update_bandwidth(wb, wb_start);
#endif
	}

	*nr_pages = 0;
	return 1;
}

static int sync_current_delta(struct sb *sb, enum unify_flags unify_flag)
{
	/* FORCE_UNIFY is not supported */
	WARN_ON(unify_flag == FORCE_UNIFY);
	/* This is called only for fsync, so we can take ->s_umount here */
	down_read(&vfs_sb(sb)->s_umount);
	sync_inodes_sb(vfs_sb(sb));
	up_read(&vfs_sb(sb)->s_umount);
	return 0;	/* FIXME: error code */
}

static void schedule_flush_delta(struct sb *sb)
{
	/* Wake up waiters for pending delta staging */
	wake_up_all(&sb->delta_event_wq);
}

#endif /* TUX3_FLUSHER == TUX3_FLUSHER_ASYNC */
