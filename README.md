# TGO: Trace Guided Optimization

Chengyi Lux Zhang, CS265 Final Project Report

*This repo is originally forked from [chipyard](https://github.com/ucb-bar/chipyard.git), and this branch is created for hosting the CS265 final project report, where the root `README.md` is replaced with this report.*

## Abstract

This project implements and systematically evaluates various different profile-guided optimization methodologies, and proposes a novel methodology.  
Using core instruction trace, we can perform low-perturbation, high precision profile-guided optimization.

## Introduction

Profile-Guided Optimization (PGO) is a compiler optimization technique that leverages runtime profiling information to improve the performance of generated code.
Typical PGO invovles two phases: a profile collection phase, where runtime data is gathered into a representative profile, and an optimization phase, where the collected profiles guide certain transforms in the middle-end.
Traditionally, this profile is gathered by either instrumenting the generated code or sampling at runtime.
Instrumentation collects detailed runtime information but introduces heavy overhead, making it prohibitively expensive to deploy in production.
Sampling has signficiantly lower overhead but collects less accurate profile, making the generated code less performant.
In this project, we explore an alternative profiling method - hardware-assisted Core Instruction Trace (CIT).
The processor will report a trace of the branches and jumps it has executed, which can later be correlated with the executed binary to decoded a few execution trace.
Using this instruction trace, we can produce a profile with both low-perturbation and high-precision, improving the efficacy and usability of PGO.

## Core Instruction Trace

This section will discuss how we produce a core instruction trace (CIT).
*Relevant code for this section is in my fork of [rocketchip](https://github.com/iansseijelly/rocket-chip/tree/l_trace_encoder)*.

### Trace Format

Rocketchip is an open-source RISC-V system-on-chip (SOC) generator.
At its core is a parameterizable 5-stage pipelined in-order RISC-V core called rocketcore.
We implemented a custom trace encoder for rocketcore, and modified rocketchip to generate a trace encoder per core.
Different from the RISC-V trace standards (N-trace and E-trace), this implementation proposes a new trace encoding standard (L-trace) that focuses on self-hosted profiling trace.
Comparing to Branch History Mode (BHM) N and E traces that produces 1 bit per control flow change, L-trace adpots Branch Target Mode (BTM), which produces 1 packet (byte-aligned) per branch and jump.
It also annotates every packet with a delta-compressed timestamp, and adpots a compressed format that constraint most packet in 1 byte to minimize trace working set.
This provides the profiler with more timing information at minimal memory penalty, capable of tracking fine-grained timing information of how long each execution of each basic blocks took.
For the trace format standard, please refer to this [specification](https://iansseijelly.notion.site/L-Trace-A-Profiler-Friendly-Trace-Format-Specification-13692828bf74807aab6bc37e7f41a8f3?pvs=74).

### Trace Sinking

We implemented two trace sinking mechanisms.
In printing mode, a verilog black box exposes a 1-byte wide ready-valid decoupled interface to write raw trace bytes to a file using register-transfer-level (RTL) print statements.
This provides a easy port for exfiltrating trace data from RTL simulation.
However, notice that RTL prints are not real synthesizable hardware.
So we also provide a DMA mode, where we implemented a DMA engine that writes the trace data directly into DRAM, bypassing the cache hierachy by directly submitting memory requests to the memory bus.
This realistically reflects how modern processor trace mechanisms handle sinking.
For the evaluation of this project, we used printing mode as it is faster to run in simulation.

### Trace Decoding

We implemented the trace decoder in rust. You can access the codebase [here](https://github.com/iansseijelly/ltrace_decoder).
We adopt a decoupled implementation.
The frontend is responsible for decoding the encoded traces and produce events.
Each packet decoded is a special `Event`, where the `timestamp`, `from_pc` and `to_pc` are reported.
Each instruction reconstructed is an `Event` of `None` type which simply reports the `pc` and instruction content.
Events are pushed into a single-producer, multiple-consumer bus.
The backend consists of a series of recipients, each needs to process every decoded event.
We implemented `txt`, `json`, `gcda` and `afdo` receipients so far, each capable of decoding the trace to a different format.
We will discuss `gcda` and `afdo` format in detail in the following sections.
This design makes loading multiple receivers modular and clean.

## Baseline: Instrumentation

This section discusses how we produce the baseline instrumentation-based PGO results. Relevant code for this section is in  [baremetal-ide/lib/gcov](https://github.com/ucb-bar/Baremetal-IDE/tree/lbr/lib/gcov).

Instrumentation is the most traditional baseline for PGO.
If certain compiler flags are specified (e.g. `-fprofile-arcs`), gcc would insert counting code on certain arcs (basic block edges) that loads from a global symbol, increments it, and stores it back.
GCC is very judicious in inserting an optimally minimal number of profiling counters by constructing a spanning tree for the CFG, and only inserting counters for non-tree edges.
The rest of the edge counts could be statically inferred later.
Running the instrumented code would produce a `*.gcda` file that dumps all the counter values.
Another compiler flag (`-ftest-coverage`) makes gcc dump a basic block and edge layout in a `*.gcno` file.
Finally, coverage tools like `gcov` and pgo flags (`-fprofile-use`) would then combine `gcno` and `gcda` files to annotate the cfg with execution frequency and branch taken probabilities, which are then used for code coverage or PGO purposes.

### Baremetal GCOV

Since we implemented the trace encoder in hardware description languages, and we need to simulate them using register-transfer-level (RTL) simulation like `vcs`, the simulation throughput is slow, so we cannot afford the cost of booting an OS. Hence, we need to run baremetal workloads.
However, traditional instrumentation-based PGO like `gcc gcov` and `llvm profgen` expects a file system to dump the `*.gcda` file and does not compile baremetal.
Hence, we need to provide a custom runtime library to support the dumping of instrumentation counters through a serial output port.

We have ported [nasa-jpl/embedded-gcov](https://github.com/nasa-jpl/embedded-gcov), an open-source baremetal gcov runtime library to `baremetal-ide`, an open-source RISC-V baremetal integrated development environment.
We have also patched `embedded-gcov` to support `gcc-13`, where some `gcda` formats have changed.
When compiled with `-fprofile-arcs`, the runtime would initialize the global counter symbols to 0 before each runs, and dump the counters in `gcda` formats to stdout (which is a simulated UART link) upon completion.
A [script](https://github.com/ucb-bar/Baremetal-IDE/blob/lbr/scripts/gcov/dump_gcda.py) is then used to write the stdout results to the host file system, co-located with the `*.gcno` files.
This makes the `-fprofile-use` optimization compilation phase happily take the `*.gcda` profile as if it's native, and correctly optimizes the binary.

## Baseline: Sampling

This section discusses how we produce the baseline sampling-based PGO results. Relevant code for this section is in [baremetal-ide/lib/perf](https://github.com/ucb-bar/Baremetal-IDE/tree/lbr/lib/perf) (softwrare support) and [rocket-chip/.../CSR.scala](https://github.com/iansseijelly/rocket-chip/blob/l_trace_encoder/src/main/scala/rocket/CSR.scala) (hardware support).

As instrumentation introduces non-trivial runtime overhead, it is difficult to be deployed in production settings like datacenters.
An alternative approach, `AutoFDO`, leverages sampling-based profilers to collect runtime information.
From a high-level, `AutoFDO` uses linux `perf` to interrupt the running process every once in a while and collect the last branch record (LBR) samples.

LBR is a hardware feature of intel processors, where the core would always store the last N taken branches in a circular buffer, accessible throught model-specific registers (MSR).
Upon interruption, linux would take a special trap handler routine that probes these LBR records and store them for later report.
AutoFDO tools then leverages this perf report to generate a source-code-level profile, which is then fed into the compiler for optimization phase through a different gcc command line option `-fauto-profile=PATH`.

### LBR in RISC-V

To support this feature, we need to implement both the proper hardware and software support.
For hardware, rocketchip already generates timer interrupts through CLINT and rocketcore supports precise trapping.
All what's left is to implement an LBR record in the control-status registers (CSR), a RISC-V equivalent of MSR.

We implemented LBR in rocketcore CSR, with a few minor modifications.
First, we implemented a fixed LBR depth of 8, instead of being programmable.
Second, we implemented LBR record as a shift register instead of a circular buffer with a separate pointer register.
This makes the programming model slightly easier, at the cost of higher activate power.

### Baremetal Perf

In software, we added a runtime library that provides utilities to set timer interrupts and a trap handler routine to read and store the LBR records.
This library pre-allocates a static memory region for LBR, and reports everything at the end of the simulation, after the total execution time is reported.
Note that this choice deliberately favors sampling by reducing its perturbation lower than reality, as real linux perf needs to perform file I/O operations to store the records periodically.

We also added a script `dump_lbr.py` that post-processes the produced LBR records, and dump them into a text format.
Autofdo tools will then use this record to generate a binary symbol map of execution frequency.

## From Trace to Profile

This section discusses how we produce the trace-driven instrumentation-like and sampling-like profiles. Relevant code for this section is in the [decoder](https://github.com/iansseijelly/ltrace_decoder).

### Trace2GCDA

Typical `gcda` format only contains per-function records that looke like:

```gcda
FUNCTION: ident=*, lineno_checksum=*, cfg_checksum=*
  COUNTERS: num_counters, records...
```

It does not track metadata like file paths, function name, basic block and edge information.
These metadata are stored in the static `gcno` files produced at compile time.
To generate the .gcda file format, we first needs to create a .gcno format to optain the metadata, without instrumenting the generated code to avoid the overhead.
This is achieved by a special cmake config `GCNO_ONLY`, which turns on `-ftest-coverage` but not `-fprofile-arcs` under the hood.

We implemented a crate that parses the gcno format.
You can refer to the detailed implementation [here](https://github.com/iansseijelly/ltrace_decoder/tree/main/crates/gcno_reader).
This is a tedious process as the current upstream rust gcno parser crate is out of date, and the gcc gcno file format documentation is also ambiguous, so writing this parser involves many trials and errors.
We also found that there are a few bubble `0u32` in the gcno binary that must be read and thrown away, as they do not carry actual information, but prevents the parsing of the rest of the bytes.

Notice that gcov also comes with a utility command line tool `gcov_dump` that can dump detailed records of the gcno and gcda binary files.
We cross-checked our decoding result with the dump to make sure they match.
We chose to implement the gcno binary parser over a regex parser of the dump, because this gcno parser crate can potentially be useful to others interested in doing coverage or PGO using rust in the future.

After parsing the gcno, we post-process it to a different and cleaner data strucutre - `FunctionCFG`.
Looking back at this design choice, it is not strictly necessary, and this conversion could be avoided with cleaner abstractions and more careful design of the original `gcno` struct.

Regardless, each gcno edge comes with a flag field that characterises it.
We specifically pick out the ones in `FunctionCFG` that are non-tree edges, as these are the ones that will be instrumented if we turn on `-fprofile-arcs`, and hence we should produce a counter for.
We leverage the gcno basic block lines metadata to convert edges that looks like `block_id2 -> block_id6` into source line level edges that looks like `{file1:line17} -> {file1:line21; file1:line22}`.

We then used an open-source crate `addr2line` to match our trace record events `Event::TakenBranch | Event::NonTakenBranch | Event::InferrableJump | Event::UninferableJump`, which contain source and destination addresses, into source line number records.
We then use this record to match with our cfg, and bump the counts of certain records if anything hits.
Under the hood, `addr2line` uses the text symbol debug information from the `DWARF` in the elf, combined with static reconstruction of program flow, to infer the source line number of each instruction.

Additionally, we realized that all gcc CFGs have two special basic blocks.
`block0` is always a dummy entry block, and `block1` is always a dummy exit block.
`block1` is usually on the minimum spanning tree, so it can be safely ignored for our purpose.
However, edge `block0->block2` is usually an instrumented edge that is not on the tree.
Since block0 does not match to any line number, our previous method will always miss this record and always say that every function is never entered - which is not desirable.
To fix this, we added a special handler in the gcda receiver that matches our instruction execution record events `Event:None`, and keeps a table of the first instruction of every function.
We obtained this mapping by parsing the elf text symbols.
At the end of receiving everything, we will merge the two counter tables and produce a `gcda` file by writing all the counters to a file, as well as all the necessary metadata like checksum we obtained from the original `gcno` file.
This produces a legit gcda file that both gcov and PGO happily accepts.

We verified that for most cases, the generated coverage report from trace suggests exactly half of what the instrumented code suggests.
This is a desirable behavior for `embench`, as this benchmark has a cache warm-up phase that typically just runs the benchmark once.
Instrumentation always counts this in its record as it is a global counter, and trace is only activated during the actual run and not in the warm-up run, so this verifies that our mapping correctly captures this behavior.

### Trace2AFDO

Luckily, producing `afdo` format records is less challenging.
AFDO parser, `create_gcov` supports a variety of profiler formats, including `perf` and `text`.
Even though the native linux `perf` format is binary and difficult to produce, `text` is relatively easy.
However, due to a completely vaccum in documentation for autofdo, this process also involves a lot of trials and errors.
An afdo source txt file contains three sections: range, address, and branch.
Range tracks a section of code executed sequentially without control flow changes.
Then are generated as `{last_branch:dst-this_branch:src}`.
Address does not seem to have any effect on the produced profile, so we also make it 0.
Branch tracks each LBR record as-is.
Then are generated as `{this_branch:src->this_branch:dst}`.
The following code block demonstrates a generated record:

```text
# ranges
10 # number of ranges
10cf-10ff: 10 # this range happend 10 times
...
# address
0 # number of addresses
# branch
7 # number of branches
10ff->2048: 5 # this branch is taken 5 times
...
```

## Evaluation

For evaluation, we run `wikisort` from `embench` as our workload.

### Perturbation

In this section, we quantify the perturbation incurred by different profiling methodologies.
All experiments in this section are run under `-O1`.

![perturbation](/imgs/runtime.png)

As we can see, instrumentation incurs a runtime perturbation of 32.6%, which is quite significant.
Sampling incurs a runtime perturbation of 1.6%, which is acceptable though not neglegible.
Notice that here we used a sampling frequency of 40Hz, which is typically considered towards the lower side of the spectrum.
Also we do not account for the file I/O cost of flushing the records.

Tracing has no overhead at all.
This is exepcted, because profiling is handled by a separate hardware module, running as the core is running.
The only possible case where tracing would perturbe the workload is when the sink back-pressures the encoder, and hence halting the core until more sinking space is available, which rarely happens.

### Optimization Effectiveness

In this section, we quantify the performance gained by different profiling methodologies when using gcc native PGO.
The first experiment runs eveything under `-O1`.

![effectiveness](/imgs/effectiveness.png)

As we can see, both instrumentation-based and trace-based PGO are capable of correctly improving the runtime by 38%.
This is very great optimization.
*Notice that not all optimizations are uniquely obtainable from PGO - certain passes can be turned on by -O2,-O3, or by -fprofile-use or -fauto-profile, like `-finline-functions`.*
Nevertheless, this shows that using trace, we can generate high-quality profiles that produce good optimization results just like instrumentation, without paying the heavy overhead of instrumentation.

Both sample-afdo and trace-afdo produce code that does not significantly differ from the vannilla `-O1`.
By turning on the `-fopt-info` command line option, we observe that certain passes under afdo are indeed in effect and attempted to optimze the code.
In fact, afdo has decided to inline `blockswap`, a hot function, whereas traditional pgo decided not to, showing that they are just making different optimization decisions.
Comparing the optimization between pgo and afdo, afdo makes much more conservative decisions. It performed significantly less loop unrollings, and it also performed less common store sinking.
(Common store sinking can be improved with PGO after knowing loads and stores from previous loop iterations. `-fpredictive-commoning` is enabled by -O3, -fprofile-use, or -fauto-profile.)

It is yet a mystery why we are not getting much optimization from AutoFDO.
In literature, AutoFDO claims to achieve an average of 85% of the gains of traditional PGO.[1]
It could be that our profile provided to AFDO is not accurate or representative enough.
Or potentially we did not fully understand the AFDO file format and generated a bad profile.
Or maybe autofdo is no longer actively supported on gcc, and is only maintained for llvm?
(We did notice that the default version number AutoFDO produced is `875575082` and GCC expects `2`.)
We do not know yet, and correctly using AutoFDO in baremetal environment as a baseline comparison point is an ongoing effort.

As an initial attempt to unravel this mystery, we plotted the effectiveness of PGO and AFDO under different optimization switches.

In the following diagram, we plot the effectiveness of turning on PGO under different gcc optimization levels.

![pgo_on](/imgs/pgo.png)

In the following diagram, we plot the effectiveness of turning on AFDO under different gcc optimization levels.

![afdo_on](/imgs/afdo.png)

As shown, both pgo and afdo shows consistent performance across -O2 and -O3.
This suggests that the performance difference is not due to the optimization level, but rather the profile quality.

## Future Work

### Known Issues

Under `-O2` optimization level and above, for some functions, the current method of producing gcda from gcno may produce unmatching number of counter records.
"Unmatching" here means the number of counters does not match what gcc is expecting, which is the number of non-tree edges of CFG.
We suspect this is because `-fprofile-use` produces a separate CFG representation under the hood, and may mismatch with the produced gcno when certain optimizations are active.
For now, the workaround is to add `-Wno-error=coverage-mismatch` to supress this error.
Additionally, certain functions will be aggressively reorderd such that the DWARF address to source line mapping is no longer accurate.
Overall, these two factors causes a signficant drop in the generated profile quality when optimization level is `-O2+`.

To properly tackle this problem, we need to approach from first principle.
Fundamentally, this mismatch comes from post-processing the CFG metadata and relying on source line information for matching records, which is a heuristical approach that may not hold true when other aggressive optimizations are turned on.
We hence need to modify the compiler itself during instrumentation/CFG dumping passes, and dump special annotations, instead of counter increment code, that maps to certain instructions, that can be used as key joints for matching CFG with trace records, in order to get a fully accurate profile.

In an effort towrads this goal, we brought up a flow to compile LLVM from source, and compiling llvm `compiler-rt` runtime library support for RISC-V separately.
We added this flow to `baremetal-ide` and got hello-world running.

In the future, we still need to figure out a clean way to modify the llvm instrumentation passes to support a mode of dumping annotations instead of counter increment code.
This is possible as the insertion of instrumentation annotations and the conversion of annotation into counter increment code are two separate passes.
We also need to implement a baremetal profiling runtime like `embedded-gcov` for the baseline.

Additionally, there are many other forms of instrumentation beyond simply profiling branches taken or not.
For example, `-fprofile-timestamp` profiles the order of when functions are first called to reorder function paging for faster mobile application boot up time.
`-fprofile-value` tracks values of branch conditions.
Having a standard interface to correlate and inject traced information facilitates systematically leveraging various exisitng PGO passes beyond just inlining and loop unrolling.

### Long Term Project Ideas

I would like to start this section with a fun side story.
While following the GCC autofdo tutorial, I ran into a very strange performance bug for upstream gcc.
Specifically, when compiling `bubble_sort`:

```C
void bubble_sort (uint32_t *a, uint32_t n) {
    uint32_t i, t, s = 1;
    while (s) {
        s = 0;
        for (i = 1; i < n; i++) {
            if (a[i] < a[i - 1]) {
                t = a[i];
                a[i] = a[i - 1];
                a[i - 1] = t;
                s = 1;
            }
        }
    }
}
```

Under -O3 for gcc-11 and -O2+ for gcc-12+ for intel and amd cpus (arm and risc-v are unaffected), the generated code performance is significantly worse, even comparing to -O0 and -O1.
After investigation, this bug comes from auto-vectorization of basic blocks.
After submitting bugs to gcc bugzilla [#117717](https://gcc.gnu.org/bugzilla/show_bug.cgi?id=117717), a gcc dev Andrew helped me identified the root cause:

```x86-assembly
.L4:
  movq (%rax), %xmm0
  pshufd $0xe5, %xmm0, %xmm1
  movd %xmm0, %edx
  movd %xmm1, %ecx
  cmpl %edx, %ecx
  jnb .L3
  pshufd $225, %xmm0, %xmm0
  movl $1, %edi
  movq %xmm0, (%rax)
.L3:
  addq $4, %rax
  cmpq %rsi, %rax
  jne .L4


vs:
.L4:
  movl 4(%rax), %edx
  movl (%rax), %ecx
  cmpl %ecx, %edx
  jnb .L3
  movl %ecx, 4(%rax)
  movl $1, %edi
  movl %edx, (%rax)
.L3:
  addq $4, %rax
  cmpq %rsi, %rax
  jne .L4

Note the aarch64 cost model rejects the vectorization. 

X86_64 (on the trunk) cost model says:
/app/example.cpp:10:26: note: Cost model analysis for part in loop 2:
  Vector cost: 44
  Scalar cost: 48

While aarch64 says:
/app/example.cpp:10:26: note: Cost model analysis for part in loop 2:
  Vector cost: 12
  Scalar cost: 4
```

The x86_64 cost model does not see the unaligned load-after-store vector pattern from this sequence:

```x86-assembly
  movq %xmm0, (%rax)
  addq $4, %rax
  ...
  movq (%rax), %xmm0
```

This matches the profiling results I got from running intel Vtune, suggesting that a load waiting for store caused significant backend memory blocking.

Imagine that this faulty vectorization is causing millions of sorts wasting cpu cycles on trivial inefficiencies...
And this is also so hard to detect, and even harder to pinpoint the root cause.
Potentially, can we apply PGO to passes like auto-vectorization that struggle to use static heuristics as they do not model runtime dynamic behaviors?
This is a pontetially interesting and influential project.

Traditionally, it is hard to collect information on how long a block has run in PGO because of the prohibitively expensive perturbation.
However, with the new trace format we proposed, we naturally get the execution timing information of each basic block along with what branches has executed.
So far we have just ingored these timestamps, as existing PGO passes do not use them.
However, these information can potentially inform us about whether we should accept or reject certain optimizations that are aggressive and may potentially harm performances.

I am envisioning a new PGO paradigm where auto-vectorization produces two copies of the basic block living in the same binary, one vectorized and one remains scalar, chosen randomly under some conditions.
Then we stimulate the binary with different conditions to sufficiently execute both paths under various conditions.
We then take the profiling results from traces to tell which path has better performance, and make auto-vectorization decisions accordingly.

## Citation

```latex
@inproceedings{45290,title	= {AutoFDO: Automatic Feedback-Directed Optimization for Warehouse-Scale Applications},author	= {Dehao Chen and David Xinliang Li and Tipp Moseley},year	= {2016},booktitle	= {CGO 2016 Proceedings of the 2016 International Symposium on Code Generation and Optimization},pages	= {12-23},address	= {New York, NY, USA}}
```
