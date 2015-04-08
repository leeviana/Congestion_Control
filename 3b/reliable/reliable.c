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
#define TIMEOUT 20 // Defined in 10s of milliseconds.

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
  int p_buf_seqno;
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

void update_packet(rel_t *r, struct packet *p) {
  int len = htons(p->len);
  p->ackno = htonl(r->my_ackno);
  p->rwnd = htonl(conn_bufspace(r->c) / PACKET_LENGTH);
  p->cksum = 0;
  p->cksum = cksum(p, len);
}

struct packet *build_packet(rel_t *r, int type, uint32_t ackno, uint32_t rwnd,
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
  if (data != NULL) memcpy(p->data, data, sizeof(temp.data));
  p->cksum = 0;
  p->cksum = cksum(p, len);

  return p;
}

void send_packet(rel_t *r, int type, struct packet *p) {
  int len = ntohs(p->len);
  if (type == DATA_PACKET) {
    if (r->r_window > 0) r->r_window -= 1;
    //printf("%.*s\n", len - 16, p->data);
  }
  conn_sendpkt(r->c, p, len);
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
    if (r->c_window + PACKET_LENGTH < r->ssthresh) {
      r->c_window += PACKET_LENGTH;
    } else {
      r->c_window = r->ssthresh;
      r->slow_start = 0;
    }
  } else { // AIMD.
    int diff = MAX(PACKET_LENGTH * PACKET_LENGTH / r->c_window, 1);
    r->c_window += diff;
  }
  //printf("Updated to %d, slow_start: %d, ssthresh: %d.\n", r->c_window, r->slow_start, r->ssthresh);
  // Clip the congestion window to the max window argument.
  r->c_window = MIN(r->c_window, r->r_window * PACKET_LENGTH);
  r->c_window = MIN(r->c_window, r->window * PACKET_LENGTH);
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
  r->c_window = PACKET_LENGTH;
  r->timeout = 0;

  r->window_seqno = 0;

  // Allocate the sending window size.
  r->p_buf = malloc(PACKET_LENGTH * (r->window + 1)); // TODO: Min of fc_window & c_window.
  r->p_buf_seqno = 0;

  r->my_ackno = 1;
  r->my_seqno = 0;
  r->my_eof = 0;

  r->r_ackno = 1;
  r->r_seqno = 0;
  r->r_eof = 0;

  r->slow_start = 1; // Start in slow start.
  r->ssthresh = 256 * PACKET_LENGTH;  // TODO: Window size okay?

  r->dup_ack_count = 0;

  clock_gettime(CLOCK_MONOTONIC, &(r->start_time)); // Set start time.

  // Read so we send the EOF immediately.
  if (c->sender_receiver == RECEIVER) {
    rel_read(r);
  }
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

int s_window(rel_t *r) {
  int size = MIN(r->c_window / PACKET_LENGTH, r->r_window);
  size = MAX(size, 1);
  return size;
}

void send_window(rel_t *r) {
  // Send an entire window.
  int size = s_window(r);
  int s_window_start = r->my_seqno + 1 - r->window_seqno;

  int j;
  for (j = s_window_start; j < MIN(size, r->p_buf_seqno - r->window_seqno + 1);
       j++) {
    struct packet *p_ptr = (struct packet *)
      (r->p_buf + PACKET_LENGTH * j);
    update_packet(r, p_ptr);
    send_packet(r, DATA_PACKET, p_ptr);
    //printf("send: %.*s", ntohs(p_ptr->len) - 16, p_ptr->data);
    r->my_seqno = ntohl(p_ptr->seqno);
    //printf("Sent %d.\n", ntohl(p_ptr->seqno)); fflush(stdout);
  }
  r->timeout = 0;
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
    if (pkt_ackno == r->r_ackno) {
      r->dup_ack_count += 1;
      if (r->dup_ack_count >= 3) {
        r->c_window = MAX(r->c_window / 2, PACKET_LENGTH);
	r->slow_start = 0;

	// Fast retransmit the last packet.      
	int s_window_start = r->r_ackno - r->window_seqno;
	struct packet *p_ptr = (struct packet *)
	  (r->p_buf + PACKET_LENGTH * s_window_start);
        update_packet(r, p_ptr);

        r->my_seqno = ntohl(p_ptr->seqno);
        send_window(r);
	//send_packet(r, DATA_PACKET, p_ptr);
        printf("Sent %d (trip dup).\n", ntohl(p_ptr->seqno));

	r->dup_ack_count = 0;
        r->timeout = 0;
      }
      return;
    } else r->dup_ack_count = 0;

    //printf("Got ack: %d.\n", r->r_ackno); fflush(stdout);
    r->r_ackno = MAX(pkt_ackno, r->r_ackno);
    // Only update if the receiving window is > congestion window.
    //printf("my_seqno: %d, pkt_ackno: %d\n", r->my_seqno, pkt_ackno);

    rel_read(r);
    send_window(r);
    update_window(r);

  } else {
    // Received a (theoretically) valid data packet.
    struct packet temp; // Bullshit so I can get packet header length.
    int pkt_data_len = pkt_len - (PACKET_LENGTH - sizeof(temp.data));
    //printf("Got %d, len: %d.\n", pkt_seqno, pkt_data_len); fflush(stdout);
    // Only send the acknowledge if we have the space for packet data.
    if (conn_bufspace(r->c) < pkt_data_len) {
      return;
    }
    if (pkt_len == PACKET_LENGTH - sizeof(temp.data)) {
      // EOF packet received.
      r->r_eof = 1;
    }
    // If the packet sequence number is the next is sequence, write out and
    // increment our ack.
    if (pkt_seqno == r->r_seqno + 1) {
      // If the packet is next in sequence.
      r->my_ackno += 1;
      r->r_seqno = pkt_seqno;
      // r->outbuf_empty = 0;
      //printf("Wrote %d.\n", pkt_seqno);
      //printf("%.*s\n", pkt_data_len, pkt->data);
      //conn_output(r->c, pkt->data, pkt_data_len);
    }
    // Acknowledge the last packet we accepted.
    struct packet *p = build_packet(r, ACK_PACKET, r->my_ackno,
                                    conn_bufspace(r->c) / PACKET_LENGTH,
                                    0, NULL, 0);
    send_packet(r, ACK_PACKET, p);
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
      s->window_seqno += 1;
      s->p_buf_seqno += 1;
      struct packet *p = build_packet(s, DATA_PACKET, s->my_ackno,
                                      conn_bufspace(s->c) / PACKET_LENGTH,
                                      s->my_seqno, NULL, 0);
      send_packet(s, DATA_PACKET, p);
      memcpy(s->p_buf, p, PACKET_LENGTH);
      free(p);
    }
    s->my_eof = 1;
    s->timeout = 0;
    return;
  }

  // The sending window size is the minimum of our congestion window and the
  // advertised receiving window.
  int size = s_window(s);
  
  // Sender mode.
  // Basic assertions.
  assert(size > 0);

  // Read in a single sliding window.
  // If the sliding window hasn't been sent, don't overwrite it.
  // If only part of sliding window has been acknowledged, memcpy it to
  // the front.

  // We don't read if our window is full. That means there are no packets in
  // our window have been acked.


  // If our buffer contains unacked packets, we do not allow read to run. Our
  // implementation treats buffers as rounds for simplicity. The buffer is only
  // filled up to a certain s_window, and if the window changes the rest of it
  // is not populated immediately for simplicity.
  if (s->p_buf_seqno - s->r_ackno + 1 == s->window) return;

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
  if (s->p_buf_seqno > 0) {
    // Need to deal with window = 1 case apparently...
    if (s->r_ackno > s->window_seqno) {
      int offset = s->r_ackno - s->window_seqno;
      //assert(offset <= s_window);
      // Copy last window - p_buf_pkt_len packets to the front of the buffer.
      memmove(s->p_buf, s->p_buf + PACKET_LENGTH * offset,
	     PACKET_LENGTH * (s->window - offset));
      //printf("Copied %d packets to to offset: %d.\n", s->window - offset, offset);
      //printf("Starts w/ %.*s\n", 1000, ((struct packet *) s->p_buf)->data);
    }
  }
  // The window should at this point start with the last acked packet number.
  s->window_seqno = s->r_ackno;
  // Skip all slots with unacknowledged packets.
  i += s->p_buf_seqno + 1 - s->window_seqno;

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

    s->p_buf_seqno += 1; // Increment our seqno right before a packet is read.
    // Can't fill in some values this early.
    struct packet *p = build_packet(s, DATA_PACKET, 0, 0,
                                    s->p_buf_seqno, p_ptr->data, n);
    // send_packet(s, DATA_PACKET, p);
    memcpy(p_ptr, p, PACKET_LENGTH); // Copy the whole packet to the buffer.
    //printf("%.*s", n, p_ptr->data); fflush(stdout);
    free(p);
    //printf("Read %d into slot %d, len: %d.\n", s->p_buf_seqno, i, n); fflush(stdout);
  }

  if (s->my_seqno == 0) send_window(s);
}

void rel_output (rel_t *r) {
  // TODO: Ack last received packet.
  printf("WTF!!!\n"); fflush(stdout);
}

void rel_timer () {
  rel_t *r_ptr = rel_list;
  // Check destruction conditions for each connection.
  // TODO: Talk to others about output draining condition.
  //printf("my_eof: %d, r_eof: %d, r_ackno: %d, my_seqno: %d\n", r_ptr->my_eof, r_ptr->r_eof, r_ptr->r_ackno, r_ptr->my_seqno);
  printf("r_window: %d, c_window: %d\n", r_ptr->r_window, r_ptr->c_window);
  if (r_ptr->my_eof && r_ptr->r_eof
      && (r_ptr->r_ackno == r_ptr->p_buf_seqno + 1)) {
    rel_destroy(r_ptr);
    return;
  }

  r_ptr->timeout += 1;

  // Timeout: Can happen wherever... If our last received ack is not equal to
  // our last sent packet, we timeout.
  // Need timer from when you send the window.
  // Retransmit any packets that need to be retransmitted.
  if (r_ptr->my_seqno == 0) return;

  // If we haven't received acks in time, resend the packets.
  if ((r_ptr->r_ackno < r_ptr->my_seqno + 1)
      && (r_ptr->timeout > TIMEOUT)) {
    r_ptr->ssthresh = MAX(r_ptr->c_window / 2, PACKET_LENGTH);
    r_ptr->c_window = PACKET_LENGTH;
    r_ptr->slow_start = 1;

    rel_read(r_ptr); // TODO: Need to remove?
    struct packet *p_ptr = (struct packet *) r_ptr->p_buf;
    update_packet(r_ptr, p_ptr);
    send_packet(r_ptr, DATA_PACKET, p_ptr);
    r_ptr->my_seqno = ntohl(p_ptr->seqno);
    printf("Sent %d (timeout).\n", ntohl(p_ptr->seqno));
    r_ptr->timeout = 0;
    r_ptr->r_window = INF;
  }
}
