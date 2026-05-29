# Overview
**working_set2** dedupe mode introduced the following changes:
1) *dedupe_buf_randrepeat* - when all the random seeds set randomly, keep a seed for dedupe buffers predictable across multiple runs. It may be needed for warming up a dedupe cache to have hits across runs. Positions of dedupe blocks are random to avoid *dedupe same* situation.
2) Dedupe pattern buffer of max io size prefilled with dedupe buffer seed (see #1). A dedupe set is determined based on an offset of an IO and is encoded into last 8 bytes of a block. Existing **working_set** implementation keeps 48 bytes of dedupe states for each dedupable block which can cause OOM for larger volumes (and number of jobs!) and larger dedupe_set percentage. 
3) *dedupe_use_unique_bitmap* use a bitmap of deduped/unique blocks preallocated in advance. Allows to configure continuous runs of such blocks with *dedupe_min_run (def=1)* and *dedupe_max_run (def=32)*. Runs can form a bigger sequences of such blocks because of random shuffling. 
4) *dedupe_bs* - dedupe unit block size to set it different from IO size.
5) For mutli-client configuration, *dedupe_loadgen_num* - sequence number of a client and *dedupe_loadgen_count* - number of clients to setup global, cross volume deduplication correctly.

**working_set2** preferable usage is with volumes of the same size and count on each of an IO generating client.

# Examples:
Use ./tools/check_dedup.py to check deduplication per file and cross deduplication between files.
For example FIO configurations, see examples/dedupe/*.fio files.

## Testing dedupe_buf_randrepeat=1 and randrepeat=0
> $ rm test-runs/dedupe-sim.bin; ./fio examples/dedupe/dedupe-repeatablebuf.fio; mv test-runs/dedupe-sim.bin test-runs/dedupe-sim.bin.1; ./fio examples/dedupe/dedupe-repeatablebuf.fio
>
> $ python3.9 ./tools/check_dedup.py -f test-runs/dedupe-sim.bin* -d 4096 -t fio --set_in_last_bytes -c
>
> Working on ['test-runs/dedupe-sim.bin', 'test-runs/dedupe-sim.bin.1']
>
> File: test-runs/dedupe-sim.bin, Blocks: 51200, unique: 25472, ratio 2.010, deduped withing file: 25728, patterns in the file: 2560, patterns across files: 0, max run of uniques@block=13@4202, not dedubable found being deduped 0
>
> File: test-runs/dedupe-sim.bin.1, Blocks: 51200, unique: 25582, ratio 2.001, deduped withing file: 25618, patterns in the file: 2560, patterns across files: 2560, max run of uniques@block=16@81615, not dedubable found being deduped 0
>
> Blocks: 102400, unique: 48494, ratio: **2.112**, common patterns: **2560/2560**, max dupes=19, max dedupe set is 2559


## Testing dedupe_buf_randrepeat=0 and randrepeat=0
> $ rm dedupe-sim.bin; ./fio examples/dedupe/dedupe-rndwr-randombuf.fio; mv dedupe-sim.bin dedupe-sim.bin.1; ./fio examples/dedupe/dedupe-rndwr-randombuf.fio
>
> $ python3.9 ./tools/check_dedup.py -f test-runs/dedupe-sim.bin* -d 4096 -t fio --set_in_last_bytes -c
>
> Working on ['test-runs/dedupe-sim.bin', 'test-runs/dedupe-sim.bin.1']
>
> File: test-runs/dedupe-sim.bin, Blocks: 51200, unique: 25964, ratio 1.972, deduped withing file: 25236, patterns in the file: 2560, patterns across files: 0, max run of uniques@block=15@23641, not dedubable found being deduped 0
>
> File: test-runs/dedupe-sim.bin.1, Blocks: 51200, unique: 25611, ratio 1.999, deduped withing file: 25589, patterns in the file: 2560, patterns across files: 0, max run of uniques@block=13@94410, not dedubable found being deduped 0
>
> Blocks: 102400, unique: 51575, ratio: 1.985, **common patterns: 0/0**, **max dupes=1**, max dedupe set is 2559

## Testing with dedupe_use_unique_bitmap=1
Advantages of a preallocated bitmap of uniques/deduped blocks
1) Meeting the exact dedupe percentage
2) Configurable max and min runs of unique/deduped blocks which is more natural than fully random choice. Default is a random number in [1,32] range. 

> $ rm test-runs/dedupe-sim.bin*; ./fio examples/dedupe/dedupe-with-bitmap.fio
>
> python3.9 ./tools/check_dedup.py -f test-runs/dedupe-sim.bin* -d 4096 -t fio --set_in_last_bytes -c
>
> Working on ['test-runs/dedupe-sim.bin']
>
> File: test-runs/dedupe-sim.bin, Blocks: 51200, unique: 25600, ratio 2.000, deduped withing file: 25600, patterns in the file: 2560, patterns across files: 0, **max run of uniques@block=127@13470**, not dedubable found being deduped 0
>
> Blocks: 51200, unique: 25600, **ratio: 2.000**, common patterns: 0/0, max dupes=1, max dedupe set is 2559

## Testing global dedupe over multiple files when dedupe set is larger than a volume size. Simulating load from two clients

> $ ./fio examples/dedupe/dedupe-48vols-host1.fio; ./fio examples/dedupe/dedupe-48vols-host2.fio --debug dedupe
>
Each volume is not dedupable to itself because number of dedupe patterns is greater than blocks in a volume. Global dedupe is met.

> ...
> File: test-runs/fio-48vols/fio9.bin, Blocks: 2560, unique: 2560, ratio 1.000, deduped withing file: 0, patterns in the file: 0, patterns across files: 6144, max run of uniques@block=115@122112, not dedubable found being deduped 0
> 
> Blocks: 122880, unique: 61440, ratio: 2.000, common patterns: 61440/6144, max dupes=17, max dedupe set is 6143



