#ifndef _QP_H
#define _QP_H
/*
 *
 * This file is provided under a dual BSD/GPLv2 license.  When using or
 * redistributing this file, you may do so under either license.
 *
 * GPL LICENSE SUMMARY
 *
 * Copyright(c) 2015 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * BSD LICENSE
 *
 * Copyright(c) 2015 Intel Corporation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  - Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  - Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *  - Neither the name of Intel Corporation nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <linux/hash.h>
#include "verbs.h"
#include "sdma.h"

#define QPN_MAX                 BIT(24)
#define QPNMAP_ENTRIES          (QPN_MAX / PAGE_SIZE / BITS_PER_BYTE)

/*
 * QPN-map pages start out as NULL, they get allocated upon
 * first use and are never deallocated. This way,
 * large bitmaps are not allocated unless large numbers of QPs are used.
 */
struct qpn_map {
	void *page;
};

struct hfi1_qpn_table {
	spinlock_t lock; /* protect changes in this struct */
	unsigned flags;         /* flags for QP0/1 allocated for each port */
	u32 last;               /* last QP number allocated */
	u32 nmaps;              /* size of the map table */
	u16 limit;
	u8  incr;
	/* bit map of free QP numbers other than 0/1 */
	struct qpn_map map[QPNMAP_ENTRIES];
};

struct hfi1_qp_ibdev {
	u32 qp_table_size;
	u32 qp_table_bits;
	struct hfi1_qp __rcu **qp_table;
	/* protect qpn table */
	spinlock_t qpt_lock;
	struct hfi1_qpn_table qpn_table;
};

static inline u32 qpn_hash(struct hfi1_qp_ibdev *dev, u32 qpn)
{
	return hash_32(qpn, dev->qp_table_bits);
}

/**
 * hfi1_lookup_qpn - return the QP with the given QPN
 * @ibp: the ibport
 * @qpn: the QP number to look up
 *
 * The caller must hold the rcu_read_lock(), and keep the lock until
 * the returned qp is no longer in use.
 */
static inline struct hfi1_qp *hfi1_lookup_qpn(struct hfi1_ibport *ibp,
					      u32 qpn) __must_hold(RCU)
{
	struct hfi1_qp *qp = NULL;

	if (unlikely(qpn <= 1)) {
		qp = rcu_dereference(ibp->qp[qpn]);
	} else {
		struct hfi1_ibdev *dev = &ppd_from_ibp(ibp)->dd->verbs_dev;
		u32 n = qpn_hash(dev->qp_dev, qpn);

		for (qp = rcu_dereference(dev->qp_dev->qp_table[n]); qp;
			qp = rcu_dereference(qp->next))
			if (qp->ibqp.qp_num == qpn)
				break;
	}
	return qp;
}

/**
 * clear_ahg - reset ahg status in qp
 * @qp - qp pointer
 */
static inline void clear_ahg(struct hfi1_qp *qp)
{
	qp->s_hdr->ahgcount = 0;
	qp->s_flags &= ~(HFI1_S_AHG_VALID | HFI1_S_AHG_CLEAR);
	if (qp->s_sde && qp->s_ahgidx >= 0)
		sdma_ahg_free(qp->s_sde, qp->s_ahgidx);
	qp->s_ahgidx = -1;
}

/**
 * hfi1_error_qp - put a QP into the error state
 * @qp: the QP to put into the error state
 * @err: the receive completion error to signal if a RWQE is active
 *
 * Flushes both send and receive work queues.
 * Returns true if last WQE event should be generated.
 * The QP r_lock and s_lock should be held and interrupts disabled.
 * If we are already in error state, just return.
 */
int hfi1_error_qp(struct hfi1_qp *qp, enum ib_wc_status err);

/**
 * hfi1_modify_qp - modify the attributes of a queue pair
 * @ibqp: the queue pair who's attributes we're modifying
 * @attr: the new attributes
 * @attr_mask: the mask of attributes to modify
 * @udata: user data for libibverbs.so
 *
 * Returns 0 on success, otherwise returns an errno.
 */
int hfi1_modify_qp(struct ib_qp *ibqp, struct ib_qp_attr *attr,
		   int attr_mask, struct ib_udata *udata);

int hfi1_query_qp(struct ib_qp *ibqp, struct ib_qp_attr *attr,
		  int attr_mask, struct ib_qp_init_attr *init_attr);

/**
 * hfi1_compute_aeth - compute the AETH (syndrome + MSN)
 * @qp: the queue pair to compute the AETH for
 *
 * Returns the AETH.
 */
__be32 hfi1_compute_aeth(struct hfi1_qp *qp);

/**
 * hfi1_create_qp - create a queue pair for a device
 * @ibpd: the protection domain who's device we create the queue pair for
 * @init_attr: the attributes of the queue pair
 * @udata: user data for libibverbs.so
 *
 * Returns the queue pair on success, otherwise returns an errno.
 *
 * Called by the ib_create_qp() core verbs function.
 */
struct ib_qp *hfi1_create_qp(struct ib_pd *ibpd,
			     struct ib_qp_init_attr *init_attr,
			     struct ib_udata *udata);
/**
 * hfi1_destroy_qp - destroy a queue pair
 * @ibqp: the queue pair to destroy
 *
 * Returns 0 on success.
 *
 * Note that this can be called while the QP is actively sending or
 * receiving!
 */
int hfi1_destroy_qp(struct ib_qp *ibqp);

/**
 * hfi1_get_credit - flush the send work queue of a QP
 * @qp: the qp who's send work queue to flush
 * @aeth: the Acknowledge Extended Transport Header
 *
 * The QP s_lock should be held.
 */
void hfi1_get_credit(struct hfi1_qp *qp, u32 aeth);

/**
 * hfi1_qp_init - allocate QP tables
 * @dev: a pointer to the hfi1_ibdev
 */
int hfi1_qp_init(struct hfi1_ibdev *dev);

/**
 * hfi1_qp_exit - free the QP related structures
 * @dev: a pointer to the hfi1_ibdev
 */
void hfi1_qp_exit(struct hfi1_ibdev *dev);

/**
 * hfi1_qp_wakeup - wake up on the indicated event
 * @qp: the QP
 * @flag: flag the qp on which the qp is stalled
 */
void hfi1_qp_wakeup(struct hfi1_qp *qp, u32 flag);

struct sdma_engine *qp_to_sdma_engine(struct hfi1_qp *qp, u8 sc5);
struct send_context *qp_to_send_context(struct hfi1_qp *qp, u8 sc5);

struct qp_iter;

/**
 * qp_iter_init - initialize the iterator for the qp hash list
 * @dev: the hfi1_ibdev
 */
struct qp_iter *qp_iter_init(struct hfi1_ibdev *dev);

/**
 * qp_iter_next - Find the next qp in the hash list
 * @iter: the iterator for the qp hash list
 */
int qp_iter_next(struct qp_iter *iter);

/**
 * qp_iter_print - print the qp information to seq_file
 * @s: the seq_file to emit the qp information on
 * @iter: the iterator for the qp hash list
 */
void qp_iter_print(struct seq_file *s, struct qp_iter *iter);

/**
 * qp_comm_est - handle trap with QP established
 * @qp: the QP
 */
void qp_comm_est(struct hfi1_qp *qp);

/**
 * _hfi1_schedule_send - schedule progress
 * @qp: the QP
 *
 * This schedules qp progress w/o regard to the s_flags.
 *
 * It is only used in the post send, which doesn't hold
 * the s_lock.
 */
static inline void _hfi1_schedule_send(struct hfi1_qp *qp)
{
	struct hfi1_ibport *ibp =
		to_iport(qp->ibqp.device, qp->port_num);
	struct hfi1_pportdata *ppd = ppd_from_ibp(ibp);
	struct hfi1_devdata *dd = dd_from_ibdev(qp->ibqp.device);

	iowait_schedule(&qp->s_iowait, ppd->hfi1_wq,
			qp->s_sde ?
			qp->s_sde->cpu :
			cpumask_first(cpumask_of_node(dd->node)));
}

/**
 * hfi1_schedule_send - schedule progress
 * @qp: the QP
 *
 * This schedules qp progress and caller should hold
 * the s_lock.
 */
static inline void hfi1_schedule_send(struct hfi1_qp *qp)
{
	if (hfi1_send_ok(qp))
		_hfi1_schedule_send(qp);
}

void hfi1_migrate_qp(struct hfi1_qp *qp);

/**
 * qp_get_savail - return number of avail send entries
 *
 * @qp - the qp
 *
 * This assumes the s_hlock is held but the s_last
 * is uncontrolled.
 */
static inline u32 qp_get_savail(struct hfi1_qp *qp)
{
	u32 slast;
	u32 ret;

	smp_read_barrier_depends(); /* see rc.c */
	slast = ACCESS_ONCE(qp->s_last);
	if (qp->s_head >= slast)
		ret = qp->s_size - (qp->s_head - slast);
	else
		ret = slast - qp->s_head;
	return ret - 1;
}

/**
 * add_retry_timer - add/start a retry timer
 * @qp - the QP
 *
 * add an retry timer on the QP
 */
static inline void add_retry_timer(struct hfi1_qp *qp)
{
	qp->s_flags |= HFI1_S_TIMER;
	/* 4.096 usec. * (1 << qp->timeout) */
	qp->s_timer.expires = jiffies + qp->timeout_jiffies;
	add_timer(&qp->s_timer);
}

/**
 * add_rnr_timer - add/start a retry timer
 * @qp - the QP
 *
 * add an rnr timer on the QP
 */
static inline void add_rnr_timer(struct hfi1_qp *qp, unsigned long to)
{
	qp->s_flags |= HFI1_S_WAIT_RNR;
	qp->s_rnr_timer.expires = jiffies + usecs_to_jiffies(to);
	add_timer(&qp->s_rnr_timer);
}

/**
 * mod_retry_timer - mod a retry timer
 * @qp - the QP
 *
 * Modify a potentially already running retry
 * timer
 */
static inline void mod_retry_timer(struct hfi1_qp *qp)
{
	qp->s_flags |= HFI1_S_TIMER;
	qp->s_timer.function = hfi1_rc_timeout;
	/* 4.096 usec. * (1 << qp->timeout) */
	mod_timer(&qp->s_timer, jiffies + qp->timeout_jiffies);
}

/**
 * stop_retry_timer - mod a retry timer
 * @qp - the QP
 *
 * stop a retry timer and return if the timer
 * had been pending.
 */
static inline int stop_retry_timer(struct hfi1_qp *qp)
{
	int rval = 0;

	/* Remove QP from retry */
	if (qp->s_flags & HFI1_S_TIMER) {
		qp->s_flags &= ~HFI1_S_TIMER;
		rval = del_timer(&qp->s_timer);
	}
	return rval;
}

/**
 * stop_rnr_timer - stop an rnr timer
 * @qp - the QP
 *
 * stop an rnr timer and return if the timer
 * had been pending.
 */
static inline int stop_rnr_timer(struct hfi1_qp *qp)
{
	int rval = 0;

	/* Remove QP from rnr timer */
	if (qp->s_flags & HFI1_S_WAIT_RNR) {
		qp->s_flags &= ~HFI1_S_WAIT_RNR;
		rval = del_timer(&qp->s_rnr_timer);
	}
	return rval;
}

/**
 * stop_rc_timers - mod a retry timer
 * @qp - the QP
 *
 * stop any pending timers
 */
static inline void stop_rc_timers(struct hfi1_qp *qp)
{
	/* Remove QP from all timers */
	if (qp->s_flags & (HFI1_S_TIMER | HFI1_S_WAIT_RNR)) {
		qp->s_flags &= ~(HFI1_S_TIMER | HFI1_S_WAIT_RNR);
		del_timer(&qp->s_timer);
		del_timer(&qp->s_rnr_timer);
	}
}

/**
 * del_timers_sync - wait for any timeout routines to exit
 * @qp - the QP
 */
static inline void del_timers_sync(struct hfi1_qp *qp)
{
	del_timer_sync(&qp->s_timer);
	del_timer_sync(&qp->s_rnr_timer);
}

/**
 * hfi1_error_port_qps - put a port's RC/UC qps into error state
 * @ibp: the ibport.
 * @sl: the service level.
 *
 * This function places all RC/UC qps with a given service level into error
 * state. It is generally called to force upper lay apps to abandon stale qps
 * after an sl->sc mapping change.
 */
void hfi1_error_port_qps(struct hfi1_ibport *ibp, u8 sl);

#endif /* _QP_H */
