/*
 * Copyright (c) 2026 Basalte
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief RTP streaming API
 *
 * Layers a fixed-size data block interface on top of Zephyr's RTP API
 * (@ref rtp), handling payload encoding/decoding, RTP timestamp bookkeeping,
 * and receive-side buffering, so that applications can move data blocks in
 * and out of a stream without handling RTP packets directly.
 */

#ifndef RTP_STREAM_H_
#define RTP_STREAM_H_

#include <zephyr/kernel.h>
#include <zephyr/net/rtp.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup rtp_stream RTP Stream
 * @ingroup rtp
 * @{
 */

/** Role of an RTP stream. */
enum rtp_stream_role {
	/** Stream only transmits; configures a transmit-only RTP session. */
	RTP_STREAM_ROLE_SOURCE,
	/** Stream only receives; configures a receive-only RTP session. */
	RTP_STREAM_ROLE_SINK,
	/** Stream transmits and receives; configures a full-duplex session. */
	RTP_STREAM_ROLE_BOTH,
};

/**
 * Commands accepted by @ref rtp_stream_trigger.
 *
 * Which commands are valid depends on the stream's current
 * @ref rtp_stream_state; see the individual command descriptions.
 */
enum rtp_stream_command {
	/** Start the RTP session. Valid only from
	 *  @ref RTP_STREAM_STATE_READY; transitions the stream to
	 *  @ref RTP_STREAM_STATE_RUNNING.
	 */
	RTP_STREAM_START,
	/** Stop the RTP session. Valid only from
	 *  @ref RTP_STREAM_STATE_RUNNING; transitions the stream back to
	 *  @ref RTP_STREAM_STATE_READY. Blocks already queued for receive
	 *  are left untouched; use @ref RTP_STREAM_DROP to discard them
	 *  as well.
	 */
	RTP_STREAM_STOP,
	/** Stop the RTP session, if running, and discard any buffered receive
	 *  blocks, returning them to the configured memory slab. Valid from any
	 *  state except @ref RTP_STREAM_STATE_NOT_READY; transitions the
	 *  stream to @ref RTP_STREAM_STATE_READY.
	 */
	RTP_STREAM_DROP,
	/** Recover from @ref RTP_STREAM_STATE_ERROR: stop the RTP
	 *  session, discard buffered receive blocks, and transition back to
	 *  @ref RTP_STREAM_STATE_READY. A no-op that returns success when
	 *  the stream is not in the error state; rejected while
	 *  @ref RTP_STREAM_STATE_RUNNING.
	 */
	RTP_STREAM_PREPARE,
};

/**
 * Codec transformation hooks used by a stream.
 *
 * Both callbacks transform their buffer in place, write the resulting
 * length in bytes to their output length parameter, and return 0 on
 * success or a negative errno value on failure. Either callback may be
 * left NULL, in which case the corresponding direction is a no-op.
 */
struct rtp_stream_codec_api {
	/**
	 * @brief Encode a block of data before it is sent as an RTP payload.
	 *
	 * Called from @ref rtp_stream_write with the block passed to
	 * that function.
	 *
	 * @param data              Buffer to transform in place.
	 * @param size              Length of @p data in bytes before encoding.
	 * @param[out] encoded_size Length of the encoded data in @p data, in
	 *                          bytes.
	 * @param[out] delta_ts     RTP timestamp delta represented by the
	 *                          encoded block, added to the RTP timestamp
	 *                          of the outgoing packet.
	 * @param user_data         Opaque user data, forwarded from the stream
	 *                          configuration's @p user_data set via
	 *                          @ref rtp_stream_configure.
	 *
	 * @retval 0        On success.
	 * @retval negative Errno value on failure.
	 */
	int (*encode)(void *data, size_t size, size_t *encoded_size, uint32_t *delta_ts,
		      void *user_data);

	/**
	 * @brief Decode a received RTP payload into a block of data.
	 *
	 * Called from @ref rtp_stream_read with the payload most
	 * recently popped from the stream's receive queue.
	 *
	 * @param data              Buffer to transform in place.
	 * @param size              Length of @p data in bytes before decoding.
	 * @param[out] decoded_size Length of the decoded data in @p data, in
	 *                          bytes.
	 * @param user_data         Opaque user data, forwarded from the stream
	 *                          configuration's @p user_data set via
	 *                          @ref rtp_stream_configure.
	 *
	 * @retval 0        On success.
	 * @retval negative Errno value on failure.
	 */
	int (*decode)(void *data, size_t size, size_t *decoded_size, void *user_data);
};

/** Lifecycle state of an @ref rtp_stream. */
enum rtp_stream_state {
	/** Initialized but not yet configured; set by
	 *  @ref rtp_stream_init.
	 */
	RTP_STREAM_STATE_NOT_READY,
	/** Configured and idle; set by @ref rtp_stream_configure and by
	 *  the @ref RTP_STREAM_STOP, @ref RTP_STREAM_DROP, and
	 *  @ref RTP_STREAM_PREPARE commands.
	 */
	RTP_STREAM_STATE_READY,
	/** RTP session started; set by the @ref RTP_STREAM_START
	 *  command.
	 */
	RTP_STREAM_STATE_RUNNING,
	/** A receive-side failure occurred (memory slab exhaustion or a full
	 *  receive queue). Recover with the @ref RTP_STREAM_DROP or
	 *  @ref RTP_STREAM_PREPARE command.
	 */
	RTP_STREAM_STATE_ERROR,
};

/** Configuration for an @ref rtp_stream, passed to
 *  @ref rtp_stream_configure.
 */
struct rtp_stream_config {
	/** Active transport backend for the RTP session. */
	enum rtp_transport_type transport_type;

	/** Memory slab that receive blocks are allocated from and returned
	 *  to. Must be set, with a block size of at least @p block_size. Used
	 *  to buffer received blocks when the stream role is
	 *  @ref RTP_STREAM_ROLE_SINK or @ref RTP_STREAM_ROLE_BOTH.
	 */
	struct k_mem_slab *mem_slab;

	/** Maximum accepted RTP payload length in bytes; checked against
	 *  @p mem_slab's block size. Larger received packets are dropped.
	 */
	size_t block_size;

	/** RTP payload type field value used when transmitting (0-127, see
	 *  @ref RTP_PAYLOAD_TYPE_MAX). Ignored when the stream role is
	 *  @ref RTP_STREAM_ROLE_SINK.
	 */
	uint8_t payload_type;

	/** Network interface to use. */
	struct net_if *iface;
	/** Session address and port (multicast group address or unicast
	 *  peer).
	 */
	struct net_sockaddr *sock_addr;

	/** Codec hooks used to encode outgoing and decode incoming data
	 *  blocks. Must not be NULL.
	 */
	struct rtp_stream_codec_api *codec_api;

	/** Opaque user data forwarded to @p codec_api's encode and decode
	 *  callbacks. May be NULL.
	 */
	void *user_data;
};

/** A single block of data popped from an @ref rtp_stream's receive
 *  queue by @ref rtp_stream_read.
 */
struct rtp_msg {
	/** Pointer to the decoded data block. Allocated from the stream
	 *  configuration's @p mem_slab; the caller takes ownership and is
	 *  responsible for returning it to that slab, either directly with
	 *  k_mem_slab_free(), or indirectly by handing it to a consumer that
	 *  frees it.
	 */
	void *data;

	/** Length of @p data in bytes, after decoding. */
	size_t data_len;

	/** Sequence number of the RTP packet the block was received in. */
	uint16_t seq;

	/** RTP timestamp of the packet the block was received in. */
	uint32_t ts;
};

/**
 * RTP stream state.
 *
 * Declare with @ref RTP_STREAM_DEFINE, initialize with
 * @ref rtp_stream_init, and configure with
 * @ref rtp_stream_configure before use.
 */
struct rtp_stream {
	/** Underlying RTP session used to send and receive packets. */
	struct rtp_session *rtp_session;

	/** Active configuration, copied by @ref rtp_stream_configure. */
	struct rtp_stream_config stream_config;

	/** Guard access to the stream */
	struct k_mutex lock;

	/** Backing storage for @p rx_out_msgq. */
	struct rtp_msg rx_out_msgs[CONFIG_RTP_STREAM_RX_MSGQ_SIZE];
	/** Queue of decoded receive blocks awaiting
	 *  @ref rtp_stream_read, populated from the RTP receive
	 *  callback. Holds up to @kconfig{CONFIG_RTP_STREAM_RX_MSGQ_SIZE}
	 *  entries.
	 */

	struct k_msgq rx_out_msgq;

	/** Current lifecycle state. */
	enum rtp_stream_state state;

	/** Role the stream was configured with. */
	enum rtp_stream_role role;
};

// clang-format off
/** @cond INTERNAL_HIDDEN */
#define __z_rtp_stream_define(_name, ...)                                                          \
	static RTP_SESSION_DEFINE(_name##_rtp_session, 0);                                         \
                                                                                                   \
	COND_CODE_0(NUM_VA_ARGS_LESS_1(__VA_ARGS__), (), __VA_ARGS__)                              \
	struct rtp_stream _name = {                                                                \
		.rtp_session = &_name##_rtp_session,                                               \
	};
/** @endcond */
// clang-format on

/**
 * @brief Define an RTP stream.
 *
 * Declares a backing @ref rtp_session (via @ref RTP_SESSION_DEFINE, with its
 * local port left at 0 to auto-select) together with a zero-initialized
 * @ref rtp_stream variable of the given name, wired to that session.
 * Call @ref rtp_stream_init and @ref rtp_stream_configure to
 * make the stream ready for use.
 *
 * @param _name Name of the @ref rtp_stream variable to define.
 */
#define RTP_STREAM_DEFINE(_name) __z_rtp_stream_define(_name)

/**
 * @brief Define an RTP stream in a private (static) scope.
 *
 * Declares a backing @ref rtp_session (via @ref RTP_SESSION_DEFINE, with its
 * local port left at 0 to auto-select) together with a zero-initialized
 * @ref rtp_stream variable of the given name, wired to that session.
 * Call @ref rtp_stream_init and @ref rtp_stream_configure to
 * make the stream ready for use.
 *
 * @param _name Name of the @ref rtp_stream variable to define.
 */
#define RTP_STREAM_DEFINE_STATIC(_name) __z_rtp_stream_define(_name, static)

/**
 * @brief Initialize an RTP stream.
 *
 * Prepares the stream's internal receive queue and lock. Call once, before
 * @ref rtp_stream_configure.
 *
 * @param stream Pointer to the RTP stream to initialize.
 *
 * @retval 0        On success.
 * @retval negative Errno value on failure.
 */
int rtp_stream_init(struct rtp_stream *stream);

/**
 * @brief Configure an RTP stream.
 *
 * Initializes the underlying RTP session for the given @p role (transmit
 * only, receive only, or both) and copies @p config into the stream. May be
 * called again to reconfigure the stream while it is
 * @ref RTP_STREAM_STATE_NOT_READY or
 * @ref RTP_STREAM_STATE_READY; rejected while
 * @ref RTP_STREAM_STATE_RUNNING or @ref RTP_STREAM_STATE_ERROR.
 *
 * @param stream Pointer to the RTP stream to configure.
 * @param role   Role to configure the stream for.
 * @param config Stream configuration, including the codec hooks and opaque
 *               user data used by @ref rtp_stream_read and
 *               @ref rtp_stream_write; see @ref rtp_stream_config for
 *               field requirements. Not copied until the underlying RTP
 *               session has been initialized successfully.
 *
 * @retval 0        On success; the stream transitions to
 *                  @ref RTP_STREAM_STATE_READY.
 * @retval negative Errno value on failure.
 */
int rtp_stream_configure(struct rtp_stream *stream, enum rtp_stream_role role,
			 struct rtp_stream_config *config);

/**
 * @brief Apply a lifecycle command to an RTP stream.
 *
 * @param stream Pointer to the RTP stream.
 * @param cmd    Command to apply; see @ref rtp_stream_command for the
 *               states each command is valid in.
 *
 * @retval 0        On success.
 * @retval negative Errno value on failure.
 */
int rtp_stream_trigger(struct rtp_stream *stream, enum rtp_stream_command cmd);

/**
 * @brief Read the next received data block from an RTP stream.
 *
 * Waits up to @p timeout for a block to become available in the stream's
 * receive queue, then decodes it in place with the stream's codec API.
 *
 * @param stream     Pointer to the RTP stream.
 * @param[out] msg    Populated with the decoded block, its length, and the
 *                    sequence number and timestamp of the RTP packet it
 *                    arrived in. The caller takes ownership of @p msg->data;
 *                    see @ref rtp_msg.
 * @param timeout    Maximum time to wait for a block to become available.
 *
 * @retval 0        On success.
 * @retval -EINVAL  If @p stream or @p msg is NULL.
 * @retval -EAGAIN  If no block became available within @p timeout and the
 *                  stream is not in @ref RTP_STREAM_STATE_ERROR; safe
 *                  to retry.
 * @retval -EIO     If no block became available and the stream is in
 *                  @ref RTP_STREAM_STATE_ERROR.
 * @retval negative Errno value from the codec's decode callback on failure.
 */
int rtp_stream_read(struct rtp_stream *stream, struct rtp_msg *msg, k_timeout_t timeout);

/**
 * @brief Encode and send a block of data on an RTP stream.
 *
 * Encodes @p data in place with the stream's codec API, then sends it as
 * the payload of an RTP packet, advancing the stream's RTP timestamp by the
 * delta reported by the codec's encode callback.
 *
 * @param stream         Pointer to the RTP stream. Must have been
 *                       configured with role @ref RTP_STREAM_ROLE_SOURCE
 *                       or @ref RTP_STREAM_ROLE_BOTH.
 * @param data           Buffer to encode and send; transformed in place by
 *                       the codec's encode callback.
 * @param size           Length of @p data in bytes before encoding.
 * @param[out] timestamp Pointer to store the RTP timestamp used for the
 *                       packet. May be NULL if not needed.
 *
 * @retval 0        On success.
 * @retval negative Errno value on failure.
 */
int rtp_stream_write(struct rtp_stream *stream, void *data, size_t size, uint32_t *timestamp);

#ifdef __cplusplus
}
#endif

/**
 * @}
 */

#endif /* RTP_STREAM_H_ */
