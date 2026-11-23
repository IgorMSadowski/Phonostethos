# Pthreads / buffers / queues test

Files: `PthreadTest.cpp` and `Makefile`. C++ allows the test to use the real CircularBuffer, CommandQueue and DataQueue classes. Build with `make`; output: `build-pi/PthreadTest`.

On the packaged Pi:

```sh
cd /tmp/phonostethos-tests/SystemTests/Pthreads
./PthreadTest
```

Expected: three PASS messages.

1. A pthread produces 16 blocks of 512 numbered samples. The main test thread pops at most 200 samples at a time. All 8192 samples must arrive exactly once, in order, across buffer wrap-around.
2. The producer closes the buffer gate at EOF and sends a worker result through CommandQueue. The test waits with pthread_join, checks the result and confirms that a new buffer session starts empty. A stopped buffer must reject later pushes.
3. Two visualization snapshots go into DataQueue. Only the latest must be retrieved, followed by an empty result.

The mutex/condition-variable calls are inside the actual buffer/queue classes. pthread_create starts the producer and passes shared data; pthread_join waits before destroying it. Different producer/consumer block sizes exercise partial pops rather than assuming every transfer is exactly one full block.

This checks the communication mechanisms, not real-time audio performance. SystemTests/FileIO and Controller test them in their application workflows.
