# tsc benchmarking

## Building
run make

## Running

This has two very simple loops.  A low IPC loop meant to produce IPC values
less than one, and a high IPC loop meant to produce IPC values over 3.

The idea is to determine the peformance impact of using different tsc
reads during those loops.  The choices are

rdtscp
rdtsc
clock_gettime()

### Example runs

perf stat ./tsc -- check on the IPC values for your machine

./tsc cmp -- compare the low IPC loop with and without tsc reads

./tsc high_ipc -- runs the high IPC loop

./tsc high_ipc cmp -- compares high IPC loop with and without tsc reads

./tsc low_ipc notsc -- runs the low IPC loop without any tsc reads

./tsc high_ipc notsc -- runs the high IPC loop without any tsc reads

./tsc low_ipc rdtsc -- runs the low IPC loop with rdtsc instead of rdtscp

./tsc low_ipc rdtsc cmp -- compares low IPC with rdtsc and without any tsc

