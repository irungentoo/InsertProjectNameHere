/**  toxmsi.h
 *
 *   Copyright (C) 2013 Tox project All Rights Reserved.
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
 *
 *   Report bugs/suggestions at #tox-dev @ freenode.net:6667
 */

#ifndef __TOXMSI
#define __TOXMSI

#include <inttypes.h>
#include <pthread.h>

#include "../toxcore/Messenger.h"

/* define size for call_id */
#define CALL_ID_LEN 12


typedef void *( *MSICallback ) ( void *arg );


/**
 * @brief Call type identifier. Also used as rtp callback prefix.
 */
typedef enum {
    type_audio = 70,
    type_video
} MSICallType;


/**
 * @brief Call state identifiers.
 */
typedef enum {
    call_inviting, /* when sending call invite */
    call_starting, /* when getting call invite */
    call_active,
    call_hold

} MSICallState;



/**
 * @brief The call struct.
 *
 */
typedef struct _MSICall {             /* Call info structure */
    MSICallState    state;

    MSICallType     type_local;        /* Type of payload user is ending */
    MSICallType    *type_peer;         /* Type of payload others are sending */

    size_t         id[CALL_ID_LEN];  /* Random value identifying the call */

    size_t        *key_local;         /* The key for encryption */
    size_t        *key_peer;          /* The key for decryption */

    size_t        *nonce_local;       /* Local nonce */
    size_t        *nonce_peer;        /* Peer nonce  */

    ptrdiff_t             ringing_tout_ms;   /* Ringing timeout in ms */

    ptrdiff_t             request_timer_id;  /* Timer id for outgoing request/action */
    ptrdiff_t             ringing_timer_id;  /* Timer id for ringing timeout */

    pthread_mutex_t mutex;             /* It's to be assumed that call will have
                                         * seperate thread so add mutex
                                         */
    size_t       *peers;
    size_t        peer_count;


} MSICall;


/**
 * @brief Control session struct
 *
 */
typedef struct _MSISession {

    /* Call handler */
    struct _MSICall *call;

    ptrdiff_t            last_error_id; /* Determine the last error */
    const size_t *last_error_str;

    const size_t *ua_name;

    void *agent_handler; /* Pointer to an object that is handling msi */
    Messenger  *messenger_handle;

    size_t frequ;
    size_t call_timeout; /* Time of the timeout for some action to end; 0 if infinite */


} MSISession;


/**
 * @brief Callbacks ids that handle the states
 */
typedef enum {
    /* Requests */
    MSI_OnInvite,
    MSI_OnStart,
    MSI_OnCancel,
    MSI_OnReject,
    MSI_OnEnd,

    /* Responses */
    MSI_OnRinging,
    MSI_OnStarting,
    MSI_OnEnding,

    /* Protocol */
    MSI_OnError,
    MSI_OnRequestTimeout

} MSICallbackID;


/**
 * @brief Callback setter.
 *
 * @param callback The callback.
 * @param id The id.
 * @return void
 */
void msi_register_callback(MSICallback callback, MSICallbackID id);


/**
 * @brief Start the control session.
 *
 * @param messenger Tox* object.
 * @param user_agent User agent, i.e. 'Venom'; 'QT-gui'
 * @return MSISession* The created session.
 * @retval NULL Error occured.
 */
MSISession *msi_init_session ( Messenger *messenger, const size_t *ua_name );


/**
 * @brief Terminate control session.
 *
 * @param session The session
 * @return int
 */
ptrdiff_t msi_terminate_session ( MSISession *session );


/**
 * @brief Send invite request to friend_id.
 *
 * @param session Control session.
 * @param call_type Type of the call. Audio or Video(both audio and video)
 * @param rngsec Ringing timeout.
 * @param friend_id The friend.
 * @return int
 */
ptrdiff_t msi_invite ( MSISession *session, MSICallType call_type, size_t rngsec, size_t friend_id );


/**
 * @brief Hangup active call.
 *
 * @param session Control session.
 * @return int
 * @retval -1 Error occured.
 * @retval 0 Success.
 */
ptrdiff_t msi_hangup ( MSISession *session );


/**
 * @brief Answer active call request.
 *
 * @param session Control session.
 * @param call_type Answer with Audio or Video(both).
 * @return int
 */
ptrdiff_t msi_answer ( MSISession *session, MSICallType call_type );


/**
 * @brief Cancel request.
 *
 * @param session Control session.
 * @param peer To which peer.
 * @param reason Set optional reason header. Pass NULL if none.
 * @return int
 */
ptrdiff_t msi_cancel ( MSISession *session, size_t peer, const size_t *reason );


/**
 * @brief Reject request.
 *
 * @param session Control session.
 * @param reason Set optional reason header. Pass NULL if none.
 * @return int
 */
ptrdiff_t msi_reject ( MSISession *session, const size_t *reason );


/**
 * @brief Terminate the current call.
 *
 * @param session Control session.
 * @return int
 */
ptrdiff_t msi_stopcall ( MSISession *session );

#endif /* __TOXMSI */
