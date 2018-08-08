/*
 *  Copyright (c) 2010 Luca Abeni
 *  Copyright (c) 2010 Csaba Kiraly
 *  Copyright (c) 2018 Massimo Girondi
 *
 *  This is free software; see lgpl-2.1.txt
 */

#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "chunk.h"
#include "chunkbuffer.h"
#include "grapes_config.h"

#include "peer.h"

typedef struct peerNode {
  struct nodeID *node_id;
  struct peerNode *next;
} peerNode;

struct chunk_buffer {
  int size;
  int num_chunks;
  int flow_id;
  struct chunk *buffer;
  peerNode **peer_ack_waiting;
};

static void insert_sort(struct chunk *b, int size)
{
  int i, j;
  struct chunk tmp;

  for(i = 1; i < size; i++) {
    tmp = b[i];
    j = i - 1;
    while(j >= 0 && tmp.id < b[j].id) {
      b[j + 1] = b[j];
      j = j - 1;
    }
    b[j + 1] = tmp;
  }
}

static void chunk_free(struct chunk *c)
{
    free(c->data);
    c->data = NULL;
    free(c->attributes);
    c->attributes = NULL;
    c->id = -1;
}

static void listPeerNode_free(peerNode *head) {
  if(head != NULL && head->next != NULL) {
    listPeerNode_free(head->next);
  }

  free(head);
}

static int listPeerNode_size(peerNode *head) {
  int deep = 0;
  while (head != NULL) {
    deep++;
    head = head->next;
  }
  return deep; 
}

static int remove_oldest_chunk(struct chunk_buffer *cb, int id, uint64_t ts)
{
  int i, min, pos_min, min_size;

  if (cb->buffer[0].id == id) {
    return E_CB_DUPLICATE;
  }
  min = cb->buffer[0].id; pos_min = 0;
  min_size = listPeerNode_size(cb->peer_ack_waiting[0]);
  for (i = 1; i < cb->num_chunks; i++) {
    if (cb->buffer[i].id == id) {
      return E_CB_DUPLICATE;
    }
    int size_i = listPeerNode_size(cb->peer_ack_waiting[i]);
    if ((cb->buffer[i].id < min && min_size == size_i) || min_size < size_i) {
      min = cb->buffer[i].id;
      min_size = size_i;
      pos_min = i;
    }
  }
  if (min < id) {
    chunk_free(&cb->buffer[pos_min]);
    listPeerNode_free(cb->peer_ack_waiting[pos_min]);
    cb->peer_ack_waiting[pos_min] = NULL;
    cb->num_chunks--;

    return pos_min;
  }

  // check for ID looparound and other anomalies
  if (cb->buffer[pos_min].timestamp < ts) {
    cb_clear(cb);
    return 0;
  }
  return E_CB_OLD;
}

struct chunk_buffer *cb_init(const char *config)
{
  struct tag *cfg_tags;
  struct chunk_buffer *cb;
  int res, i;

  cb = malloc(sizeof(struct chunk_buffer));
  if (cb == NULL) {
    return cb;
  }
  memset(cb, 0, sizeof(struct chunk_buffer));

  cfg_tags = grapes_config_parse(config);
  if (!cfg_tags) {
    free(cb);
    return NULL;
  }
  res = grapes_config_value_int(cfg_tags, "size", &cb->size);
  if (!res) {
    free(cb);
    free(cfg_tags);

    return NULL;
  }
  free(cfg_tags);

  cb->buffer = malloc(sizeof(struct chunk) * cb->size);
  if (cb->buffer == NULL) {
    free(cb);
    return NULL;
  }
  memset(cb->buffer, 0, sizeof(struct chunk) * cb->size);
  for (i = 0; i < cb->size; i++) {
    cb->buffer[i].id = -1;
  }

  cb->flow_id=0;

  cb->peer_ack_waiting = malloc(sizeof(peerNode*) * cb->size);
  for (i = 0; i < cb->size; i++) {
    cb->peer_ack_waiting[i] = NULL;
  }

  return cb;
}

void cb_ack_received(struct chunk_buffer *cb, int chunk_id, struct peer *peer_id)
{
  if(cb) {
    for (int i = 0; i < cb->size; i++) {
      if(cb->buffer[i].id == chunk_id
      && cb->buffer[i].chunk_type == DATA_TYPE) {
        peerNode * tmp_peerNode = cb->peer_ack_waiting[i];
        peerNode * tmp_peerNode_pre = NULL;
        while(tmp_peerNode != NULL) {
          if(nodeid_equal(tmp_peerNode->node_id, peer_id->id)) {      //CRASHA QUI
            if(tmp_peerNode_pre == NULL){
              cb->peer_ack_waiting[i] = NULL;
            }else{
              tmp_peerNode_pre->next = tmp_peerNode->next;             
            }

            nodeid_free(tmp_peerNode->node_id);    
            free(tmp_peerNode);
            tmp_peerNode = tmp_peerNode_pre;    
          }
          tmp_peerNode_pre = tmp_peerNode;
          tmp_peerNode = tmp_peerNode->next;
        }
      }
    }
  }
}

void cb_ack_expect(struct chunk_buffer *cb, int chunk_id, struct peer *peer_id) {
  if(cb) {    
    cb_print_peer_ack_waiting(cb);
    for (int i = 0; i < cb->size; i++) {
      if(cb->buffer[i].id == chunk_id
      && cb->buffer[i].chunk_type == DATA_TYPE) {
        printf("chunk DATA_TYPE \n");
        peerNode *newPeerNode = malloc(sizeof(peerNode));
        newPeerNode->node_id = nodeid_dup(peer_id->id);
        newPeerNode->next = NULL;

        peerNode *headNode = cb->peer_ack_waiting[i];
        if(headNode == NULL) {
          cb->peer_ack_waiting[i] = newPeerNode;
          printf("chunk DATA_TYPE first added\n");
          return;
        }

        while(headNode->next != NULL) {
          if(nodeid_equal(headNode->node_id, peer_id->id)) {
            nodeid_free(newPeerNode->node_id);
            free(newPeerNode);
            return; //Already waiting ack from this peer.
          }
          headNode = headNode->next;
        } 

        headNode->next = newPeerNode;
      }
    }
  }  
}

int cb_add_media_chunk(struct chunk_buffer *cb, const struct chunk *c) {
  int i;

  if (cb->num_chunks == cb->size) {
    i = remove_oldest_chunk(cb, c->id, c->timestamp);
  } else {
    i = 0;
  }

  if (i < 0) {
    return i;
  }
  
  while(1) {
    if (cb->buffer[i].id == c->id) {
      return E_CB_DUPLICATE;
    }
    if (cb->buffer[i].id < 0) {
      cb->buffer[i] = *c;
      cb->num_chunks++;

      return 0; 
    }
    i++;
  }
}

int cb_add_data_chunk(struct chunk_buffer *cb, const struct chunk *c) {
  int i;

  if (cb->num_chunks == cb->size) {
    i = remove_oldest_chunk(cb, c->id, c->timestamp);
  } else {
    i = 0;
  }

  if (i < 0) {
    return i;
  }
  
  while(1) {
    if (cb->buffer[i].id == c->id) {
      return E_CB_DUPLICATE;
    }
    if (cb->buffer[i].id < 0) {
      cb->buffer[i] = *c;
      cb->num_chunks++;

      return 0; 
    }
    i++;
  }
}

int cb_add_chunk(struct chunk_buffer *cb, const struct chunk *c)
{
  switch(c->chunk_type) {
    case MEDIA_TYPE: return cb_add_media_chunk(cb, c); break;
    case DATA_TYPE: return cb_add_data_chunk(cb, c); break;
  }
}

struct chunk *cb_get_chunks(const struct chunk_buffer *cb, int *n)
{
  *n = cb->num_chunks;
  if (*n == 0) {
    return NULL;
  }

  insert_sort(cb->buffer, cb->num_chunks);

  return cb->buffer;
}

int cb_clear(struct chunk_buffer *cb)
{
  int i;

  for (i = 0; i < cb->num_chunks; i++) {
    chunk_free(&cb->buffer[i]);
  }
  cb->num_chunks = 0;

  return 0;
}

void cb_destroy(struct chunk_buffer *cb)
{
  cb_clear(cb);
  free(cb->buffer);
  //free(cb->chunk_ack_waiting);
  free(cb);
}


int cb_get_flowid(const struct chunk_buffer *cb)
{
  return cb->flow_id;
}
void cb_set_flowid(struct chunk_buffer *cb, int flow_id)
{
  cb->flow_id=flow_id;
}

void cb_print_peer_ack_waiting (struct chunk_buffer *cb) {
  if(cb){
    for(int i = 0; i < cb->size; i++) {
      printf("waiting chunkId: %d \n", i);
      peerNode *tmpPeerNode = cb->peer_ack_waiting[i];
      if(tmpPeerNode == NULL) {
        printf("no peerWaiting \n");
      }
      while(tmpPeerNode != NULL) {
        printf("peerWaiting \n");
        tmpPeerNode = tmpPeerNode->next;
      }   
    }
  }
}
