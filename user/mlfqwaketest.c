#include "kernel/types.h"
#include "user/user.h"

#define SCHED_MLFQ_WEIGHTED 5
#define WORKER_NICE 11
#define INITIAL_CPU_TICKS 3
#define BLOCKED_TICKS 70
#define POST_WAKE_TICKS 55
#define COMPETITOR_EXTRA_TICKS 20
#define COMPETITOR_TICKS \
  (INITIAL_CPU_TICKS + BLOCKED_TICKS + POST_WAKE_TICKS + COMPETITOR_EXTRA_TICKS)

#define SLEEPER_ROLE 1
#define COMPETITOR_ROLE 2

struct ready_message {
  int role;
  int pid;
  int nice_value;
  uint tick;
};

static void
close_fd(int *fd)
{
  if (*fd >= 0) {
    close(*fd);
    *fd = -1;
  }
}

static void
close_pipe(int fds[2])
{
  close_fd(&fds[0]);
  close_fd(&fds[1]);
}

static int
write_all(int fd, const void *buffer, int count)
{
  const char *bytes = buffer;

  while (count > 0) {
    int written = write(fd, bytes, count);
    if (written <= 0)
      return -1;
    bytes += written;
    count -= written;
  }
  return 0;
}

static int
read_all(int fd, void *buffer, int count)
{
  char *bytes = buffer;

  while (count > 0) {
    int received = read(fd, bytes, count);
    if (received <= 0)
      return -1;
    bytes += received;
    count -= received;
  }
  return 0;
}

static void
burn_ticks(int duration)
{
  int start = uptime();
  volatile uint work = 0;

  while (uptime() - start < duration)
    work++;
}

static void
burn_until(uint start, int duration)
{
  volatile uint work = 0;

  while ((uint)uptime() - start < (uint)duration)
    work++;
}

static void
send_ready(int fd, int role, int nice_value)
{
  struct ready_message message;

  message.role = role;
  message.pid = getpid();
  message.nice_value = nice_value;
  message.tick = (uint)uptime();
  if (write_all(fd, &message, sizeof(message)) < 0)
    exit(1);
}

static void
sleeper(int ready[2], int sleeper_start[2], int competitor_start[2],
        int release[2])
{
  char start_token;
  uint release_tick;

  close_fd(&ready[0]);
  close_fd(&sleeper_start[1]);
  close_pipe(competitor_start);
  close_fd(&release[1]);

  if (nice(getpid(), WORKER_NICE) != WORKER_NICE) {
    printf("mlfqwaketest: sleeper nice setup failed pid=%d\n", getpid());
    exit(1);
  }

  if (read_all(sleeper_start[0], &start_token, sizeof(start_token)) < 0) {
    close_fd(&sleeper_start[0]);
    exit(1);
  }
  close_fd(&sleeper_start[0]);

  burn_ticks(INITIAL_CPU_TICKS);
  printf("mlfqwaketest: sleeper pid=%d sleeping-attempt tick=%d after %d CPU-alone ticks\n",
         getpid(), uptime(), INITIAL_CPU_TICKS);
  send_ready(ready[1], SLEEPER_ROLE, WORKER_NICE);
  close_fd(&ready[1]);

  if (read_all(release[0], &release_tick, sizeof(release_tick)) < 0) {
    close_fd(&release[0]);
    exit(1);
  }
  close_fd(&release[0]);

  printf("mlfqwaketest: sleeper pid=%d woke tick=%d released=%d\n",
         getpid(), uptime(), (int)release_tick);
  burn_ticks(POST_WAKE_TICKS);
  printf("mlfqwaketest: sleeper pid=%d complete tick=%d\n",
         getpid(), uptime());
  exit(0);
}

static void
competitor(int ready[2], int sleeper_start[2], int competitor_start[2],
           int release[2])
{
  uint start_tick;

  close_fd(&ready[0]);
  close_pipe(sleeper_start);
  close_fd(&competitor_start[1]);
  close_pipe(release);

  if (nice(getpid(), WORKER_NICE) != WORKER_NICE) {
    printf("mlfqwaketest: competitor nice setup failed pid=%d\n", getpid());
    exit(1);
  }
  send_ready(ready[1], COMPETITOR_ROLE, WORKER_NICE);
  close_fd(&ready[1]);

  if (read_all(competitor_start[0], &start_tick, sizeof(start_tick)) < 0) {
    close_fd(&competitor_start[0]);
    exit(1);
  }
  close_fd(&competitor_start[0]);

  printf("mlfqwaketest: competitor pid=%d active-start tick=%d\n",
         getpid(), (int)start_tick);
  burn_until(start_tick, COMPETITOR_TICKS);
  printf("mlfqwaketest: competitor pid=%d complete tick=%d\n",
         getpid(), uptime());
  exit(0);
}

static int
reap_children(int count)
{
  int failed = 0;
  int status;

  while (count-- > 0)
    if (wait(&status) < 0 || status != 0)
      failed = 1;
  return failed;
}

int
main(void)
{
  int ready[2] = { -1, -1 };
  int sleeper_start[2] = { -1, -1 };
  int competitor_start[2] = { -1, -1 };
  int release[2] = { -1, -1 };
  int sleeper_pid;
  int competitor_pid;
  int failed = 0;
  int got_sleeper = 0;
  int got_competitor = 0;
  uint start_tick;
  uint release_tick;
  uint sleeper_block_tick = 0;
  char start_token = 's';
  struct ready_message message;

  if (set_scheduler(SCHED_MLFQ_WEIGHTED) < 0) {
    printf("mlfqwaketest: set_scheduler(5) failed\n");
    exit(1);
  }
  if (nice(getpid(), -40) != -20 || nice(getpid(), 20) != 0) {
    printf("mlfqwaketest: could not normalize parent nice to 0\n");
    exit(1);
  }
  startLogging();

  if (pipe(ready) < 0 || pipe(sleeper_start) < 0 ||
      pipe(competitor_start) < 0 || pipe(release) < 0) {
    close_pipe(ready);
    close_pipe(sleeper_start);
    close_pipe(competitor_start);
    close_pipe(release);
    stopLogging();
    printf("mlfqwaketest: pipe failed\n");
    exit(1);
  }

  sleeper_pid = fork();
  if (sleeper_pid < 0) {
    close_pipe(ready);
    close_pipe(sleeper_start);
    close_pipe(competitor_start);
    close_pipe(release);
    stopLogging();
    printf("mlfqwaketest: sleeper fork failed\n");
    exit(1);
  }
  if (sleeper_pid == 0)
    sleeper(ready, sleeper_start, competitor_start, release);

  competitor_pid = fork();
  if (competitor_pid < 0) {
    close_pipe(ready);
    close_pipe(sleeper_start);
    close_pipe(competitor_start);
    close_pipe(release);
    reap_children(1);
    stopLogging();
    printf("mlfqwaketest: competitor fork failed\n");
    exit(1);
  }
  if (competitor_pid == 0)
    competitor(ready, sleeper_start, competitor_start, release);

  close_fd(&ready[1]);
  close_fd(&sleeper_start[0]);
  close_fd(&competitor_start[0]);
  close_fd(&release[0]);

  if (write_all(sleeper_start[1], &start_token, sizeof(start_token)) < 0) {
    close_pipe(sleeper_start);
    close_fd(&competitor_start[1]);
    close_fd(&release[1]);
    reap_children(2);
    stopLogging();
    printf("mlfqwaketest: sleeper start failed\n");
    exit(1);
  }
  close_fd(&sleeper_start[1]);

  for (int i = 0; i < 2; i++) {
    if (read_all(ready[0], &message, sizeof(message)) < 0) {
      failed = 1;
      break;
    }
    if (message.role == SLEEPER_ROLE && message.pid == sleeper_pid &&
        message.nice_value == WORKER_NICE) {
      got_sleeper = 1;
      sleeper_block_tick = message.tick;
      printf("mlfqwaketest: sleeper pid=%d ready-to-block tick=%d nice=%d\n",
             message.pid, (int)message.tick, message.nice_value);
    } else if (message.role == COMPETITOR_ROLE &&
               message.pid == competitor_pid &&
               message.nice_value == WORKER_NICE) {
      got_competitor = 1;
      printf("mlfqwaketest: competitor pid=%d gated tick=%d nice=%d\n",
             message.pid, (int)message.tick, message.nice_value);
    } else {
      failed = 1;
    }
  }
  close_fd(&ready[0]);

  if (failed || !got_sleeper || !got_competitor) {
    close_fd(&competitor_start[1]);
    close_fd(&release[1]);
    reap_children(2);
    stopLogging();
    printf("mlfqwaketest: worker startup failed\n");
    exit(1);
  }

  if (pause(1) < 0) {
    close_fd(&competitor_start[1]);
    close_fd(&release[1]);
    reap_children(2);
    stopLogging();
    printf("mlfqwaketest: sleeper staging pause failed\n");
    exit(1);
  }

  start_tick = (uint)uptime();
  printf("mlfqwaketest: releasing competitor pid=%d tick=%d; parent sleeps %d ticks\n",
         competitor_pid, (int)start_tick, BLOCKED_TICKS);
  if (write_all(competitor_start[1], &start_tick, sizeof(start_tick)) < 0) {
    close_fd(&competitor_start[1]);
    close_fd(&release[1]);
    reap_children(2);
    stopLogging();
    printf("mlfqwaketest: competitor release failed\n");
    exit(1);
  }
  close_fd(&competitor_start[1]);

  if (pause(BLOCKED_TICKS) < 0) {
    close_fd(&release[1]);
    reap_children(2);
    stopLogging();
    printf("mlfqwaketest: parent wait failed\n");
    exit(1);
  }

  release_tick = (uint)uptime();
  printf("mlfqwaketest: releasing sleeper pid=%d tick=%d blocked=%d ticks\n",
         sleeper_pid, (int)release_tick,
      (int)(release_tick - sleeper_block_tick));
  if (write_all(release[1], &release_tick, sizeof(release_tick)) < 0)
    failed = 1;
  close_fd(&release[1]);

  if (reap_children(2) != 0)
    failed = 1;
  stopLogging();

  if (failed) {
    printf("mlfqwaketest: worker release or exit failed\n");
    exit(1);
  }
  printf("mlfqwaketest: done\n");
  exit(0);
}