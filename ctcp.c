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

#define FIN_SENT 1UL
#define FIN_RECEIVED 1UL << 1
#define EOF_FLAG 1UL << 2
#define DESTROY_FLAG (1UL << 3 - 1)

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
  linked_list_t *output_buffer;
                               /* Linked list of segments sent to this connection.
                               It may be useful to have multiple linked lists
                               for unacknowledged segments, segments that
                               haven't been sent, etc. Lab 1 uses the
                               stop-and-wait protocol and therefore does not
                               necessarily need a linked list. You may remove
                               this if this is the case for you */

  /* FIXME: Add other needed fields. */
  ctcp_config_t cfg;
  linked_list_t *send_buffer;
  linked_list_t *unacked_buffer;
  linked_list_t *ackno_list;
  uint32_t seqno;
  uint32_t ackno;
  int retransmitted_times;
  long last_sent_time;
  uint32_t destroy_flag;
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
  state->ackno = 1;
  state->output_buffer = ll_create();
  state->send_buffer = ll_create();
  state->unacked_buffer = ll_create();
  state->ackno_list = ll_create();
  state->last_sent_time = 0;
  state->retransmitted_times = 0;
  state->destroy_flag = 0;
  fprintf(stderr, "init call ended %s\n", "");
  return state;
}
void free_segments_list(linked_list_t *list) {
  ll_node_t *segment_node = list->head;
  while(segment_node != NULL) {
    free(segment_node->object);
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
  free_segments_list(state->output_buffer);
  free_segments_list(state->send_buffer);
  free_segments_list(state->unacked_buffer);
  free_segments_list(state->ackno_list);
  free(state);
  end_client();
}


void ctcp_read(ctcp_state_t *state) {
  /* FIXME */
  char input[MAX_SEG_DATA_SIZE];
  int data_size = conn_input(state->conn, input, MAX_SEG_DATA_SIZE);
  fprintf(stderr, "read data size = %u", data_size);
  if (data_size == -1) {
    state->destroy_flag |= EOF_FLAG;
    data_size = 0;
  }
  uint16_t total_size = sizeof(ctcp_segment_t) + sizeof(char) * data_size;
  ctcp_segment_t *segment = calloc(total_size, 1);
  segment->seqno = htonl(state->seqno);
  segment->len = htons(total_size);
  segment->window = htons(MAX_SEG_DATA_SIZE);
  segment->cksum = 0;
  if (data_size > 0) {
    memcpy(segment->data, input, data_size);
  } else if (state->destroy_flag & EOF_FLAG){
    segment->flags = FIN;
  }
  segment->flags |= ACK;
  segment->flags = htonl(segment->flags);
  ll_add (state->send_buffer, segment);
}

void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {
  /* FIXME */
  fprintf(stderr, "receive called %s\n", "");
  if (ntohl(segment->ackno) == state->seqno) {
      if (state->send_buffer->head != NULL) {
        free(state->unacked_buffer->head->object);
        ll_remove(state->unacked_buffer, state->unacked_buffer->head);
      }
      state->retransmitted_times = 0;
  }

  if (cksum(segment, sizeof(segment)) == segment->cksum) {
    if (segment->data != NULL || ntohl(segment->flags) & FIN) {
      uint32_t * ackno = calloc(sizeof(uint32_t), 1);
      *ackno = ntohl(segment->seqno) + ntohs(segment->len);
      ll_add(state->ackno_list, ackno);
    }
    if (ntohl(segment->seqno) == state->ackno) {
      if (segment->data != NULL || ntohl(segment->flags) & FIN) {
        ll_add(state->output_buffer, segment);
        ctcp_output(state);
      } else {
        free(segment);
      }
      state->ackno += ntohs(segment->len);
      fprintf(stderr, "%s", segment->data);
    } else {
      free(segment);
    }
  }

  if (ntohl(segment->flags) & FIN) {
    state->destroy_flag |= FIN_RECEIVED;
  }

}

void ctcp_output(ctcp_state_t *state) {
  /* FIXME */
  fprintf(stderr, "output called %s", "");
  ll_node_t *node =  state->output_buffer->head;
  while (node != NULL) {
    ctcp_segment_t *segment = node->object;
    int bufspace = conn_bufspace(state->conn);
    if (segment->data != NULL) {
      if (bufspace >= strlen(segment->data)){
        if (conn_output(state->conn, segment->data, strlen(segment->data)) == strlen(segment->data)) {
          ll_remove(state->output_buffer, node);
        } else {
          fprintf(stderr, "output length not equal to segment data length %u", strlen(segment->data));
        }
        if (ntohl(segment->flags) & FIN) {
          conn_output(state->conn, NULL, 0);
        }
      }
    } else if (ntohl(segment->flags) & FIN) {
      conn_output(state->conn, NULL, 0);
    }
    free(segment);
    node = node->next;
  }
}

void ctcp_timer() {
  /* FIXME */
  ctcp_state_t *state = state_list;
  while (state != NULL) {
    long cur_time = current_time();
    ctcp_segment_t *segment_to_send = NULL;
    if (ll_length(state->unacked_buffer) > 0) {  // check if last transmitted frame timed out
      if (cur_time - state->last_sent_time > state->cfg.rt_timeout) {  // time out
        if (state->retransmitted_times == 5) {
          ctcp_destroy(state);
        } else {
          segment_to_send = state->unacked_buffer->head->object;
          state->last_sent_time = cur_time;
          state->retransmitted_times++;
        }
      }
    } else if (ll_length(state->send_buffer) > 0){
      segment_to_send = state->send_buffer->head->object;
      if (ntohl(segment_to_send->flags) & FIN) {
        state->destroy_flag |= FIN_SENT;
      }
      ll_add(state->unacked_buffer, segment_to_send);
      ll_remove(state->send_buffer, segment_to_send);
      state->seqno += ntohs(segment_to_send->len);
      state->last_sent_time = cur_time;
      state->retransmitted_times = 0;
    }
    ll_node_t *ackNode = state->ackno_list->head;
    if (segment_to_send != NULL) {  // piggyback
      if (ackNode != NULL) {
        uint32_t *ackno = ackNode->object;
        segment_to_send->ackno = htonl(*ackno);
        ll_remove(state->ackno_list, ackNode);
        free(ackno);
      } else {
        segment_to_send->ackno = htonl(state->ackno);
      }
      segment_to_send->cksum = cksum(segment_to_send, sizeof(segment_to_send));
      conn_send(state->conn, segment_to_send, sizeof(segment_to_send));
    } else {
      if (ackNode != NULL) {
        uint32_t *ackno = ackNode->object;
        segment_to_send = calloc(sizeof(ctcp_segment_t), 1);
        segment_to_send->seqno = state->seqno;
        segment_to_send->ackno = htonl(*ackno);
        segment_to_send->window = htons(MAX_SEG_DATA_SIZE);
        segment_to_send->flags = htonl(ACK);
        segment_to_send->cksum = 0;
        segment_to_send->len = htons(sizeof(ctcp_segment_t));
        segment_to_send->cksum = cksum(segment_to_send, sizeof(segment_to_send));
        conn_send(state->conn, segment_to_send, sizeof(segment_to_send));
        ll_remove(state->ackno_list, ackNode);
        free(ackno);
        state->seqno += sizeof(segment_to_send);
      }
    }

    if (state->destroy_flag & DESTROY_FLAG) {
      ctcp_destroy(state);
    }

  }
}
