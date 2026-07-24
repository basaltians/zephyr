/*
 * Copyright (c) 2026 Basalte
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/logging/log.h>
#include <zephyr/net/rtp_stream.h>
#include <zephyr/net/rtp.h>

LOG_MODULE_REGISTER(rtp_stream, CONFIG_RTP_STREAM_LOG_LEVEL);

#define LOCK_TIMEOUT_CB  K_MSEC(1)
#define LOCK_TIMEOUT_API K_MSEC(50)

static inline bool role_is_source(enum rtp_stream_role role)
{
	return role == RTP_STREAM_ROLE_SOURCE || role == RTP_STREAM_ROLE_BOTH;
}

static inline bool role_is_sink(enum rtp_stream_role role)
{
	return role == RTP_STREAM_ROLE_SINK || role == RTP_STREAM_ROLE_BOTH;
}

static int rtp_stream_codec_encode(struct rtp_stream *stream, void *data, size_t size,
				   size_t *encoded_size, uint32_t *delta_ts, void *user_data)
{
	struct rtp_stream_codec_api *api = stream->stream_config.codec_api;

	if (api->encode == NULL) {
		/* No transformation when no encode function given */
		*encoded_size = size;
		*delta_ts = size;

		return 0;
	}

	return api->encode(data, size, encoded_size, delta_ts, user_data);
}
static int rtp_stream_codec_decode(struct rtp_stream *stream, void *data, size_t size,
				   size_t *decoded_size, void *user_data)
{
	struct rtp_stream_codec_api *api = stream->stream_config.codec_api;

	if (api->decode == NULL) {
		/* No transformation when no decode function given */
		*decoded_size = size;

		return 0;
	}

	return api->decode(data, size, decoded_size, user_data);
}

static void rtp_stream_rtp_callback(struct rtp_session *session, struct rtp_packet *packet,
				    void *user_data)
{
	__ASSERT_NO_MSG(user_data != NULL);
	struct rtp_stream *stream = (struct rtp_stream *)user_data;
	struct rtp_stream_config *cfg;
	struct rtp_msg msg = {};
	void *mem_block;
	int ret;

	ret = k_mutex_lock(&stream->lock, LOCK_TIMEOUT_CB);
	if (ret < 0) {
		LOG_DBG("Failed to take lock (%d)", ret);
		return;
	}
	cfg = &stream->stream_config;

	if (!role_is_sink(stream->role)) {
		LOG_DBG("Invalid role in rtp callback");
		goto unlock;
	}

	if (stream->state != RTP_STREAM_STATE_RUNNING) {
		LOG_DBG("Invalid state in rtp callback (%d)", stream->state);
		goto unlock;
	}

	LOG_DBG_RATELIMIT("RX memslab (used/total): (%d/%d)", cfg->mem_slab->info.num_used,
			  cfg->mem_slab->info.num_blocks);

	if (packet->payload_len > cfg->block_size) {
		LOG_DBG("RTP payload too large (%d > %d)", packet->payload_len, cfg->block_size);
		goto unlock;
	}

	ret = k_mem_slab_alloc(cfg->mem_slab, &mem_block, K_NO_WAIT);
	if (ret < 0) {
		LOG_ERR("Failed to alloc memslab (%d)", ret);
		goto error;
	}

	memcpy(mem_block, packet->payload, packet->payload_len);

	msg.data = mem_block;
	msg.data_len = packet->payload_len;
	msg.seq = packet->header.seq;
	msg.ts = packet->header.ts;

	ret = k_msgq_put(&stream->rx_out_msgq, &msg, K_NO_WAIT);
	if (ret < 0) {
		LOG_ERR("Failed to put to rx_out_msgq (%d)", ret);
		k_mem_slab_free(cfg->mem_slab, mem_block);
		goto error;
	}

	goto unlock;

error:
	stream->state = RTP_STREAM_STATE_ERROR;

unlock:
	(void)k_mutex_unlock(&stream->lock);
}

int rtp_stream_init(struct rtp_stream *stream)
{
	if (stream == NULL) {
		return -EINVAL;
	}

	k_msgq_init(&stream->rx_out_msgq, (char *)stream->rx_out_msgs, sizeof(struct rtp_msg),
		    CONFIG_RTP_STREAM_RX_MSGQ_SIZE);

	return k_mutex_init(&stream->lock);
}

int rtp_stream_configure(struct rtp_stream *stream, enum rtp_stream_role role,
			 struct rtp_stream_config *config)
{
	int ret;

	if (stream == NULL || config == NULL) {
		return -EINVAL;
	}

	if (config->sock_addr == NULL) {
		return -EINVAL;
	}

	if (config->codec_api == NULL) {
		return -EINVAL;
	}

	if (config->mem_slab != NULL && config->block_size > config->mem_slab->info.block_size) {
		LOG_ERR("config->block_size is larger then actual mem slab block size (%zu > %zu)",
			config->block_size, config->mem_slab->info.block_size);
		return -EINVAL;
	}

	if (role_is_sink(role) && config->mem_slab == NULL) {
		LOG_ERR("Sink role requires a mem_slab");
		return -EINVAL;
	}

	ret = k_mutex_lock(&stream->lock, LOCK_TIMEOUT_API);
	if (ret < 0) {
		LOG_ERR("Failed to take lock (%d)", ret);
		return ret;
	}

	if (stream->state != RTP_STREAM_STATE_READY &&
	    stream->state != RTP_STREAM_STATE_NOT_READY) {
		LOG_ERR("Invalid state to configure (%d)", stream->state);
		ret = -EINVAL;
		goto unlock;
	}

	switch (role) {
	case RTP_STREAM_ROLE_SOURCE:
		ret = rtp_session_init_tx(stream->rtp_session, config->iface, config->sock_addr,
					  config->payload_type, config->transport_type);
		if (ret < 0) {
			LOG_DBG("Failed to init rtp tx (%d)", ret);
			goto unlock;
		}

		break;
	case RTP_STREAM_ROLE_SINK:
		ret = rtp_session_init_rx(stream->rtp_session, config->iface, config->sock_addr,
					  rtp_stream_rtp_callback, (void *)stream,
					  config->transport_type);
		if (ret < 0) {
			LOG_DBG("Failed to init rtp rx (%d)", ret);
			goto unlock;
		}

		break;
	case RTP_STREAM_ROLE_BOTH:
		ret = rtp_session_init(stream->rtp_session, config->iface, config->sock_addr,
				       RTP_ROLE_BOTH, config->payload_type, rtp_stream_rtp_callback,
				       (void *)stream, config->transport_type);
		if (ret < 0) {
			LOG_DBG("Failed to init rtp tx and rx (%d)", ret);
			goto unlock;
		}

		break;
	default:
		ret = -EINVAL;
		goto unlock;
	}

	memcpy(&stream->stream_config, config, sizeof(stream->stream_config));

	stream->state = RTP_STREAM_STATE_READY;
	stream->role = role;

	ret = 0;

unlock:
	(void)k_mutex_unlock(&stream->lock);

	return ret;
}

int rtp_stream_trigger(struct rtp_stream *stream, enum rtp_stream_command cmd)
{
	struct rtp_msg msg;
	int ret;

	if (stream == NULL) {
		return -EINVAL;
	}

	ret = k_mutex_lock(&stream->lock, LOCK_TIMEOUT_API);
	if (ret < 0) {
		LOG_ERR("Failed to take lock (%d)", ret);
		return ret;
	}

	switch (cmd) {
	case RTP_STREAM_START:
		if (stream->state != RTP_STREAM_STATE_READY) {
			LOG_ERR("Cannot start, invalid state (%d)", stream->state);
			ret = -EINVAL;
			goto unlock;
		}

		ret = rtp_session_start(stream->rtp_session);
		if (ret < 0) {
			LOG_DBG("Failed to start rtp session (%d)", ret);
			goto unlock;
		}

		stream->state = RTP_STREAM_STATE_RUNNING;
		break;
	case RTP_STREAM_STOP:
		if (stream->state != RTP_STREAM_STATE_RUNNING) {
			LOG_ERR("Cannot stop, invalid state (%d)", stream->state);
			ret = -EINVAL;
			goto unlock;
		}

		ret = rtp_session_stop(stream->rtp_session);
		if (ret < 0) {
			LOG_DBG("Failed to stop rtp session (%d)", ret);
			goto unlock;
		}

		stream->state = RTP_STREAM_STATE_READY;
		break;
	case RTP_STREAM_DROP:
		if (stream->state == RTP_STREAM_STATE_NOT_READY) {
			LOG_ERR("Cannot drop, invalid state (%d)", stream->state);
			ret = -EINVAL;
			goto unlock;
		}

		ret = rtp_session_stop(stream->rtp_session);

		while (k_msgq_get(&stream->rx_out_msgq, &msg, K_NO_WAIT) == 0) {
			k_mem_slab_free(stream->stream_config.mem_slab, msg.data);
		}

		if (ret < 0) {
			LOG_DBG("Failed to stop rtp session (%d)", ret);
			goto unlock;
		}

		stream->state = RTP_STREAM_STATE_READY;
		break;
	case RTP_STREAM_PREPARE:
		if (stream->state == RTP_STREAM_STATE_RUNNING) {
			LOG_ERR("Cannot prepare, running");
			ret = -EINVAL;
			goto unlock;
		} else if (stream->state != RTP_STREAM_STATE_ERROR) {
			/* Ignore */
			ret = 0;
			goto unlock;
		}

		ret = rtp_session_stop(stream->rtp_session);

		while (k_msgq_get(&stream->rx_out_msgq, &msg, K_NO_WAIT) == 0) {
			k_mem_slab_free(stream->stream_config.mem_slab, msg.data);
		}

		if (ret < 0) {
			LOG_DBG("Failed to stop rtp session (%d)", ret);
			goto unlock;
		}

		stream->state = RTP_STREAM_STATE_READY;
		break;
	default:
		ret = -EINVAL;
		goto unlock;
	}

	ret = 0;

unlock:
	(void)k_mutex_unlock(&stream->lock);

	return ret;
}

int rtp_stream_read(struct rtp_stream *stream, struct rtp_msg *msg, k_timeout_t timeout)
{
	struct rtp_stream_config *cfg;
	size_t decoded_size;
	int status, ret;

	if (stream == NULL || msg == NULL) {
		return -EINVAL;
	}
	cfg = &stream->stream_config;

	ret = k_mutex_lock(&stream->lock, LOCK_TIMEOUT_API);
	if (ret < 0) {
		LOG_ERR("Failed to take lock (%d)", ret);
		return ret;
	}

	if (!role_is_sink(stream->role)) {
		LOG_ERR("Stream is no sink");
		(void)k_mutex_unlock(&stream->lock);
		return -EINVAL;
	}

	(void)k_mutex_unlock(&stream->lock);

	status = k_msgq_get(&stream->rx_out_msgq, msg, timeout);

	ret = k_mutex_lock(&stream->lock, LOCK_TIMEOUT_API);
	if (ret < 0) {
		LOG_ERR("Failed to take lock (%d)", ret);
		if (status == 0) {
			k_mem_slab_free(cfg->mem_slab, msg->data);
		}
		return ret;
	}

	if (status < 0) {
		if (stream->state == RTP_STREAM_STATE_ERROR) {
			ret = -EIO;
		} else {
			LOG_DBG("need retry");
			ret = -EAGAIN;
		}

		goto unlock;
	}

	ret = rtp_stream_codec_decode(stream, msg->data, msg->data_len, &decoded_size,
				      cfg->user_data);
	if (ret < 0) {
		LOG_ERR("Failed to decode payload (%d)", ret);

		k_mem_slab_free(cfg->mem_slab, msg->data);
		msg->data = NULL;
		msg->data_len = 0;

		goto unlock;
	}
	msg->data_len = decoded_size;

unlock:
	(void)k_mutex_unlock(&stream->lock);

	return ret;
}

int rtp_stream_write(struct rtp_stream *stream, void *data, size_t size, uint32_t *timestamp)
{
	struct rtp_stream_config *cfg;
	uint32_t delta_ts;
	size_t encoded_size;
	int ret;

	if (stream == NULL) {
		return -EINVAL;
	}

	ret = k_mutex_lock(&stream->lock, LOCK_TIMEOUT_API);
	if (ret < 0) {
		LOG_ERR("Failed to take lock (%d)", ret);
		return ret;
	}

	cfg = &stream->stream_config;

	if (!role_is_source(stream->role)) {
		LOG_ERR("Stream is no source");
		ret = -EINVAL;
		goto unlock;
	}

	if (stream->state != RTP_STREAM_STATE_READY && stream->state != RTP_STREAM_STATE_RUNNING) {
		LOG_ERR("Invalid state (%d)", stream->state);
		ret = -EINVAL;
		goto unlock;
	}

	ret = rtp_stream_codec_encode(stream, data, size, &encoded_size, &delta_ts, cfg->user_data);
	if (ret < 0) {
		LOG_ERR("Failed to encode data (%d)", ret);
		goto unlock;
	}

	ret = rtp_session_send(stream->rtp_session, data, encoded_size, delta_ts, 0, 0, NULL,
			       timestamp);
	if (ret < 0) {
		LOG_DBG("Failed to send rtp session (%d)", ret);
		goto unlock;
	}

	ret = 0;

unlock:
	(void)k_mutex_unlock(&stream->lock);

	return ret;
}
