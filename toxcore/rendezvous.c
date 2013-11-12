
#include "rendezvous.h"
#include "network.h"
#include "net_crypto.h"
#include "assoc.h"
#include "util.h"

#ifndef ASSOC_AVAILABLE

/* The number of seconds for a non responsive node to become bad. */
#define BAD_NODE_TIMEOUT 72

#endif /* ! ASSOC_AVAILABLE */

/* how often the same match may be re-announced */
#define RENDEZVOUS_SEND_AGAIN 45U

/* stored entries */
#define RENDEZVOUS_STORE_SIZE 8

/* total len of hash over input/time */
#define HASHLEN crypto_hash_sha512_BYTES

typedef struct RendezVousPacket {
    uint8_t  type;
    uint8_t  hash_unspecific_half[HASHLEN / 2];
    uint8_t  hash_specific_half[HASHLEN / 2];
    uint8_t  target_id[crypto_box_PUBLICKEYBYTES];
} RendezVousPacket;

typedef struct {
    uint64_t           recv_at;
    IP_Port            ipp;

    RendezVousPacket   packet;

    uint8_t            match;
    uint64_t           sent_at;
} RendezVous_Entry;

/* somewhat defined in messenger.c, but don't want to pull in all that sh*t */
#define ADDRESS_EXTRA_BYTES (sizeof(uint32_t) + sizeof(uint16_t))

typedef struct RendezVous {
#ifdef ASSOC_AVAILABLE
    Assoc             *assoc;
#else
    DHT               *dht;
#endif
    Networking_Core   *net;

    uint8_t           *self_public;
    uint64_t           block_store_until;

    uint64_t           timestamp;
    uint64_t           publish_starttime;
    RendezVous_callbacks   functions;
    void                  *data;
    uint8_t            hash_unspecific_complete[HASHLEN];
    uint8_t            hash_specific_half[HASHLEN / 2];

    uint8_t            found[crypto_box_PUBLICKEYBYTES + ADDRESS_EXTRA_BYTES];

    RendezVous_Entry   store[RENDEZVOUS_STORE_SIZE];
} RendezVous;

/* Input:  unspecific of length HASHLEN
 *         id of length crypto_box_PUBLICKEYBYTES
 * Output: specific of length HASHLEN / 2
 */
static void hash_specific_half_calc(uint8_t *unspecific, uint8_t *id, uint8_t *specific)
{
    uint8_t validate_in[HASHLEN / 2 + crypto_box_PUBLICKEYBYTES];
    memcpy(validate_in, unspecific + HASHLEN / 2, HASHLEN / 2);
    id_copy(validate_in + HASHLEN / 2, id);

    uint8_t validate_out[HASHLEN];
    crypto_hash_sha512(validate_out, validate_in, sizeof(validate_in));
    memcpy(specific, validate_out, HASHLEN / 2);
}

/* Input:  specific of length HASHLEN / 2
 *         extra of length ADDRESS_EXTRA_BYTES
 * Output: modified specific
 */
static void hash_specific_extra_insert(uint8_t *specific, uint8_t *extra)
{
    size_t i;

    for (i = 0; i < ADDRESS_EXTRA_BYTES; i++)
        specific[i] ^= extra[i];
}

/* Input:  specific_calc of length HASHLEN / 2
 *         specific_recv of length HASHLEN / 2
 * Output: extra of length ADDRESS_EXTRA_BYTES
 */
static void hash_specific_extra_extract(uint8_t *specific_recv, uint8_t *specific_calc, uint8_t *extra)
{
    size_t i;

    for (i = 0; i < ADDRESS_EXTRA_BYTES; i++)
        extra[i] = specific_calc[i] ^ specific_recv[i];
}

static uint8_t *client_ptr_compare_ref_id;

static int client_ptr_compare_func(const void *_A, const void *_B)
{
    Client_data **A = (Client_data **)_A;
    Client_data **B = (Client_data **)_B;
    int res = id_closest(client_ptr_compare_ref_id, (*A)->client_id, (*B)->client_id);
    /* res: 0 => equal, 1: first id closer, 2: second id closer */
    /*      => 0        => -1               => 1                */
    return ((3 * res - 5) * res) >> 1;
}

static void publish(RendezVous *rendezvous)
{
    RendezVousPacket packet;
    // uint8_t packet[1 + HASHLEN + crypto_box_PUBLICKEYBYTES];
    packet.type = NET_PACKET_RENDEZVOUS;
    memcpy(packet.hash_unspecific_half, rendezvous->hash_unspecific_complete, HASHLEN / 2);
    memcpy(packet.hash_specific_half, rendezvous->hash_specific_half, HASHLEN / 2);
    memcpy(packet.target_id, rendezvous->self_public, crypto_box_PUBLICKEYBYTES);

#ifdef ASSOC_AVAILABLE
    /* ask DHT_assoc for IP_Ports for client_ids "close" to hash_unspecific_complete/2 */
    Assoc_close_nodes_simple state;
    memset(&state, 0, sizeof(state));
    state.close_count = 16;
    state.close_indices = calloc(16, sizeof(*state.close_indices));

    uint8_t found_cnt = Assoc_close_nodes_find(rendezvous->assoc, packet.hash_unspecific_half, &state);

    if (!found_cnt) {
#ifdef LOGGING
        loglog("rendezvous::publish(): no nodes to send data to. :-(\n");
#endif
        return;
    }

    uint8_t i, sent = 0;

    /* send to the four best verified and four random of the best 16
     * (requires some additions in assoc.*) */
    for (i = 0; i < found_cnt; i++)
        if ((i < 4) || !(rand() % 4)) {
            Client_data *entry = Assoc_client(rendezvous->assoc, state.close_indices[i]);

            if (entry) {
                IPPTsPng *assoc;

                if (entry->assoc4.timestamp > entry->assoc6.timestamp)
                    assoc = &entry->assoc4;
                else
                    assoc = &entry->assoc6;

                sendpacket(rendezvous->net, assoc->ip_port, &packet.type, sizeof(packet));
#ifdef LOGGING
                sprintf(logbuffer, "rendezvous::publish(): [%u] => [%u]\n", htons(rendezvous->net->port), htons(assoc->ip_port.port));
                loglog(logbuffer);
#endif
                sent++;
            }
        }

#ifdef LOGGING
    sprintf(logbuffer, "rendezvous::publish(): sent data to %u of %u clients.\n", sent, found_cnt);
    loglog(logbuffer);
#endif

#else /* ! ASSOC_AVAILABLE */

    /* no assoc: collect all nodes stored somewhere */
    DHT *dht = rendezvous->dht;
    size_t i, k, cnt = 0;
    Client_data *clients[256];

    for (i = 0; i < LCLIENT_LIST; i++) {
        Client_data *client = &dht->close_clientlist[i];

        if (!is_timeout(client->assoc4.timestamp, BAD_NODE_TIMEOUT) ||
                !is_timeout(client->assoc6.timestamp, BAD_NODE_TIMEOUT)) {
            clients[cnt++] = client;
        }
    }

    for (k = 0; k < dht->num_friends; k++)
        for (i = 0; i < MAX_FRIEND_CLIENTS; i++) {
            Client_data *client = &dht->friends_list[k].client_list[i];

            if (!is_timeout(client->assoc4.timestamp, BAD_NODE_TIMEOUT) ||
                    !is_timeout(client->assoc6.timestamp, BAD_NODE_TIMEOUT)) {
                clients[cnt++] = client;

                if (cnt == 256)
                    break;
            }
        }

    client_ptr_compare_ref_id = packet.hash_unspecific_half;
    qsort(clients, cnt, sizeof(clients[0]), client_ptr_compare_func);

    size_t sent = 0;

    for (i = 0; i < cnt; i++) {
        /* skip duplicates */
        if (i > 0)
            if (id_equal(clients[i]->client_id, clients[i - 1]->client_id))
                continue;

        /* the first four are sent to unconditionally
         * from there, they're sent to with 25% probability
         */
        if ((sent >= 4) && (rand() % 4))
            continue;

        Client_data *client = clients[i];

        IPPTsPng *assoc;

        if (client->assoc4.timestamp > client->assoc6.timestamp)
            assoc = &client->assoc4;
        else
            assoc = &client->assoc6;

        sendpacket(rendezvous->net, assoc->ip_port, &packet.type, sizeof(packet));
#ifdef LOGGING
        sprintf(logbuffer, "rendezvous::publish(): [%u] => [%u]\n", htons(rendezvous->net->port), htons(assoc->ip_port.port));
        loglog(logbuffer);
#endif
        sent++;

        if (sent >= 8)
            break;
    }

#ifdef LOGGING
    sprintf(logbuffer, "rendezvous::publish(): sent data to %u clients.\n", sent);
#endif

#endif /* ! ASSOC_AVAILABLE */
}

static void send_replies(RendezVous *rendezvous, size_t i, size_t k)
{
    if (is_timeout(rendezvous->store[i].sent_at, RENDEZVOUS_SEND_AGAIN)) {
        rendezvous->store[i].sent_at = unix_time();
        sendpacket(rendezvous->net, rendezvous->store[i].ipp, &rendezvous->store[k].packet.type, sizeof(RendezVousPacket));
    }

    if (is_timeout(rendezvous->store[k].sent_at, RENDEZVOUS_SEND_AGAIN)) {
        rendezvous->store[k].sent_at = unix_time();
        sendpacket(rendezvous->net, rendezvous->store[k].ipp, &rendezvous->store[i].packet.type, sizeof(RendezVousPacket));
    }
}

static int packet_is_wanted(RendezVous *rendezvous, RendezVousPacket *packet, uint64_t now_floored)
{
    /* only if we're currently searching */
    if (rendezvous->timestamp == now_floored)
        if (!memcmp(packet->hash_unspecific_half, rendezvous->hash_unspecific_complete, HASHLEN / 2)) {
            if (id_equal(rendezvous->found, packet->target_id))
                return 1;

            uint8_t hash_specific_half[HASHLEN / 2];
            hash_specific_half_calc(rendezvous->hash_unspecific_complete, packet->target_id, hash_specific_half);

            if (!memcmp(packet->hash_specific_half + ADDRESS_EXTRA_BYTES, hash_specific_half + ADDRESS_EXTRA_BYTES,
                        HASHLEN / 2 - ADDRESS_EXTRA_BYTES)) {
                id_copy(rendezvous->found, packet->target_id);
                hash_specific_extra_extract(packet->hash_specific_half, hash_specific_half,
                                            rendezvous->found + crypto_box_PUBLICKEYBYTES);
                rendezvous->functions.found_function(rendezvous->data, rendezvous->found);
                return 1;
            }
        }

    return 0;
}

static int packet_is_update(RendezVous *rendezvous, RendezVousPacket *packet, uint64_t now_floored, IP_Port *ipp)
{
    size_t i, k;

    for (i = 0; i < RENDEZVOUS_STORE_SIZE; i++)
        if (rendezvous->store[i].match != 0) {
            /* one slot per target_id to catch resends */
            if (id_equal(rendezvous->store[i].packet.target_id, packet->target_id)) {
                /* if the entry is timed out, and it changed, reset flag for match and store */
                if (rendezvous->store[i].recv_at < now_floored) {
                    if (memcmp(&rendezvous->store[i].packet, packet, sizeof(*packet)) != 0) {
                        rendezvous->store[i].recv_at = now_floored;
                        rendezvous->store[i].ipp = *ipp;
                        rendezvous->store[i].packet = *packet;

                        rendezvous->store[i].match = 1;
                        rendezvous->store[i].sent_at = 0;
                    }
                } else if (rendezvous->store[i].match == 2) {
                    /* there exists a match, send the pairing their data
                     * (if RENDEZVOUS_SEND_AGAIN seconds have passed) */
                    for (k = 0; k < RENDEZVOUS_STORE_SIZE; k++)
                        if ((i != k) && (rendezvous->store[k].match == 2))
                            if (rendezvous->store[k].recv_at == now_floored)
                                if (!memcmp(rendezvous->store[i].packet.hash_unspecific_half,
                                            rendezvous->store[k].packet.hash_unspecific_half, HASHLEN / 2))
                                    send_replies(rendezvous, i, k);
                }

                return 1;
            }
        }

    return 0;
}

static int rendezvous_network_handler(void *object, IP_Port ip_port, uint8_t *data, uint32_t len)
{
    if (!object)
        return 0;

    /*
     * got to do two things here:
     * a) store up to 8 entries
     * b) on an incoming packet, see if the unencrypted half matches a previous one,
     *    if yes, send back the previous one
     * c) look if we got a match and callback
     */
    RendezVous *rendezvous = object;

    if (len != sizeof(RendezVousPacket))
        return 0;

    RendezVousPacket *packet = (RendezVousPacket *)data;

    if (rendezvous->self_public && id_equal(packet->target_id, rendezvous->self_public))
        return 0;

    uint64_t now = unix_time();
    uint64_t now_floored = now - (now % RENDEZVOUS_INTERVAL);

    if (packet_is_wanted(rendezvous, packet, now_floored))
        return 1;

    if (packet_is_update(rendezvous, packet, now_floored, &ip_port))
        return 1;

    size_t i, matching = RENDEZVOUS_STORE_SIZE;

    /* if the data is a match to an existing, unmatched entry,
     * skip blocking */
    if (rendezvous->block_store_until >= now)
        for (i = 0; i < RENDEZVOUS_STORE_SIZE; i++)
            if (rendezvous->store[i].match == 1)
                if (rendezvous->store[i].recv_at == now_floored)
                    if (!memcmp(rendezvous->store[i].packet.hash_unspecific_half,
                                packet->hash_unspecific_half, HASHLEN / 2)) {
                        /* "encourage" storing */
                        rendezvous->block_store_until = now - 1;
                        matching = i;
                        break;
                    }

    size_t pos = RENDEZVOUS_STORE_SIZE;

    if (!rendezvous->block_store_until) {
        pos = 0;
    } else if (rendezvous->block_store_until < now) {
        /* find free slot to store into */
        for (i = 0; i < RENDEZVOUS_STORE_SIZE; i++)
            if ((rendezvous->store[i].match == 0) ||
                    is_timeout(rendezvous->store[i].recv_at, RENDEZVOUS_INTERVAL)) {
                pos = i;
                break;
            }

        if (pos == RENDEZVOUS_STORE_SIZE) {
            /* all full: randomize opening again */
            rendezvous->block_store_until = now_floored + RENDEZVOUS_INTERVAL + rand() % 30;

            if (matching < RENDEZVOUS_STORE_SIZE) {
                /* we got a match but can't store due to space:
                 * send replies, mark second slot */
                sendpacket(rendezvous->net, ip_port, &rendezvous->store[i].packet.type, sizeof(RendezVousPacket));
                sendpacket(rendezvous->net, rendezvous->store[i].ipp, data, sizeof(RendezVousPacket));

                rendezvous->store[i].match = 2;
                rendezvous->store[i].sent_at = now;
            }

            return 0;
        }
    } else {
        /* blocking */
        /* TODO: blacklist insisting publishers */
        return 0;
    }

    /* store */
    rendezvous->store[pos].recv_at = now_floored;
    rendezvous->store[pos].ipp = ip_port;
    rendezvous->store[pos].packet = *packet;

    rendezvous->store[pos].match = 1;
    rendezvous->store[pos].sent_at = 0;

    rendezvous->block_store_until = now + RENDEZVOUS_STORE_BLOCK;

    for (i = matching; i < RENDEZVOUS_STORE_SIZE; i++)
        if ((i != pos) && (rendezvous->store[i].match == 1))
            if (rendezvous->store[i].recv_at == now_floored)
                if (!memcmp(rendezvous->store[i].packet.hash_unspecific_half,
                            rendezvous->store[pos].packet.hash_unspecific_half, HASHLEN / 2)) {

                    send_replies(rendezvous, i, pos);

                    rendezvous->store[i].match = 2;
                    rendezvous->store[pos].match = 2;
                }

    return 0;
}

#ifdef ASSOC_AVAILABLE
RendezVous *new_rendezvous(Assoc *assoc, Networking_Core *net)
#else
RendezVous *new_rendezvous(DHT *dht, Networking_Core *net)
#endif
{
    if (!net)
        return NULL;

#ifdef ASSOC_AVAILABLE

    if (!assoc)
        return NULL;

#else

    if (!dht)
        return NULL;

#endif

    RendezVous *rendezvous = calloc(1, sizeof(*rendezvous));

    if (!rendezvous)
        return NULL;

#ifdef ASSOC_AVAILABLE
    rendezvous->assoc = assoc;
#else
    rendezvous->dht = dht;
#endif
    rendezvous->net = net;

    networking_registerhandler(net, NET_PACKET_RENDEZVOUS, rendezvous_network_handler, rendezvous);
    return rendezvous;
}

void rendezvous_init(RendezVous *rendezvous, uint8_t *self_public)
{
    if (rendezvous && self_public)
        rendezvous->self_public = self_public;
}

int rendezvous_publish(RendezVous *rendezvous, uint8_t *nospam_chksm, char *text, uint64_t timestamp,
                       RendezVous_callbacks *functions, void *data)
{
    if (!rendezvous || !text || !functions)
        return 0;

    if (!rendezvous->self_public)
        return 0;

    if (!functions->found_function)
        return 0;

    if (strlen(text) < RENDEZVOUS_PASSPHRASE_MINLEN)
        return 0;

    if (((timestamp % RENDEZVOUS_INTERVAL) != 0) || (timestamp + RENDEZVOUS_INTERVAL < unix_time()))
        return 0;

    char texttime[32 + strlen(text)];
    size_t texttimelen = sprintf(texttime, "%ld@%s", timestamp, text);
    crypto_hash_sha512(rendezvous->hash_unspecific_complete, (const unsigned char *)texttime, texttimelen);

    hash_specific_half_calc(rendezvous->hash_unspecific_complete, rendezvous->self_public, rendezvous->hash_specific_half);
    hash_specific_extra_insert(rendezvous->hash_specific_half, nospam_chksm);

    /* +30s: allow *some* slack in keeping the system time up to date */
    if (timestamp < unix_time())
        rendezvous->publish_starttime = timestamp;
    else
        rendezvous->publish_starttime = timestamp + RENDEZVOUS_PUBLISH_INITIALDELAY;

    rendezvous->timestamp = timestamp;
    rendezvous->functions = *functions;
    rendezvous->data = data;
    do_rendezvous(rendezvous);

    return 1;
}

void do_rendezvous(RendezVous *rendezvous)
{
    /* nothing to publish */
    if (!rendezvous->publish_starttime)
        return;

    uint64_t now = unix_time();

    if (rendezvous->publish_starttime < now) {
        rendezvous->publish_starttime = 0;
        uint64_t now_floored = now - (now % RENDEZVOUS_INTERVAL);

        /* timed out: stop publishing? */
        if (rendezvous->timestamp < now_floored) {
            rendezvous->timestamp = 0;

            if (rendezvous->functions.timeout_function)
                if (rendezvous->functions.timeout_function(rendezvous->data))
                    rendezvous->timestamp = now_floored;

#ifdef LOGGING

            if (!rendezvous->timestamp)
                loglog("rendezvous: timed out.\n");

#endif
        }

        if ((rendezvous->timestamp >= now_floored) && (rendezvous->timestamp < now_floored + RENDEZVOUS_INTERVAL)) {
            publish(rendezvous);

            /* on average, publish about once per 45 seconds */
            rendezvous->publish_starttime = now + RENDEZVOUS_PUBLISH_SENDAGAIN;
        }
    }
}

void kill_rendezvous(RendezVous *rendezvous)
{
    if (rendezvous) {
        networking_registerhandler(rendezvous->net, NET_PACKET_RENDEZVOUS, NULL, NULL);
        free(rendezvous);
    }
}
