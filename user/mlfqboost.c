#include "kernel/types.h"
#include "user/user.h"

#define BLOCKED_CPU_TICKS 3
#define BOOST_RELEASE_TICKS 65
#define POST_WAKE_PROBE_TICKS 7

static void
burn_until(int start, int elapsed)
{
  volatile int work = 0;

  while (uptime() - start < elapsed)
    work++;
}

int
main(void)
{
  int blocked[2], ready[2];
  int sleeper_pid, writer_pid, children = 0, failed = 0, status;
  int boost_start;
  char token = 'x';

  if (set_scheduler(3) < 0) {
    printf("mlfqboost: set_scheduler(3) failed\n");
    exit(1);
  }
  boost_start = uptime();
  startLogging();

  if (pipe(blocked) < 0) {
    stopLogging();
    printf("mlfqboost: blocked pipe failed\n");
    exit(1);
  }
  if (pipe(ready) < 0) {
    close(blocked[0]);
    close(blocked[1]);
    stopLogging();
    printf("mlfqboost: ready pipe failed\n");
    exit(1);
  }

  sleeper_pid = fork();
  if (sleeper_pid < 0) {
    close(blocked[0]);
    close(blocked[1]);
    close(ready[0]);
    close(ready[1]);
    stopLogging();
    printf("mlfqboost: sleeper fork failed\n");
    exit(1);
  }
  if (sleeper_pid == 0) {
    close(blocked[1]);
    close(ready[0]);
    if (nice(getpid(), 11) != 11) {
      printf("mlfqboost: could not place sleeper in Q0\n");
      exit(1);
    }
    burn_until(uptime(), BLOCKED_CPU_TICKS);
    printf("mlfqboost: blocking pid %d at %d\n", getpid(), uptime());
    if (write(ready[1], &token, 1) != 1) {
      printf("mlfqboost: ready signal failed\n");
      exit(1);
    }
    close(ready[1]);
    if (read(blocked[0], &token, 1) != 1) {
      printf("mlfqboost: blocked read failed\n");
      exit(1);
    }
    close(blocked[0]);
    printf("mlfqboost: woke pid %d at %d\n", getpid(), uptime());
    burn_until(uptime(), POST_WAKE_PROBE_TICKS);
    printf("mlfqboost: post-wake probe complete at %d\n", uptime());
    exit(0);
  }
  children++;
  close(blocked[0]);
  close(ready[1]);

  writer_pid = fork();
  if (writer_pid < 0) {
    failed = 1;
    close(blocked[1]);
    close(ready[0]);
  } else if (writer_pid == 0) {
    close(ready[1]);
    if (read(ready[0], &token, 1) != 1) {
      printf("mlfqboost: writer ready read failed\n");
      exit(1);
    }
    close(ready[0]);

    // Let the lower-priority child reach its blocking read.
    if (pause(2) < 0) {
      printf("mlfqboost: writer pause failed\n");
      exit(1);
    }
    burn_until(boost_start, BOOST_RELEASE_TICKS);
    printf("mlfqboost: writer releasing pipe at %d\n", uptime());
    if (write(blocked[1], &token, 1) != 1) {
      printf("mlfqboost: writer pipe write failed\n");
      exit(1);
    }
    close(blocked[1]);
    exit(0);
  } else {
    children++;
    close(ready[0]);
    close(blocked[1]);
  }

  while (children-- > 0) {
    if (wait(&status) < 0 || status != 0)
      failed = 1;
  }

  stopLogging();
  if (failed) {
    printf("mlfqboost: test setup or child failed\n");
    exit(1);
  }
  printf("mlfqboost: done\n");
  exit(0);
}