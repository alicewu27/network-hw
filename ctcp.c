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
#define DESTROY_FLAG ((1UL << 3) - 1)
#define MAX_SEND_BUF_SIZE 300

typedef struct unack_info {
  ctcp_segment_t *segment;
  int retransmitted_times;
  int last_sent_time;
} unack_info_t;

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
  linked_list_t *received_buffer;
  uint32_t seqno;
  uint32_t ackno;
  uint32_t send_seqno_lo;
  uint32_t receive_seqno_lo;
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
  memcpy(&(state->cfg), cfg, sizeof(ctcp_config_t));
  state->seqno = 1;
  state->ackno = 1;
  state->output_buffer = ll_create();
  state->send_buffer = ll_create();
  state->unacked_buffer = ll_create();
  state->ackno_list = ll_create();
  state->received_buffer = ll_create();
  state->send_seqno_lo = 1;
  //state->send_seqno_hi = 1 + cfg->send_window;
  state->receive_seqno_lo = 1;
  //state->receive_seqno_hi = 1 + cfg->recv_window;
  state->destroy_flag = 0;
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
  //free_segments_list(state->output_buffer);
  free_segments_list(state->send_buffer);
  free_segments_list(state->unacked_buffer);
  free_segments_list(state->ackno_list);
  free_segments_list(state->received_buffer);
  free(state);
  end_client();
}


void ctcp_read(ctcp_state_t *state) {   // TODO: 1.send buffer full, 2.data larger than window size
  /* FIXME */
  /*if (ll_length(state->send_buffer) == MAX_SEND_BUF_SIZE) {
    fprintf(stderr, "send buffer full, can't read more");
    return;
    }*/char input[MAX_SEG_DATA_SIZE];
  int data_size = 0;
  while ((data_size = conn_input(state->conn, input, MAX_SEG_DATA_SIZE)) > 0) {
    fprintf(stderr, "read data size = %u", data_size);
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
    state->seqno += total_size;
  }
  if (data_size == -1) {
    state->destroy_flag |= EOF_FLAG;
  }
}


uint16_t data_length(ctcp_segment_t *segment) {
  return ntohs(segment->len) - sizeof(ctcp_segment_t);
}

int check_range(ctcp_segment_t *segment, ctcp_state_t* state) {
  return ntohl(segment->seqno) >= state->receive_seqno_lo
         && ntohl(segment->seqno) + ntohs(segment->len) < state->receive_seqno_lo + state->cfg.recv_window;
}

void add_and_sort(linked_list_t* list, ctcp_segment_t *segment) {
  ll_node_t *ptr = ll_front(list);
  if (ptr == NULL) {
    ll_add(list, segment);
  } else {
    ctcp_segment_t *seg = ptr->object;
    if (ntohl(seg->seqno) > ntohl(segment->seqno)) {
      ll_add_front(list, segment);
    } else {
      int added_flag = 0;
      while (ptr != NULL) {
        seg = ptr->object;
        if (ntohl(seg->seqno) > ntohl(segment->seqno)) {
          ll_add_after(list, ptr->prev, segment);
          added_flag = 1;
          break;
        }
        ptr = ptr->next;
      }
      if (!added_flag) {
        ll_add(list, segment);
      }
    }
  }
}

uint32_t find_inorder(ctcp_state_t* state) {
  ll_node_t *ptr = ll_front(state->received_buffer);
  uint32_t ackno = state->receive_seqno_lo;
  while (ptr != NULL) {
    ctcp_segment_t *segment = ptr->object;
    if (ntohl(segment->seqno) == ackno) {
      ackno += ntohs(segment->len);
      ll_add(state->output_buffer, segment);
      ll_node_t *temp = ptr->next;
      ll_remove(state->received_buffer, ptr);
      ptr = temp;
    } else {
      break;
    }
  }
  if (ackno > state->receive_seqno_lo) {
    return ackno;
  } else {
    return 0;
  }
}

void ack_received(ctcp_state_t *state, uint32_t ackno) {
  if (ackno < state->send_seqno_lo) {  // delayed ackno
    return;
  }
  ll_node_t *node_ptr = ll_front(state->unacked_buffer);
  while (node_ptr != NULL) {
    unack_info_t *info = node_ptr->object;
    if (ntohl(info->segment->seqno) < ackno) {
      free(info->segment);
      free(info);
      ll_node_t *temp = node_ptr->next;
      ll_remove(state->unacked_buffer, node_ptr);
      node_ptr = temp;
    }
  }
  // forward window
  state->send_seqno_lo = ackno;
}

void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {
  /* FIXME */
  fprintf(stderr, "message received  %s with lenth %hu\n", segment->data, data_length(segment));
  ack_received(state, ntohl(segment->ackno));
  // received content
  uint16_t old_cksum = segment->cksum;
  segment->cksum = 0;
  if (cksum(segment, ntohs(segment->len)) == old_cksum) {
    if (data_length(segment) > 0 || ntohl(segment->flags) & FIN) {
      uint32_t * ackno = calloc(sizeof(uint32_t), 1);
      *ackno = ntohl(segment->seqno) + ntohs(segment->len);
      ll_add(state->ackno_list, ackno);
      fprintf(stderr, "ackno list size %u\n", ll_length(state->ackno_list));
      if (check_range(segment, state)) {
        add_and_sort(state->received_buffer, segment);
        uint32_t ackno = find_inorder(state);
        if (ackno > 0) {
          state->receive_seqno_lo = ackno;  // move forward receive window
          ctcp_output(state);  //segment to be freed in ctcp_output
        }
      } else {  // out of bound
        free(segment);
      }
    } else {  //ack package
      free(segment);
    }
  } else {  //cksum not right
    fprintf(stderr, "checksum not correct\n");
    free(segment);
  }

  if (ntohl(segment->flags) & FIN) {
    state->destroy_flag |= FIN_RECEIVED;
  }

}

void ctcp_output(ctcp_state_t *state) {
  /* FIXME */
  fprintf(stderr, "output called");
  ll_node_t *node =  ll_front(state->output_buffer);
  while (node != NULL) {
    ctcp_segment_t *segment = node->object;
    int bufspace = conn_bufspace(state->conn);
    if (data_length(segment) > 0) {
      if (bufspace > data_length(segment)) {
        if (conn_output(state->conn, segment->data, data_length(segment)) != data_length(segment)) {
          fprintf(stderr, "output length not equal to segment data length %hu", data_length(segment));
        }
      } else {
        fprintf(stderr, "output buf space full");
      }
    } else if (ntohl(segment->flags) & FIN) {
      fprintf(stderr, "eof");
      conn_output(state->conn, NULL, 0);
    }
    free(segment);
    ll_node_t* temp = node->next;
    ll_remove(state->output_buffer, node);
    node = temp;
  }
}

void ctcp_timer() {
  /* FIXME */
  ctcp_state_t *state = state_list;
  while (state != NULL){
    long cur_time = current_time();
    ctcp_segment_t *segment_to_send = NULL;
    if (ll_length(state->unacked_buffer) > 0) {  // check if last transmitted frame timed out
      unack_info_t *info = ll_front(state->unacked_buffer)->object;
      if (cur_time - info->last_sent_time > state->cfg.rt_timeout) {  // time out
        if (info->retransmitted_times == 5) {
          fprintf(stderr, "retransmitted times = 5\n");
          ctcp_destroy(state);
        } else {
          segment_to_send = info->segment;
          segment_to_send->cksum = 0;
          info->last_sent_time = cur_time;
          info->retransmitted_times++;
        }
      }
    } else if (ll_length(state->send_buffer) > 0) {
      ll_node_t *seg_node = state->send_buffer->head;
      segment_to_send = seg_node->object;
      if (ntohl(segment_to_send->seqno) + ntohs(segment_to_send->len) - state->send_seqno_lo <= state->cfg.send_window) {
        unack_info_t *info = calloc(sizeof(unack_info_t), 1);
        info->segment = segment_to_send;
        info->retransmitted_times = 0;
        info->last_sent_time = cur_time;
        ll_add(state->unacked_buffer, info);
        ll_remove(state->send_buffer, seg_node);
        if (ntohl(segment_to_send->flags) & FIN) {
          state->destroy_flag |= FIN_SENT;
        }
      } else {
        segment_to_send = NULL;
      }
    }
    ll_node_t *ackNode = ll_front(state->ackno_list);
    if (segment_to_send != NULL) {  // piggyback
      if (ackNode != NULL) {
        uint32_t *ackno = ackNode->object;
        segment_to_send->ackno = htonl(*ackno);
        ll_remove(state->ackno_list, ackNode);
        free(ackno);
      } else {
        segment_to_send->ackno = htonl(state->receive_seqno_lo);
      }
      segment_to_send->cksum = cksum(segment_to_send, ntohs(segment_to_send->len));
      conn_send(state->conn, segment_to_send, ntohs(segment_to_send->len));
    } else if (ackNode != NULL) {
      uint32_t *ackno = ackNode->object;
      segment_to_send = calloc(sizeof(ctcp_segment_t), 1);
      segment_to_send->ackno = htonl(*ackno);
      segment_to_send->window = htons(MAX_SEG_DATA_SIZE);
      segment_to_send->flags = htonl(ACK);
      segment_to_send->cksum = 0;
      segment_to_send->len = htons(sizeof(ctcp_segment_t));
      segment_to_send->cksum = cksum(segment_to_send, sizeof(ctcp_segment_t));
      conn_send(state->conn, segment_to_send, sizeof(ctcp_segment_t));
      free(ackno);
      free(segment_to_send);
      ll_remove(state->ackno_list, ackNode);
    }
    ctcp_state_t *temp = state->next;
    if (state->destroy_flag & DESTROY_FLAG) {
      //fprintf(stderr, "destroy_flag");
      ctcp_destroy(state);
    }
    state = temp;
  }
}
