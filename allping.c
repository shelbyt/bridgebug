#define _GNU_SOURCE 1

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <time.h>

#include <sys/timerfd.h>
#include <sys/epoll.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>

#define PORT 34567

#define BUSY_WAIT 0 // 0 or 1

#define SEND_DELAY 0.0 // only works when BUSY_WAIT is false

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

#define SAVE_ERRNO(block) do { int errno_ = errno; { block; } errno = errno_; } while (0)

typedef struct {
  int fd;
  int id;
  unsigned int wanttoping:1;
  unsigned int wanttopong:1;
  unsigned long address;
  struct timespec ping_timestamp;
  struct timespec pong_timestamp;
} peer_t;

static int tmfd = -1;
static int startfd = -1;
static int timeoutfd = -1;
static int delaytmfd = -1;
static int epfd = -1;

static int selfid;
static int npeers;
static peer_t *peers, delaytmpeer;

static struct timespec start_pinging;
static unsigned int pongs_sent;
static unsigned int pongs_recvd;

static void do_exit(int);
static int do_close(int);

static void do_report(void) {
  fprintf(stderr,
    "=====================\n"
    "%u/%d pongs sent\n"
    "%u/%d pongs recvd\n",
    pongs_sent, npeers, pongs_recvd, npeers);
}

static void print_time(char *msg) {
  struct timespec ts;
  if (clock_gettime(CLOCK_REALTIME, &ts)){
    perror("clock_gettime");
    do_exit(1);
  }
  fprintf(stderr, "%s at %lu.%.9lu\n", msg, ts.tv_sec, ts.tv_nsec);
}

static void do_exit(int status) {
  for (int i = 0; i < npeers; i++) {
    if (peers[i].fd) {
      do_close(peers[i].fd);
    }
  }
  if (tmfd) do_close(tmfd);
  if (startfd) do_close(startfd);
  if (timeoutfd) do_close(timeoutfd);
  if (delaytmfd) do_close(delaytmfd);

  exit(status);
}

static int do_close(int fd) {
  int r;

  do
    r = close(fd);
  while (r == -1 && errno == EINTR);

  return r;
}

static int add_fd(int fd, void *arg, int events) {
  struct epoll_event ee = { .data.ptr = arg, .events = events };
  return epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ee);
}

static int mod_fd(int fd, void *arg, int events) {
  struct epoll_event ee = { .data.ptr = arg, .events = events };
  return epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ee);
}

static int make_sock_fd(unsigned short port) {
  struct sockaddr_in sin;
  int sockfd;

  if ((sockfd = socket(AF_INET, SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC, 0)) == -1)
    return -1;

  memset(&sin, 0, sizeof sin);
  sin.sin_addr.s_addr = htonl(INADDR_ANY);
  sin.sin_port = htons(port);
  sin.sin_family = AF_INET;

  int enable = 1;
  if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)) < 0) {
    perror("setsockopt");
    return -1;
  }

  if (bind(sockfd, (struct sockaddr *) &sin, sizeof sin) == -1)
    return -1;

  return sockfd;
} 

static int make_timer_fd(unsigned int ms) {
  struct itimerspec its;
  int timerfd;

  if ((timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK|TFD_CLOEXEC)) == -1)
    return -1;

#define SET_TS(ts, ms) ((ts)->tv_sec = (ms) / 1000, (ts)->tv_nsec = ((ms) % 1000) * 1e6)
  SET_TS(&its.it_interval, ms);
  SET_TS(&its.it_value, ms);

  if (timerfd_settime(timerfd, 0, &its, NULL)) {
    SAVE_ERRNO(do_close(timerfd));
    return -1;
  }

  return timerfd;
}

static int make_one_use_timer_fd(long sec, long nsec) {
  struct itimerspec its;
  int timerfd;

  its.it_value.tv_sec = sec;
  its.it_value.tv_nsec = nsec;
  its.it_interval.tv_sec = its.it_interval.tv_nsec = 0;

  if ((timerfd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK|TFD_CLOEXEC)) == -1)
    return -1;
  
  if (timerfd_settime(timerfd, TFD_TIMER_ABSTIME, &its, NULL)) {
    SAVE_ERRNO(do_close(timerfd));
    return -1;
  }

  return timerfd;
}

static int update_events(peer_t *peer) {
  return mod_fd(peer->fd, peer, EPOLLIN | EPOLLOUT * (peer->wanttoping || peer->wanttopong));
}

static int delay_expires() {
  static int current = 0;
  int ret = 0;

  if (current < npeers) {
    peers[current].wanttoping = 1;
    ret = update_events(peers + current);
    current++;
  }
  return ret;
}


static int do_recv(peer_t *peer) {
  char buf[1024];
  ssize_t r;

  do
    r = read(peer->fd, buf, sizeof buf);
  while (r == -1 && errno == EINTR);

  if (r == 4) {
    if (buf[1] == 'I') {
      peer->wanttopong = 1;
      if (update_events(peer) == -1) {
        perror("update_events");
        return -1;
      }

    } else if (buf[1] == 'O') {
      pongs_recvd++;
      if (peer->pong_timestamp.tv_sec) {
        fprintf(stderr, "pong timestamp exits!");
        return -1;
      }
      if (clock_gettime(CLOCK_REALTIME, &peer->pong_timestamp)) {
        perror("clock_gettime");
        return -1;
      }
    } else {
      fprintf(stderr, "unknown msg");
      return -1;
    }
  }
  return r;
}


static int do_send(peer_t *peer, const void *data, size_t size) {
  struct sockaddr_in sin;
  ssize_t r;

  sin.sin_family = AF_INET;
  sin.sin_addr.s_addr = peer->address;
  sin.sin_port = htons(PORT + selfid);

  if (((char*)data)[1] == 'I') {
    print_time("SENDING PING");
  }

  do
    r = sendto(peer->fd, data, size, 0, (struct sockaddr *) &sin, sizeof sin);
  while (r == -1 && errno == EINTR);
  
  return r;
}


static int do_epoll(void) {
  struct epoll_event out[1024];
  peer_t *peer;
  int i, nfds, r;
  int events;
  int fd;

  do
    nfds = epoll_wait(epfd, out, ARRAY_SIZE(out), -1);
  while (nfds == -1 && errno == EINTR);
  
  //fprintf(stderr, "nfds: %d\n", n);

  if (nfds == -1) {
    perror("epoll_wait");
    return -1;
  }

  for (i = 0; i < nfds; i++) {
    events = out[i].events;
    peer = out[i].data.ptr;
    fd = peer->fd;

    if (events & EPOLLERR) {
      fprintf(stderr, "epoll_wait returned EPOLLERR");
      return -1;
    }

    if (events & ~(EPOLLIN|EPOLLOUT)) {
      fprintf(stderr, "not an in/out event\n");
      return -1;
    }

    if (fd == tmfd) {
      char buf[8];
      read(fd, buf, 8);
      do_report();
      continue;
    }

    if (fd == startfd) {
      char buf[8];
      read(fd, buf, 8);

      if (clock_gettime(CLOCK_REALTIME, &start_pinging))
        return -1;
      fprintf(stderr, "start pinging at %lu.%.9lu\n", start_pinging.tv_sec, start_pinging.tv_nsec);
      
      if (SEND_DELAY > 0.0) { // set up the delay clock
        if ((delaytmfd = make_timer_fd(SEND_DELAY * 1000)) == -1) {
          perror("make_timer_fd");
          do_exit(1);
        }
        delaytmpeer.fd = delaytmfd;
        if (add_fd(delaytmfd, &delaytmpeer, EPOLLIN)) {
          perror("add_fd");
          do_exit(1);
        }
      } else {
        for (int j = 0; j < npeers; j++) {
          peers[j].wanttoping = 1;
          if (update_events(peers + j) == -1) {
            perror("update_events");
            return -1;
          }
        }
      }
      continue;
    } 

    if (fd == delaytmfd) {
      uint64_t exp;
      if (read(fd, &exp, sizeof(exp)) != sizeof(exp)) {
        perror("read delaytmfd");
        return -1;
      } else {
        while (exp--) {
          if (delay_expires() == -1) {
            perror("delay_expires");
            return -1;
          }
        }
      }
      continue;
    }

    if (fd == timeoutfd) {
      char buf[8];
      read(fd, buf, 8);
      fprintf(stderr, "timeout!");
      return -1;
    }

    if (events & EPOLLIN) {
      if ((r = do_recv(peer)) == -1) {
        perror("do_recv");
        return -1;
      }
    }

    if (events & EPOLLOUT) {
      const char ping[] = "PING";
      const char pong[] = "PONG";

      if (peer->wanttoping) {
        if ((r = do_send(peer, ping, sizeof(ping) - 1)) == -1) {
          perror("do_send");
          if (errno == EPERM) {
            goto finish;
          }
          return -1;
        }
        if (peer->ping_timestamp.tv_sec) {
          fprintf(stderr, "ping timestamp exits!");
          return -1;
        }
        if (clock_gettime(CLOCK_REALTIME, &peer->ping_timestamp)) {
          perror("clock_gettime");
          return -1;
        }
finish:
        peer->wanttoping = 0;
        if (update_events(peer) == -1) {
          perror("update_events");
          return -1;
        }
        continue;
      }

      if (peer->wanttopong) {
        if ((r = do_send(peer, pong, sizeof(pong) - 1)) == -1) {
          perror("do_send");
          return -1;
        }
        pongs_sent++;
        peer->wanttopong = 0;
        if (update_events(peer) == -1) {
          perror("update_events");
          return -1;
        }
      }
    }
  }

  return 0;
}


int load_addresses() {
  const int BUFSIZE = 4096*255;
  char buffer[BUFSIZE];
  int newline = -1;

  for (int i = 0; i < npeers; i++) {
      fgets(buffer, BUFSIZE, stdin);
      newline = strcspn(buffer, "\n");
      if (newline > 0)
        buffer[newline] = '\0';
      if (inet_pton(AF_INET, buffer, &peers[i].address) != 1) {
        perror("inet_pton");
        return -1;
      }
  }
}

static double diff_in_seconds(struct timespec t1, struct timespec t2)
{
  struct timespec diff;
  if (t2.tv_nsec-t1.tv_nsec < 0) {
    diff.tv_sec  = t2.tv_sec - t1.tv_sec - 1;
    diff.tv_nsec = t2.tv_nsec - t1.tv_nsec + 1000000000;
  } else {
    diff.tv_sec  = t2.tv_sec - t1.tv_sec;
    diff.tv_nsec = t2.tv_nsec - t1.tv_nsec;
  }
  return diff.tv_sec + diff.tv_nsec / 1000000000.0;
}


int main(int argc, char **argv) {
  peer_t tmp, startp, timeoutp;
  int i, r;
  long start_time;

  if (argc != 4) {
    fprintf(stderr, "usage: %s <start_time_in_epoch> <selfid> <number-of-addresses>\n", argv[0]);
    do_exit(1);
  }

  start_time = atol(argv[1]);
  selfid = atoi(argv[2]);
  npeers = atoi(argv[3]);

  fprintf(stderr, "intented start time: %lu, id: %d, # of addresses: %d\n", start_time, selfid, npeers);

  if ((peers = (peer_t*) calloc(npeers, sizeof(peer_t))) == NULL) {
    perror("calloc");
    do_exit(1);
  }

  if (load_addresses() == -1) {
    do_exit(1);
  }

  if ((epfd = epoll_create1(EPOLL_CLOEXEC)) == -1) {
    perror("make_epoll_fd");
    do_exit(1);
  }

  if ((tmfd = make_timer_fd(2000)) == -1) {
    perror("make_timer_fd");
    do_exit(1);
  }

  tmp.fd = tmfd;
  if (add_fd(tmfd, &tmp, EPOLLIN)) {
    perror("add_fd");
    do_exit(1);
  }

  if ((startfd = make_one_use_timer_fd(start_time, 0)) == -1) {
    perror("make_one_use_timer_fd");
    do_exit(1);
  }

  startp.fd = startfd;
  if (add_fd(startfd, &startp, EPOLLIN)) {
    perror("add_fd");
    do_exit(1);
  }

  if ((timeoutfd = make_one_use_timer_fd(start_time + 5 + SEND_DELAY * npeers, 0)) == -1) {
    perror("make_one_use_timer_fd");
    do_exit(1);
  }

  timeoutp.fd = timeoutfd;
  if (add_fd(timeoutfd, &timeoutp, EPOLLIN)) {
    perror("add_fd");
    do_exit(1);
  }

  for (i = 0; i < npeers; i++) {
    peers[i].id = i;
    if ((peers[i].fd = make_sock_fd(PORT + i)) == -1) {
      perror("make_sock_fd");
      do_exit(1);
    }
    if (add_fd(peers[i].fd, peers + i, EPOLLIN | (EPOLLOUT * BUSY_WAIT))) {
      perror("add_fd");
      do_exit(1);
    }

  }

  print_time("start epoll");

  while ((r = do_epoll()) == 0 && (pongs_sent < npeers || pongs_recvd < npeers))
    /* nop */ ;

  if (r == -1) {
    perror("do_epoll");
    printf("%lu.%.9lu\n", start_pinging.tv_sec, start_pinging.tv_nsec);
    for (i = 0; i < npeers; i++) {
      printf("%lf\n", diff_in_seconds(peers[i].ping_timestamp, peers[i].pong_timestamp));
    }
    do_exit(1);
  }

  do_report();
  printf("%lu.%.9lu\n", start_pinging.tv_sec, start_pinging.tv_nsec);
  for (i = 0; i < npeers; i++) {
    printf("%lf\n", diff_in_seconds(peers[i].ping_timestamp, peers[i].pong_timestamp));
  }

  do_exit(0);
}
