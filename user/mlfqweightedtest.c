#include "kernel/types.h"
#include "user/user.h"

#define SCHED_MLFQ_WEIGHTED 5
#define MEASURE_TICKS 300

#ifndef MLFQWT_REVERSE
#define MLFQWT_REVERSE 0
#endif

#if MLFQWT_REVERSE
#define SCENARIO_LABEL "reverse"
#define WORKER_A_NICE 15
#define WORKER_B_NICE 11
#else
#define SCENARIO_LABEL "default"
#define WORKER_A_NICE 11
#define WORKER_B_NICE 15
#endif

struct ready_message {
  int pid;
  int nice_value;
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
worker(int ready[2], int gate[2], int other_gate[2], int increment,
       int expected_nice)
{
  struct ready_message message;
  uint start_tick;
  uint active_start;
  uint end_tick;
  volatile uint work = 0;

  close_fd(&ready[0]);
  close_fd(&gate[1]);
  close_pipe(other_gate);

  message.pid = getpid();
  message.nice_value = nice(message.pid, increment);
  if (write_all(ready[1], &message, sizeof(message)) < 0)
    exit(1);
  close_fd(&ready[1]);

  if (message.nice_value != expected_nice ||
      read_all(gate[0], &start_tick, sizeof(start_tick)) < 0)
    exit(1);
  close_fd(&gate[0]);

  active_start = (uint)uptime();
  while ((uint)uptime() - start_tick < MEASURE_TICKS)
    work++;
  end_tick = (uint)uptime();

  printf("mlfqweightedtest: worker pid=%d nice=%d start=%d active=%d end=%d work=%d\n",
         message.pid, message.nice_value, (int)start_tick,
         (int)active_start, (int)end_tick, (int)work);
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
  int gate_a[2] = { -1, -1 };
  int gate_b[2] = { -1, -1 };
  int worker_a;
  int worker_b;
  int failed = 0;
  struct ready_message message;
  uint start_tick;

  if (set_scheduler(SCHED_MLFQ_WEIGHTED) < 0) {
    printf("mlfqweightedtest: set_scheduler(5) failed\n");
    exit(1);
  }

  if (nice(getpid(), -40) != -20 || nice(getpid(), 20) != 0) {
    printf("mlfqweightedtest: could not normalize parent nice to 0\n");
    exit(1);
  }

  if (pipe(ready) < 0 || pipe(gate_a) < 0 || pipe(gate_b) < 0) {
    close_pipe(ready);
    close_pipe(gate_a);
    close_pipe(gate_b);
    printf("mlfqweightedtest: pipe failed\n");
    exit(1);
  }

  worker_a = fork();
  if (worker_a < 0) {
    close_pipe(ready);
    close_pipe(gate_a);
    close_pipe(gate_b);
    printf("mlfqweightedtest: worker A fork failed\n");
    exit(1);
  }
  if (worker_a == 0)
    worker(ready, gate_a, gate_b, WORKER_A_NICE, WORKER_A_NICE);

  worker_b = fork();
  if (worker_b < 0) {
    close_pipe(ready);
    close_pipe(gate_a);
    close_pipe(gate_b);
    reap_children(1);
    printf("mlfqweightedtest: worker B fork failed\n");
    exit(1);
  }
  if (worker_b == 0)
    worker(ready, gate_b, gate_a, WORKER_B_NICE, WORKER_B_NICE);

  close_fd(&ready[1]);
  close_fd(&gate_a[0]);
  close_fd(&gate_b[0]);

  for (int i = 0; i < 2; i++) {
    if (read_all(ready[0], &message, sizeof(message)) < 0) {
      failed = 1;
      break;
    }
    if ((message.pid == worker_a && message.nice_value != WORKER_A_NICE) ||
      (message.pid == worker_b && message.nice_value != WORKER_B_NICE) ||
        (message.pid != worker_a && message.pid != worker_b))
      failed = 1;
  }
  close_fd(&ready[0]);

  if (failed) {
    close_fd(&gate_a[1]);
    close_fd(&gate_b[1]);
    reap_children(2);
    printf("mlfqweightedtest: worker nice setup failed\n");
    exit(1);
  }

  startLogging();
    printf("mlfqweightedtest: scenario=%s workers A=%d nice=%d B=%d nice=%d; Q0 window=%d ticks\n",
      SCENARIO_LABEL, worker_a, WORKER_A_NICE, worker_b, WORKER_B_NICE,
      MEASURE_TICKS);
  start_tick = (uint)uptime();
  if (write_all(gate_a[1], &start_tick, sizeof(start_tick)) < 0 ||
      write_all(gate_b[1], &start_tick, sizeof(start_tick)) < 0)
    failed = 1;
  close_fd(&gate_a[1]);
  close_fd(&gate_b[1]);

  if (reap_children(2) != 0)
    failed = 1;
  stopLogging();

  if (failed) {
    printf("mlfqweightedtest: worker release or exit failed\n");
    exit(1);
  }
  printf("mlfqweightedtest: done\n");
  exit(0);
}