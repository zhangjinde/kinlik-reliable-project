# Reliable Transport Layer

Sliding window over User Datagram Protocol (UDP). Project 1 of Computer Networks at ETH Zurich, Spring 2022.

## Features

- Implemented in C (my work is in [`reliable.c`](reliable.c))
- Reliable against packet loss, reordering, duplication and corruption
- Cumulative ACK, sender and receiver buffer, timer and retransmission

## Project Guidelines

The requirements are given [here](docs/reliable-project.pdf). Additional documents are also provided by the teaching team.

## Test Results

Test for transport layer correctness:
```
$ ./tester -w 5 ./reliable
TEST  1:  Bi-directionally transfer data...                           passed
TEST  2:  Ping-pong short messages back and forth...                  passed
TEST  3:  Ping-pong with test for excessive retransmissions...        passed
TEST  4:  Receiving data from reference implementation...             passed
TEST  5:  Flow control when application doesn't read output...        passed
TEST  6:  Sending data to reference implementation...                 passed
TEST  7:  Bi-directionally interoperating with reference...           passed
TEST  8:  Test for proper end-of-file handling...                     passed
TEST  9:  Two-way transfer injecting 5% garbage packets...            passed
TEST 10:  Receiving from reference with 2% reordering...              passed
TEST 11:  Two-way transfer with 5% packet duplication...              passed
TEST 12:  Two-way transfer with 2% of packets having bad length...    passed
TEST 13:  One-way transfer with 2% packet loss...                     passed
TEST 14:  Two-way transfer with 2% packet corruption...               passed
```

Test for implementation being sliding window:
```
$ python3 window_test.py 5 ./reliable ./reliable_no_answer
Sequence numbers found: {1, 2, 3, 4, 5}
All sequence numbers are correct.
Window test outcome: passed
```

## Test Files

[Stanford's CS144 website](https://www.scs.stanford.edu/10au-cs144/lab/reliable/reliable.html) contains the implementation of test files.
