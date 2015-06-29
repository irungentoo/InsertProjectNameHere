/**  audio.h
 *
 *   Copyright (C) 2013-2015 Tox project All Rights Reserved.
 *
 *   This file is part of Tox.
 *
 *   Tox is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   Tox is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Tox. If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef AUDIO_H
#define AUDIO_H

#include <opus.h>
#include <pthread.h>

#include "toxav.h"

#include "../toxcore/util.h"

struct RTPMessage_s;

/*
 * Base Audio Codec session type.
 */
typedef struct ACSession_s {
    /* encoding */
    OpusEncoder *encoder;
    int32_t last_encoding_sampling_rate;
    int32_t last_encoding_channel_count;
    int32_t last_encoding_bit_rate;
    
    /* Testing encoder for dynamic bit rate streaming */
    OpusEncoder *test_encoder;
    int32_t last_test_encoding_sampling_rate;
    int32_t last_test_encoding_channel_count;
    int32_t last_test_encoding_bit_rate;
    
    /* decoding */
    OpusDecoder *decoder;
    int32_t last_packet_channel_count;
    int32_t last_packet_sampling_rate;
    int32_t last_packet_frame_duration;
    int32_t last_decoding_sampling_rate;
    int32_t last_decoding_channel_count;
    uint64_t last_decoder_reconfiguration;
    void *j_buf;
    
    pthread_mutex_t queue_mutex[1];
    
    ToxAV* av;
    uint32_t friend_number;
    PAIR(toxav_audio_receive_frame_cb *, void *) acb; /* Audio frame receive callback */
} ACSession;

/*
 * Create new Audio Codec session.
 */
ACSession* ac_new(ToxAV* av, uint32_t friend_number, toxav_audio_receive_frame_cb *cb, void *cb_data);
/*
 * Kill the Audio Codec session.
 */
void ac_kill(ACSession* ac);
/*
 * Do periodic work. Work is consisted out of decoding only.
 */
void ac_do(ACSession* ac);
/*
 * Queue new rtp message.
 */
int ac_queue_message(void *acp, struct RTPMessage_s *msg);
/*
 * Set new values to the encoders.
 */
int ac_reconfigure_encoder(ACSession* ac, int32_t bit_rate, int32_t sampling_rate, uint8_t channels);
int ac_reconfigure_test_encoder(ACSession* ac, int32_t bit_rate, int32_t sampling_rate, uint8_t channels);

#endif /* AUDIO_H */