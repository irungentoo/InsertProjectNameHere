/**  toxav.h
 *
 *   Copyright (C) 2013 Tox project All Rights Reserved.
 *
 *   This file is part of Tox.
 *
 *   Tox is open source software: you can redistribute it and/or modify
 *   it under the terms of the StopNerds Public License as published by
 *   the StopNerds Foundation, either version 1 of the License, or
 *   (at your option) any later version.
 *
 *   Tox is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   StopNerds Public License for more details.
 *
 *   You should have received a copy of the StopNerds Public License
 *   along with Tox. If not, see <http://stopnerds.org/license/>.
 *
 */


#ifndef __TOXAV
#define __TOXAV
#include <inttypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/* vpx_image_t */
#include <vpx/vpx_image.h>

typedef void ( *ToxAVCallback ) ( int32_t, void *arg );
typedef struct _ToxAv ToxAv;

#ifndef __TOX_DEFINED__
#define __TOX_DEFINED__
typedef struct Tox Tox;
#endif

#define RTP_PAYLOAD_SIZE 65535


/**
 * @brief Callbacks ids that handle the call states.
 */
typedef enum {
    /* Requests */
    av_OnInvite,
    av_OnStart,
    av_OnCancel,
    av_OnReject,
    av_OnEnd,

    /* Responses */
    av_OnRinging,
    av_OnStarting,
    av_OnEnding,

    /* Protocol */
    av_OnError,
    av_OnRequestTimeout,
    av_OnPeerTimeout
} ToxAvCallbackID;


/**
 * @brief Call type identifier.
 */
typedef enum {
    TypeAudio = 192,
    TypeVideo
} ToxAvCallType;


/**
 * @brief Error indicators.
 */
typedef enum {
    ErrorNone = 0,
    ErrorInternal = -1, /* Internal error */
    ErrorAlreadyInCall = -2, /* Already has an active call */
    ErrorNoCall = -3, /* Trying to perform call action while not in a call */
    ErrorInvalidState = -4, /* Trying to perform call action while in invalid state*/
    ErrorNoRtpSession = -5, /* Trying to perform rtp action on invalid session */
    ErrorAudioPacketLost = -6, /* Indicating packet loss */
    ErrorStartingAudioRtp = -7, /* Error in toxav_prepare_transmission() */
    ErrorStartingVideoRtp = -8 , /* Error in toxav_prepare_transmission() */
    ErrorTerminatingAudioRtp = -9, /* Returned in toxav_kill_transmission() */
    ErrorTerminatingVideoRtp = -10, /* Returned in toxav_kill_transmission() */
    ErrorPacketTooLarge = -11, /* Buffer exceeds size while encoding */

} ToxAvError;


/**
 * @brief Locally supported capabilities.
 */
typedef enum {
    AudioEncoding = 1 << 0,
    AudioDecoding = 1 << 1,
    VideoEncoding = 1 << 2,
    VideoDecoding = 1 << 3
} ToxAvCapabilities;


/**
 * @brief Encoding settings.
 */
typedef struct _ToxAvCodecSettings {
    uint32_t video_bitrate; /* In bits/s */
    uint16_t video_width; /* In px */
    uint16_t video_height; /* In px */

    uint32_t audio_bitrate; /* In bits/s */
    uint16_t audio_frame_duration; /* In ms */
    uint32_t audio_sample_rate; /* In Hz */
    uint32_t audio_channels;

    uint32_t jbuf_capacity; /* Size of jitter buffer */
} ToxAvCodecSettings;

extern const ToxAvCodecSettings av_DefaultSettings;

/**
 * @brief Start new A/V session. There can only be one session at the time. If you register more
 *        it will result in undefined behaviour.
 *
 * @param messenger The messenger handle.
 * @param userdata The agent handling A/V session (i.e. phone).
 * @param video_width Width of video frame.
 * @param video_height Height of video frame.
 * @return ToxAv*
 * @retval NULL On error.
 */
ToxAv *toxav_new(Tox *messenger, int32_t max_calls);

/**
 * @brief Remove A/V session.
 *
 * @param av Handler.
 * @return void
 */
void toxav_kill(ToxAv *av);

/**
 * @brief Register callback for call state.
 *
 * @param callback The callback
 * @param id One of the ToxAvCallbackID values
 * @return void
 */
void toxav_register_callstate_callback (ToxAVCallback callback, ToxAvCallbackID id, void *userdata);

/**
 * @brief Call user. Use its friend_id.
 *
 * @param av Handler.
 * @param user The user.
 * @param call_type Call type.
 * @param ringing_seconds Ringing timeout.
 * @return int
 * @retval 0 Success.
 * @retval ToxAvError On error.
 */
int toxav_call(ToxAv *av, int32_t *call_index, int user, ToxAvCallType call_type, int ringing_seconds);

/**
 * @brief Hangup active call.
 *
 * @param av Handler.
 * @return int
 * @retval 0 Success.
 * @retval ToxAvError On error.
 */
int toxav_hangup(ToxAv *av, int32_t call_index);

/**
 * @brief Answer incomming call.
 *
 * @param av Handler.
 * @param call_type Answer with...
 * @return int
 * @retval 0 Success.
 * @retval ToxAvError On error.
 */
int toxav_answer(ToxAv *av, int32_t call_index, ToxAvCallType call_type );

/**
 * @brief Reject incomming call.
 *
 * @param av Handler.
 * @param reason Optional reason. Set NULL if none.
 * @return int
 * @retval 0 Success.
 * @retval ToxAvError On error.
 */
int toxav_reject(ToxAv *av, int32_t call_index, const char *reason);

/**
 * @brief Cancel outgoing request.
 *
 * @param av Handler.
 * @param reason Optional reason.
 * @param peer_id peer friend_id
 * @return int
 * @retval 0 Success.
 * @retval ToxAvError On error.
 */
int toxav_cancel(ToxAv *av, int32_t call_index, int peer_id, const char *reason);

/**
 * @brief Terminate transmission. Note that transmission will be terminated without informing remote peer.
 *
 * @param av Handler.
 * @return int
 * @retval 0 Success.
 * @retval ToxAvError On error.
 */
int toxav_stop_call(ToxAv *av, int32_t call_index);

/**
 * @brief Must be call before any RTP transmission occurs.
 *
 * @param av Handler.
 * @param support_video Is video supported ? 1 : 0
 * @return int
 * @retval 0 Success.
 * @retval ToxAvError On error.
 */
int toxav_prepare_transmission(ToxAv *av, int32_t call_index, ToxAvCodecSettings *codec_settings, int support_video);

/**
 * @brief Call this at the end of the transmission.
 *
 * @param av Handler.
 * @return int
 * @retval 0 Success.
 * @retval ToxAvError On error.
 */
int toxav_kill_transmission(ToxAv *av, int32_t call_index);

/**
 * @brief Receive decoded video packet.
 *
 * @param av Handler.
 * @param output Storage.
 * @return int
 * @retval 0 Success.
 * @retval ToxAvError On Error.
 */
int toxav_recv_video ( ToxAv *av, int32_t call_index, vpx_image_t **output);

/**
 * @brief Receive decoded audio frame.
 *
 * @param av Handler.
 * @param frame_size The size of dest in frames/samples (one frame/sample is 16 bits or 2 bytes
 *                   and corresponds to one sample of audio.)
 * @param dest Destination of the raw audio (16 bit signed pcm with AUDIO_CHANNELS channels).
 *             Make sure it has enough space for frame_size frames/samples.
 * @return int
 * @retval >=0 Size of received data in frames/samples.
 * @retval ToxAvError On error.
 */
int toxav_recv_audio( ToxAv *av, int32_t call_index, int frame_size, int16_t *dest );

/**
 * @brief Encode and send video packet.
 *
 * @param av Handler.
 * @param frame The encoded frame.
 * @param frame_size The size of the encoded frame.
 * @return int
 * @retval 0 Success.
 * @retval ToxAvError On error.
 */
int toxav_send_video ( ToxAv *av, int32_t call_index, const uint8_t *frame, int frame_size);

/**
 * @brief Send audio frame.
 *
 * @param av Handler.
 * @param frame The frame (raw 16 bit signed pcm with AUDIO_CHANNELS channels audio.)
 * @param frame_size Its size in number of frames/samples (one frame/sample is 16 bits or 2 bytes)
 *                   frame size should be AUDIO_FRAME_SIZE.
 * @return int
 * @retval 0 Success.
 * @retval ToxAvError On error.
 */
int toxav_send_audio ( ToxAv *av, int32_t call_index, const uint8_t *frame, int frame_size);

/**
 * @brief Encode video frame
 *
 * @param av Handler
 * @param dest Where to
 * @param dest_max Max size
 * @param input What to encode
 * @return int
 * @retval ToxAvError On error.
 * @retval >0 On success
 */
int toxav_prepare_video_frame ( ToxAv *av, int32_t call_index, uint8_t *dest, int dest_max, vpx_image_t *input );

/**
 * @brief Encode audio frame
 *
 * @param av Handler
 * @param dest dest
 * @param dest_max Max dest size
 * @param frame The frame
 * @param frame_size The frame size
 * @return int
 * @retval ToxAvError On error.
 * @retval >0 On success
 */
int toxav_prepare_audio_frame ( ToxAv *av, int32_t call_index, uint8_t *dest, int dest_max, const int16_t *frame,
                                int frame_size);

/**
 * @brief Get peer transmission type. It can either be audio or video.
 *
 * @param av Handler.
 * @param peer The peer
 * @return int
 * @retval ToxAvCallType On success.
 * @retval ToxAvError On error.
 */
int toxav_get_peer_transmission_type ( ToxAv *av, int32_t call_index, int peer );

/**
 * @brief Get id of peer participating in conversation
 *
 * @param av Handler
 * @param peer peer index
 * @return int
 * @retval ToxAvError No peer id
 */
int toxav_get_peer_id ( ToxAv *av, int32_t call_index, int peer );

/**
 * @brief Is certain capability supported
 *
 * @param av Handler
 * @return int
 * @retval 1 Yes.
 * @retval 0 No.
 */
int toxav_capability_supported ( ToxAv *av, int32_t call_index, ToxAvCapabilities capability );

/**
 * @brief Set queue limit
 *
 * @param av Handler
 * @param call_index index
 * @param limit the limit
 * @return void
 */
int toxav_set_audio_queue_limit ( ToxAv *av, int32_t call_index, uint64_t limit );

/**
 * @brief Set queue limit
 *
 * @param av Handler
 * @param call_index index
 * @param limit the limit
 * @return void
 */
int toxav_set_video_queue_limit ( ToxAv *av, int32_t call_index, uint64_t limit );


Tox *toxav_get_tox(ToxAv *av);

#ifdef __cplusplus
}
#endif

#endif /* __TOXAV */