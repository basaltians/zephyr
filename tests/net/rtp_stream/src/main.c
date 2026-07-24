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
#include <zephyr/ztest.h>

#include <string.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(test);

#define LOCALHOST_ADDR "127.0.0.1"

#define SOURCE_SINK_PORT 5004
#define BOTH_PORT        5006
#define SCRATCH_PORT     5008

#define PAYLOAD_TYPE 97
#define PAYLOAD_SIZE 32

#define N_PACKETS 10

#define RX_TIMEOUT       K_MSEC(200)
#define RX_TIMEOUT_SHORT K_MSEC(20)

#define SINK_MEM_SLAB_BLOCKS (CONFIG_RTP_STREAM_RX_MSGQ_SIZE + 3)

RTP_STREAM_DEFINE_STATIC(source_stream);
RTP_STREAM_DEFINE_STATIC(sink_stream);
RTP_STREAM_DEFINE(both_stream);
RTP_STREAM_DEFINE(uninit_stream);

K_MEM_SLAB_DEFINE(sink_mem_slab, PAYLOAD_SIZE, SINK_MEM_SLAB_BLOCKS, 4);
K_MEM_SLAB_DEFINE(both_mem_slab, PAYLOAD_SIZE, 4, 4);

/* NULL encode/decode: payload bytes pass through unchanged */
static struct rtp_stream_codec_api passthrough_codec_api;

static struct net_if *lo_iface;

static void build_sockaddr(struct net_sockaddr_in *addr, uint16_t port)
{
	struct net_in_addr in_addr;

	zassert_ok(net_addr_pton(NET_AF_INET, LOCALHOST_ADDR, &in_addr));

	addr->sin_family = NET_AF_INET;
	addr->sin_addr = in_addr;
	addr->sin_port = net_htons(port);
}

/* Test incorrect API calls */
ZTEST(rtp_stream_tests, test_api)
{
	struct net_sockaddr_in scratch_addr;
	struct rtp_stream_config config = {0};
	struct rtp_msg msg;
	uint8_t buf[PAYLOAD_SIZE] = {0};

#ifdef CONFIG_RTP_TRANSPORT_NET_PKT
	config.transport_type = RTP_TRANSPORT_NET_PKT;
#else
	config.transport_type = RTP_TRANSPORT_SOCKET;
#endif

	build_sockaddr(&scratch_addr, SCRATCH_PORT);

	/* rtp_stream_init */
	zassert_not_ok(rtp_stream_init(NULL));
	zassert_ok(rtp_stream_init(&uninit_stream));

	/* rtp_stream_configure: NULL stream / NULL config */
	zassert_not_ok(rtp_stream_configure(NULL, RTP_STREAM_ROLE_SOURCE, &config));
	zassert_not_ok(rtp_stream_configure(&uninit_stream, RTP_STREAM_ROLE_SOURCE, NULL));

	/* NULL codec_api */
	config.iface = lo_iface;
	config.sock_addr = (struct net_sockaddr *)&scratch_addr;
	zassert_not_ok(rtp_stream_configure(&uninit_stream, RTP_STREAM_ROLE_SOURCE, &config));

	config.codec_api = &passthrough_codec_api;

	/* Invalid role */
	zassert_not_ok(rtp_stream_configure(&uninit_stream, (enum rtp_stream_role)99, &config));

	/* block_size larger than the mem_slab's own block size */
	config.mem_slab = &sink_mem_slab;
	config.block_size = PAYLOAD_SIZE * 2;
	zassert_not_ok(rtp_stream_configure(&uninit_stream, RTP_STREAM_ROLE_SOURCE, &config));

	/* Still NOT_READY: START/STOP/DROP rejected, PREPARE is a no-op success */
	zassert_not_ok(rtp_stream_trigger(&uninit_stream, RTP_STREAM_START));
	zassert_not_ok(rtp_stream_trigger(&uninit_stream, RTP_STREAM_STOP));
	zassert_not_ok(rtp_stream_trigger(&uninit_stream, RTP_STREAM_DROP));
	zassert_ok(rtp_stream_trigger(&uninit_stream, RTP_STREAM_PREPARE));

	/* SOURCE config without mem_slab/block_size must configure cleanly, not crash */
	memset(&config, 0, sizeof(config));
	config.payload_type = PAYLOAD_TYPE;
	config.iface = lo_iface;
	config.sock_addr = (struct net_sockaddr *)&scratch_addr;
	config.codec_api = &passthrough_codec_api;

	zassert_ok(rtp_stream_configure(&uninit_stream, RTP_STREAM_ROLE_SOURCE, &config));

	/* rtp_stream_trigger: role mismatch, invalid state transitions, bad cmd */
	zassert_not_ok(rtp_stream_trigger(&source_stream, RTP_STREAM_STOP));

	zassert_ok(rtp_stream_trigger(&source_stream, RTP_STREAM_START));
	zassert_not_ok(rtp_stream_trigger(&source_stream, RTP_STREAM_START));
	zassert_not_ok(rtp_stream_trigger(&source_stream, (enum rtp_stream_command)99));

	/* configure() rejected while RUNNING */
	zassert_not_ok(rtp_stream_configure(&source_stream, RTP_STREAM_ROLE_SOURCE, &config));

	zassert_ok(rtp_stream_trigger(&source_stream, RTP_STREAM_STOP));

	/* rtp_stream_read: NULL args */
	zassert_not_ok(rtp_stream_read(NULL, &msg, K_NO_WAIT));
	zassert_not_ok(rtp_stream_read(&sink_stream, NULL, K_NO_WAIT));

	/* rtp_stream_write: NULL stream, wrong role */
	zassert_not_ok(rtp_stream_write(NULL, buf, sizeof(buf), NULL));
	zassert_not_ok(rtp_stream_write(&sink_stream, buf, sizeof(buf), NULL));
}

/* Test normal RTP audio stream operation */
ZTEST(rtp_stream_tests, test_source_to_sink_basic)
{
	struct rtp_msg msg;
	uint8_t tx_buf[PAYLOAD_SIZE];
	uint32_t tx_ts;
	uint16_t prev_seq = 0;
	uint32_t prev_ts = 0;

	zassert_ok(rtp_stream_trigger(&sink_stream, RTP_STREAM_START));
	zassert_ok(rtp_stream_trigger(&source_stream, RTP_STREAM_START));

	for (size_t i = 0; i < N_PACKETS; i++) {
		memset(tx_buf, (uint8_t)i, sizeof(tx_buf));

		zassert_ok(rtp_stream_write(&source_stream, tx_buf, sizeof(tx_buf), &tx_ts));
		zassert_ok(rtp_stream_read(&sink_stream, &msg, RX_TIMEOUT));

		zassert_equal(msg.data_len, sizeof(tx_buf));
		zassert_mem_equal(msg.data, tx_buf, sizeof(tx_buf));
		zassert_equal(msg.ts, tx_ts);

		if (i > 0) {
			zassert_equal(msg.seq, (uint16_t)(prev_seq + 1));
			zassert_equal(msg.ts, prev_ts + PAYLOAD_SIZE);
		}
		prev_seq = msg.seq;
		prev_ts = msg.ts;

		k_mem_slab_free(&sink_mem_slab, msg.data);
	}

	zassert_ok(rtp_stream_trigger(&source_stream, RTP_STREAM_DROP));

	/* Source is down: sink stays healthy but sees nothing, so keep this wait short */
	zassert_not_ok(rtp_stream_read(&sink_stream, &msg, RX_TIMEOUT_SHORT));

	/* Start again after DROP.  */
	zassert_ok(rtp_stream_trigger(&source_stream, RTP_STREAM_START));

	for (size_t i = 0; i < 3; i++) {
		memset(tx_buf, (uint8_t)(0xA0 + i), sizeof(tx_buf));

		zassert_ok(rtp_stream_write(&source_stream, tx_buf, sizeof(tx_buf), &tx_ts));
		zassert_ok(rtp_stream_read(&sink_stream, &msg, RX_TIMEOUT));

		zassert_mem_equal(msg.data, tx_buf, sizeof(tx_buf));
		zassert_equal(msg.ts, tx_ts);

		if (i > 0) {
			zassert_equal(msg.seq, (uint16_t)(prev_seq + 1));
			zassert_equal(msg.ts, prev_ts + PAYLOAD_SIZE);
		}
		prev_seq = msg.seq;
		prev_ts = msg.ts;

		k_mem_slab_free(&sink_mem_slab, msg.data);
	}
}

/* Test the RTP_STREAM_ROLE_BOTH role */
ZTEST(rtp_stream_tests, test_role_both_loopback)
{
	struct rtp_msg msg;
	uint8_t tx_buf[PAYLOAD_SIZE];
	uint32_t tx_ts;

	memset(tx_buf, 0x5A, sizeof(tx_buf));

	zassert_ok(rtp_stream_trigger(&both_stream, RTP_STREAM_START));

	zassert_ok(rtp_stream_write(&both_stream, tx_buf, sizeof(tx_buf), &tx_ts));
	zassert_ok(rtp_stream_read(&both_stream, &msg, RX_TIMEOUT));

	zassert_equal(msg.data_len, sizeof(tx_buf));
	zassert_mem_equal(msg.data, tx_buf, sizeof(tx_buf));
	zassert_equal(msg.ts, tx_ts);

	k_mem_slab_free(&both_mem_slab, msg.data);
}

/* Test overflowwing rx_msgq in sink. Note that some headroom is present in the memslab */
ZTEST(rtp_stream_tests, test_rx_msgq_overflow)
{
	struct rtp_msg msg;
	uint8_t tx_buf[PAYLOAD_SIZE];

	zassert_ok(rtp_stream_trigger(&sink_stream, RTP_STREAM_START));
	zassert_ok(rtp_stream_trigger(&source_stream, RTP_STREAM_START));

	for (size_t i = 0; i < CONFIG_RTP_STREAM_RX_MSGQ_SIZE + 1; i++) {
		memset(tx_buf, (uint8_t)(0x50 + i), sizeof(tx_buf));
		zassert_ok(rtp_stream_write(&source_stream, tx_buf, sizeof(tx_buf), NULL));
	}

#if !defined(CONFIG_RTP_TRANSPORT_NET_PKT)
	/* Give socket thread time to process received packets */
	k_msleep(50);
#endif

	for (size_t i = 0; i < CONFIG_RTP_STREAM_RX_MSGQ_SIZE; i++) {
		zassert_ok(rtp_stream_read(&sink_stream, &msg, RX_TIMEOUT));
		k_mem_slab_free(&sink_mem_slab, msg.data);
	}

	/* Queue now empty, but the sink is in ERROR: read() must fail */
	zassert_not_ok(rtp_stream_read(&sink_stream, &msg, RX_TIMEOUT_SHORT));

	/* Can't leave ERROR via START; PREPARE recovers it back to READY */
	zassert_not_ok(rtp_stream_trigger(&sink_stream, RTP_STREAM_START));
	zassert_ok(rtp_stream_trigger(&sink_stream, RTP_STREAM_PREPARE));
	zassert_equal(sink_stream.state, RTP_STREAM_STATE_READY);
}

/* Test sending an oversized payload to the sink */
ZTEST(rtp_stream_tests, test_oversized_payload_dropped)
{
	struct rtp_msg msg;
	uint8_t tx_buf[PAYLOAD_SIZE * 2];

	memset(tx_buf, 0xD0, sizeof(tx_buf));

	zassert_ok(rtp_stream_trigger(&sink_stream, RTP_STREAM_START));
	zassert_ok(rtp_stream_trigger(&source_stream, RTP_STREAM_START));

	/* Larger than the sink's configured block_size: silently dropped, no ERROR state */
	zassert_ok(rtp_stream_write(&source_stream, tx_buf, sizeof(tx_buf), NULL));

	zassert_not_ok(rtp_stream_read(&sink_stream, &msg, RX_TIMEOUT_SHORT));
	zassert_equal(sink_stream.state, RTP_STREAM_STATE_RUNNING);
}

/* Setup of the tests */
static void *suite_setup(void)
{
	static struct net_sockaddr_in source_sink_addr;
	static struct net_sockaddr_in both_addr;
	struct rtp_stream_config config = {};

	lo_iface = net_if_lookup_by_dev(device_get_binding("lo"));
	zassert_not_null(lo_iface);

	build_sockaddr(&source_sink_addr, SOURCE_SINK_PORT);
	build_sockaddr(&both_addr, BOTH_PORT);

	zassert_ok(rtp_stream_init(&source_stream));
	zassert_ok(rtp_stream_init(&sink_stream));
	zassert_ok(rtp_stream_init(&both_stream));

	/* Source never receives, so it needs no mem_slab/block_size */
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
	config.mem_slab = &sink_mem_slab;
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
	config.mem_slab = &both_mem_slab;
	config.block_size = PAYLOAD_SIZE;
	config.iface = lo_iface;
	config.sock_addr = (struct net_sockaddr *)&both_addr;
	config.codec_api = &passthrough_codec_api;
#ifdef CONFIG_RTP_TRANSPORT_NET_PKT
	config.transport_type = RTP_TRANSPORT_NET_PKT;
#else
	config.transport_type = RTP_TRANSPORT_SOCKET;
#endif

	zassert_ok(rtp_stream_configure(&both_stream, RTP_STREAM_ROLE_BOTH, &config));

	return NULL;
}

/* Drop all streams after each test */
static void test_after(void *f)
{
	ARG_UNUSED(f);

	LOG_INF("%s", __func__);

	/* Best-effort reset back to READY. Not asserted: a stream that a test never
	 * actually started never registered its RX side, so the underlying
	 * rtp_session_stop() legitimately fails to unregister it here.
	 */
	(void)rtp_stream_trigger(&source_stream, RTP_STREAM_DROP);
	(void)rtp_stream_trigger(&sink_stream, RTP_STREAM_DROP);
	(void)rtp_stream_trigger(&both_stream, RTP_STREAM_DROP);
}

ZTEST_SUITE(rtp_stream_tests, NULL, suite_setup, NULL, test_after, NULL);
