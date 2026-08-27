/*
 * Copyright (c) 2026 Basalte
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/net/rtp_stream.h>
#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/rtio/rtio.h>
#include <zephyr/ztest.h>

#include <string.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(test);

#define LOCALHOST_ADDR "127.0.0.1"

#define SOURCE_SINK_PORT  5004
#define SOURCE_ONLY_PORT  5006
#define DECODE_CODEC_PORT 5008

#define PAYLOAD_TYPE 97
#define PAYLOAD_SIZE 32

#define RTIO_SQ_SZ    8
#define RTIO_CQ_SZ    8
#define RTIO_N_BLOCKS 8

#define CQE_TIMEOUT_MS       200
#define CQE_TIMEOUT_SHORT_MS 20

RTP_STREAM_DEFINE_STATIC(source_stream);
RTP_STREAM_IODEV_DEFINE(sink_stream);
RTP_STREAM_IODEV_DEFINE(source_only_stream);
RTP_STREAM_DEFINE_STATIC(decode_codec_stream);

RTIO_DEFINE_WITH_MEMPOOL(rtio_ctx, RTIO_SQ_SZ, RTIO_CQ_SZ, RTIO_N_BLOCKS, PAYLOAD_SIZE, 4);

/* NULL encode/decode: payload bytes pass through unchanged */
static struct rtp_stream_codec_api passthrough_codec_api;

static int dummy_decode(void *data, size_t size, size_t *decoded_size, void *user_data)
{
	ARG_UNUSED(data);
	ARG_UNUSED(user_data);

	*decoded_size = size;
	return 0;
}

static struct rtp_stream_codec_api decode_codec_api = {
	.decode = dummy_decode,
};

static struct net_if *lo_iface;

static void build_sockaddr(struct net_sockaddr_in *addr, uint16_t port)
{
	struct net_in_addr in_addr;

	zassert_ok(net_addr_pton(NET_AF_INET, LOCALHOST_ADDR, &in_addr));

	addr->sin_family = NET_AF_INET;
	addr->sin_addr = in_addr;
	addr->sin_port = net_htons(port);
}

/* Bounded wait for a CQE: unlike the sample's rtio_cqe_consume_block(), tests must not be able
 * to hang forever if a completion never arrives.
 */
static struct rtio_cqe *cqe_consume_timeout(struct rtio *r, int timeout_ms)
{
	int64_t deadline = k_uptime_get() + timeout_ms;
	struct rtio_cqe *cqe;

	do {
		cqe = rtio_cqe_consume(r);
		if (cqe != NULL) {
			return cqe;
		}
		k_msleep(1);
	} while (k_uptime_get() < deadline);

	return NULL;
}

/* rtp_stream_read() is compiled out under CONFIG_RTP_STREAM_RTIO */
ZTEST(rtp_stream_rtio_tests, test_read_disabled)
{
	struct rtp_msg msg;

	zassert_equal(rtp_stream_read(&sink_stream, &msg, K_NO_WAIT), -ENOSYS);
}

/* A decode codec would run on the RTP transport's shared RX dispatch context, so it's
 * rejected for sink/both roles; source is unaffected since it only ever needs encode.
 */
ZTEST(rtp_stream_rtio_tests, test_configure_rejects_decode_codec)
{
	struct net_sockaddr_in addr;
	struct rtp_stream_config config = {0};

	build_sockaddr(&addr, DECODE_CODEC_PORT);

	config.block_size = PAYLOAD_SIZE;
	config.iface = lo_iface;
	config.sock_addr = (struct net_sockaddr *)&addr;
	config.codec_api = &decode_codec_api;
#ifdef CONFIG_RTP_TRANSPORT_NET_PKT
	config.transport_type = RTP_TRANSPORT_NET_PKT;
#else
	config.transport_type = RTP_TRANSPORT_SOCKET;
#endif

	zassert_equal(rtp_stream_configure(&decode_codec_stream, RTP_STREAM_ROLE_SINK, &config),
		      -EINVAL);
	zassert_equal(rtp_stream_configure(&decode_codec_stream, RTP_STREAM_ROLE_BOTH, &config),
		      -EINVAL);

	config.payload_type = PAYLOAD_TYPE;
	zassert_ok(rtp_stream_configure(&decode_codec_stream, RTP_STREAM_ROLE_SOURCE, &config));
}

/* Only RTIO_OP_RX is supported on the iodev */
ZTEST(rtp_stream_rtio_tests, test_unsupported_op_rejected)
{
	struct rtio_sqe *sqe;
	struct rtio_cqe *cqe;
	uint8_t buf[PAYLOAD_SIZE] = {0};

	sqe = rtio_sqe_acquire(&rtio_ctx);
	zassert_not_null(sqe);
	rtio_sqe_prep_write(sqe, &sink_stream_iodev, RTIO_PRIO_NORM, buf, sizeof(buf), NULL);
	rtio_submit(&rtio_ctx, 0);

	cqe = cqe_consume_timeout(&rtio_ctx, CQE_TIMEOUT_MS);
	zassert_not_null(cqe);
	zassert_equal(cqe->result, -EINVAL);
	rtio_cqe_release(&rtio_ctx, cqe);
}

/* RTIO_OP_RX against a source-only (non-sink) stream is rejected */
ZTEST(rtp_stream_rtio_tests, test_rx_wrong_role_rejected)
{
	struct rtio_sqe *sqe;
	struct rtio_cqe *cqe;
	uint8_t buf[PAYLOAD_SIZE];

	sqe = rtio_sqe_acquire(&rtio_ctx);
	zassert_not_null(sqe);
	rtio_sqe_prep_read(sqe, &source_only_stream_iodev, RTIO_PRIO_NORM, buf, sizeof(buf), NULL);
	rtio_submit(&rtio_ctx, 0);

	cqe = cqe_consume_timeout(&rtio_ctx, CQE_TIMEOUT_MS);
	zassert_not_null(cqe);
	zassert_equal(cqe->result, -EINVAL);
	rtio_cqe_release(&rtio_ctx, cqe);
}

/* RTIO_OP_RX with a caller-supplied buffer, submitted before the packet arrives */
ZTEST(rtp_stream_rtio_tests, test_rx_fixed_buffer)
{
	struct rtio_sqe *sqe;
	struct rtio_cqe *cqe;
	uint8_t rx_buf[PAYLOAD_SIZE] = {0};
	uint8_t tx_buf[PAYLOAD_SIZE];
	uint32_t tx_ts;

	zassert_ok(rtp_stream_trigger(&sink_stream, RTP_STREAM_START));
	zassert_ok(rtp_stream_trigger(&source_stream, RTP_STREAM_START));

	sqe = rtio_sqe_acquire(&rtio_ctx);
	zassert_not_null(sqe);
	rtio_sqe_prep_read(sqe, &sink_stream_iodev, RTIO_PRIO_NORM, rx_buf, sizeof(rx_buf), NULL);
	rtio_submit(&rtio_ctx, 0);

	memset(tx_buf, 0xAB, sizeof(tx_buf));
	zassert_ok(rtp_stream_write(&source_stream, tx_buf, sizeof(tx_buf), &tx_ts));

	cqe = cqe_consume_timeout(&rtio_ctx, CQE_TIMEOUT_MS);
	zassert_not_null(cqe);
	zassert_equal(cqe->result, PAYLOAD_SIZE);
	zassert_mem_equal(rx_buf, tx_buf, sizeof(tx_buf));
	rtio_cqe_release(&rtio_ctx, cqe);
}

/* RTIO_OP_RX with mempool-backed multishot reads keeps refilling across packets */
ZTEST(rtp_stream_rtio_tests, test_rx_multishot)
{
	struct rtio_sqe *sqes[3];
	struct rtio_cqe *cqe;
	uint8_t tx_buf[PAYLOAD_SIZE];
	uint8_t *buf;
	uint32_t buf_len;

	zassert_ok(rtp_stream_trigger(&sink_stream, RTP_STREAM_START));
	zassert_ok(rtp_stream_trigger(&source_stream, RTP_STREAM_START));

	ARRAY_FOR_EACH(sqes, i) {
		sqes[i] = rtio_sqe_acquire(&rtio_ctx);
		zassert_not_null(sqes[i]);
		rtio_sqe_prep_read_multishot(sqes[i], &sink_stream_iodev, RTIO_PRIO_NORM, NULL);
	}
	rtio_submit(&rtio_ctx, 0);

	for (size_t i = 0; i < 3 * ARRAY_SIZE(sqes); i++) {
		memset(tx_buf, (uint8_t)(0x10 + i), sizeof(tx_buf));
		zassert_ok(rtp_stream_write(&source_stream, tx_buf, sizeof(tx_buf), NULL));

		cqe = cqe_consume_timeout(&rtio_ctx, CQE_TIMEOUT_MS);
		zassert_not_null(cqe);
		zassert_equal(cqe->result, PAYLOAD_SIZE);

		zassert_ok(rtio_cqe_get_mempool_buffer(&rtio_ctx, cqe, &buf, &buf_len));
		zassert_not_null(buf);
		zassert_mem_equal(buf, tx_buf, sizeof(tx_buf));

		rtio_release_buffer(&rtio_ctx, buf, buf_len);
		rtio_cqe_release(&rtio_ctx, cqe);
	}
}

/* A packet that finds the mempool empty is dropped, the multishot read stays pending */
ZTEST(rtp_stream_rtio_tests, test_rx_mempool_empty_keeps_read)
{
	struct rtio_sqe *sqe;
	struct rtio_cqe *cqe;
	uint8_t tx_buf[PAYLOAD_SIZE];
	uint8_t *held[RTIO_N_BLOCKS];
	uint32_t held_len[RTIO_N_BLOCKS];

	zassert_ok(rtp_stream_trigger(&sink_stream, RTP_STREAM_START));
	zassert_ok(rtp_stream_trigger(&source_stream, RTP_STREAM_START));

	sqe = rtio_sqe_acquire(&rtio_ctx);
	zassert_not_null(sqe);
	rtio_sqe_prep_read_multishot(sqe, &sink_stream_iodev, RTIO_PRIO_NORM, NULL);
	rtio_submit(&rtio_ctx, 0);

	ARRAY_FOR_EACH(held, i) {
		memset(tx_buf, (uint8_t)i, sizeof(tx_buf));
		zassert_ok(rtp_stream_write(&source_stream, tx_buf, sizeof(tx_buf), NULL));

		cqe = cqe_consume_timeout(&rtio_ctx, CQE_TIMEOUT_MS);
		zassert_not_null(cqe);
		zassert_equal(cqe->result, PAYLOAD_SIZE);
		zassert_ok(rtio_cqe_get_mempool_buffer(&rtio_ctx, cqe, &held[i], &held_len[i]));
		rtio_cqe_release(&rtio_ctx, cqe);
	}

	zassert_ok(rtp_stream_write(&source_stream, tx_buf, sizeof(tx_buf), NULL));
	zassert_is_null(cqe_consume_timeout(&rtio_ctx, CQE_TIMEOUT_SHORT_MS));

	rtio_release_buffer(&rtio_ctx, held[0], held_len[0]);

	memset(tx_buf, 0x5A, sizeof(tx_buf));
	zassert_ok(rtp_stream_write(&source_stream, tx_buf, sizeof(tx_buf), NULL));

	cqe = cqe_consume_timeout(&rtio_ctx, CQE_TIMEOUT_MS);
	zassert_not_null(cqe);
	zassert_equal(cqe->result, PAYLOAD_SIZE);
	zassert_ok(rtio_cqe_get_mempool_buffer(&rtio_ctx, cqe, &held[0], &held_len[0]));
	zassert_mem_equal(held[0], tx_buf, sizeof(tx_buf));
	rtio_cqe_release(&rtio_ctx, cqe);

	ARRAY_FOR_EACH(held, i) {
		rtio_release_buffer(&rtio_ctx, held[i], held_len[i]);
	}
}

/* RTP_STREAM_DROP cancels a still-pending RTIO_OP_RX submission */
ZTEST(rtp_stream_rtio_tests, test_drop_cancels_pending_rx)
{
	struct rtio_sqe *sqe;
	struct rtio_cqe *cqe;
	uint8_t rx_buf[PAYLOAD_SIZE];

	zassert_ok(rtp_stream_trigger(&sink_stream, RTP_STREAM_START));

	sqe = rtio_sqe_acquire(&rtio_ctx);
	zassert_not_null(sqe);
	rtio_sqe_prep_read(sqe, &sink_stream_iodev, RTIO_PRIO_NORM, rx_buf, sizeof(rx_buf), NULL);
	rtio_submit(&rtio_ctx, 0);

	zassert_ok(rtp_stream_trigger(&sink_stream, RTP_STREAM_DROP));

	cqe = cqe_consume_timeout(&rtio_ctx, CQE_TIMEOUT_MS);
	zassert_not_null(cqe);
	zassert_equal(cqe->result, -ECANCELED);
	rtio_cqe_release(&rtio_ctx, cqe);
}

/* RTP_STREAM_PREPARE (from ERROR) also cancels a still-pending RTIO_OP_RX submission */
ZTEST(rtp_stream_rtio_tests, test_prepare_cancels_pending_rx)
{
	struct rtio_sqe *sqe;
	struct rtio_cqe *cqe;
	uint8_t rx_buf[PAYLOAD_SIZE];

	zassert_ok(rtp_stream_trigger(&sink_stream, RTP_STREAM_START));

	sqe = rtio_sqe_acquire(&rtio_ctx);
	zassert_not_null(sqe);
	rtio_sqe_prep_read(sqe, &sink_stream_iodev, RTIO_PRIO_NORM, rx_buf, sizeof(rx_buf), NULL);
	rtio_submit(&rtio_ctx, 0);

	sink_stream.state = RTP_STREAM_STATE_ERROR;

	zassert_ok(rtp_stream_trigger(&sink_stream, RTP_STREAM_PREPARE));

	cqe = cqe_consume_timeout(&rtio_ctx, CQE_TIMEOUT_MS);
	zassert_not_null(cqe);
	zassert_equal(cqe->result, -ECANCELED);
	rtio_cqe_release(&rtio_ctx, cqe);
}

/* Setup of the tests */
static void *suite_setup(void)
{
	static struct net_sockaddr_in source_sink_addr;
	static struct net_sockaddr_in source_only_addr;
	struct rtp_stream_config config = {};

	lo_iface = net_if_lookup_by_dev(device_get_binding("lo"));
	zassert_not_null(lo_iface);

	build_sockaddr(&source_sink_addr, SOURCE_SINK_PORT);
	build_sockaddr(&source_only_addr, SOURCE_ONLY_PORT);

	zassert_ok(rtp_stream_init(&source_stream));
	zassert_ok(rtp_stream_init(&sink_stream));
	zassert_ok(rtp_stream_init(&source_only_stream));
	zassert_ok(rtp_stream_init(&decode_codec_stream));

	config.payload_type = PAYLOAD_TYPE;
	config.iface = lo_iface;
	config.sock_addr = (struct net_sockaddr *)&source_sink_addr;
	config.codec_api = &passthrough_codec_api;
#ifdef CONFIG_RTP_TRANSPORT_NET_PKT
	config.transport_type = RTP_TRANSPORT_NET_PKT;
#else
	config.transport_type = RTP_TRANSPORT_SOCKET;
#endif
	zassert_ok(rtp_stream_configure(&source_stream, RTP_STREAM_ROLE_SOURCE, &config));

	memset(&config, 0, sizeof(config));
	config.block_size = PAYLOAD_SIZE;
	config.iface = lo_iface;
	config.sock_addr = (struct net_sockaddr *)&source_sink_addr;
	config.codec_api = &passthrough_codec_api;
#ifdef CONFIG_RTP_TRANSPORT_NET_PKT
	config.transport_type = RTP_TRANSPORT_NET_PKT;
#else
	config.transport_type = RTP_TRANSPORT_SOCKET;
#endif
	zassert_ok(rtp_stream_configure(&sink_stream, RTP_STREAM_ROLE_SINK, &config));

	memset(&config, 0, sizeof(config));
	config.payload_type = PAYLOAD_TYPE;
	config.iface = lo_iface;
	config.sock_addr = (struct net_sockaddr *)&source_only_addr;
	config.codec_api = &passthrough_codec_api;
#ifdef CONFIG_RTP_TRANSPORT_NET_PKT
	config.transport_type = RTP_TRANSPORT_NET_PKT;
#else
	config.transport_type = RTP_TRANSPORT_SOCKET;
#endif
	zassert_ok(rtp_stream_configure(&source_only_stream, RTP_STREAM_ROLE_SOURCE, &config));

	return NULL;
}

/* Drop all streams and drain any leftover completions after each test, so that e.g. the
 * cancellation CQEs of test_rx_multishot's still-pending multishot reads don't leak into the
 * next test's cqe_consume_timeout() calls.
 */
static void test_after(void *f)
{
	struct rtio_cqe *cqe;

	ARG_UNUSED(f);

	LOG_INF("%s", __func__);

	/* Best-effort reset back to READY; see the non-RTIO loopback test for why errors here
	 * aren't asserted.
	 */
	(void)rtp_stream_trigger(&source_stream, RTP_STREAM_DROP);
	(void)rtp_stream_trigger(&sink_stream, RTP_STREAM_DROP);
	(void)rtp_stream_trigger(&source_only_stream, RTP_STREAM_DROP);
	(void)rtp_stream_trigger(&decode_codec_stream, RTP_STREAM_DROP);

	while ((cqe = rtio_cqe_consume(&rtio_ctx)) != NULL) {
		uint8_t *buf;
		uint32_t buf_len;

		if (RTIO_CQE_FLAG_GET(cqe->flags) == RTIO_CQE_FLAG_MEMPOOL_BUFFER &&
		    rtio_cqe_get_mempool_buffer(&rtio_ctx, cqe, &buf, &buf_len) == 0) {
			rtio_release_buffer(&rtio_ctx, buf, buf_len);
		}
		rtio_cqe_release(&rtio_ctx, cqe);
	}
}

ZTEST_SUITE(rtp_stream_rtio_tests, NULL, suite_setup, NULL, test_after, NULL);
