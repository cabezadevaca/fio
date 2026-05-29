import hashlib
import argparse
import glob
import csv

def add_to_dedupe_cache(tdc, sha256, inc=1):
    if tdc.get(sha256) is None:
        tdc[sha256] = 1
        return False
    else:
        tdc[sha256] += inc
        return True

def merge_dedupe_caches(global_tdc, file_tdc):
    c = 0
    for k,v in file_tdc.items():
        if add_to_dedupe_cache(global_tdc, k, v):
            c += 1
    return c

def num_patterns(tdc):
    result = sum(1 for value in tdc.values() if value > 1)
    return result

def fio_dedupe_set(value):
    if value != 0:
        return True, value
    return False, 0

def vdb_dedupe_set(value):
    if value & 1:
        return True, value >> 32
    return False, 0

def get_dedupe_set(block, set_in_last_bytes):
    if set_in_last_bytes:
        dedupe_set = block[-8:]
    else:
        dedupe_set = block[:8]
    value = int.from_bytes(dedupe_set, byteorder='little', signed=False)
    if args.tool == 'fio':
        return fio_dedupe_set(value)
    elif args.tool == 'vdbench':
        return vdb_dedupe_set(value)
    else:
        assert(False)


parser = argparse.ArgumentParser(description='Process io failes anf check for DRR in files and across files')
parser.add_argument('-f', '--files', nargs='+', help='List of files to process')
parser.add_argument('-d', '--dedupe_unit_size', type=int, default=4096, help='Dedupe unit size (bytes!)')
parser.add_argument('-c', '--check_dedupe_set', action='store_true', help='Dump dedupe set distribution')
parser.add_argument('-o', '--output', default=None, help='Output CSV file name')
parser.add_argument('--chart', default=None, help='Output image file name for the scatter plot')
parser.add_argument('-t', '--tool', default='fio', choices=['fio', 'vdbench'], help='Type of benchmark tool.')
parser.add_argument('--set_in_last_bytes', action='store_true', help='Set # is encoded in last 8 bytes (fio)')


args = parser.parse_args()

# Dedupe cache
global_dedupe_cache = {}
total_block_cnt = 0
common_patterns = 0
file_cnt = 0

comp_cache = {}

if args.files:

    # Create a list of all files to acccept globbed 
    file_list = []
    start_and_ends = []
    for file in args.files:
        globbed_files = glob.glob(file)
        if globbed_files:
            file_list.extend(globbed_files)
        else:
            file_list.append(file)

    print(f'Working on {file_list}')

    values = []
    global_block = 0
    max_set = 0
    for file in file_list:
        file_cnt += 1
        block_cnt = 0
        dedup_cnt = 0
        file_dedupe_cache = {}
        max_seq = 0
        current_seq = 0
        max_seq_pos = 0
        seq_pos =0
        not_dedupable_found = 0
        with open(file, 'rb') as f:
            while True:
                block = f.read(args.dedupe_unit_size)
                if not block:
                    break

                block_cnt += 1
                global_block += 1

                dedupable, dedupe_set = get_dedupe_set(block, args.set_in_last_bytes)
                # not dedupable but found in the dedupe cache 
                if not dedupable and global_dedupe_cache.get(block) is not None and global_dedupe_cache.get(block) > 1:
                    not_dedupable_found += 1

                sha256 = hashlib.sha256(block).hexdigest()
                if add_to_dedupe_cache(file_dedupe_cache, sha256):
                    dedup_cnt += 1

                if args.check_dedupe_set:
                    if dedupable:
                        if current_seq > max_seq:
                            max_seq = max(max_seq, current_seq)
                            max_seq_pos = seq_pos
                        current_seq = 0
                    else:
                        if current_seq == 0:
                            seq_pos = global_block
                        current_seq += 1

                    max_set = max(max_set, dedupe_set)
                    values.append((file, global_block, dedupe_set, (f"0x{dedupe_set:016X}"), dedupe_set, (f"0x{dedupe_set:016X}")))
        common_patterns += merge_dedupe_caches(global_dedupe_cache, file_dedupe_cache)
        total_block_cnt += block_cnt

        print(f"File: {file}, Blocks: {block_cnt}, unique: {block_cnt-dedup_cnt}, ratio {block_cnt/(block_cnt-dedup_cnt):.3f}, deduped withing file: {dedup_cnt}, " + 
              f"patterns in the file: {num_patterns(file_dedupe_cache)}, patterns across files: {num_patterns(global_dedupe_cache)}, " + 
              f"max run of uniques@block={max_seq}@{max_seq_pos}, not dedubable found being deduped {not_dedupable_found}"
              )
else:
    print('No files specified.')

print(f"\nBlocks: {total_block_cnt}, unique: {len(global_dedupe_cache)}, ratio: {total_block_cnt/len(global_dedupe_cache):.3f}, common patterns: {common_patterns}/{num_patterns(global_dedupe_cache)}, "
      f"max dupes={max(global_dedupe_cache.values())}, max dedupe set is {max_set}")

if args.output is not None:
    with open(args.output, 'w', newline='') as csvfile:
        csvwriter = csv.writer(csvfile)
        csvwriter.writerow(['file', 'Block Number', 'Int Value', 'Hex Value', 'Vdb set', 'Vdb set hex'])
        csvwriter.writerows(values)
    with open(f'starts-{args.output}', 'w', newline='') as csvfile:
        csvwriter = csv.writer(csvfile)
        csvwriter.writerow(['file', 'Block Number', 'Int Value', 'Hex Value', 'Vdb set', 'Vdb set hex'])
        csvwriter.writerows(start_and_ends)
