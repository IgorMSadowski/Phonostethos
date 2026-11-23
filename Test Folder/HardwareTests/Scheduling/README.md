# Scheduling / CPU-affinity test

Files: `SchedulerTest.c` and `Makefile`. Build with `make`; output: `build-pi/SchedulerTest`.

On the Pi, as root:

```sh
cd /tmp/phonostethos-tests/HardwareTests/Scheduling
./SchedulerTest
echo $?
```

It creates four short-lived test threads, one at a time, requests the design settings and reads them back:

| Role | Core | Policy | Static priority | Nice |
|---|---|---|---|---|
| th_Main | 0 | SCHED_OTHER | 0 | 0 |
| th_Network | 3 | SCHED_OTHER | 0 | 0 |
| th_AudioDSP | 1 | SCHED_FIFO | 80 | Does not determine FIFO priority |
| th_FileIO | 2 | SCHED_FIFO | 50 | Does not determine FIFO priority |

Expected: four PASS lines, each with the requested CPU/policy/priority, then an overall PASS. Main and Network have the same scheduling priority; placing them on different cores is a separate setting.

APIs: pthread_create/join create and finish test threads; pthread_setaffinity_np/getaffinity_np set/check CPU masks; pthread_setschedparam/getschedparam set/check policy and static priority; setpriority/getpriority set/check nice on the thread's Linux TID.

Operation not permitted -> check root/CAP_SYS_NICE and RT limits. Invalid argument for affinity -> the requested core may be unavailable or restricted. A container/laptop failure does not automatically mean the Pi will fail.

This test changes only its own workers. It proves that the target allows these settings, not that the real server requested them successfully or meets every audio deadline. Also inspect the real server's startup log for scheduling/affinity errors during the complete system test.
