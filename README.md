# A Dynamic Memory Allocator

A 64-bit struct-based explicit segregated-free-list memory allocator with best-fit policy approximation. The dynamic memory allocator manages the 'heap' which can be viewed as a collection of various-sized memory blocks. All blocks are 16-byte aligned. Tested on `64-bit Ubuntu 22.04.1 LTS (Linux kernel 5.15.0)`.

# Block Types

The blocks are broadly divided into two categories:

- `Allocated` blocks are marked as allocated and remain allocated until they are explicitly free'd. They are structured as:
  `<HEADER><PAYLOAD>`
- `Free` blocks are are marked are free and remain so until explicitly allocated. Free blocks are further divided into two sub-categories depending on their size:
- A mini free block is a minimum_block_size'd block (16 bytes). They are structured as: `<HEADER><PAYLOAD (next)>`.
  Since a header is 8 bytes, and the Payload is 8 bytes, The payload is aliased to be a pointer (next) that points  
   to the next mini free block. This is used to manage a singly-linked list of mini-free blocks.
- A standard free block is atleast 32 bytes and is structured as: `<HEADER><PAYLOAD(next)(prev)><FOOTER>`. Since, the payload is atleast 16 bytes, it is aliased to hold pointers to the next and previous free blocks. This is used to manage a doubly-linked list of free-blocks.

- Headers/footers are single word-size'd and are used to encode info required for performing various operations:
  - 60 MSB's encode the size of the block.
  - LSB encodes the current allocation status of the block.
  - 2nd LSB encodes the allocation status of the previous block.
  - 3rd LSB tells us if the previous block is a mini-block or not.

# Overview

A broad overview of the workings of my allocator is as follows:

- Free block organization: Free blocks are managed as segregated free lists. The segregated list is an array of pointers to singly/doubly linked lists that are used to manage free blocks belonging to different size classes. The pointer at index 0 points to a list of blocks that are min_block_size'd (16). This is the only list that is singly-linked, Every other size lass is large enough to have the payload alias'd to two pointers, making it possible for them to be managed as doubly linked lists.
- Placement: An approximation of the Best-fit policy is used. Segregated lists are used as the initial approximator to find the best fit. An attempt is made to further improve the best-fit approximation by searching for the next 6 blocks that also satisfy the size criterion and choosing the smallest possible block to reduce fragmentation. See `find_fit` function for more information.
- Splitting: If the block size is large enough to merit a split while maintaining alignment, an allocated block is split into an allocated and free block. The prev_alloc and prev_mini flags of the next block are set appropriately. See `split_block` function for more information.
- Coalescing: Adjacent free blocks are immediately merged to combat false fragmentation. See `coalecse_block` function for more information.

# Project Structure

## Main Files

- `mdriver, mdriver-emulate`: After running make, run ./mdriver to test. Run ./mdriver-emulate to test 64-bit allocations

- `traces/`: Directory that contains the trace files that the driver uses to test. Files with names of the form XXX-short.rep contain very short traces that for debugging.

## Other support files for the driver

- `config.h`: Configures the malloc lab driver
- `clock.{c,h}`: Low-level timing functions
- `fcyc.{c,h}`: Function-level timing functions
- `memlib.{c,h}`: Models the heap and sbrk function
- `stree.{c,h}`: Data structure used by the driver to check for overlapping allocations
- `MLabInst.so`: Code that combines with LLVM compiler infrastructure to enable sparse memory emulation
- `macro-check.pl`: Code to check for disallowed macro definitions
- `driver.pl`: Runs both mdriver and mdriver-emulate and generates the autolab result. (Not included with checkpoint)
- `calibrate.pl`: Code to generate benchmark throughput throughputs.txt Benchmark throughputs, indexed by CPU type

# Malloc packages

- `mm.c`: The Explicit segregated-free-list allocator
- `mm-naive.c`: Fast but extremely memory-inefficient package

# Building and running the driver

To build the driver, type `make` to the shell.

To run the driver on a tiny test trace:

        unix> ./mdriver -V -f traces/malloc.rep

To get a list of the driver flags:

        unix> ./mdriver -h

The `-V` option prints out helpful tracing information

You can use `mdriver-dbg` to test with the `DEBUG` preprocessor
flag set to 1. This enables the dbg macros such as `dbg_printf`, which
you can use to print debugging output. It also uses the optimization
level `-Og`, which helps GDB display more meaningful debug information.

        unix> ./mdriver-dbg

You can use `mdriver-emulate` to test the correctness of your code in
handling 64-bit addresses:

        unix> ./mdriver-emulate

You should see the exact same utilization numbers as you did with the
regular driver. No timing is done, and so the time and throughput
numbers show up as zeros.

You can use `mdriver-uninit` to test your code using `MemorySanitizer`,
a tool that detects uses of uninitialized memory.

        unix> ./mdriver-uninit

# Test Output

```
# Run driver
#
./driver.pl

Calibration: CPU type Intel(R)Xeon(R)Gold6248RCPU@3.00GHz, benchmark regular, throughput 8218
Running ./mdriver -s 180
Found benchmark throughput 8218 for cpu type Intel(R)Xeon(R)Gold6248RCPU@3.00GHz, benchmark regular
Throughput targets: min=4109, max=7396, benchmark=8218
..........................
Results for mm malloc:
  valid    util     ops   msecs  Kops/s  trace
   yes    81.3%      20     0.032    633 ./traces/syn-array-short.rep
   yes    76.7%      20     0.008   2486 ./traces/syn-struct-short.rep
   yes    79.7%      20     0.008   2487 ./traces/syn-string-short.rep
   yes    99.0%      20     0.010   2073 ./traces/syn-mix-short.rep
   yes    83.7%      36     0.008   4608 ./traces/ngram-fox1.rep
   yes    69.3%     757     0.552   1372 ./traces/syn-mix-realloc.rep
 * yes    76.0%    5748     0.199  28887 ./traces/bdd-aa4.rep
 * yes    71.6%   87830     2.825  31094 ./traces/bdd-aa32.rep
 * yes    71.6%   41080     1.269  32361 ./traces/bdd-ma4.rep
 * yes    71.8%  115380     4.457  25890 ./traces/bdd-nq7.rep
 * yes    75.9%   20547     0.591  34737 ./traces/cbit-abs.rep
 * yes    78.9%   95276     3.137  30374 ./traces/cbit-parity.rep
 * yes    77.4%   89623     2.815  31837 ./traces/cbit-satadd.rep
 * yes    71.6%   50583     1.473  34341 ./traces/cbit-xyz.rep
 * yes    62.3%   32540     0.821  39648 ./traces/ngram-gulliver1.rep
 * yes    58.3%  127912     3.156  40536 ./traces/ngram-gulliver2.rep
 * yes    59.8%   67012     1.751  38265 ./traces/ngram-moby1.rep
 * yes    60.0%   94828     2.664  35600 ./traces/ngram-shake1.rep
 * yes    95.1%   80000    15.600   5128 ./traces/syn-array.rep
 * yes    92.8%   80000     8.373   9555 ./traces/syn-mix.rep
 * yes    84.9%   80000     5.034  15891 ./traces/syn-string.rep
 * yes    86.8%   80000     4.726  16926 ./traces/syn-struct.rep
 p yes       --   80000    25.650   3119 ./traces/syn-array-scaled.rep
 p yes       --   80000    23.814   3359 ./traces/syn-string-scaled.rep
 p yes       --   80000    20.245   3952 ./traces/syn-struct-scaled.rep
 p yes       --   80000    16.932   4725 ./traces/syn-mix-scaled.rep
16 20     74.7% 1468359   145.531
Harmonic mean utilization = 74.7%.
Harmonic mean throughput (Kops/sec) = 10710.
Perf index = 60.0 (util) + 40.0 (thru) = 100.0/100
Running ./mdriver-emulate -s 180
Found benchmark throughput 8218 for cpu type Intel(R)Xeon(R)Gold6248RCPU@3.00GHz, benchmark regular
Throughput targets: min=4109, max=7396, benchmark=8218
...............................
Results for mm malloc:
  valid    util     ops   msecs  Kops/s  trace
   yes    81.5%      20     0.000      0 ./traces/syn-giantarray-short.rep
   yes   100.0%      20     0.000      0 ./traces/syn-giantmix-short.rep
   yes    93.4%    1000     0.000      0 ./traces/syn-giantarray-med.rep
   yes    93.2%   80000     0.000      0 ./traces/syn-giantarray.rep
   yes    93.0%   80000     0.000      0 ./traces/syn-giantmix.rep
   yes    81.3%      20     0.000      0 ./traces/syn-array-short.rep
   yes    76.7%      20     0.000      0 ./traces/syn-struct-short.rep
   yes    79.7%      20     0.000      0 ./traces/syn-string-short.rep
   yes    99.0%      20     0.000      0 ./traces/syn-mix-short.rep
   yes    83.7%      36     0.000      0 ./traces/ngram-fox1.rep
   yes    69.3%     757     0.000      0 ./traces/syn-mix-realloc.rep
 * yes    76.0%    5748     0.000      0 ./traces/bdd-aa4.rep
 * yes    71.6%   87830     0.000      0 ./traces/bdd-aa32.rep
 * yes    71.6%   41080     0.000      0 ./traces/bdd-ma4.rep
 * yes    71.8%  115380     0.000      0 ./traces/bdd-nq7.rep
 * yes    75.9%   20547     0.000      0 ./traces/cbit-abs.rep
 * yes    78.9%   95276     0.000      0 ./traces/cbit-parity.rep
 * yes    77.4%   89623     0.000      0 ./traces/cbit-satadd.rep
 * yes    71.6%   50583     0.000      0 ./traces/cbit-xyz.rep
 * yes    62.3%   32540     0.000      0 ./traces/ngram-gulliver1.rep
 * yes    58.3%  127912     0.000      0 ./traces/ngram-gulliver2.rep
 * yes    59.8%   67012     0.000      0 ./traces/ngram-moby1.rep
 * yes    60.0%   94828     0.000      0 ./traces/ngram-shake1.rep
 * yes    95.1%   80000     0.000      0 ./traces/syn-array.rep
 * yes    92.8%   80000     0.000      0 ./traces/syn-mix.rep
 * yes    84.9%   80000     0.000      0 ./traces/syn-string.rep
 * yes    86.8%   80000     0.000      0 ./traces/syn-struct.rep
 p yes       --   80000     0.000      0 ./traces/syn-array-scaled.rep
 p yes       --   80000     0.000      0 ./traces/syn-string-scaled.rep
 p yes       --   80000     0.000      0 ./traces/syn-struct-scaled.rep
 p yes       --   80000     0.000      0 ./traces/syn-mix-scaled.rep
16 20     74.7% 1468359     0.000
Harmonic mean utilization = 74.7%.
```
