/******************************************************************************
 * ctcp.c
 * ------
 * Implementation of cTCP done here. This is the only file you need to change.
 * Look at the following files for references and useful functions:
 *   - ctcp.h: Headers for this file.
 *   - ctcp_iinked_list.h: Linked list functions for managing a linked list.
 *   - ctcp_sys.h: Connection-related structs and functions, cTCP segment
 *                 definition.
 *   - ctcp_utils.h: Checksum computation, getting the current time.
 *
 *****************************************************************************/

#include "ctcp.h"
#include "ctcp_linked_list.h"
#include "ctcp_sys.h"
#include "ctcp_utils.h"
#include "stdio.h"
/**
 * Connection state.
 *
 * Stores per-connection information such as the current sequence number,
 * unacknowledged packets, etc.
 *
 * You should add to this to store other fields you might need.
 */
struct ctcp_state {
  struct ctcp_state *next;  /* Next in linked list */
  struct ctcp_state **prev; /* Prev in linked list */

  conn_t *conn;             /* Connection object -- needed in order to figure
                               out destination when sending */
  linked_list_t *segments;  /* Linked list of segments sent to this connection.
                               It may be useful to have multiple linked lists
                               for unacknowledged segments, segments that
                               haven't been sent, etc. Lab 1 uses the
                               stop-and-wait protocol and therefore does not
                               necessarily need a linked list. You may remove
                               this if this is the case for you */

  /* FIXME: Add other needed fields. */
  //linked_list_t *unacked_segments;
  ctcp_config_t cfg;
  linked_list_t *buffer;
  uint32_t seqno;
  uint32_t received_ackno;
  uint32_t sent_ackno;
  int retransmitted_times;
  int received_ackno_sent;
  long last_received_time;
  long last_sent_time;
};

/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * resubmit segments and tear down connections.
 */
static ctcp_state_t *state_list;

/* FIXME: Feel free to add as many helper functions as needed. Don't repeat
          code! Helper functions make the code clearer and cleaner. */


ctcp_state_t *ctcp_init(conn_t *conn, ctcp_config_t *cfg) {
  /* Connection could not be established. */
fprintf(stderr, "init called %s\n", "");
  if (conn == NULL) {
    return NULL;
  }

  /* Established a connection. Create a new state and update the linked list
     of connection states. */
  ctcp_state_t *state = calloc(sizeof(ctcp_state_t), 1);
  state->next = state_list;
  state->prev = &state_list;
  if (state_list)
    state_list->prev = &state->next;
  state_list = state;

  /* Set fields. */
  state->conn = conn;
  /* FIXME: Do any other initialization here. */
  state->cfg = *cfg;
  state->seqno = 1;
  state->sent_ackno = 1;
  state->received_ackno = 1;
  state->retransmitted_times = 0;
  state->segments = ll_create();
  state->buffer = ll_create();
  state->received_ackno_sent = 0;
  state->last_received_time = 0;
  state->last_sent_time = 0;
  state->last_sent_ack_time = 0;
  fprintf(stderr, "init call ended %s\n", "");
  return state;
}
void free_segments_list(linked_list_t *list) {
  ll_node_t *segment_node = list->head;
  while(segment_node != NULL) {
    // ll_remove(list, segment);
    free(segment_node->object);
    // free(segment_node);
    segment_node = segment_node->next;
  }
  ll_destroy(list);
}
void ctcp_destroy(ctcp_state_t *state) {
  /* Update linked list. */
  fprintf(stderr, "destroy called %s\n", "");
  if (state->next)
    state->next->prev = state->prev;

  *state->prev = state->next;
  conn_remove(state->conn);

  /* FIXME: Do any other cleanup here. */
  free_segments_list(state->segments);
  free_segments_list(state->buffer);
  free(state);
  end_client();
}


void ctcp_read(ctcp_state_t *state) {
  /* FIXME */
  fprintf(stderr, "read called %s\n", "");
  char input[MAX_SEG_DATA_SIZE];
  int data_size = conn_input(state->conn, input, MAX_SEG_DATA_SIZE);
  int fin_flag = data_size;
  if (fin_flag == -1) {
    data_size = 0;
  }
  uint16_t total_size = data_size > 0 ?
          sizeof(ctcp_segment_t) + sizeof(char) * data_size : sizeof(ctcp_segment_t);
  fprintf(stderr, "segment size = %u", total_size);
  ctcp_segment_t *segment = calloc(total_size, 1);
  segment->seqno = htonl(state->seqno);
  segment->len = htons(total_size);
  fprintf(stderr, "segment size htons = %u",segment->len);
  segment->window = MAX_SEG_DATA_SIZE;
  segment->cksum = 0;
  if (data_size > 0) {
    memcpy(segment->data, input, data_size);
  } else {
    segment->flags = FIN;
  }
  segment->flags = SYN;
  segment->flags = htonl(segment->flags);
  ll_add (state->buffer, segment);
}

void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {
  /* FIXME */
  fprintf(stderr, "receive called %s\n", "");
  state->last_received_time = current_time();
  if (ntohl(segment->flags) & ACK) {
    if (ntohl(segment->ackno) == state->seqno) {
      state->sent_ackno = ntohl(segment->ackno);
      if (state->buffer->head != NULL) {
        free(state->buffer->head->object);
        ll_remove(state->buffer, state->buffer->head);
      }
      state->retransmitted_times = 0;
    }
  }

  if (ntohl(segment->flags) & FIN) {
    conn_output(state->conn, NULL, 0);
    if (state->seqno == state->sent_ackno) {
      ctcp_destroy(state);
    } else {
      state->received_ackno += ntohs(segment->len);
      state->received_ackno_sent = 0;
    }
  } else {
    if (cksum(segment, sizeof(segment)) == segment->cksum) {
      if (ntohl(segment->seqno) == state->received_ackno) {
        ll_add(state->segments, segment);
        fprintf(stderr, "%s", segment->data);
        ctcp_output(state);
        state->received_ackno += ntohs(segment->len);
        state->received_ackno_sent = 0;
      } else {  // duplicated segment, must also acknowledge it
        state->received_ackno -= ntohs(segment->len);
        state->received_ackno_sent = 0;
      }
    }
  }
  free(segment);
}

void ctcp_output(ctcp_state_t *state) {
  /* FIXME */
  fprintf(stderr, "output called %s", "");
  ll_node_t *node =  state->segments->head;
  ctcp_segment_t *segment = node->object;
  int bufspace = conn_bufspace(state->conn);
  if (bufspace == -1) {
    ctcp_destroy(state);
  }
 
  if (bufspace >= strlen(segment->data)){
      if (conn_output(state->conn, segment->data, strlen(segment->data)) == strlen(segment->data)) {
        ll_remove(state->segments, node);
      }
  }
}

void ctcp_timer() {
  /* FIXME */
  ctcp_state_t *state = state_list;
  while (state != NULL) {
    if (state->sent_ackno == state->seqno) {      
      ll_node_t* segment_node = state->buffer->head;
      if (segment_node != NULL) {
        ctcp_segment_t* segment = (ctcp_segment_t*)segment_node->object;
        uint16_t len = ntohs(segment->len);
        segment->ackno = htonl(state->received_ackno);
        segment->flags |= htonl(ACK);
        segment->cksum = cksum(segment, len);
        fprintf(stderr, "has message: %s with lenth %u",segment->data, len);
        conn_send(state->conn, segment, len);
        state->received_ackno_sent = 1;
        state->seqno += len;
        state->last_sent_time = current_time();
        state->retransmitted_times = 0;
      } else {
        long cur_time= current_time();
        if (!state->received_ackno_sent && cur_time - state->last_received_time > state->cfg.rt_timeout) {
          ctcp_segment_t *ack_segment = calloc(sizeof(ctcp_segment_t), 1);
          ack_segment->seqno = htonl(state->seqno);
          ack_segment->ackno = htonl(state->received_ackno);
          ack_segment->window = state->cfg.send_window;
          ack_segment->flags = htonl(ACK);
          ack_segment->cksum = 0;
          uint16_t len = sizeof(ctcp_segment_t);
          ack_segment->len = htons(len);
          ack_segment->cksum = cksum(ack_segment, sizeof(ack_segment));
          conn_send(state->conn, ack_segment, len);
          state->received_ackno_sent = 1;
        }
      }
    } else {       // last transmitted frame timed out, need resend
      ctcp_segment_t* segment_resend = state->buffer->head->object;
      long cur_time = current_time();
      segment_resend->ackno = htonl(state->received_ackno);
      if (cur_time - state->last_sent_time > state->cfg.rt_timeout) {  // time out 2 seconds
        if (state->retransmitted_times == 5) {
          ctcp_destroy(state);
        } else {
          conn_send(state->conn, segment_resend, ntohs(segment_resend->len));
          state->received_ackno_sent = 1;
          state->last_sent_time = cur_time;
          state->retransmitted_times++;
        }
      }
    }
    if (state->segments->length > 0) {
      ctcp_output(state);
    }
    state = state->next;
  }
}
