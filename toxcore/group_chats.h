/* group_chats.h
 *
 * An implementation of massive text only group chats.
 *
 *
 *  Copyright (C) 2013 Tox project All Rights Reserved.
 *
 *  This file is part of Tox.
 *
 *  Tox is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  Tox is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with Tox.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef GROUP_CHATS_H
#define GROUP_CHATS_H

#include "net_crypto.h"

#define MAX_NICK_BYTES 128

typedef struct {
    uint8_t     client_id[crypto_box_PUBLICKEYBYTES];
    uint64_t    pingid;
    uint64_t    last_pinged;

    uint64_t    last_recv;
    uint64_t    last_recv_msgping;
    uint32_t    last_message_number;

    uint8_t     nick[MAX_NICK_BYTES];
    uint16_t    nick_len;
} Group_Peer;

typedef struct {
    uint8_t     client_id[crypto_box_PUBLICKEYBYTES];
    IP_Port     ip_port;
    uint64_t    last_recv;

} Group_Close;

#define GROUP_CLOSE_CONNECTIONS 6

typedef struct Group_Chat {
    Networking_Core *net;
    uint8_t     self_public_key[crypto_box_PUBLICKEYBYTES];
    uint8_t     self_secret_key[crypto_box_SECRETKEYBYTES];

    Group_Peer *group;
    Group_Close  close[GROUP_CLOSE_CONNECTIONS];
    uint32_t numpeers;

    uint32_t message_number;
    void (*group_message)(struct Group_Chat *m, int, uint8_t *, uint16_t, void *);
    void *group_message_userdata;
    uint64_t last_sent_ping;

} Group_Chat;

#define GROUP_CHAT_PING 0
#define GROUP_CHAT_NEW_PEER 16
#define GROUP_CHAT_CHAT_MESSAGE 64

/* Copy the name of peernum to name.
 * name must be at least MAX_NICK_BYTES long.
 *
 * return length of name if success
 * return -1 if failure
 */
int group_peername(Group_Chat *chat, int peernum, uint8_t *name);

/*
 * Set callback function for chat messages.
 *
 * format of function is: function(Group_Chat *chat, peer number, message, message length, userdata)
 */

void callback_groupmessage(Group_Chat *chat, void (*function)(Group_Chat *chat, int, uint8_t *, uint16_t, void *),
                           void *userdata);

/*
 * Send a message to the group.
 *
 * returns the number of peers it has sent it to.
 */
uint32_t group_sendmessage(Group_Chat *chat, uint8_t *message, uint32_t length);


/*
 * Tell everyone about a new peer (a person we are inviting for example.)
 *
 */
uint32_t group_newpeer(Group_Chat *chat, uint8_t *client_id);


/* Create a new group chat.
 *
 * Returns a new group chat instance if success.
 *
 * Returns a NULL pointer if fail.
 */
Group_Chat *new_groupchat(Networking_Core *net);


/* Kill a group chat
 *
 * Frees the memory and everything.
 */
void kill_groupchat(Group_Chat *chat);

/*
 * This is the main loop.
 */
void do_groupchat(Group_Chat *chat);

/* if we receive a group chat packet we call this function so it can be handled.
    return 0 if packet is handled correctly.
    return 1 if it didn't handle the packet or if the packet was shit. */
int handle_groupchatpacket(Group_Chat *chat, IP_Port source, uint8_t *packet, uint32_t length);


void chat_bootstrap(Group_Chat *chat, IP_Port ip_port, uint8_t *client_id);
void chat_bootstrap_nonlazy(Group_Chat *chat, IP_Port ip_port, uint8_t *client_id);


#endif
