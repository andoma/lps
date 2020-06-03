#pragma once

typedef struct lps lps_t;
typedef struct lps_sub lps_sub_t;

typedef enum {
  LPS_VALUE,
  LPS_COUNTER,
  LPS_REQUEST_VALUE,
} lps_kind_t;

lps_t *lps_start(const char *domain);

void lps_stop(lps_t *lps);

lps_sub_t *lps_sub(lps_t *lps, const char *pattern,
                   void (*cb)(void *opaque, const char *topic,
                              lps_kind_t kind, ...),
                   void *opaque);

void lps_unsub(lps_sub_t *ls);

void lps_pub(lps_t *lps, const char *topic,
             lps_kind_t kind, ...);



/// IMPLEMENTATION

#ifdef LPS_IMPLEMENTATION
#include <sys/socket.h>
#include <sys/queue.h>

#include <netinet/in.h>
#include <arpa/inet.h>

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define LPS_MULTICAST_ADDRESS "239.255.255.210"
#define LPS_PORT 9988

LIST_HEAD(lps_sub_list, lps_sub);
SLIST_HEAD(lps_sub_slist, lps_sub);

struct lps_sub {
  LIST_ENTRY(lps_sub) ls_link;
  SLIST_ENTRY(lps_sub) ls_tmp_link;
  char *ls_pattern;
  void (*ls_cb)(void *opaque, const char *topic, lps_kind_t kind, ...);
  void *ls_opaque;
  lps_t *ls_lps;
  int ls_refcount;
};


struct lps {

  pthread_t l_tid;

  int l_fd;

  struct lps_sub_list l_subs;
  pthread_mutex_t l_subs_mutex;

  char l_domain[3];
};



static void
lps_release(lps_sub_t *ls)
{
  if(__atomic_sub_fetch(&ls->ls_refcount, 1, __ATOMIC_RELAXED))
    return;

  free(ls->ls_pattern);
  free(ls);
}



static int
lps_pattern_match(const char *s, const char *p)
{
  if(s == NULL)
    return 0;
  for(; *p != '*'; ++p, ++s) {
    if(!*s)
      return !*p;
    if(*s != *p)
      return 0;
  }
  while(p[1] == '*')
    p++;
  do {
    if(lps_pattern_match(s, p + 1))
      return 1;
  } while(*s++);
  return 0;
}



static void *
lps_thread(void *arg)
{
  lps_t *l = arg;
  lps_sub_t *ls;
  uint8_t buf[1500];
  struct lps_sub_slist slist;

  while(1) {
    const int r = read(l->l_fd, buf, sizeof(buf) - 1);
    if(r < 4)
      continue;

    if(memcmp(buf, l->l_domain, 3))
      continue;

    buf[r] = 0;
    lps_kind_t kind = buf[3];
    double v_double = 0;
    size_t value_size = 0;

    switch(kind) {
    case LPS_VALUE:
    case LPS_COUNTER:
    case LPS_REQUEST_VALUE:
      value_size = sizeof(double);
      if(r - 4 < value_size)
        continue;
      memcpy(&v_double, buf + 4, value_size);
      break;

    default:
      continue;
    }

    const char *topic = (const char *)buf + 4 + value_size;

    SLIST_INIT(&slist);

    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    pthread_mutex_lock(&l->l_subs_mutex);

    LIST_FOREACH(ls, &l->l_subs, ls_link) {
      if(lps_pattern_match(topic, ls->ls_pattern)) {
        SLIST_INSERT_HEAD(&slist, ls, ls_tmp_link);
        __atomic_add_fetch(&ls->ls_refcount, 1, __ATOMIC_RELAXED);
      }
    }
    pthread_mutex_unlock(&l->l_subs_mutex);

    SLIST_FOREACH(ls, &slist, ls_tmp_link) {
      switch(kind) {
      case LPS_VALUE:
      case LPS_COUNTER:
      case LPS_REQUEST_VALUE:
        ls->ls_cb(ls->ls_opaque, topic, kind, v_double);
        break;
      }
      lps_release(ls);
    }

    pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
  }
  return NULL;
}


static int
lps_create_socket(void)
{
  int fd = socket(AF_INET, SOCK_DGRAM, 0);
  if(fd == -1) {
    perror("socket");
    return -1;
  }

  if(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &(const int[]){1},
                sizeof(int))) {
    perror("setsockopt(SO_REUSEADDR)");
    close(fd);
    return -1;
  }

  struct sockaddr_in local = {
    .sin_family = AF_INET,
    .sin_port = htons(LPS_PORT)
  };


  if(bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
    perror("bind");
    close(fd);
    return -1;
  }

  struct ip_mreq imr = {
    .imr_multiaddr.s_addr = inet_addr(LPS_MULTICAST_ADDRESS)
  };

  if(setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &imr,
                sizeof(imr)) < 0) {
    perror("setsockopt(SO_REUSEADDR)");
    close(fd);
    return -1;
  }
  return fd;
}

lps_t *
lps_start(const char *domain)
{
  int fd = lps_create_socket();
  if(fd == -1)
    return NULL;

  lps_t *l = calloc(1, sizeof(lps_t));
  strncpy(l->l_domain, domain, 3);

  l->l_fd = fd;
  pthread_create(&l->l_tid, NULL, lps_thread, l);
  return l;
}

void
lps_stop(lps_t *l)
{
  assert(l->l_tid != pthread_self());
  pthread_cancel(l->l_tid);
  pthread_join(l->l_tid, NULL);
}


lps_sub_t *
lps_sub(lps_t *l, const char *pattern,
        void (*cb)(void *opaque, const char *topic,
                   lps_kind_t kind, ...),
        void *opaque)
{
  lps_sub_t *ls = calloc(1, sizeof(lps_sub_t));

  ls->ls_pattern = strdup(pattern);
  ls->ls_cb = cb;
  ls->ls_opaque = opaque;
  ls->ls_refcount = 1;

  pthread_mutex_lock(&l->l_subs_mutex);
  LIST_INSERT_HEAD(&l->l_subs, ls, ls_link);
  pthread_mutex_unlock(&l->l_subs_mutex);
  return ls;
}


void
lps_unsub(lps_sub_t *ls)
{
  lps_t *l = ls->ls_lps;
  pthread_mutex_lock(&l->l_subs_mutex);
  LIST_REMOVE(ls, ls_link);
  pthread_mutex_unlock(&l->l_subs_mutex);
  lps_release(ls);
}


void
lps_pub(lps_t *lps, const char *topic, lps_kind_t kind, ...)
{
  const size_t topic_len = strlen(topic);
  if(topic_len > 255)
    return; // Some arbitrary limit

  va_list ap;
  va_start(ap, kind);

  char buf[4 + sizeof(double) + topic_len];

  memcpy(buf, lps->l_domain, 3);
  buf[3] = kind;

  size_t value_len;
  double dbl;
  switch(kind) {
  case LPS_VALUE:
  case LPS_COUNTER:
  case LPS_REQUEST_VALUE:
    dbl = va_arg(ap, double);
    memcpy(buf + 4, &dbl, sizeof(double));
    value_len = sizeof(double);
    break;
  default:
    return;
  }

  va_end(ap);

  memcpy(buf + 4 + value_len, topic, topic_len);

  const size_t total_len = 4 + value_len + topic_len;

  struct sockaddr_in dst = {
    .sin_family = AF_INET,
    .sin_port = htons(LPS_PORT),
    .sin_addr.s_addr = inet_addr(LPS_MULTICAST_ADDRESS)
  };

  sendto(lps->l_fd, buf, total_len, 0,
         (struct sockaddr *)&dst, sizeof(dst));
}


#endif
