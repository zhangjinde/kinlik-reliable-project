#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <assert.h>
#include <poll.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <netinet/in.h>

#include "rlib.h"
#include "buffer.h"

#define ACK_LEN 8
#define HEADER_LEN 12

struct reliable_state {
    rel_t *next;			/* Linked list for traversing all connections */
    rel_t **prev;

    conn_t *c;			/* This is the connection object */

    // sender
    buffer_t* send_buffer;
    uint32_t send_una; // lowest seqno of outstanding frames = max(send_una, ackno)
    uint32_t send_nxt; // seqno of next frame to send out
    int send_eof;
    int send_eof_acked;

    // receiver
    buffer_t* rec_buffer;
    uint32_t rec_nxt; // next seqno expected
    int rec_eof;

    int window; // sender & receiver window size
    int timeout; // retransmission timeout in ms
};
rel_t *rel_list;

int is_dup_pkt(rel_t *r, packet_t *pkt);
long now_ms();
void send_ack_pkt(rel_t *r, uint32_t ackno);
packet_t *create_data_pkt(rel_t *r);
uint32_t send_wnd(rel_t *r);

/* Creates a new reliable protocol session, returns NULL on failure.
* ss is always NULL */
rel_t *
rel_create (conn_t *c, const struct sockaddr_storage *ss,
const struct config_common *cc)
{
    rel_t *r;

    r = xmalloc (sizeof (*r));
    memset (r, 0, sizeof (*r));

    if (!c) {
        c = conn_create (r, ss);
        if (!c) {
            free (r);
            return NULL;
        }
    }

    r->c = c;
    r->next = rel_list;
    r->prev = &rel_list;
    if (rel_list)
    rel_list->prev = &r->next;
    rel_list = r;

    // sender
    r->send_buffer = xmalloc(sizeof(buffer_t));
    r->send_buffer->head = NULL;
    r->send_una = 1;
    r->send_nxt = 1;
    r->send_eof = 0;
    r->send_eof_acked = 0;

    // receiver
    r->rec_buffer = xmalloc(sizeof(buffer_t));
    r->rec_buffer->head = NULL;
    r->rec_nxt = 1;
    r->rec_eof = 0;

    // others
    r->window = cc->window;
    r->timeout = cc->timeout;

    return r;
}

void
rel_destroy (rel_t *r)
{
    if (r->next) {
        r->next->prev = r->prev;
    }
    *r->prev = r->next;
    conn_destroy (r->c);

    /* Free any other allocated memory here */
    buffer_clear(r->send_buffer);
    free(r->send_buffer);
    buffer_clear(r->rec_buffer);
    free(r->rec_buffer);
    free(r);
}

// n is the expected length of pkt
void
rel_recvpkt (rel_t *r, packet_t *pkt, size_t n)
{
    // check length corruption
    if (ntohs(pkt->len) != n) return;

    // check cksum corruption
    uint16_t receivedCksum = pkt->cksum;
    pkt->cksum = 0;
    if (receivedCksum != cksum(pkt, n)) return;
    pkt->cksum = receivedCksum;

    // sender: handle ACK packet
    if (n == ACK_LEN) {
        // no packets remain un-acked
        if (buffer_size(r->send_buffer) == 0) return;

        // new packets are acked
        if (ntohl(pkt->ackno) > r->send_una) {
            // increment sender un-acked
            r->send_una = ntohl(pkt->ackno); // = max(send_una, ackno)
            buffer_remove(r->send_buffer, r->send_una);
        }

        // handle EOF ACK
        if (r->send_eof) {
            if (buffer_size(r->send_buffer) == 0) r->send_eof_acked = 1;
        }

        if (r->send_eof_acked && r->rec_eof && buffer_size(r->send_buffer) == 0) {
            rel_destroy(r);
        } else if (send_wnd(r) < r->window) {
            rel_read(r);
        }
        return;
    }

    // receiver: handle data packet
    // resend ACK for duplicate packets
    if (is_dup_pkt(r, pkt)) {
        send_ack_pkt(r, ntohl(pkt->seqno) + 1);
        return;
    }

    if (r->rec_eof) {
        if (r->send_eof_acked) rel_destroy(r);
        return;
    }

    // ignore out-of-window packets
    if (ntohl(pkt->seqno) >= r->rec_nxt + r->window) return;

    if (buffer_size(r->rec_buffer) == r->window) {
        rel_output(r);
    }

    // store in buffer if not already there
    if (!buffer_contains(r->rec_buffer, ntohl(pkt->seqno))) {
        buffer_insert(r->rec_buffer, pkt, now_ms());
    }

    rel_output(r);
}

void
rel_read (rel_t *s)
{
    assert(send_wnd(s) <= s->window);
    // read and send as long as:
    // 1. send window is not full, and
    // 2. no EOF has been read from stdin
    while ((send_wnd(s) < s->window) && !s->send_eof) {
        packet_t *pkt = create_data_pkt(s);

        // if stdin is empty, return
        if (pkt == NULL) return;

        // set flag if EOF has been read from stdin
        if (ntohs(pkt->len) == HEADER_LEN) s->send_eof = 1;

        conn_sendpkt(s->c, pkt, ntohs(pkt->len));
        buffer_insert(s->send_buffer, pkt, now_ms());
        free(pkt);
        s->send_nxt++;
    }
}

void
rel_output (rel_t *r)
{
    // no packets to output
    if (buffer_size(r->rec_buffer) == 0) return;

    if (r->rec_eof) {
        if (r->send_eof_acked) rel_destroy(r);
        return;
    }

    buffer_node_t* node = buffer_get_first(r->rec_buffer);
    size_t free_bytes = conn_bufspace(r->c);
    size_t used_bytes = 0;

    for (uint32_t i = 0; i < r->window && node != NULL; i++) {
        if (!buffer_contains(r->rec_buffer, r->rec_nxt) || r->rec_nxt != ntohl(node->packet.seqno)) break;

        uint16_t payload_size = ntohs(node->packet.len) - HEADER_LEN;
        if (used_bytes + payload_size > free_bytes) break;
        if (payload_size == 0) r->rec_eof = 1;
        int output = conn_output(r->c, node->packet.data, payload_size);
        assert(output >= 0);
        used_bytes += payload_size;
        r->rec_nxt++;
        node = node->next;
        buffer_remove_first(r->rec_buffer);
    }
    
    // send ACK
    send_ack_pkt(r, r->rec_nxt);
}

void
rel_timer ()
{
    // Go over all reliable senders, and have them send out
    // all packets whose timer has expired
    rel_t *current = rel_list;
    while (current != NULL) {
        for (buffer_node_t* node = buffer_get_first(current->send_buffer); node != NULL; node = node->next) {
            long _now_ms = now_ms();
            if (_now_ms - node->last_retransmit > current->timeout) {
                node->last_retransmit = _now_ms;
                conn_sendpkt(current->c, &node->packet, ntohs(node->packet.len));
            }
        }
        current = current->next;
    }
}

inline int is_dup_pkt(rel_t *r, packet_t *pkt) {
    return ntohl(pkt->seqno) < r->rec_nxt;
}

// get current time in milliseconds
long now_ms() {
    struct timeval now;
    gettimeofday(&now, NULL);
    return now.tv_sec * 1000 + now.tv_usec / 1000;
}

// ackno should be in host order
void send_ack_pkt(rel_t *r, uint32_t ackno) {
    // ACK packet is smaller than a data packet
    struct ack_packet *ack = xmalloc(sizeof(struct ack_packet));
    ack->cksum = 0;
    ack->len = htons(ACK_LEN);
    ack->ackno = htonl(ackno);
    ack->cksum = cksum(ack, ACK_LEN);

    conn_sendpkt(r->c, (packet_t *) ack, ACK_LEN); // explicit casting
    free(ack);
}

// NOTE: must not be called when sender window is full
packet_t *create_data_pkt(rel_t *r) {
    packet_t *pkt;
    pkt = xmalloc(sizeof(packet_t));
    int input = conn_input(r->c, pkt->data, 500);

    if (input == 0) {
        free(pkt);
        return NULL;
    }
    
    pkt->cksum = 0;
    pkt->len = htons(HEADER_LEN + ((input == -1) ? 0 : input));
    pkt->ackno = htonl(r->rec_nxt);
    pkt->seqno = htonl(r->send_nxt);
    pkt->cksum = cksum(pkt, ntohs(pkt->len));
    return pkt;
}

inline uint32_t send_wnd(rel_t *r) {
    return r->send_nxt - r->send_una;
}
