#include <assert.h>
#include <math.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "rlib.h"

#define ACK_PACKET_LENGTH sizeof(struct ack_packet)
#define PACKET_LENGTH sizeof(struct packet)

#define MAX(x, y) (((x) > (y)) ? (x) : (y))

struct reliable_state {
  rel_t *next; // Linked list.
  rel_t **prev;

  conn_t *c; // Connection object.

  // Extra space.
  int window;       // Sending window length in packets;
  int timeout;

  int window_seqno; // Sequence number of first packet.

  char *p_buf;                 // Sending window buffer.
  struct timespec window_time; // Last sent times.

  int my_ackno; // Our current acknowledged packet #.
  int my_seqno; // Our current sequence number.
  int my_eof;  // Our EOF status.
 
  int r_ackno; // Last received remote ackno.
  int r_seqno; // Last received remote seqno.
  int r_eof;  // Remote EOF status.

};
rel_t *rel_list;

// Creates a new reliable protocol session, returns NULL on failure. Exactly
// one of c and ss should be NULL (ss is NULL when called from rlib.c, while c
// is NULL when this function is called from rel_demux).
rel_t *rel_create (conn_t *c, const struct sockaddr_storage *ss,
                   const struct config_common *cc) {
  rel_t *r;

  r = xmalloc(sizeof (*r));
  memset(r, 0, sizeof (*r));

  if (!c) {
    c = conn_create(r, ss);
    if (!c) {
      free(r);
      return NULL;
    }
  }

  r->c = c;
  r->next = rel_list;
  r->prev = &rel_list;
  if (rel_list)
    rel_list->prev = &r->next;
  rel_list = r;

  // Init.
  r->window = cc->window;
  r->timeout = cc->timeout;

  r->window_seqno = 0;

  // Allocate the sending window size.
  r->p_buf = malloc(PACKET_LENGTH * (cc->window));

  r->my_ackno = 1;
  r->my_seqno = 0;
  r->my_eof = 0;

  r->r_ackno = 1;
  r->r_seqno = 0;
  r->r_eof = 0;

  return r;
}

void rel_destroy (rel_t *r) {
  if (r->next) r->next->prev = r->prev;
  *r->prev = r->next;
  conn_destroy(r->c);

  // Free any other allocated memory here.
  free(r->p_buf);
}

// This function only gets called when the process is running as a server and
// must handle connections from multiple clients. You have to look up the rel_t
// structure based on the address in the sockaddr_storage passed in. If this is
// a new connection (sequence number 1), you will need to allocate a new conn_t
// using rel_create() (Pass rel_create NULL for the conn_t, so it will know to
// allocate a new connection).
// TODO: DON'T NEED TO IMPLEMENT.
void rel_demux (const struct config_common *cc,
                const struct sockaddr_storage *ss,
                packet_t *pkt, size_t len) {
}

void rel_recvpkt (rel_t *r, packet_t *pkt, size_t n) {
  // Fields below are the host order versions of the input packet.
  uint16_t pkt_cksum = pkt->cksum;
  uint16_t pkt_len = ntohs(pkt->len);
  uint32_t pkt_ackno = ntohl(pkt->ackno);
  uint32_t pkt_seqno = ntohl(pkt->seqno);

  pkt->cksum = 0;
  if (cksum(pkt, n) != pkt_cksum) {
    //printf("Packet rejected due to bad checksum.");
    return;
  }
  if (pkt_len > sizeof(packet_t) || pkt_len < ACK_PACKET_LENGTH
      || pkt_len != n) {
    //printf("Packet rejected due to invalid size.");
    return;
  }

  if (pkt_len == ACK_PACKET_LENGTH) {
    // Received an acknowledge packet.
    // Store the highest one.
    r->r_ackno = pkt_ackno > r->r_ackno ? pkt_ackno : r->r_ackno;
    assert(r->r_ackno <= r->my_seqno + 1);
    // TODO: Should we limit reads?
    rel_read(r); // Read if we can.
  } else {
    // Received a (theoretically) valid data packet.
    struct packet temp; // Bullshit so I can get packet header length.
    int pkt_data_len = pkt_len - (PACKET_LENGTH - sizeof(temp.data));
    // Only send the acknowledge if we have the space for packet data.
    if (conn_bufspace(r->c) < pkt_data_len) {
      return;
    }
    struct ack_packet ack;
    ack.len = htons(ACK_PACKET_LENGTH);
    if (pkt_len == PACKET_LENGTH - sizeof(temp.data)) {
      // EOF packet received.
      r->r_eof = 1;
    }
    if (pkt_seqno == r->my_ackno) {
      // If the packet is next in sequence.
      r->my_ackno += 1;
      r->r_seqno = pkt_seqno;
      // r->outbuf_empty = 0;
      conn_output(r->c, pkt->data, pkt_data_len);
    }
    // Acknowledge the last packet we accepted.
    ack.ackno = htonl(r->my_ackno);
    ack.cksum = 0;
    ack.cksum = cksum(&ack, ACK_PACKET_LENGTH);
    conn_sendpkt(r->c, (packet_t *) &ack, ACK_PACKET_LENGTH);
  }
}


void rel_read (rel_t *s) {
  // Read in a single sliding window.
  // If the sliding window hasn't been sent, don't overwrite it.
  // If only part of sliding window has been acknowledged, memcpy it to
  // the front.

  // We don't read if our window is full. That means there are no packets in
  // our window have been acked.

  // Our window is full if my_seqno + 1 - r_ackno == window
  // If our sequence number - remote acknowledge number is the window size.
  if (s->my_seqno + 1 - s->r_ackno == s->window) // If we haven't received an ack yet don't read.
    return;

  // If our window isn't full, it means that either the entire window is
  // available to read, or part of it is.
  // If only part of the window is available (it must be the rightmost
  // packets), we need to shift out the leftmost packets that have already been
  // acked.
  // The entire window is available if the r_ackno is 1 window size above the
  // current window_seqno. Otherwise, the offset into the window can be derived
  // by r_ackno - window_seqno.
  int i = 0; // Buffer frame index, assume whole window available initially.

  // If r_ackno > window_seqno, then there are acked packets at the beginning
  // of our buffer.
  // If our buffer begins with acked packets, we first move those out.
  // fprintf(stderr, "my_seqno: %d, my_ackno: %d, r_seqno: %d, r_ackno: %d, window_seqno: %d\n", s->my_seqno, s->my_ackno, s->r_seqno, s->r_ackno, s->window_seqno);
  if (s->my_seqno > 0) {
    // Need to deal with window = 1 case apparently...
    if (s->r_ackno > s->window_seqno) {
      int offset = s->r_ackno - s->window_seqno;
      assert(offset <= s->window);
      // Copy last window - offset packets to the front of the buffer.
      memcpy(s->p_buf, s->p_buf + PACKET_LENGTH * offset,
	     PACKET_LENGTH * (s->window - offset));
    }
  }
  // The window should at this point start with the last acked packet number.
  s->window_seqno = s->r_ackno;
  // Skip all slots with unacknowledged packets.
  i += s->my_seqno + 1 - s->window_seqno;

  // Region of buffer that is not acked is given by:
  // r_ackno - w_seqno : my_seqno - window_seqno

  // Read as many packets as we can into the buffer.
  struct packet p;
  for (; i < s->window; i++) {
    // Once we read an EOF it'll be put in the buffer and sent. Afterwards we
    // return no matter what since we're done.
    if (s->my_eof == 1) break;

    struct packet *p_ptr = (struct packet *) (s->p_buf + PACKET_LENGTH * i);
    int n = conn_input(s->c, p_ptr->data, sizeof(p.data));
    if (n == 0) break; // If there's no data available.
    if (n < 0) { // If we hit an EOF mark it and set data length = 0.
      s->my_eof = 1;
      n = 0; // Send a packet of length 12.
    }

    int p_len = PACKET_LENGTH - sizeof(p.data) + n; // Corrected packet size.
    p.len = htons(p_len);
    p.ackno = htonl(s->my_ackno);
    s->my_seqno += 1; // Increment our seqno right before a packet is read.
    p.seqno = htonl(s->my_seqno);
    memcpy(&p.data, p_ptr->data, n);
    p.cksum = 0;
    p.cksum = cksum(&p, p_len);
    memcpy(p_ptr, &p, PACKET_LENGTH); // Copy the whole packet to the buffer.
    conn_sendpkt(s->c, &p, p_len);
  }
  clock_gettime(CLOCK_MONOTONIC, &(s->window_time)); // Reset window clock.
}

void rel_output (rel_t *r) {
  // TALK TO OTHERS ABOUT THIS...
  /*int i;
  for (i = r->my_ackno; i <= r->r_seqno; i++) {
    struct ack_packet ack;
    ack.len = htons(ACK_PACKET_LENGTH);
    // Acknowledge the last packet we received.
    ack.ackno = htonl(i + 1);
    ack.cksum = 0;
    ack.cksum = cksum(&ack, ACK_PACKET_LENGTH);
    conn_sendpkt(r->c, (packet_t *) &ack, ACK_PACKET_LENGTH);
  }*/
  //r->my_ackno = r->r_seqno + 1;
}

void rel_timer () {
  rel_t *r_ptr = rel_list;
  // Check destruction conditions for each connection.
  for(; r_ptr != NULL;) {
    rel_t *n_ptr = r_ptr->next;
    // TODO: Talk to others about output draining condition.
    if (r_ptr->my_eof && r_ptr->r_eof
	&& (r_ptr->r_ackno == r_ptr->my_seqno + 1)) {
      rel_destroy(r_ptr);
    }
    r_ptr = n_ptr;
  }

  for (r_ptr = rel_list; r_ptr != NULL; r_ptr = r_ptr->next) {
    // Retransmit any packets that need to be retransmitted.
    if (r_ptr->my_seqno == 0) continue;


    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    long now = t.tv_sec;
    long old = (r_ptr->window_time).tv_sec;
    // If we haven't received acks in time, resend the packets.
    if ((r_ptr->r_ackno < r_ptr->my_seqno + 1)
	&& (now - old > r_ptr->timeout / 1000)) {
      int j;
      // Send the whole window again.
      for (j = r_ptr->r_ackno - r_ptr->window_seqno; j < r_ptr->my_seqno - r_ptr->window_seqno + 1; j++) {
	struct packet p;
        struct packet *p_ptr = (struct packet *)
          (r_ptr->p_buf + PACKET_LENGTH * j);
	int p_len = ntohs(p_ptr->len);
	memcpy(&p, p_ptr, p_len);
	conn_sendpkt(r_ptr->c, &p, p_len);
      }
      clock_gettime(CLOCK_MONOTONIC, &(r_ptr->window_time));
    }
  }
}
