/* DHT.c
 *
 * An implementation of the DHT as seen in http://wiki.tox.im/index.php/DHT
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

/*----------------------------------------------------------------------------------*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "DHT.h"
#include "network.h"
#include "ping.h"
#include "misc_tools.h"
#include "util.h"

/* The number of seconds for a non responsive node to become bad. */
#define BAD_NODE_TIMEOUT 70

/* The max number of nodes to send with send nodes. */
#define MAX_SENT_NODES 8

/* Ping timeout in seconds */
#define PING_TIMEOUT 5

/* The timeout after which a node is discarded completely. */
#define Kill_NODE_TIMEOUT 300

/* Ping interval in seconds for each node in our lists. */
#define PING_INTERVAL 60

/* Ping interval in seconds for each random sending of a get nodes request. */
#define GET_NODE_INTERVAL 10

#define MAX_PUNCHING_PORTS 32

/* Interval in seconds between punching attempts*/
#define PUNCH_INTERVAL 10

/* Ping newly announced nodes to ping per TIME_TOPING seconds*/
#define TIME_TOPING 5

#define NAT_PING_REQUEST    0
#define NAT_PING_RESPONSE   1

/* Used in the comparison function for sorting lists of Client_data. */
typedef struct {
    Client_data c1;
    Client_data c2;
} ClientPair;

/* Create the declaration for a quick sort for ClientPair structures. */
declare_quick_sort(ClientPair);
/* Create the quicksort function. See misc_tools.h for the definition. */
make_quick_sort(ClientPair);

Client_data *DHT_get_close_list(DHT *dht)
{
    return dht->close_clientlist;
}

/* Compares client_id1 and client_id2 with client_id.
 *
 *  return 0 if both are same distance.
 *  return 1 if client_id1 is closer.
 *  return 2 if client_id2 is closer.
 */
static int id_closest(uint8_t *id, uint8_t *id1, uint8_t *id2)
{
    size_t   i;
    uint8_t distance1, distance2;

    for (i = 0; i < CLIENT_ID_SIZE; ++i) {

        distance1 = abs(((int8_t *)id)[i] ^ ((int8_t *)id1)[i]);
        distance2 = abs(((int8_t *)id)[i] ^ ((int8_t *)id2)[i]);

        if (distance1 < distance2)
            return 1;

        if (distance1 > distance2)
            return 2;
    }

    return 0;
}

/* Turns the result of id_closest into something quick_sort can use.
 * Assumes p1->c1 == p2->c1.
 */
static int client_id_cmp(ClientPair p1, ClientPair p2)
{
    int c = id_closest(p1.c1.client_id, p1.c2.client_id, p2.c2.client_id);

    if (c == 2)
        return -1;

    return c;
}

static int id_equal(uint8_t *a, uint8_t *b)
{
    return memcmp(a, b, CLIENT_ID_SIZE) == 0;
}

static int is_timeout(uint64_t time_now, uint64_t timestamp, uint64_t timeout)
{
    return timestamp + timeout <= time_now;
}

/* Check if client with client_id is already in list of length length.
 * If it is then set its corresponding timestamp to current time.
 * If the id is already in the list with a different ip_port, update it.
 *  TODO: Maybe optimize this.
 *
 *  return True(1) or False(0)
 */
static int client_or_ip_port_in_list(Client_data *list, uint32_t length, uint8_t *client_id, IP_Port ip_port)
{
    uint32_t i;
    uint64_t temp_time = unix_time();

    uint8_t candropipv4 = 1;
    if (ip_port.ip.family == AF_INET6) {
        uint8_t ipv6cnt = 0;

        /* ipv6: count how many spots are used */
        for(i = 0; i < length; i++)
            if (list[i].ip_port.ip.family == AF_INET6)
                ipv6cnt++;

        /* more than half the list filled with ipv6: block ipv4->ipv6 overwrite */
        if (ipv6cnt > length / 2)
            candropipv4 = 0;
    }

    /* if client_id is in list, find it and maybe overwrite ip_port */
    for (i = 0; i < length; ++i)
        if (id_equal(list[i].client_id, client_id)) {
            /* if we got "too many" ipv6 addresses already, keep the ipv4 address */
            if (!candropipv4 && (list[i].ip_port.ip.family == AF_INET))
                return 1;

            /* Refresh the client timestamp. */
            list[i].timestamp = temp_time;
            list[i].ip_port = ip_port;
            return 1;
        }

    /* client_id not in list yet: find ip_port to overwrite */
    for (i = 0; i < length; ++i)
        if (ipport_equal(&list[i].ip_port, &ip_port)) {
            /* Refresh the client timestamp. */
            list[i].timestamp = temp_time;
            memcpy(list[i].client_id, client_id, CLIENT_ID_SIZE);
            return 1;
        }

    return 0;
}

/* Check if client with client_id is already in node format list of length length.
 *
 *  return 1 if true.
 *  return 2 if false.
 */
static int client_in_nodelist(Node_format *list, uint32_t length, uint8_t *client_id)
{
    uint32_t i;

    for (i = 0; i < length; ++i) {
        if (id_equal(list[i].client_id, client_id))
            return 1;
    }

    return 0;
}

/*  return friend number from the client_id.
 *  return -1 if a failure occurs.
 */
static int friend_number(DHT *dht, uint8_t *client_id)
{
    uint32_t i;

    for (i = 0; i < dht->num_friends; ++i) {
        if (id_equal(dht->friends_list[i].client_id, client_id))
            return i;
    }

    return -1;
}

/*
 * helper for get_close_nodes(). argument list is a monster :D
 */
static void get_close_nodes_inner(DHT *dht, uint8_t *client_id, Node_format *nodes_list,
                                  sa_family_t sa_family, Client_data *client_list, uint32_t client_list_length,
                                  time_t timestamp, int *num_nodes_ptr)
{
    int num_nodes = *num_nodes_ptr;
    int tout, inlist, ipv46x, j, closest;
    uint32_t i;

    for (i = 0; i < client_list_length; i++) {
        Client_data *client = &client_list[i];
        tout = is_timeout(timestamp, client->timestamp, BAD_NODE_TIMEOUT);
        inlist = client_in_nodelist(nodes_list, MAX_SENT_NODES, client->client_id);

#ifdef TOX_ENABLE_IPV6
        IP *client_ip = &client->ip_port.ip;

        /*
         * Careful: AF_INET isn't seen as AF_INET on dual-stack sockets for
         * our connections, instead we have to look if it is an embedded
         * IPv4-in-IPv6 here and convert it down in sendnodes().
         */
        sa_family_t ip_treat_as_family = client_ip->family;

        if ((dht->c->lossless_udp->net->family == AF_INET6) &&
                (client_ip->family == AF_INET6)) {
            /* socket is AF_INET6, address claims AF_INET6:
             * check for embedded IPv4-in-IPv6 */
            if (IN6_IS_ADDR_V4MAPPED(&client_ip->ip6.in6_addr))
                ip_treat_as_family = AF_INET;
        }

        ipv46x = !(sa_family == ip_treat_as_family);
#else
        ipv46x = !(sa_family == AF_INET);
#endif

        /* If node isn't good or is already in list. */
        if (tout || inlist || ipv46x)
            continue;

        if (num_nodes < MAX_SENT_NODES) {
            memcpy(nodes_list[num_nodes].client_id,
                   client->client_id,
                   CLIENT_ID_SIZE );

            nodes_list[num_nodes].ip_port = client->ip_port;
            num_nodes++;
        } else {
            /* see if node_list contains a client_id that's "further away"
             * compared to the one we're looking at at the moment, if there
             * is, replace it
             */
            for (j = 0; j < MAX_SENT_NODES; ++j) {
                closest = id_closest(   client_id,
                                        nodes_list[j].client_id,
                                        client->client_id );

                /* second client_id is closer than current: change to it */
                if (closest == 2) {
                    memcpy( nodes_list[j].client_id,
                            client->client_id,
                            CLIENT_ID_SIZE);

                    nodes_list[j].ip_port = client->ip_port;
                    break;
                }
            }
        }
    }

    *num_nodes_ptr = num_nodes;
}

/* Find MAX_SENT_NODES nodes closest to the client_id for the send nodes request:
 * put them in the nodes_list and return how many were found.
 *
 * TODO: For the love of based <your favorite deity, in doubt use "love"> make
 * this function cleaner and much more efficient.
 */
static int get_close_nodes(DHT *dht, uint8_t *client_id, Node_format *nodes_list, sa_family_t sa_family)
{
    time_t timestamp = unix_time();
    int num_nodes = 0, i;
    get_close_nodes_inner(dht, client_id, nodes_list, sa_family,
                          dht->close_clientlist, LCLIENT_LIST, timestamp, &num_nodes);

    for (i = 0; i < dht->num_friends; ++i)
        get_close_nodes_inner(dht, client_id, nodes_list, sa_family,
                              dht->friends_list[i].client_list, MAX_FRIEND_CLIENTS,
                              timestamp, &num_nodes);

    return num_nodes;
}

/* Replace first bad (or empty) node with this one.
 *
 *  return 0 if successful.
 *  return 1 if not (list contains no bad nodes).
 */
static int replace_bad(    Client_data    *list,
                           uint32_t        length,
                           uint8_t        *client_id,
                           IP_Port         ip_port )
{
    uint32_t i;
    uint64_t temp_time = unix_time();

    uint8_t candropipv4 = 1;
    if (ip_port.ip.family == AF_INET6) {
        uint32_t ipv6cnt = 0;

        /* ipv6: count how many spots are used */
        for(i = 0; i < length; i++)
            if (list[i].ip_port.ip.family == AF_INET6)
                ipv6cnt++;

        /* more than half the list filled with ipv6: block ipv4->ipv6 overwrite */
        if (ipv6cnt > length / 2)
            candropipv4 = 0;
    }

    for (i = 0; i < length; ++i) {
        /* If node is bad */
        Client_data *client = &list[i];
        if ((candropipv4 || (client->ip_port.ip.family == AF_INET6)) &&
            is_timeout(temp_time, client->timestamp, BAD_NODE_TIMEOUT)) {
            memcpy(client->client_id, client_id, CLIENT_ID_SIZE);
            client->ip_port = ip_port;
            client->timestamp = temp_time;
            ip_reset(&client->ret_ip_port.ip);
            client->ret_ip_port.port = 0;
            client->ret_timestamp = 0;
            return 0;
        }
    }

    return 1;
}

/* Sort the list. It will be sorted from furthest to closest.
 *  Turns list into data that quick sort can use and reverts it back.
 */
static void sort_list(Client_data *list, uint32_t length, uint8_t *comp_client_id)
{
    Client_data cd;
    ClientPair pairs[length];
    uint32_t i;

    memcpy(cd.client_id, comp_client_id, CLIENT_ID_SIZE);

    for (i = 0; i < length; ++i) {
        pairs[i].c1 = cd;
        pairs[i].c2 = list[i];
    }

    ClientPair_quick_sort(pairs, length, client_id_cmp);

    for (i = 0; i < length; ++i)
        list[i] = pairs[i].c2;
}

/* Replace the first good node that is further to the comp_client_id than that of the client_id in the list */
static int replace_good(   Client_data    *list,
                           uint32_t        length,
                           uint8_t        *client_id,
                           IP_Port         ip_port,
                           uint8_t        *comp_client_id )
{
    sort_list(list, length, comp_client_id);

    uint8_t candropipv4 = 1;
    if (ip_port.ip.family == AF_INET6) {
        uint32_t i, ipv6cnt = 0;

        /* ipv6: count how many spots are used */
        for(i = 0; i < length; i++)
            if (list[i].ip_port.ip.family == AF_INET6)
                ipv6cnt++;

        /* more than half the list filled with ipv6: block ipv4->ipv6 overwrite */
        if (ipv6cnt > length / 2)
            candropipv4 = 0;
    }

    int8_t replace = -1;
    uint32_t i;

    if (candropipv4) {
        /* either we got an ipv4 address, or we're "allowed" to push out an ipv4
         * address in favor of an ipv6 one
         *
         * because the list is sorted, we can simply check the client_id at the
         * border, either it is closer, then every other one is as well, or it is
         * further, then it gets pushed out in favor of the new address, which
         * will with the next sort() move to its "rightful" position
         *
         * CAVEAT: weirdly enough, the list is sorted DESCENDING in distance
         * so the furthest element is the first, NOT the last (at least that's
         * what the comment above sort_list() claims)
         */
        if (id_closest(comp_client_id, list[0].client_id, client_id) == 2)
            replace = 0;
    } else {
        /* ipv6 case without a right to push out an ipv4: only look for ipv6
         * addresses, the first one we find is either closer (then we can skip
         * out like above) or further (then we can replace it, like above)
         */
        for (i = 0; i < length; i++) {
            Client_data *client = &list[i];
            if (client->ip_port.ip.family == AF_INET6) {
                if (id_closest(comp_client_id, list[i].client_id, client_id) == 2)
                    replace = i;

                break;
            }
        }
    }

    if (replace != -1) {
#ifdef DEBUG
        assert(replace >= 0 && replace < length);
#endif
        Client_data *client = &list[replace];
        memcpy(client->client_id, client_id, CLIENT_ID_SIZE);
        client->ip_port = ip_port;
        client->timestamp = unix_time();
        ip_reset(&client->ret_ip_port.ip);
        client->ret_ip_port.port = 0;
        client->ret_timestamp = 0;
        return 0;
    }

    return 1;
}

/* Attempt to add client with ip_port and client_id to the friends client list
 * and close_clientlist.
 */
void addto_lists(DHT *dht, IP_Port ip_port, uint8_t *client_id)
{
    uint32_t i;

    /* convert IPv4-in-IPv6 to IPv4 */
    if ((ip_port.ip.family == AF_INET6) && IN6_IS_ADDR_V4MAPPED(&ip_port.ip.ip6.in6_addr)) {
        ip_port.ip.family = AF_INET;
        ip_port.ip.ip4.uint32 = ip_port.ip.ip6.uint32[3];
    }

    /* NOTE: Current behavior if there are two clients with the same id is
     * to replace the first ip by the second.
     */
    if (!client_or_ip_port_in_list(dht->close_clientlist, LCLIENT_LIST, client_id, ip_port)) {
        if (replace_bad(dht->close_clientlist, LCLIENT_LIST, client_id, ip_port)) {
            /* If we can't replace bad nodes we try replacing good ones. */
            replace_good(dht->close_clientlist, LCLIENT_LIST, client_id, ip_port,
                         dht->c->self_public_key);
        }
    }

    for (i = 0; i < dht->num_friends; ++i) {
        if (!client_or_ip_port_in_list(dht->friends_list[i].client_list,
                                       MAX_FRIEND_CLIENTS, client_id, ip_port)) {

            if (replace_bad(dht->friends_list[i].client_list, MAX_FRIEND_CLIENTS,
                            client_id, ip_port)) {
                /* If we can't replace bad nodes we try replacing good ones. */
                replace_good(dht->friends_list[i].client_list, MAX_FRIEND_CLIENTS,
                             client_id, ip_port, dht->friends_list[i].client_id);
            }
        }
    }
}

/* If client_id is a friend or us, update ret_ip_port
 * nodeclient_id is the id of the node that sent us this info.
 */
static void returnedip_ports(DHT *dht, IP_Port ip_port, uint8_t *client_id, uint8_t *nodeclient_id)
{
    uint32_t i, j;
    uint64_t temp_time = unix_time();

    if (id_equal(client_id, dht->c->self_public_key)) {

        for (i = 0; i < LCLIENT_LIST; ++i) {
            if (id_equal(nodeclient_id, dht->close_clientlist[i].client_id)) {
                dht->close_clientlist[i].ret_ip_port = ip_port;
                dht->close_clientlist[i].ret_timestamp = temp_time;
                return;
            }
        }

    } else {

        for (i = 0; i < dht->num_friends; ++i) {
            if (id_equal(client_id, dht->friends_list[i].client_id)) {

                for (j = 0; j < MAX_FRIEND_CLIENTS; ++j) {
                    if (id_equal(nodeclient_id, dht->friends_list[i].client_list[j].client_id)) {
                        dht->friends_list[i].client_list[j].ret_ip_port = ip_port;
                        dht->friends_list[i].client_list[j].ret_timestamp = temp_time;
                        return;
                    }
                }
            }
        }

    }
}

/* Same as last function but for get_node requests. */
static int is_gettingnodes(DHT *dht, IP_Port ip_port, uint64_t ping_id)
{
    uint32_t i;
    uint8_t pinging;
    uint64_t temp_time = unix_time();

    for (i = 0; i < LSEND_NODES_ARRAY; ++i ) {
        if (!is_timeout(temp_time, dht->send_nodes[i].timestamp, PING_TIMEOUT)) {
            pinging = 0;

            if (ping_id != 0 && dht->send_nodes[i].ping_id == ping_id)
                ++pinging;

            if (ip_isset(&ip_port.ip) && ipport_equal(&dht->send_nodes[i].ip_port, &ip_port))
                ++pinging;

            if (pinging == (ping_id != 0) + ip_isset(&ip_port.ip))
                return 1;
        }
    }

    return 0;
}

/* Same but for get node requests. */
static uint64_t add_gettingnodes(DHT *dht, IP_Port ip_port)
{
    uint32_t i, j;
    uint64_t ping_id = ((uint64_t)random_int() << 32) + random_int();
    uint64_t temp_time = unix_time();

    for (i = 0; i < PING_TIMEOUT; ++i ) {
        for (j = 0; j < LSEND_NODES_ARRAY; ++j ) {
            if (is_timeout(temp_time, dht->send_nodes[j].timestamp, PING_TIMEOUT - i)) {
                dht->send_nodes[j].timestamp = temp_time;
                dht->send_nodes[j].ip_port = ip_port;
                dht->send_nodes[j].ping_id = ping_id;
                return ping_id;
            }
        }
    }

    return 0;
}

/* Send a getnodes request. */
static int getnodes(DHT *dht, IP_Port ip_port, uint8_t *public_key, uint8_t *client_id)
{
    /* Check if packet is going to be sent to ourself. */
    if (id_equal(public_key, dht->c->self_public_key) || is_gettingnodes(dht, ip_port, 0))
        return -1;

    uint64_t ping_id = add_gettingnodes(dht, ip_port);

    if (ping_id == 0)
        return -1;

    uint8_t data[1 + CLIENT_ID_SIZE + crypto_box_NONCEBYTES + sizeof(ping_id) + CLIENT_ID_SIZE + ENCRYPTION_PADDING];
    uint8_t plain[sizeof(ping_id) + CLIENT_ID_SIZE];
    uint8_t encrypt[sizeof(ping_id) + CLIENT_ID_SIZE + ENCRYPTION_PADDING];
    uint8_t nonce[crypto_box_NONCEBYTES];
    new_nonce(nonce);

    memcpy(plain, &ping_id, sizeof(ping_id));
    memcpy(plain + sizeof(ping_id), client_id, CLIENT_ID_SIZE);

    int len = encrypt_data( public_key,
                            dht->c->self_secret_key,
                            nonce,
                            plain,
                            sizeof(ping_id) + CLIENT_ID_SIZE,
                            encrypt );

    if (len != sizeof(ping_id) + CLIENT_ID_SIZE + ENCRYPTION_PADDING)
        return -1;

    data[0] = NET_PACKET_GET_NODES;
    memcpy(data + 1, dht->c->self_public_key, CLIENT_ID_SIZE);
    memcpy(data + 1 + CLIENT_ID_SIZE, nonce, crypto_box_NONCEBYTES);
    memcpy(data + 1 + CLIENT_ID_SIZE + crypto_box_NONCEBYTES, encrypt, len);

    return sendpacket(dht->c->lossless_udp->net, ip_port, data, sizeof(data));
}

/* Send a send nodes response. */
/* because of BINARY compatibility, the Node_format MUST BE Node4_format,
 * IPv6 nodes are sent in a different message */
static int sendnodes(DHT *dht, IP_Port ip_port, uint8_t *public_key, uint8_t *client_id, uint64_t ping_id)
{
    /* Check if packet is going to be sent to ourself. */
    if (id_equal(public_key, dht->c->self_public_key))
        return -1;

    size_t Node4_format_size = sizeof(Node4_format);
    uint8_t data[1 + CLIENT_ID_SIZE + crypto_box_NONCEBYTES + sizeof(ping_id)
                 + Node4_format_size * MAX_SENT_NODES + ENCRYPTION_PADDING];

    Node_format nodes_list[MAX_SENT_NODES];
    int num_nodes = get_close_nodes(dht, client_id, nodes_list, AF_INET);

    if (num_nodes == 0)
        return 0;

    uint8_t plain[sizeof(ping_id) + Node4_format_size * MAX_SENT_NODES];
    uint8_t encrypt[sizeof(ping_id) + Node4_format_size * MAX_SENT_NODES + ENCRYPTION_PADDING];
    uint8_t nonce[crypto_box_NONCEBYTES];
    new_nonce(nonce);

    memcpy(plain, &ping_id, sizeof(ping_id));
#ifdef TOX_ENABLE_IPV6
    Node4_format *nodes4_list = (Node4_format *)(plain + sizeof(ping_id));
    int i, num_nodes_ok = 0;

    for (i = 0; i < num_nodes; i++) {
        memcpy(nodes4_list[num_nodes_ok].client_id, nodes_list[i].client_id, CLIENT_ID_SIZE);
        nodes4_list[num_nodes_ok].ip_port.port = nodes_list[i].ip_port.port;

        IP *node_ip = &nodes_list[i].ip_port.ip;

        if ((node_ip->family == AF_INET6) && IN6_IS_ADDR_V4MAPPED(&node_ip->ip6.in6_addr))
            /* embedded IPv4-in-IPv6 address: return it in regular sendnodes packet */
            nodes4_list[num_nodes_ok].ip_port.ip.uint32 = node_ip->ip6.uint32[3];
        else if (node_ip->family == AF_INET)
            nodes4_list[num_nodes_ok].ip_port.ip.uint32 = node_ip->ip4.uint32;
        else /* shouldn't happen */
            continue;

        num_nodes_ok++;
    }

    if (num_nodes_ok < num_nodes) {
        /* shouldn't happen */
        num_nodes = num_nodes_ok;
    }

#else
    memcpy(plain + sizeof(ping_id), nodes_list, num_nodes * Node4_format_size);
#endif

    int len = encrypt_data( public_key,
                            dht->c->self_secret_key,
                            nonce,
                            plain,
                            sizeof(ping_id) + num_nodes * Node4_format_size,
                            encrypt );

    if (len == -1)
        return -1;

    if ((unsigned int)len != sizeof(ping_id) + num_nodes * Node4_format_size + ENCRYPTION_PADDING)
        return -1;

    data[0] = NET_PACKET_SEND_NODES;
    memcpy(data + 1, dht->c->self_public_key, CLIENT_ID_SIZE);
    memcpy(data + 1 + CLIENT_ID_SIZE, nonce, crypto_box_NONCEBYTES);
    memcpy(data + 1 + CLIENT_ID_SIZE + crypto_box_NONCEBYTES, encrypt, len);

    return sendpacket(dht->c->lossless_udp->net, ip_port, data, 1 + CLIENT_ID_SIZE + crypto_box_NONCEBYTES + len);
}

#ifdef TOX_ENABLE_IPV6
/* Send a send nodes response: message for IPv6 nodes */
static int sendnodes_ipv6(DHT *dht, IP_Port ip_port, uint8_t *public_key, uint8_t *client_id, uint64_t ping_id)
{
    /* Check if packet is going to be sent to ourself. */
    if (id_equal(public_key, dht->c->self_public_key))
        return -1;

    size_t Node_format_size = sizeof(Node_format);
    uint8_t data[1 + CLIENT_ID_SIZE + crypto_box_NONCEBYTES + sizeof(ping_id)
                 + Node_format_size * MAX_SENT_NODES + ENCRYPTION_PADDING];

    Node_format nodes_list[MAX_SENT_NODES];
    int num_nodes = get_close_nodes(dht, client_id, nodes_list, AF_INET6);

    if (num_nodes == 0)
        return 0;

    uint8_t plain[sizeof(ping_id) + Node_format_size * MAX_SENT_NODES];
    uint8_t encrypt[sizeof(ping_id) + Node_format_size * MAX_SENT_NODES + ENCRYPTION_PADDING];
    uint8_t nonce[crypto_box_NONCEBYTES];
    new_nonce(nonce);

    memcpy(plain, &ping_id, sizeof(ping_id));
    memcpy(plain + sizeof(ping_id), nodes_list, num_nodes * Node_format_size);

    int len = encrypt_data( public_key,
                            dht->c->self_secret_key,
                            nonce,
                            plain,
                            sizeof(ping_id) + num_nodes * Node_format_size,
                            encrypt );

    if (len == -1)
        return -1;

    if ((unsigned int)len != sizeof(ping_id) + num_nodes * Node_format_size + ENCRYPTION_PADDING)
        return -1;

    data[0] = NET_PACKET_SEND_NODES_IPV6;
    memcpy(data + 1, dht->c->self_public_key, CLIENT_ID_SIZE);
    memcpy(data + 1 + CLIENT_ID_SIZE, nonce, crypto_box_NONCEBYTES);
    memcpy(data + 1 + CLIENT_ID_SIZE + crypto_box_NONCEBYTES, encrypt, len);

    return sendpacket(dht->c->lossless_udp->net, ip_port, data, 1 + CLIENT_ID_SIZE + crypto_box_NONCEBYTES + len);
}
#endif

static int handle_getnodes(void *object, IP_Port source, uint8_t *packet, uint32_t length)
{
    DHT *dht = object;
    uint64_t ping_id;

    if (length != ( 1 + CLIENT_ID_SIZE + crypto_box_NONCEBYTES
                    + sizeof(ping_id) + CLIENT_ID_SIZE + ENCRYPTION_PADDING ))
        return 1;

    /* Check if packet is from ourself. */
    if (id_equal(packet + 1, dht->c->self_public_key))
        return 1;

    uint8_t plain[sizeof(ping_id) + CLIENT_ID_SIZE];

    int len = decrypt_data( packet + 1,
                            dht->c->self_secret_key,
                            packet + 1 + CLIENT_ID_SIZE,
                            packet + 1 + CLIENT_ID_SIZE + crypto_box_NONCEBYTES,
                            sizeof(ping_id) + CLIENT_ID_SIZE + ENCRYPTION_PADDING,
                            plain );

    if (len != sizeof(ping_id) + CLIENT_ID_SIZE)
        return 1;

    memcpy(&ping_id, plain, sizeof(ping_id));
    sendnodes(dht, source, packet + 1, plain + sizeof(ping_id), ping_id);
#ifdef TOX_ENABLE_IPV6
    sendnodes_ipv6(dht, source, packet + 1, plain + sizeof(ping_id),
                   ping_id); /* TODO: prevent possible amplification attacks */
#endif

    //send_ping_request(dht, source, packet + 1); /* TODO: make this smarter? */

    return 0;
}

static int handle_sendnodes(void *object, IP_Port source, uint8_t *packet, uint32_t length)
{
    DHT *dht = object;
    uint64_t ping_id;
    uint32_t cid_size = 1 + CLIENT_ID_SIZE;
    cid_size += crypto_box_NONCEBYTES + sizeof(ping_id) + ENCRYPTION_PADDING;

    size_t Node4_format_size = sizeof(Node4_format);

    if (length > (cid_size + Node4_format_size * MAX_SENT_NODES) ||
            ((length - cid_size) % Node4_format_size) != 0 ||
            (length < cid_size + Node4_format_size))
        return 1;

    uint32_t num_nodes = (length - cid_size) / Node4_format_size;
    uint8_t plain[sizeof(ping_id) + Node4_format_size * MAX_SENT_NODES];

    int len = decrypt_data(
                  packet + 1,
                  dht->c->self_secret_key,
                  packet + 1 + CLIENT_ID_SIZE,
                  packet + 1 + CLIENT_ID_SIZE + crypto_box_NONCEBYTES,
                  sizeof(ping_id) + num_nodes * Node4_format_size + ENCRYPTION_PADDING, plain );

    if ((unsigned int)len != sizeof(ping_id) + num_nodes * Node4_format_size)
        return 1;

    memcpy(&ping_id, plain, sizeof(ping_id));

    if (!is_gettingnodes(dht, source, ping_id))
        return 1;

    uint32_t i;
    Node_format nodes_list[MAX_SENT_NODES];

#ifdef TOX_ENABLE_IPV6
    Node4_format *nodes4_list = (Node4_format *)(plain + sizeof(ping_id));

    uint32_t num_nodes_ok = 0;

    for (i = 0; i < num_nodes; i++)
        if ((nodes4_list[i].ip_port.ip.uint32 != 0) && (nodes4_list[i].ip_port.ip.uint32 != (uint32_t)~0)) {
            memcpy(nodes_list[num_nodes_ok].client_id, nodes4_list[i].client_id, CLIENT_ID_SIZE);
            nodes_list[num_nodes_ok].ip_port.ip.family = AF_INET;
            nodes_list[num_nodes_ok].ip_port.ip.ip4.uint32 = nodes4_list[i].ip_port.ip.uint32;
            nodes_list[num_nodes_ok].ip_port.port = nodes4_list[i].ip_port.port;

            num_nodes_ok++;
        }

    if (num_nodes_ok < num_nodes) {
        /* shouldn't happen */
        num_nodes = num_nodes_ok;
    }

#else
    memcpy(nodes_list, plain + sizeof(ping_id), num_nodes * sizeof(Node_format));
#endif

    addto_lists(dht, source, packet + 1);

    for (i = 0; i < num_nodes; ++i)  {
        send_ping_request(dht->ping, dht->c, nodes_list[i].ip_port, nodes_list[i].client_id);
        returnedip_ports(dht, nodes_list[i].ip_port, nodes_list[i].client_id, packet + 1);
    }

    return 0;
}

#ifdef TOX_ENABLE_IPV6
static int handle_sendnodes_ipv6(void *object, IP_Port source, uint8_t *packet, uint32_t length)
{
    DHT *dht = object;
    uint64_t ping_id;
    uint32_t cid_size = 1 + CLIENT_ID_SIZE;
    cid_size += crypto_box_NONCEBYTES + sizeof(ping_id) + ENCRYPTION_PADDING;

    size_t Node_format_size = sizeof(Node_format);

    if (length > (cid_size + Node_format_size * MAX_SENT_NODES) ||
            ((length - cid_size) % Node_format_size) != 0 ||
            (length < cid_size + Node_format_size))
        return 1;

    uint32_t num_nodes = (length - cid_size) / Node_format_size;
    uint8_t plain[sizeof(ping_id) + Node_format_size * MAX_SENT_NODES];

    int len = decrypt_data(
                  packet + 1,
                  dht->c->self_secret_key,
                  packet + 1 + CLIENT_ID_SIZE,
                  packet + 1 + CLIENT_ID_SIZE + crypto_box_NONCEBYTES,
                  sizeof(ping_id) + num_nodes * Node_format_size + ENCRYPTION_PADDING, plain );

    if ((unsigned int)len != sizeof(ping_id) + num_nodes * Node_format_size)
        return 1;

    memcpy(&ping_id, plain, sizeof(ping_id));

    if (!is_gettingnodes(dht, source, ping_id))
        return 1;

    uint32_t i;
    Node_format nodes_list[MAX_SENT_NODES];
    memcpy(nodes_list, plain + sizeof(ping_id), num_nodes * sizeof(Node_format));

    addto_lists(dht, source, packet + 1);

    for (i = 0; i < num_nodes; ++i)  {
        send_ping_request(dht->ping, dht->c, nodes_list[i].ip_port, nodes_list[i].client_id);
        returnedip_ports(dht, nodes_list[i].ip_port, nodes_list[i].client_id, packet + 1);
    }

    return 0;
}
#endif

/*----------------------------------------------------------------------------------*/
/*------------------------END of packet handling functions--------------------------*/

/*
 * Send get nodes requests with client_id to max_num peers in list of length length
 */
static void get_bunchnodes(DHT *dht, Client_data *list, uint16_t length, uint16_t max_num, uint8_t *client_id)
{
    uint64_t temp_time = unix_time();
    uint32_t i, num = 0;

    for (i = 0; i < length; ++i)
        if (ipport_isset(&(list[i].ip_port)) && !is_timeout(temp_time, list[i].ret_timestamp, BAD_NODE_TIMEOUT)) {
            getnodes(dht, list[i].ip_port, list[i].client_id, client_id);
            ++num;

            if (num >= max_num)
                return;
        }
}

int DHT_addfriend(DHT *dht, uint8_t *client_id)
{
    if (friend_number(dht, client_id) != -1) /* Is friend already in DHT? */
        return 1;

    DHT_Friend *temp;
    temp = realloc(dht->friends_list, sizeof(DHT_Friend) * (dht->num_friends + 1));

    if (temp == NULL)
        return 1;

    dht->friends_list = temp;
    memset(&dht->friends_list[dht->num_friends], 0, sizeof(DHT_Friend));
    memcpy(dht->friends_list[dht->num_friends].client_id, client_id, CLIENT_ID_SIZE);

    dht->friends_list[dht->num_friends].NATping_id = ((uint64_t)random_int() << 32) + random_int();
    ++dht->num_friends;
    get_bunchnodes(dht, dht->close_clientlist, LCLIENT_LIST, MAX_FRIEND_CLIENTS, client_id);/*TODO: make this better?*/
    return 0;
}

int DHT_delfriend(DHT *dht, uint8_t *client_id)
{
    uint32_t i;
    DHT_Friend *temp;

    for (i = 0; i < dht->num_friends; ++i) {
        /* Equal */
        if (id_equal(dht->friends_list[i].client_id, client_id)) {
            --dht->num_friends;

            if (dht->num_friends != i) {
                memcpy( dht->friends_list[i].client_id,
                        dht->friends_list[dht->num_friends].client_id,
                        CLIENT_ID_SIZE );
            }

            if (dht->num_friends == 0) {
                free(dht->friends_list);
                dht->friends_list = NULL;
                return 0;
            }

            temp = realloc(dht->friends_list, sizeof(DHT_Friend) * (dht->num_friends));

            if (temp == NULL)
                return 1;

            dht->friends_list = temp;
            return 0;
        }
    }

    return 1;
}

/* TODO: Optimize this. */
int DHT_getfriendip(DHT *dht, uint8_t *client_id, IP_Port *ip_port)
{
    uint32_t i, j;
    uint64_t temp_time = unix_time();

    ip_reset(&ip_port->ip);
    ip_port->port = 0;

    for (i = 0; i < dht->num_friends; ++i) {
        /* Equal */
        if (id_equal(dht->friends_list[i].client_id, client_id)) {
            for (j = 0; j < MAX_FRIEND_CLIENTS; ++j) {
                if (id_equal(dht->friends_list[i].client_list[j].client_id, client_id)
                        && !is_timeout(temp_time, dht->friends_list[i].client_list[j].timestamp, BAD_NODE_TIMEOUT)) {
                    *ip_port = dht->friends_list[i].client_list[j].ip_port;
                    return 1;
                }
            }

            return 0;
        }
    }

    return -1;
}

/* Ping each client in the "friends" list every PING_INTERVAL seconds. Send a get nodes request
 * every GET_NODE_INTERVAL seconds to a random good node for each "friend" in our "friends" list.
 */
static void do_DHT_friends(DHT *dht)
{
    uint32_t i, j;
    uint64_t temp_time = unix_time();
    uint32_t rand_node;
    uint32_t index[MAX_FRIEND_CLIENTS];

    for (i = 0; i < dht->num_friends; ++i) {
        uint32_t num_nodes = 0;

        for (j = 0; j < MAX_FRIEND_CLIENTS; ++j) {
            /* If node is not dead. */
            if (!is_timeout(temp_time, dht->friends_list[i].client_list[j].timestamp, Kill_NODE_TIMEOUT)) {
                if ((dht->friends_list[i].client_list[j].last_pinged + PING_INTERVAL) <= temp_time) {
                    send_ping_request(dht->ping, dht->c, dht->friends_list[i].client_list[j].ip_port,
                                      dht->friends_list[i].client_list[j].client_id );
                    dht->friends_list[i].client_list[j].last_pinged = temp_time;
                }

                /* If node is good. */
                if (!is_timeout(temp_time, dht->friends_list[i].client_list[j].timestamp, BAD_NODE_TIMEOUT)) {
                    index[num_nodes] = j;
                    ++num_nodes;
                }
            }
        }

        if (dht->friends_list[i].lastgetnode + GET_NODE_INTERVAL <= temp_time && num_nodes != 0) {
            rand_node = rand() % num_nodes;
            getnodes(dht, dht->friends_list[i].client_list[index[rand_node]].ip_port,
                     dht->friends_list[i].client_list[index[rand_node]].client_id,
                     dht->friends_list[i].client_id );
            dht->friends_list[i].lastgetnode = temp_time;
        }
    }
}

/* Ping each client in the close nodes list every PING_INTERVAL seconds.
 * Send a get nodes request every GET_NODE_INTERVAL seconds to a random good node in the list.
 */
static void do_Close(DHT *dht)
{
    uint32_t i;
    uint64_t temp_time = unix_time();
    uint32_t num_nodes = 0;
    uint32_t rand_node;
    uint32_t index[LCLIENT_LIST];

    for (i = 0; i < LCLIENT_LIST; ++i) {
        /* If node is not dead. */
        if (!is_timeout(temp_time, dht->close_clientlist[i].timestamp, Kill_NODE_TIMEOUT)) {
            if ((dht->close_clientlist[i].last_pinged + PING_INTERVAL) <= temp_time) {
                send_ping_request(dht->ping, dht->c, dht->close_clientlist[i].ip_port,
                                  dht->close_clientlist[i].client_id );
                dht->close_clientlist[i].last_pinged = temp_time;
            }

            /* If node is good. */
            if (!is_timeout(temp_time, dht->close_clientlist[i].timestamp, BAD_NODE_TIMEOUT)) {
                index[num_nodes] = i;
                ++num_nodes;
            }
        }
    }

    if (dht->close_lastgetnodes + GET_NODE_INTERVAL <= temp_time && num_nodes != 0) {
        rand_node = rand() % num_nodes;
        getnodes(dht, dht->close_clientlist[index[rand_node]].ip_port,
                 dht->close_clientlist[index[rand_node]].client_id,
                 dht->c->self_public_key );
        dht->close_lastgetnodes = temp_time;
    }
}

void DHT_bootstrap(DHT *dht, IP_Port ip_port, uint8_t *public_key)
{
    getnodes(dht, ip_port, public_key, dht->c->self_public_key);
    send_ping_request(dht->ping, dht->c, ip_port, public_key);
}
int DHT_bootstrap_from_address(DHT *dht, const char *address, uint8_t ipv6enabled,
                               uint16_t port, uint8_t *public_key)
{
    IP_Port ip_port_v64;
    IP *ip_extra = NULL;
#ifdef TOX_ENABLE_IPV6
    IP_Port ip_port_v4;
    ip_init(&ip_port_v64.ip, ipv6enabled);

    if (ipv6enabled) {
        ip_port_v64.ip.family = AF_UNSPEC;
        ip_reset(&ip_port_v4.ip);
        ip_extra = &ip_port_v4.ip;
    }

#else
    ip_init(&ip_port_v64.ip, 0);
#endif

    if (addr_resolve_or_parse_ip(address, &ip_port_v64.ip, ip_extra)) {
        ip_port_v64.port = port;
        DHT_bootstrap(dht, ip_port_v64, public_key);
#ifdef TOX_ENABLE_IPV6

        if ((ip_extra != NULL) && ip_isset(ip_extra)) {
            ip_port_v4.port = port;
            DHT_bootstrap(dht, ip_port_v4, public_key);
        }

#endif
        return 1;
    } else
        return 0;
}

/* Send the given packet to node with client_id
 *
 *  return -1 if failure.
 */
int route_packet(DHT *dht, uint8_t *client_id, uint8_t *packet, uint32_t length)
{
    uint32_t i;

    for (i = 0; i < LCLIENT_LIST; ++i) {
        if (id_equal(client_id, dht->close_clientlist[i].client_id))
            return sendpacket(dht->c->lossless_udp->net, dht->close_clientlist[i].ip_port, packet, length);
    }

    return -1;
}

/* Puts all the different ips returned by the nodes for a friend_num into array ip_portlist.
 * ip_portlist must be at least MAX_FRIEND_CLIENTS big.
 *
 *  return the number of ips returned.
 *  return 0 if we are connected to friend or if no ips were found.
 *  return -1 if no such friend.
 */
static int friend_iplist(DHT *dht, IP_Port *ip_portlist, uint16_t friend_num)
{
    int num_ips = 0;
    uint32_t i;
    uint64_t temp_time = unix_time();

    if (friend_num >= dht->num_friends)
        return -1;

    DHT_Friend *friend = &dht->friends_list[friend_num];
    Client_data *client;

    for (i = 0; i < MAX_FRIEND_CLIENTS; ++i) {
        client = &friend->client_list[i];

        /* If ip is not zero and node is good. */
        if (ip_isset(&client->ret_ip_port.ip) && !is_timeout(temp_time, client->ret_timestamp, BAD_NODE_TIMEOUT)) {

            if (id_equal(client->client_id, friend->client_id))
                return 0;

            ip_portlist[num_ips] = client->ret_ip_port;
            ++num_ips;
        }
    }

    return num_ips;
}


/* Send the following packet to everyone who tells us they are connected to friend_id.
 *
 *  return ip for friend.
 *  return number of nodes the packet was sent to. (Only works if more than (MAX_FRIEND_CLIENTS / 2).
 */
int route_tofriend(DHT *dht, uint8_t *friend_id, uint8_t *packet, uint32_t length)
{
    int num = friend_number(dht, friend_id);

    if (num == -1)
        return 0;

    uint32_t i, sent = 0;

    IP_Port ip_list[MAX_FRIEND_CLIENTS];
    int ip_num = friend_iplist(dht, ip_list, num);

    if (ip_num < (MAX_FRIEND_CLIENTS / 2))
        return 0;

    uint64_t temp_time = unix_time();
    DHT_Friend *friend = &dht->friends_list[num];
    Client_data *client;

    for (i = 0; i < MAX_FRIEND_CLIENTS; ++i) {
        client = &friend->client_list[i];

        /* If ip is not zero and node is good. */
        if (ip_isset(&client->ret_ip_port.ip) && !is_timeout(temp_time, client->ret_timestamp, BAD_NODE_TIMEOUT)) {
            int retval = sendpacket(dht->c->lossless_udp->net, client->ip_port, packet, length);

            if ((unsigned int)retval == length)
                ++sent;
        }
    }

    return sent;
}

/* Send the following packet to one random person who tells us they are connected to friend_id.
 *
 *  return number of nodes the packet was sent to.
 */
static int routeone_tofriend(DHT *dht, uint8_t *friend_id, uint8_t *packet, uint32_t length)
{
    int num = friend_number(dht, friend_id);

    if (num == -1)
        return 0;

    DHT_Friend *friend = &dht->friends_list[num];
    Client_data *client;

    IP_Port ip_list[MAX_FRIEND_CLIENTS];
    int n = 0;
    uint32_t i;
    uint64_t temp_time = unix_time();

    for (i = 0; i < MAX_FRIEND_CLIENTS; ++i) {
        client = &friend->client_list[i];

        /* If ip is not zero and node is good. */
        if (ip_isset(&client->ret_ip_port.ip) && !is_timeout(temp_time, client->ret_timestamp, BAD_NODE_TIMEOUT)) {
            ip_list[n] = client->ip_port;
            ++n;
        }
    }

    if (n < 1)
        return 0;

    int retval = sendpacket(dht->c->lossless_udp->net, ip_list[rand() % n], packet, length);

    if ((unsigned int)retval == length)
        return 1;

    return 0;
}

/* Puts all the different ips returned by the nodes for a friend_id into array ip_portlist.
 * ip_portlist must be at least MAX_FRIEND_CLIENTS big.
 *
 *  return number of ips returned.
 *  return 0 if we are connected to friend or if no ips were found.
 *  return -1 if no such friend.
 */
int friend_ips(DHT *dht, IP_Port *ip_portlist, uint8_t *friend_id)
{
    uint32_t i;

    for (i = 0; i < dht->num_friends; ++i) {
        /* Equal */
        if (id_equal(dht->friends_list[i].client_id, friend_id))
            return friend_iplist(dht, ip_portlist, i);
    }

    return -1;
}

/*----------------------------------------------------------------------------------*/
/*---------------------BEGINNING OF NAT PUNCHING FUNCTIONS--------------------------*/

static int send_NATping(DHT *dht, uint8_t *public_key, uint64_t ping_id, uint8_t type)
{
    uint8_t data[sizeof(uint64_t) + 1];
    uint8_t packet[MAX_DATA_SIZE];

    int num = 0;

    data[0] = type;
    memcpy(data + 1, &ping_id, sizeof(uint64_t));
    /* 254 is NAT ping request packet id */
    int len = create_request(dht->c->self_public_key, dht->c->self_secret_key, packet, public_key, data,
                             sizeof(uint64_t) + 1, CRYPTO_PACKET_NAT_PING);

    if (len == -1)
        return -1;

    if (type == 0) /* If packet is request use many people to route it. */
        num = route_tofriend(dht, public_key, packet, len);
    else if (type == 1) /* If packet is response use only one person to route it */
        num = routeone_tofriend(dht, public_key, packet, len);

    if (num == 0)
        return -1;

    return num;
}

/* Handle a received ping request for. */
static int handle_NATping(void *object, IP_Port source, uint8_t *source_pubkey, uint8_t *packet, uint32_t length)
{
    if (length != sizeof(uint64_t) + 1)
        return 1;

    DHT *dht = object;
    uint64_t ping_id;
    memcpy(&ping_id, packet + 1, sizeof(uint64_t));

    int friendnumber = friend_number(dht, source_pubkey);

    if (friendnumber == -1)
        return 1;

    DHT_Friend *friend = &dht->friends_list[friendnumber];

    if (packet[0] == NAT_PING_REQUEST) {
        /* 1 is reply */
        send_NATping(dht, source_pubkey, ping_id, NAT_PING_RESPONSE);
        friend->recvNATping_timestamp = unix_time();
        return 0;
    } else if (packet[0] == NAT_PING_RESPONSE) {
        if (friend->NATping_id == ping_id) {
            friend->NATping_id = ((uint64_t)random_int() << 32) + random_int();
            friend->hole_punching = 1;
            return 0;
        }
    }

    return 1;
}

/* Get the most common ip in the ip_portlist.
 * Only return ip if it appears in list min_num or more.
 * len must not be bigger than MAX_FRIEND_CLIENTS.
 *
 *  return ip of 0 if failure.
 */
static IP NAT_commonip(IP_Port *ip_portlist, uint16_t len, uint16_t min_num)
{
    IP zero;
    ip_reset(&zero);

    if (len > MAX_FRIEND_CLIENTS)
        return zero;

    uint32_t i, j;
    uint16_t numbers[MAX_FRIEND_CLIENTS] = {0};

    for (i = 0; i < len; ++i) {
        for (j = 0; j < len; ++j) {
            if (ip_equal(&ip_portlist[i].ip, &ip_portlist[j].ip))
                ++numbers[i];
        }

        if (numbers[i] >= min_num)
            return ip_portlist[i].ip;
    }

    return zero;
}

/* Return all the ports for one ip in a list.
 * portlist must be at least len long,
 * where len is the length of ip_portlist.
 *
 *  return number of ports and puts the list of ports in portlist.
 */
static uint16_t NAT_getports(uint16_t *portlist, IP_Port *ip_portlist, uint16_t len, IP ip)
{
    uint32_t i;
    uint16_t num = 0;

    for (i = 0; i < len; ++i) {
        if (ip_equal(&ip_portlist[i].ip, &ip)) {
            portlist[num] = ntohs(ip_portlist[i].port);
            ++num;
        }
    }

    return num;
}

static void punch_holes(DHT *dht, IP ip, uint16_t *port_list, uint16_t numports, uint16_t friend_num)
{
    if (numports > MAX_FRIEND_CLIENTS || numports == 0)
        return;

    uint32_t i;
    uint32_t top = dht->friends_list[friend_num].punching_index + MAX_PUNCHING_PORTS;

    for (i = dht->friends_list[friend_num].punching_index; i != top; i++) {
        /* TODO: Improve port guessing algorithm. */
        uint16_t port = port_list[(i / 2) % numports] + (i / (2 * numports)) * ((i % 2) ? -1 : 1);
        IP_Port pinging;
        ip_copy(&pinging.ip, &ip);
        pinging.port = htons(port);
        send_ping_request(dht->ping, dht->c, pinging, dht->friends_list[friend_num].client_id);
    }

    dht->friends_list[friend_num].punching_index = i;
}

static void do_NAT(DHT *dht)
{
    uint32_t i;
    uint64_t temp_time = unix_time();

    for (i = 0; i < dht->num_friends; ++i) {
        IP_Port ip_list[MAX_FRIEND_CLIENTS];
        int num = friend_iplist(dht, ip_list, i);

        /* If already connected or friend is not online don't try to hole punch. */
        if (num < MAX_FRIEND_CLIENTS / 2)
            continue;

        if (dht->friends_list[i].NATping_timestamp + PUNCH_INTERVAL < temp_time) {
            send_NATping(dht, dht->friends_list[i].client_id, dht->friends_list[i].NATping_id, NAT_PING_REQUEST);
            dht->friends_list[i].NATping_timestamp = temp_time;
        }

        if (dht->friends_list[i].hole_punching == 1 &&
                dht->friends_list[i].punching_timestamp + PUNCH_INTERVAL < temp_time &&
                dht->friends_list[i].recvNATping_timestamp + PUNCH_INTERVAL * 2 >= temp_time) {

            IP ip = NAT_commonip(ip_list, num, MAX_FRIEND_CLIENTS / 2);

            if (!ip_isset(&ip))
                continue;

            uint16_t port_list[MAX_FRIEND_CLIENTS];
            uint16_t numports = NAT_getports(port_list, ip_list, num, ip);
            punch_holes(dht, ip, port_list, numports, i);

            dht->friends_list[i].punching_timestamp = temp_time;
            dht->friends_list[i].hole_punching = 0;
        }
    }
}

/*----------------------------------------------------------------------------------*/
/*-----------------------END OF NAT PUNCHING FUNCTIONS------------------------------*/


/* Add nodes to the toping list.
 * All nodes in this list are pinged every TIME_TOPING seconds
 * and are then removed from the list.
 * If the list is full the nodes farthest from our client_id are replaced.
 * The purpose of this list is to enable quick integration of new nodes into the
 * network while preventing amplification attacks.
 *
 *  return 0 if node was added.
 *  return -1 if node was not added.
 */
int add_toping(DHT *dht, uint8_t *client_id, IP_Port ip_port)
{
    if (!ip_isset(&ip_port.ip))
        return -1;

    uint32_t i;

    for (i = 0; i < MAX_TOPING; ++i) {
        if (!ip_isset(&dht->toping[i].ip_port.ip)) {
            memcpy(dht->toping[i].client_id, client_id, CLIENT_ID_SIZE);
            ipport_copy(&dht->toping[i].ip_port, &ip_port);
            return 0;
        }
    }

    for (i = 0; i < MAX_TOPING; ++i) {
        if (id_closest(dht->c->self_public_key, dht->toping[i].client_id, client_id) == 2) {
            memcpy(dht->toping[i].client_id, client_id, CLIENT_ID_SIZE);
            ipport_copy(&dht->toping[i].ip_port, &ip_port);
            return 0;
        }
    }

    return -1;
}

/* Ping all the valid nodes in the toping list every TIME_TOPING seconds.
 * This function must be run at least once every TIME_TOPING seconds.
 */
static void do_toping(DHT *dht)
{
    uint64_t temp_time = unix_time();

    if (!is_timeout(temp_time, dht->last_toping, TIME_TOPING))
        return;

    dht->last_toping = temp_time;
    uint32_t i;

    for (i = 0; i < MAX_TOPING; ++i) {
        if (!ip_isset(&dht->toping[i].ip_port.ip))
            return;

        send_ping_request(dht->ping, dht->c, dht->toping[i].ip_port, dht->toping[i].client_id);
        ip_reset(&dht->toping[i].ip_port.ip);
    }
}


DHT *new_DHT(Net_Crypto *c)
{
    if (c == NULL)
        return NULL;

    DHT *temp = calloc(1, sizeof(DHT));

    if (temp == NULL)
        return NULL;

    temp->ping = new_ping();

    if (temp->ping == NULL) {
        kill_DHT(temp);
        return NULL;
    }

    temp->c = c;
    networking_registerhandler(c->lossless_udp->net, NET_PACKET_PING_REQUEST, &handle_ping_request, temp);
    networking_registerhandler(c->lossless_udp->net, NET_PACKET_PING_RESPONSE, &handle_ping_response, temp);
    networking_registerhandler(c->lossless_udp->net, NET_PACKET_GET_NODES, &handle_getnodes, temp);
    networking_registerhandler(c->lossless_udp->net, NET_PACKET_SEND_NODES, &handle_sendnodes, temp);
#ifdef TOX_ENABLE_IPV6
    networking_registerhandler(c->lossless_udp->net, NET_PACKET_SEND_NODES_IPV6, &handle_sendnodes_ipv6, temp);
#endif
    init_cryptopackets(temp);
    cryptopacket_registerhandler(c, CRYPTO_PACKET_NAT_PING, &handle_NATping, temp);
    return temp;
}

void do_DHT(DHT *dht)
{
    do_Close(dht);
    do_DHT_friends(dht);
    do_NAT(dht);
    do_toping(dht);
}
void kill_DHT(DHT *dht)
{
    kill_ping(dht->ping);
    free(dht->friends_list);
    free(dht);
}

/* Get the size of the DHT (for saving). */
uint32_t DHT_size_old(DHT *dht)
{
    return sizeof(dht->close_clientlist) + sizeof(DHT_Friend) * dht->num_friends;
}

/* Save the DHT in data where data is an array of size DHT_size(). */
void DHT_save_old(DHT *dht, uint8_t *data)
{
    memcpy(data, dht->close_clientlist, sizeof(dht->close_clientlist));
    memcpy(data + sizeof(dht->close_clientlist), dht->friends_list, sizeof(DHT_Friend) * dht->num_friends);
}

/* Load the DHT from data of size size.
 *
 *  return -1 if failure.
 *  return 0 if success.
 */
int DHT_load_old(DHT *dht, uint8_t *data, uint32_t size)
{
    if (size < sizeof(dht->close_clientlist)) {
#ifdef DEBUG
        fprintf(stderr, "DHT_load: Expected at least %u bytes, got %u.\n", sizeof(dht->close_clientlist), size);
#endif
        return -1;
    }

    uint32_t friendlistsize = size - sizeof(dht->close_clientlist);

    if (friendlistsize % sizeof(DHT_Friend) != 0) {
#ifdef DEBUG
        fprintf(stderr, "DHT_load: Expected a multiple of %u, got %u.\n", sizeof(DHT_Friend), friendlistsize);
#endif
        return -1;
    }

    uint32_t i, j;
    Client_data *client;
    uint16_t friends_num = friendlistsize / sizeof(DHT_Friend);

    if (friends_num != 0) {
        DHT_Friend *tempfriends_list = (DHT_Friend *)(data + sizeof(dht->close_clientlist));

        for (i = 0; i < friends_num; ++i) {
            DHT_addfriend(dht, tempfriends_list[i].client_id);

            for (j = 0; j < MAX_FRIEND_CLIENTS; ++j) {
                client = &tempfriends_list[i].client_list[j];

                if (client->timestamp != 0)
                    getnodes(dht, client->ip_port, client->client_id, tempfriends_list[i].client_id);
            }
        }
    }

    Client_data *tempclose_clientlist = (Client_data *)data;

    for (i = 0; i < LCLIENT_LIST; ++i) {
        if (tempclose_clientlist[i].timestamp != 0)
            DHT_bootstrap(dht, tempclose_clientlist[i].ip_port,
                          tempclose_clientlist[i].client_id );
    }

    return 0;
}


/* new DHT format for load/save, more robust and forward compatible */

#define DHT_STATE_COOKIE_GLOBAL 0x159000d

#define DHT_STATE_COOKIE_TYPE      0x11ce
#define DHT_STATE_TYPE_FRIENDS     1
#define DHT_STATE_TYPE_CLIENTS     2

/* Get the size of the DHT (for saving). */
uint32_t DHT_size(DHT *dht)
{
    uint32_t num = 0, i;

    for (i = 0; i < LCLIENT_LIST; ++i)
        if (dht->close_clientlist[i].timestamp != 0)
            num++;

    uint32_t size32 = sizeof(uint32_t), sizesubhead = size32 * 2;
    return size32
           + sizesubhead + sizeof(DHT_Friend) * dht->num_friends
           + sizesubhead + sizeof(Client_data) * num;
}

static uint8_t *z_state_save_subheader(uint8_t *data, uint32_t len, uint16_t type)
{
    uint32_t *data32 = (uint32_t *)data;
    data32[0] = len;
    data32[1] = (DHT_STATE_COOKIE_TYPE << 16) | type;
    data += sizeof(uint32_t) * 2;
    return data;
}

/* Save the DHT in data where data is an array of size DHT_size(). */
void DHT_save(DHT *dht, uint8_t *data)
{
    uint32_t len;
    uint16_t type;
    *(uint32_t *)data = DHT_STATE_COOKIE_GLOBAL;
    data += sizeof(uint32_t);

    len = sizeof(DHT_Friend) * dht->num_friends;
    type = DHT_STATE_TYPE_FRIENDS;
    data = z_state_save_subheader(data, len, type);
    memcpy(data, dht->friends_list, len);
    data += len;

    uint32_t num = 0, i;

    for (i = 0; i < LCLIENT_LIST; ++i)
        if (dht->close_clientlist[i].timestamp != 0)
            num++;

    len = num * sizeof(Client_data);
    type = DHT_STATE_TYPE_CLIENTS;
    data = z_state_save_subheader(data, len, type);

    if (num) {
        Client_data *clients = (Client_data *)data;

        for (num = 0, i = 0; i < LCLIENT_LIST; ++i)
            if (dht->close_clientlist[i].timestamp != 0)
                memcpy(&clients[num++], &dht->close_clientlist[i], sizeof(Client_data));
    }

    data += len;
}

static int dht_load_state_callback(void *outer, uint8_t *data, uint32_t length, uint16_t type)
{
    DHT *dht = outer;
    uint32_t num, i, j;

    switch (type) {
        case DHT_STATE_TYPE_FRIENDS:
            if (length % sizeof(DHT_Friend) != 0)
                break;

            DHT_Friend *friend_list = (DHT_Friend *)data;
            num = length / sizeof(DHT_Friend);

            for (i = 0; i < num; ++i) {
                DHT_addfriend(dht, friend_list[i].client_id);

                for (j = 0; j < MAX_FRIEND_CLIENTS; ++j) {
                    Client_data *client = &friend_list[i].client_list[j];

                    if (client->timestamp != 0)
                        getnodes(dht, client->ip_port, client->client_id, friend_list[i].client_id);
                }
            }

            break;

        case DHT_STATE_TYPE_CLIENTS:
            if ((length % sizeof(Client_data)) != 0)
                break;

            num = length / sizeof(Client_data);
            Client_data *client_list = (Client_data *)data;

            for (i = 0; i < num; ++i)
                if (client_list[i].timestamp != 0)
                    DHT_bootstrap(dht, client_list[i].ip_port, client_list[i].client_id);

            break;

        default:
            fprintf(stderr, "Load state (DHT): contains unrecognized part (len %u, type %u)\n",
                    length, type);
    }

    return 0;
}

/* Load the DHT from data of size size.
 *
 *  return -1 if failure.
 *  return 0 if success.
 */
int DHT_load_new(DHT *dht, uint8_t *data, uint32_t length)
{
    uint32_t cookie_len = sizeof(uint32_t);

    if (length > cookie_len) {
        uint32_t *data32 = (uint32_t *)data;

        if (data32[0] == DHT_STATE_COOKIE_GLOBAL)
            return load_state(dht_load_state_callback, dht, data + cookie_len,
                              length - cookie_len, DHT_STATE_COOKIE_TYPE);
    }

    return DHT_load_old(dht, data, length);
}
/*  return 0 if we are not connected to the DHT.
 *  return 1 if we are.
 */
int DHT_isconnected(DHT *dht)
{
    uint32_t i;
    uint64_t temp_time = unix_time();

    for (i = 0; i < LCLIENT_LIST; ++i) {
        if (!is_timeout(temp_time, dht->close_clientlist[i].timestamp, BAD_NODE_TIMEOUT))
            return 1;
    }

    return 0;
}
