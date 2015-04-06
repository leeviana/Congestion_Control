// TODO: Check EOF file handling.

#include <assert.h>
#include <errno.h>
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

#define ACK_PACKET 0
#define DATA_PACKET 1

#define ACK_PACKET_LENGTH sizeof(struct ack_packet)
#define PACKET_LENGTH sizeof(struct packet)

#define INF 12345678

#define INITIAL_C_WINDOW 1
#define TIMEOUT 1 // Defined in seconds.

#define MAX(x, y) (((x) > (y)) ? (x) : (y))
#define MIN(x, y) (((x) > (y)) ? (y) : (x))

struct reliable_state {
  conn_t *c;

  // Extra space.
  struct timespec start_time;

  int window;   // Max window size.
  int r_window; // Receiver advertised window length in packets.
  int c_window; // Congestion control window length in packets.
  int timeout;  // TODO: Do I need this?

  int window_seqno; // Sequence number of the first packet in our window.

  char *p_buf;                 // Sending window buffer.
  int p_buf_pkt_len;           // Length of the buffer in packets.
  struct timespec window_time; // Last send time of the window.

  int my_ackno; // Our current acknowledged packet #.
  int my_seqno; // Our current sequence number.
  int my_eof;   // Our EOF status.
 
  int r_ackno; // Last received remote ackno.
  int r_seqno; // Last received remote seqno.
  int r_eof;   // Remote EOF status.

  int slow_start; // Slow start status.
  int ssthresh;

  int dup_ack_count; // Triple duplicate ack counter.
};
rel_t *rel_list;


struct packet *send_packet(rel_t *r, int type, uint32_t ackno, uint32_t rwnd,
                           uint32_t seqno, char *data, int n) {
  assert(type == ACK_PACKET || type == DATA_PACKET);
  struct packet *p = malloc(PACKET_LENGTH);
  struct packet temp;
  int len = ACK_PACKET_LENGTH;
  if (type == DATA_PACKET) {
    len = PACKET_LENGTH - sizeof(temp.data) + n;
  }
  p->len = htons(len);
  p->ackno = htonl(ackno);
  p->rwnd = htonl(rwnd);
  p->seqno = htonl(seqno);
  if (data != NULL) memcpy(p->data, data, n);
  p->cksum = 0;
  p->cksum = cksum(p, len);
  if (r->r_window > 0) conn_sendpkt(r->c, p, len);
  r->r_window -= 1;

  return p;
}

void update_window(rel_t *r) {
  // If we're in slow start:
  //   If double slow start window < ssthresh:
  //     Set congestion window to double congestion window.
  //   Else:
  //     Set congestion window = ssthresh and set slow start = false.
  // Else:
  //   Set congestion window + 1.
  if (r->slow_start == 1) { // Slow start.
    if (r->c_window * 2 < r->ssthresh) {
      r->c_window *= 2;
    } else {
      r->c_window = r->ssthresh;
      r->slow_start = 0;
    }
  } else { // AIMD.
    r->c_window += 1;
  }
}

// Resizes p_buf given that n is larger than the current size of p_buf.
void resize_p_buf(rel_t *r, int n) {
  if (n <= r->p_buf_pkt_len) return;
  char *t = malloc(PACKET_LENGTH * n);
  memcpy(t, r->p_buf, PACKET_LENGTH * r->p_buf_pkt_len);
  free(r->p_buf);

  r->p_buf_pkt_len = n;
  r->p_buf = t;
}

// Creates a new reliable protocol session, returns NULL on failure.  Exactly
// one of c and ss should be NULL.  (ss is NULL when called from rlib.c, while
// c is NULL when this function is called from rel_demux).
rel_t * rel_create (conn_t *c, const struct sockaddr_storage *ss,
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
  rel_list = r;

  // Init. TODO: Check.
  r->window = cc->window;
  r->r_window = INF;
  r->c_window = INITIAL_C_WINDOW;
  r->timeout = cc->timeout;

  r->window_seqno = 0;

  // Allocate the sending window size.
  r->p_buf = malloc(PACKET_LENGTH * 1); // TODO: Min of fc_window & c_window.
  r->p_buf_pkt_len = 1;

  r->my_ackno = 1;
  r->my_seqno = 0;
  r->my_eof = 0;

  r->r_ackno = 1;
  r->r_seqno = 0;
  r->r_eof = 0;

  r->slow_start = 1; // Start in slow start.
  r->ssthresh = 32;  // Arbitrarily chose 32 b/c it divides evenly by 2.

  r->dup_ack_count = 0;

  clock_gettime(CLOCK_MONOTONIC, &(r->start_time)); // Set start time.
  return r;
}

void rel_destroy (rel_t *r) {
  conn_destroy (r->c);

  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  printf("Transmitted file in %ld seconds.\n",
         t.tv_sec - (r->start_time).tv_sec);

  // Free any other allocated memory here.
  free(r->p_buf);
}

// DON'T NEED TO IMPLEMENT.
void rel_demux (const struct config_common *cc,
                const struct sockaddr_storage *ss,
                packet_t *pkt, size_t len) {
}

void rel_recvpkt (rel_t *r, packet_t *pkt, size_t n) {
  // Triple duplicate acks, store last 3 ack numbers.

  // Fields below are the host order versions of the input packet.
  uint16_t pkt_cksum = pkt->cksum;
  uint16_t pkt_len = ntohs(pkt->len);
  uint32_t pkt_ackno = ntohl(pkt->ackno);
  uint32_t pkt_rwnd = ntohl(pkt->rwnd);
  uint32_t pkt_seqno = ntohl(pkt->seqno);

  pkt->cksum = 0;
  if (cksum(pkt, pkt_len) != pkt_cksum) {
    printf("Packet rejected due to bad checksum.\n");
    return;
  }
  if (pkt_len > sizeof(packet_t) || pkt_len < ACK_PACKET_LENGTH
      || pkt_len != n) {
    printf("Packet rejected due to invalid size.\n");
    return;
  }

  r->r_window = pkt_rwnd;

  if (pkt_len == ACK_PACKET_LENGTH) {
    // Received an acknowledge packet.
    // Store the highest one.
    // r->r_ackno = pkt_ackno > r->r_ackno ? pkt_ackno : r->r_ackno;
    if (pkt_ackno == r->r_ackno) r->dup_ack_count += 1;
    else r->dup_ack_count = 0;

    if (r->dup_ack_count >= 3) {
      // TODO: Triple duplicate ack, cut ssthresh, c_window to ssthresh, start
      // AIMD.
      // TODO: ssthresh gets cut to max c_window / 2 or 2?
      r->ssthresh = MAX(r->c_window / 2, 2);
      r->c_window = r->ssthresh;
      r->slow_start = 0;

      // Fast retransmit the last packet.      
      int s_window_start = r->r_ackno - r->window_seqno;
      struct packet *p_ptr = (struct packet *)
        (r->p_buf + PACKET_LENGTH * s_window_start);
      int p_len = ntohs(p_ptr->len);
      if (r->r_window > 0) conn_sendpkt(r->c, p_ptr, p_len);
      r->r_window -= 1;

      r->dup_ack_count = 0;
    }
    r->r_ackno = MAX(pkt_ackno, r->r_ackno);
    if (r->my_seqno == pkt_ackno + 1)
      update_window(r); // Update congestion vars.

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
    if (pkt_len == PACKET_LENGTH - sizeof(temp.data)) {
      // EOF packet received.
      r->r_eof = 1;
    }
    // If the packet sequence number is the last packet number the remote
    // acked, we write out and increment our ackno.
    if (pkt_seqno == r->my_ackno) {
      // If the packet is next in sequence.
      r->my_ackno += 1;
      r->r_seqno = pkt_seqno;
      // r->outbuf_empty = 0;
      conn_output(r->c, pkt->data, pkt_data_len);
    }
    // Acknowledge the last packet we accepted.
    struct packet *p = send_packet(r, ACK_PACKET, r->my_ackno,
                                   conn_bufspace(r->c) / PACKET_LENGTH,
                                   0, NULL, 0);
    free(p);
  }
}

void rel_read (rel_t *s) {
  if (s->c->sender_receiver == RECEIVER) {
    // If we already sent the EOF, return.
    if (s->my_eof == 1) return;
    // If we're in receiver mode and haven't sent the EOF yet, create the
    // packet, send it, and copy it to our window. The rest of the
    // implementation will deal with waiting for the ack.
    else {
      s->my_seqno += 1;
      struct packet *p = send_packet(s, DATA_PACKET, s->my_ackno,
                                     conn_bufspace(s->c) / PACKET_LENGTH,
                                     s->my_seqno, NULL, 0);
      memcpy(s->p_buf, p, PACKET_LENGTH);
      free(p);
    }
    return;
  }

  // The sending window size is the minimum of our congestion window and the
  // advertised receiving window.
  int s_window = MIN(s->c_window, s->r_window);
  s_window = MIN(s->window, s_window); // TODO: Clip window size if need to.
  resize_p_buf(s, s_window); // Resize the buffer if we need to.
  
  // Sender mode.
  // Basic assertions.
  assert(s_window > 0);

  // Read in a single sliding window.
  // If the sliding window hasn't been sent, don't overwrite it.
  // If only part of sliding window has been acknowledged, memcpy it to
  // the front.

  // We don't read if our window is full. That means there are no packets in
  // our window have been acked.

  // Our window is full if my_seqno + 1 - r_ackno >= window
  // If our sequence number - remote acknowledge number is the window size.
  // Don't read if we haven't received any acks yet and the window is full.
  // TODO: Make sure second argument is right.
  // We do read if there are acked packets sitting at the front of our buffer.
  // This check is required now that the buffer is independent of the window
  // size.
  if (s->my_seqno + 1 - s->r_ackno >= s_window
      && s->r_ackno == s->window_seqno)
    return;

  // TODO: Expand buffer size according to s_window.

  // If our window isn't full, it means that either the entire window is
  // available to read, or part of it is.
  // If only part of the window is available (it must be the rightmost
  // packets), we need to shift out the leftmost packets that have already been
  // acked.
  // The entire window is available if the r_ackno is 1 window size above the
  // current window_seqno. Otherwise, the offset into the window can be derived
  // by r_ackno - window_seqno.

  // TODO: This used to be handled based on the window size, but since now
  // window size does not guarantee buffer size, we packet shift by the buffer
  // length.
  int i = 0; // Buffer frame index, assume whole window available initially.

  // If r_ackno > window_seqno, then there are acked packets at the beginning
  // of our buffer.
  // If our buffer begins with acked packets, we first move those out.
  if (s->my_seqno > 0) {
    // Need to deal with window = 1 case apparently...
    if (s->r_ackno > s->window_seqno) {
      int offset = s->r_ackno - s->window_seqno;
      //assert(offset <= s_window);
      // Copy last window - p_buf_pkt_len packets to the front of the buffer.
      memcpy(s->p_buf, s->p_buf + PACKET_LENGTH * offset,
	     PACKET_LENGTH * (s->p_buf_pkt_len - offset));
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
  for (; i < s_window; i++) {
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

    s->my_seqno += 1; // Increment our seqno right before a packet is read.
    struct packet *p = send_packet(s, DATA_PACKET, s->my_ackno,
                                   conn_bufspace(s->c) / PACKET_LENGTH,
                                   s->my_seqno, p_ptr->data, n);
    memcpy(p_ptr, p, PACKET_LENGTH); // Copy the whole packet to the buffer.
    free(p);
  }
  clock_gettime(CLOCK_MONOTONIC, &(s->window_time)); // Reset window clock.
}

void rel_output (rel_t *r) {
  // TODO: Ack last received packet.
}

void rel_timer () {
  rel_t *r_ptr = rel_list;
  // Check destruction conditions for each connection.
  // TODO: Talk to others about output draining condition.
  if (r_ptr->my_eof && r_ptr->r_eof
      && (r_ptr->r_ackno == r_ptr->my_seqno + 1)) {
    rel_destroy(r_ptr);
    return;
  }

  // Timeout: Can happen wherever... If our last received ack is not equal to
  // our last sent packet, we timeout.
  // Need timer from when you send the window.
  // Retransmit any packets that need to be retransmitted.
  if (r_ptr->my_seqno == 0) return;

  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  long now = t.tv_sec;
  long old = (r_ptr->window_time).tv_sec;
  // If we haven't received acks in time, resend the packets.
  if ((r_ptr->r_ackno < r_ptr->my_seqno + 1)
      && (now - old > TIMEOUT)) {
    r_ptr->ssthresh = r_ptr->c_window / 2;
    r_ptr->c_window = INITIAL_C_WINDOW;
    int j;
    // Send the whole window again.
    int s_window = MIN(r_ptr->c_window, r_ptr->r_window);
    int s_window_start = r_ptr->r_ackno - r_ptr->window_seqno;
    int s_window_end = r_ptr->my_seqno - r_ptr->window_seqno + 1;
    for (j = s_window_start;
         j < MIN(s_window_end, s_window_start + s_window); j++) {
      struct packet *p_ptr = (struct packet *)
        (r_ptr->p_buf + PACKET_LENGTH * j);
      int p_len = ntohs(p_ptr->len);
      if (r_ptr->r_window > 0) conn_sendpkt(r_ptr->c, p_ptr, p_len);
      r_ptr->r_window -= 1;
    }
    clock_gettime(CLOCK_MONOTONIC, &(r_ptr->window_time));
  }
}
