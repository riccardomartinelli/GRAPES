/*
 *  Copyright (c) 2010 Luca Abeni
 *
 *  This is free software; see lgpl-2.1.txt
 */

#include <sys/time.h>
#include <time.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "net_helper.h"
#include "topmanager.h"
#include "topocache.h"
#include "topo_proto.h"
#include "proto.h"
#include "msg_types.h"

#define MAX_PEERS 30

static uint64_t currtime;
static int cache_size = MAX_PEERS;
static struct peer_cache *local_cache;
static bool bootstrap = true;
static int bootstrap_period = 2000000;
static int period = 10000000;

static uint64_t gettime(void)
{
  struct timeval tv;

  gettimeofday(&tv, NULL);

  return tv.tv_usec + tv.tv_sec * 1000000ull;
}

static int time_to_send(void)
{
  int p = bootstrap ? bootstrap_period : period;
  if (gettime() - currtime > p) {
    currtime += p;

    return 1;
  }

  return 0;
}

/*
 * Public Functions!
 */
int topInit(struct nodeID *myID, void *metadata, int metadata_size, const char *config)
{
  local_cache = cache_init(cache_size, metadata_size);
  if (local_cache == NULL) {
    return -1;
  }
  topo_proto_init(myID, metadata, metadata_size);
  currtime = gettime();
  bootstrap = true;

  return 1;
}

int topChangeMetadata(struct nodeID *peer, void *metadata, int metadata_size)
{
  if (topo_proto_metadata_update(peer, metadata, metadata_size) <= 0) {
    return -1;
  }

  return 1;
}

int topAddNeighbour(struct nodeID *neighbour, void *metadata, int metadata_size)
{
  if (cache_add(local_cache, neighbour, metadata, metadata_size) < 0) {
    return -1;
  }
  return topo_query_peer(local_cache, neighbour);
}

int topParseData(const uint8_t *buff, int len)
{
  if (len) {
    const struct topo_header *h = (const struct topo_header *)buff;
    struct peer_cache *new, *remote_cache;

    if (h->protocol != MSG_TYPE_TOPOLOGY) {
      fprintf(stderr, "NCAST: Wrong protocol!\n");

      return -1;
    }

    bootstrap = false;

    if (h->type == NCAST_QUERY) {
      topo_reply(buff + sizeof(struct topo_header), len - sizeof(struct topo_header), local_cache);
    }
    remote_cache = entries_undump(buff + sizeof(struct topo_header), len - sizeof(struct topo_header));
    new = merge_caches(local_cache, remote_cache, cache_size);
    cache_free(remote_cache);
    if (new != NULL) {
      cache_free(local_cache);
      local_cache = new;
    }
  }

  if (time_to_send()) {
    cache_update(local_cache);
    topo_query(local_cache);
  }

  return 0;
}

const struct nodeID **topGetNeighbourhood(int *n)
{
  static struct nodeID **r;

  r = realloc(r, cache_size * sizeof(struct nodeID *));
  if (r == NULL) {
    return NULL;
  }

  for (*n = 0; nodeid(local_cache, *n) && (*n < cache_size); (*n)++) {
    r[*n] = nodeid(local_cache, *n);
    //fprintf(stderr, "Checking table[%d]\n", *n);
  }

  return (const struct nodeID **)r;
}

const void *topGetMetadata(int *metadata_size)
{
  return get_metadata(local_cache, metadata_size);
}

int topGrowNeighbourhood(int n)
{
  cache_size += n;

  return cache_size;
}

int topShrinkNeighbourhood(int n)
{
  if (cache_size < n) {
    return -1;
  }
  cache_size -= n;

  return cache_size;
}

int topRemoveNeighbour(struct nodeID *neighbour)
{
  return cache_del(local_cache, neighbour);
}

