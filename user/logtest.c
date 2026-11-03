#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

static void
priority_child(void)
{
  int start;
  volatile uint64 work = 0;

  printf("pid %d nice %d\n", getpid(), nice(getpid(), 0));
  start = uptime();
  while (uptime() - start < 5) {
    for (int i = 0; i < 10000; i++)
      work += i;
  }
  printf("pid %d work finished\n", getpid());
  exit(0);
}

static void
check_result(const char *label, int result, int expected)
{
  printf("TEST %s: result = %d\n", label, result);
  if (result != expected) {
    printf("FAIL %s: expected %d\n", label, expected);
    exit(1);
  }
}

static void
set_self_nice_zero(void)
{
  int pid = getpid();

  if (nice(pid, -100) != -20 || nice(pid, 20) != 0) {
    printf("FAIL nice reset\n");
    exit(1);
  }
}

static void
nice_tests(void)
{
  int pid = getpid();
  int result;
  int caller_nice;

  set_self_nice_zero();
  result = nice(pid, 1);
  check_result("logging off nice", result, 1);

  startLogging();
  result = nice(pid, -1);
  stopLogging();
  check_result("logging on nice", result, 0);

  set_self_nice_zero();
  check_result("nice positive increment", nice(pid, 3), 3);

  set_self_nice_zero();
  check_result("nice negative increment", nice(pid, -4), -4);

  set_self_nice_zero();
  nice(pid, 18);
  check_result("nice upper clamp", nice(pid, 5), 19);

  set_self_nice_zero();
  nice(pid, -19);
  check_result("nice lower clamp", nice(pid, -5), -20);

  set_self_nice_zero();
  nice(pid, 7);
  result = nice(-1, 5);
  caller_nice = nice(pid, 0);
  printf("TEST nice invalid pid: result = %d, caller nice = %d\n",
         result, caller_nice);
  if (result != 0 || caller_nice != 7) {
    printf("FAIL nice invalid pid\n");
    exit(1);
  }
}

static void
fork_test(void)
{
  int pid = getpid();
  int child;
  int status;
  int parent_nice;

  nice(pid, -100);
  nice(pid, 25);
  parent_nice = nice(pid, 0);
  printf("TEST fork: parent nice = %d\n", parent_nice);
  if (parent_nice != 5) {
    printf("FAIL fork: could not set parent nice to 5\n");
    exit(1);
  }

  child = fork();
  if (child < 0) {
    printf("FAIL fork: fork failed\n");
    exit(1);
  }
  if (child == 0) {
    int child_nice = nice(getpid(), 0);

    printf("TEST fork: child nice = %d\n", child_nice);
    exit(child_nice == 0 ? 0 : 1);
  }

  if (wait(&status) != child || status != 0) {
    printf("FAIL fork: child did not report nice 0\n");
    exit(1);
  }
}

static void
equal_child(void)
{
  int pid = getpid();
  int child_nice = nice(pid, 0);
  int start;
  volatile uint64 work = 0;

  if (child_nice != 0)
    exit(1);

  start = uptime();
  while (uptime() - start < 50) {
    for (int i = 0; i < 10000; i++)
      work += i;
  }
  printf("TEST equal: child %d finished\n", pid);
  exit(0);
}

static void
equal_test(void)
{
  int parent = getpid();
  int first, second;
  int status;

  nice(parent, -100);

  first = fork();
  if (first < 0) {
    printf("FAIL equal: first fork failed\n");
    exit(1);
  }
  if (first == 0)
    equal_child();
  if (nice(first, 0) != 0) {
    printf("FAIL equal: first child nice is not 0\n");
    wait(0);
    exit(1);
  }

  second = fork();
  if (second < 0) {
    printf("FAIL equal: second fork failed\n");
    wait(0);
    exit(1);
  }
  if (second == 0)
    equal_child();
  if (nice(second, 0) != 0) {
    printf("FAIL equal: second child nice is not 0\n");
    wait(0);
    wait(0);
    exit(1);
  }

    printf("TEST equal: children %d and %d both nice = 0, running 50 ticks\n",
      first, second);
  startLogging();
  if (nice(parent, 39) != 19) {
    printf("FAIL equal: could not lower parent priority\n");
    wait(0);
    wait(0);
    stopLogging();
    exit(1);
  }

  if (wait(&status) < 0 || status != 0) {
    printf("FAIL equal: a child failed\n");
    wait(0);
    stopLogging();
    exit(1);
  }
  if (wait(&status) < 0 || status != 0) {
    printf("FAIL equal: a child failed\n");
    stopLogging();
    exit(1);
  }

  stopLogging();
  printf("TEST equal: both children finished\n");
}

int
main(int argc, char *argv[])
{
  if (argc > 1 && strcmp(argv[1], "nice") == 0) {
    nice_tests();
    exit(0);
  }

  if (argc > 1 && strcmp(argv[1], "fork") == 0) {
    fork_test();
    exit(0);
  }

  if (argc > 1 && strcmp(argv[1], "equal") == 0) {
    equal_test();
    exit(0);
  }

  if (argc > 1 && strcmp(argv[1], "priority") == 0) {
    int first, second;

    startLogging();
    nice(getpid(), -20);

    first = fork();
    if (first < 0)
      exit(1);
    if (first == 0)
      priority_child();
    nice(first, 10);

    second = fork();
    if (second < 0) {
      wait(0);
      exit(1);
    }
    if (second == 0)
      priority_child();
    nice(second, 5);

    wait(0);
    wait(0);
    stopLogging();
    exit(0);
  }

  startLogging();
  int pid = fork();
  if (pid == 0) {
    nice(getpid(), 10);
    volatile uint64 count = 0;
    for (uint64 i = 0; i < 500000000; i++)
      count += i;
    exit(0);
  }
  wait(0);
  stopLogging();
  exit(0);
}
