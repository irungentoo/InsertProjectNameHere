/*
 * group_announce.h -- Similar to ping.h, but designed for group chat purposes
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
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdint.h>

#include "DHT.h"
#include "announce.h"
#include "ping.h"

#include "network.h"
#include "util.h"
#include "ping_array.h"

/* Maximum newly announced online (in terms of group chats) nodes to ping per TIME_TO_PING seconds. */
#define MAX_ANNOUNCED_NODES 30

 /* Ping newly announced nodes every TIME_TO_PING seconds*/
#define TIME_TO_PING 20

#define ANNOUNCE_PLAIN_SIZE (1 + sizeof(uint64_t))
#define DHT_PING_SIZE (1 + CLIENT_ID_SIZE + crypto_box_NONCEBYTES + ANNOUNCE_PLAIN_SIZE + crypto_box_MACBYTES)
#define ANNOUNCE_DATA_SIZE (CLIENT_ID_SIZE + CLIENT_ID_SIZE + sizeof(IP_Port))


struct ANNOUNCE {
    DHT *dht;

    Ping_Array ping_array; //not sure if we need this for now... see do_announced_nodes function
    Announced_node_format announced_nodes[MAX_ANNOUNCED_NODES];
    uint64_t last_to_ping;
};

/* Send announce request
 * For members of group chat, who want to announce being online now
 * Announcing node should send chat_id together with other info
 */
int send_announce_request(PING *ping, IP_Port ipp, uint8_t *client_id)
{
    uint8_t   pk[DHT_PING_SIZE];
    int       rc;
    uint64_t  ping_id;

    if (id_equal(client_id, ping->dht->self_public_key))
        return 1;

    uint8_t shared_key[crypto_box_BEFORENMBYTES];

    // generate key to encrypt ping_id with recipient privkey
    DHT_get_shared_key_sent(ping->dht, shared_key, client_id);
    // Generate random ping_id.
    uint8_t data[PING_DATA_SIZE];
    id_copy(data, client_id);
    memcpy(data + CLIENT_ID_SIZE, &ipp, sizeof(IP_Port));
    ping_id = ping_array_add(&ping->ping_array, data, sizeof(data));

    if (ping_id == 0)
        return 1;

    uint8_t ping_plain[PING_PLAIN_SIZE];
    ping_plain[0] = NET_PACKET_PING_REQUEST;
    memcpy(ping_plain + 1, &ping_id, sizeof(ping_id));

    pk[0] = NET_PACKET_PING_REQUEST;
    id_copy(pk + 1, ping->dht->self_public_key);     // Our pubkey
    new_nonce(pk + 1 + CLIENT_ID_SIZE); // Generate new nonce


    rc = encrypt_data_symmetric(shared_key,
                                pk + 1 + CLIENT_ID_SIZE,
                                ping_plain, sizeof(ping_plain),
                                pk + 1 + CLIENT_ID_SIZE + crypto_box_NONCEBYTES);

    if (rc != PING_PLAIN_SIZE + crypto_box_MACBYTES)
        return 1;

    return sendpacket(ping->dht->net, ipp, pk, sizeof(pk));    
}

static int handle_announce_request(DHT * dht, IP_Port source, uint8_t *packet, uint32_t length)
{
}

static int get_announced_nodes_request(DHT * dht, IP_Port ip_port, uint8_t *public_key, uint8_t *client_id, Node_format *sendback_node)
{
}

static int handle_get_announced_nodes_request(DHT * dht, IP_Port source, uint8_t *packet, uint32_t length)
{
}


/*
 * static int sendnodes_ipv6(DHT *dht, IP_Port ip_port, uint8_t *public_key, uint8_t *client_id, uint8_t *sendback_data,
 *                         uint16_t length, uint8_t *shared_encryption_key)
 */


/* Add nodes to the announced_nodes list.
 * All nodes in this list are pinged every TIME_TO_PING seconds
 * and are then removed from the list.
 * If the list is full the nodes farthest from our client_id are replaced.
 * The purpose of this list is to store information about members of
 * group chats who are online now and give that info to users who want to join.
 *
 *  return 0 if node was added.
 *  return -1 if node was not added.
 */
int add_announced_nodes(ANNOUNCE *announce, uint8_t *client_id, uint8_t *chat_id, IP_Port ip_port)
{
    if (!ip_isset(&ip_port.ip))
    return -1;

    if (in_list(announce->dht->close_clientlist, LCLIENT_LIST, client_id, ip_port))
        return -1;

    uint32_t i;

    for (i = 0; i < MAX_TO_PING; ++i) {
        if (!ip_isset(&announce->announced_nodes[i].ip_port.ip)) {
            memcpy(announce->announced_nodes[i].client_id, client_id, CLIENT_ID_SIZE);
            memcpy(announce->announced_nodes[i].chat_id, chat_id, CLIENT_ID_SIZE);
            ipport_copy(&announce->announced_nodes[i].ip_port, &ip_port);
            return 0;
        }

        if (memcmp(announce->announced_nodes[i].client_id, client_id, CLIENT_ID_SIZE) == 0) {
            return -1;
        }

        if (memcmp(announce->announced_nodes[i].chat_id, chat_id, CLIENT_ID_SIZE) == 0) {
            return -1;
        }
    }

    uint32_t r = rand();

    for (i = 0; i < MAX_TO_PING; ++i) {
        if (id_closest(announce->dht->self_public_key, announce->announced_nodes[(i + r) % MAX_TO_PING].client_id, client_id) == 2) {
            memcpy(announce->announced_nodes[(i + r) % MAX_TO_PING].client_id, client_id, CLIENT_ID_SIZE);
            memcpy(announce->announced_nodes[(i + r) % MAX_TO_PING].chat_id, chat_id, CLIENT_ID_SIZE);
            ipport_copy(&announce->announced_nodes[(i + r) % MAX_TO_PING].ip_port, &ip_port);
            return 0;
        }
    }

    return -1;
}

/* Ping all the valid nodes in the announced_nodes list every TIME_TO_PING seconds.
 * This function must be run at least once every TIME_TO_PING seconds.
 */
//Probably we need to send another new type of request - is_announced, but I would rather go for
//storing announced node in the list until they replaced by new ones.

/*void do_announced_nodes(ANNOUNCE *announce)
{
}
*/

ANNOUNCE *new_announce(DHT *dht)
{
	ANNOUNCE *announce = calloc(1, sizeof(ANNOUNCE));

    if (announce == NULL)
        return NULL;

    if (ping_array_init(&announce->ping_array, PING_NUM_MAX, PING_TIMEOUT) != 0) {
        free(announce);
        return NULL;
    }

    announce->dht = dht;
    networking_registerhandler(announce->dht->net, NET_PACKET_ANNOUNCE_REQUEST, &handle_announce_request, dht);
    networking_registerhandler(announce->dht->net, NET_PACKET_GET_ANNOUNCED_NODES, &handle_get_announced_nodes_request, dht);

    return announce;
}

void kill_announce(ANNOUNCE *announce)
{
	networking_registerhandler(announce->dht->net, NET_PACKET_ANNOUNCE_REQUEST, NULL, NULL);
    networking_registerhandler(announce->dht->net, NET_PACKET_GET_ANNOUNCED_NODES, NULL, NULL);
    ping_array_free_all(&announce->ping_array);

    free(announce);
}
