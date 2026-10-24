# causext

This repository explains causal profiling and gives an example of it. The program is also a black-box utility for ordinary use. It runs one Windows command as that command already stands, and it reports which code changed the command's end-to-end time.

The code is one C file with no `#include` and no C runtime. Windows entry points are declared in the source. Build with `build.bat`. `config.bat` holds the path to the Visual Studio x64 tools; set `VCVARS64` in the environment to override it for one run. `build.bat run` builds and runs the self-test. The executable is `out\causext.exe`. Profiles, the self-test log, and the build products all go under `out\`, which is gitignored.

## Check

```text
build.bat run
```

A failure exits with the source line of the check that failed. The log is `out\self_test.txt`.

## Causal profile

```text
out\causext.exe --runs 20 --samples 30 -- my.exe args
```

During each run the profiler suspends one of the command's threads, records the instruction pointer, holds that suspend for a random measured time, and resumes the thread. It does this many times. The run's end-to-end time and every pause go into `out\ledger_locations.txt`.

Debug symbols build two more ledgers. `ledger_blocks.txt` sums pauses that land on the same source line, or in the same 16-byte window of a function when the PDB has no line. `ledger_functions.txt` sums pauses that land in the same function. An instruction with no symbol stays its own column.

`rank_locations.txt`, `rank_blocks.txt`, and `rank_functions.txt` each start with the estimator specification. They list columns by estimated impact on end-to-end runtime. Impact is the partial effect of the pauses at that column, in microseconds per completed run. The rank file is the description of how those effects are separated from the pauses that landed everywhere else.

## Sampling profile, for comparison

```text
out\causext.exe --sampling --runs 3 --samples 40 --out out\sample -- my.exe args
```

`--sampling` reads every thread's instruction pointer and resumes it at once. The same three rank files then list code by its share of those samples. That share is where threads were found. The effect on end-to-end time is in the causal rank files. The rank header says which kind of file it is.

`out\causext.exe --help` lists the options and the output files.

## Example

`--burn` is a workload in this program. One thread runs a counted loop for about the requested number of milliseconds. A second thread spins on another core until that loop finishes. The process also has threads blocked in `NtWaitForWorkViaWorkerFactory`. Pausing the loop lengthens the process by about the length of the pause. Pausing the spin, or a thread already blocked in that wait, leaves the end-to-end time where it was.

```text
out\causext.exe --runs 12 --samples 6 --pause-min-us 120000 --pause-max-us 400000 --gap-min-us 200 --gap-max-us 800 --warmup-us 2500000 --permutations 20 --seed 42 --out out\causal -- out\causext.exe --burn 4500

out\causext.exe --sampling --runs 2 --samples 40 --gap-min-us 200 --gap-max-us 800 --warmup-us 2500000 --seed 42 --out out\sample -- out\causext.exe --burn 4500
```

The warmup covers the burn's own calibration, so the pauses fall inside the timed loop.

`out\causal\rank_functions.txt` ranks `causext.exe!burn_critical` first. Its slope is about one microsecond of end-to-end time per microsecond of pause, the two estimators agree, and the row is stable. Runs that never paused that function finish in a tight band. A run that spent more than a second paused on `NtWaitForWorkViaWorkerFactory` finishes in that same band, and that column's slope is near zero. `causext.exe!burn_slack` stays in the table and is marked unstable. The two estimators disagree, and those runs do not get longer when the spin is the thread that was held.

`out\sample\rank_functions.txt` gives the waiting threads most of the samples, because several of them are blocked for the whole run. `burn_critical` and `burn_slack` each receive the share of one thread. The sampling rank reports where threads were found. The causal function rank reports which of those places changed the end-to-end time.

## License

The code is covered by [LICENSE](LICENSE). Use is unrestricted. There is no warranty, no guarantee, and no liability.
