/*
 *  SPDX-FileCopyrightText: 2026 Basalte bv
 *
 *  SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/i2s.h>
#include "i2s_rtio.h"
#include <zephyr/sys/mpsc_lockfree.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(i2s_rtio, CONFIG_I2S_LOG_LEVEL);

const struct rtio_iodev_api i2s_iodev_api = {
	.submit = i2s_iodev_submit,
};

static inline k_spinlock_key_t i2s_spin_lock(struct i2s_rtio *ctx)
{
	return k_spin_lock(&ctx->slock);
}

static inline void i2s_spin_unlock(struct i2s_rtio *ctx, k_spinlock_key_t key)
{
	k_spin_unlock(&ctx->slock, key);
}

static bool i2s_rtio_next(struct i2s_rtio *ctx, bool completion)
{
	k_spinlock_key_t key = i2s_spin_lock(ctx);
	bool has_next;

	if (!completion && ctx->txn_curr != NULL) {
		i2s_spin_unlock(ctx, key);
		return false;
	}

	struct mpsc_node *next = mpsc_pop(&ctx->io_q);

	if (next != NULL) {
		struct rtio_iodev_sqe *next_sqe = CONTAINER_OF(next, struct rtio_iodev_sqe, q);

		ctx->txn_head = next_sqe;
		ctx->txn_curr = next_sqe;
	} else {
		ctx->txn_head = NULL;
		ctx->txn_curr = NULL;
	}

	/* Read under the lock, another context may take ownership right after unlocking */
	has_next = ctx->txn_curr != NULL;

	i2s_spin_unlock(ctx, key);

	return has_next;
}

void i2s_rtio_init(struct i2s_rtio *ctx, const struct device *dev)
{
	mpsc_init(&ctx->io_q);
	ctx->txn_head = NULL;
	ctx->txn_curr = NULL;
	ctx->i2s_dev = dev;
	ctx->iodev.data = &ctx->i2s_dev;
	ctx->iodev.api = &i2s_iodev_api;
	k_sem_init(&ctx->lock, 1, 1);
}

bool i2s_rtio_submit(struct i2s_rtio *ctx, struct rtio_iodev_sqe *iodev_sqe)
{
	mpsc_push(&ctx->io_q, &iodev_sqe->q);
	return i2s_rtio_next(ctx, false);
}

bool i2s_rtio_advance(struct i2s_rtio *ctx)
{
	return i2s_rtio_next(ctx, true);
}

bool i2s_rtio_complete(struct i2s_rtio *ctx, int status)
{
	struct rtio_iodev_sqe *txn_head = ctx->txn_head;
	bool result;

	result = i2s_rtio_next(ctx, true);

	if (status < 0) {
		rtio_iodev_sqe_err(txn_head, status);
	} else {
		rtio_iodev_sqe_ok(txn_head, status);
	}

	return result;
}

int i2s_rtio_set_config(struct rtio *ctx, const struct rtio_iodev *iodev, enum i2s_dir dir,
			struct i2s_config *cfg)
{
	struct rtio_sqe *sqe;
	struct rtio_cqe *cqe;
	int ret;

	sqe = rtio_sqe_acquire(ctx);
	if (sqe == NULL) {
		LOG_ERR("Failed to acquire sqe to configure I2S");
		return -ENOMEM;
	}

	rtio_sqe_prep_i2s_configure(sqe, iodev, RTIO_PRIO_NORM, dir, (void *)cfg, NULL);
	rtio_submit(ctx, 1);
	cqe = rtio_cqe_consume(ctx);
	ret = cqe->result;
	rtio_cqe_release(ctx, cqe);

	return ret;
}

int i2s_rtio_trigger(struct rtio *ctx, const struct rtio_iodev *iodev, enum i2s_dir dir,
		     enum i2s_trigger_cmd cmd)
{
	struct rtio_sqe *sqe;
	struct rtio_cqe *cqe;
	int ret;

	sqe = rtio_sqe_acquire(ctx);
	if (sqe == NULL) {
		LOG_ERR("Failed to acquire sqe to trigger I2S");
		return -ENOMEM;
	}

	rtio_sqe_prep_i2s_trigger(sqe, iodev, RTIO_PRIO_NORM, dir, cmd, NULL);
	rtio_submit(ctx, 1);
	cqe = rtio_cqe_consume(ctx);
	ret = cqe->result;
	rtio_cqe_release(ctx, cqe);

	return ret;
}
