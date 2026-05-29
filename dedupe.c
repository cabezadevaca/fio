#include "fio.h"
#include "fio_time.h"
#include <math.h>
#include "lib/axmap.h"

#define DEDUPE_BUFFER_SIZE (1024 * 1024)

typedef struct bits_run {
	unsigned long value;
	unsigned long count;
} bits_run;

unsigned long long calc_total_size(void) 
{
	unsigned long long total_size = 0;
	unsigned int lg_cnt = 0;

	struct thread_data *global = get_global_options();
	if (global->o.dedupe_total_ta != 0) {
		dprint(FD_DEDUPE, "Using global TA %llu\n", global->o.dedupe_total_ta);
		return global->o.dedupe_total_ta;
	}
		

	lg_cnt = global->o.lg_cnt;
	// Calculate total size on this load generator and assume other 
	// hosts have the same total size of volumes.
	for_each_td(td) {
		if (!td->o.dedupe_global)
			continue;
		total_size += td->o.size;
	} end_for_each();

	return lg_cnt * total_size;
}

static void log_time_elapsed(struct timespec *start_ts, const char *msg)
{
	if (d_is_set(FD_DEDUPE)) {
		double elapsed = ntime_since_now(start_ts) / 1000000000.; // seconds
		dprint(FD_DEDUPE, "%s took %f sec\n", msg, elapsed);
	}
}

/**
 * Sets an offset of each volume as it has been one global address space
 */
void set_td_start_offset(unsigned long long total_size) 
{
	unsigned long long start = 0;
	bool start_offset_set = false;
	for_each_td(td) {
		if (!td->o.dedupe_global)
			continue;
		if (td->o.lg_num > 0 && !start_offset_set) {
			start = (td->o.lg_num-1) * total_size / td->o.lg_cnt;
			start_offset_set = true;
			dprint(FD_DEDUPE, "Total LGs is %u, LG is %u, Set the lg's offset to: %llu\n", 
				td->o.lg_cnt, td->o.lg_num, start);
		}

		td->global_offset = start;
		start += td->o.size;

	} end_for_each();

}

void shuffle_runs(struct thread_data *td, struct bits_run *runs, int run_count)
{
	for (int i = run_count - 1; i > 0; i--) {
		int j = rand_between(&td->dedupe_working_set_index_state, 0, i);
		struct bits_run temp = runs[i];
		runs[i] = runs[j];
		runs[j] = temp;
	}
}

int flatten_runs(struct thread_data *td, struct bits_run *runs, int run_count, int bits_needed)
{
	int i, bit_idx = 0;
	unsigned int nr_set = 0;
	td->dedupe_bitmap = axmap_new(bits_needed);

	if (!td->dedupe_bitmap) {
		return 1;
	}
	for (i = 0; i < run_count; i++) {
		if (runs[i].value == 1) {
			nr_set += axmap_set_nr(td->dedupe_bitmap, bit_idx, runs[i].count);
			bit_idx += runs[i].count;
		} else {
			bit_idx += runs[i].count;
		}
	}
	if (d_is_set(FD_DEDUPE)) {
		dprint(FD_DEDUPE,"flatten_runs: td %p, nr_set %u, bits_needed %u, deduped %.1f\n", 
			(void*)td, nr_set, bits_needed, (bits_needed-nr_set)*100./bits_needed);
	}
	return 0;
}

int generate_bitmap(struct thread_data *td, unsigned long long bits_needed, unsigned long long bits_to_set)
{
	struct timespec ts;
	struct bits_run *runs;
	unsigned long long zero_bits = bits_needed - bits_to_set;
	unsigned long long one_bits = bits_to_set;
	int run_len;
	int run_idx = 0;

	runs = malloc(sizeof(struct bits_run) * bits_needed);
	if (!runs) {
		log_err("fio: could not allocate buffer for filling bitmap %u\n", td->thread_number);
		return 1;
	}

	fill_start_time(&ts);

	while (zero_bits > 0 || one_bits > 0) {
		run_len = rand_between(&td->dedupe_working_set_index_state, td->o.dedupe_min_run, td->o.dedupe_max_run);
		if (one_bits > zero_bits) {
			run_len = (run_len > one_bits) ? one_bits : run_len;
			runs[run_idx++] = (struct bits_run) {1, run_len};
			one_bits -= run_len;
		} else {
			run_len = (run_len > zero_bits) ? zero_bits : run_len;
			runs[run_idx++] = (struct bits_run) {0, run_len};
			zero_bits -= run_len;
		}
	}
	log_time_elapsed(&ts, "generate_bitmap (runs)");
	
	shuffle_runs(td, runs, run_idx);
	log_time_elapsed(&ts, "generate_bitmap (shuffle)");

	if (d_is_set(FD_DEDUPE)) {
		int i;
		unsigned long long zero_cnt = 0;
		unsigned long long ones_cnt = 0;
		for (i = 0; i < run_idx; i++) {
			if (runs[i].value == 1) {
				ones_cnt += runs[i].count;
			} else {
				zero_cnt += runs[i].count;
			}
		}
		dprint(FD_DEDUPE, "generate_bitmap: zero count is %llu, ones count is %llu, bits to set %llu, \n", zero_cnt, ones_cnt, bits_to_set);
	}

	if (flatten_runs(td, runs, run_idx, bits_needed)) {
		free(runs);
		return 1;
	}
	free(runs);
	log_time_elapsed(&ts, "generate_bitmap (total)");

	return 0;
}

int init_dedupe_buffer(struct thread_data *td) 
{
	unsigned long buf_size = DEDUPE_BUFFER_SIZE;
	char *buf;
	unsigned long long num_pages, page_size;
	struct frand_state dedupe_working_set_state = {0};

	buf_size = max(td->o.max_bs[DDIR_WRITE], (unsigned long long) buf_size);
	
	td->dedupe_buffer = malloc(buf_size);
	if (!td->dedupe_buffer) {
		log_err("fio: could not allocate dedupe buffer for %u\n", td->thread_number);
		return 1;
	}

	page_size = min_not_zero(td->o.dedupe_block_size, (unsigned long long) td->o.compress_chunk);

	frand_copy(&dedupe_working_set_state, &td->dedupe_buf_state);
	num_pages = buf_size / page_size;
	if (num_pages < 1) {
		log_err("fio: num dedupe pages in a buffer is 0 %u\n", td->thread_number);
		return 1;
	}

	// Fill up the dedupe pattern buffer with random data
	buf = td->dedupe_buffer;
	for (unsigned long long i = 0; i < num_pages; i++) {
		if (td->o.compress_percentage) {
			fill_random_buf_percentage(&dedupe_working_set_state, 
					buf, td->o.compress_percentage,
					page_size, page_size,
					td->o.buffer_pattern,
					td->o.buffer_pattern_bytes);

		} else {
			fill_random_buf(&dedupe_working_set_state, buf, page_size);
		}
		dedupe_encode_dedupe_set(buf, page_size, 0);
		buf += page_size;
	}
	return 0;
}

void dedupe_encode_dedupe_set(char *buf, unsigned long long page_size, unsigned long long dedupe_set)
{
	unsigned long long *buf_ptr = (unsigned long long *) (buf+page_size-sizeof(unsigned long long));
	*buf_ptr = dedupe_set;
}

unsigned long long dedupe_get_dedupe_set(struct thread_data *td, unsigned long long io_offset_in_dedupe_blocks)
{

	io_offset_in_dedupe_blocks = io_offset_in_dedupe_blocks + td->global_offset / td->o.dedupe_block_size;
	return io_offset_in_dedupe_blocks % td->num_unique_pages;
}



// Initialize global dedupe buffer of 1MB size for set# based deduplication
// The dedupe pattern buffer comprises of dedupe_unit_size blocks. 8 bytes of each is set to a relevant set# 
// later in during an IO generation based on an IO LBA and number of dedupe sets.
// Parameters: td - The thread data structure.
//            global_dedup - A boolean indicating whether global deduplication is enabled.
//            total_size - The total test area size in bytes.
// Return type: int
// Returns: 0 if the buffers are initialized successfully, 1 otherwise.
int init_global_dedupe_buffers(struct thread_data *td, bool global_dedup, unsigned long long total_size)
{
	unsigned long long total_unique_pages, dedupe_set_size;
	unsigned long long bits_to_set, bits_needed;
	unsigned int adjusted_dedupe_percentage;
	
	if (!td->o.dedupe_percentage || !(td->o.dedupe_mode == DEDUPE_MODE_WORKING_SET2))
		return 0;

	// the whole dedupe set in bytes
	dedupe_set_size = total_size * (unsigned long long)td->o.dedupe_working_set_percentage / 100;
	td->num_unique_pages = dedupe_set_size / td->o.dedupe_block_size;

	// Important - round the dedupe set to the nearest multiple of min_bs. 
	// Otherwise global dedupe will not work properly when dedupe set size is greater than a volume size.
	// Example: 48x11MB (2816 pages) volumes with 5% dedupe set will call for 6758 pages in the dedupe set. 
	// It is 105.593 IOs of 256K. So 106s IO will rollover and start from dedupe set 0. The patterns will shift and 
	// may be not repeated. Rounding that to 105 (6720 pages) will fix the rollover issue. Effective dedupe set size 
	// becomes 4.7% of the total volumes' size.
	if (td->o.min_bs[DDIR_WRITE] != 0) {
		dedupe_set_size = dedupe_set_size / td->o.min_bs[DDIR_WRITE] * td->o.min_bs[DDIR_WRITE];
		dprint(FD_DEDUPE, "td %p, num unique pages: %llu, rounded to: %llu, effective set %.2f\n", 
			(void*)td, td->num_unique_pages, dedupe_set_size/td->o.dedupe_block_size, 
			dedupe_set_size * td->o.dedupe_block_size *100. / total_size/td->o.dedupe_block_size
		);
		td->num_unique_pages = dedupe_set_size / td->o.dedupe_block_size;
	}

	if (init_dedupe_buffer(td)) {
		return 1;
	}

	total_unique_pages = total_size / td->o.dedupe_block_size;
	// round up 
	adjusted_dedupe_percentage = ceil(td->o.dedupe_percentage + td->num_unique_pages * 100. / total_unique_pages);

	dprint(FD_DEDUPE, "td %p, adjusted dedupe: %u\n", (void*)td, adjusted_dedupe_percentage);

	// 100% is treated specially in io_c, so set 101%
	// TODO: do we still use this?
	if (adjusted_dedupe_percentage == 100) {
		adjusted_dedupe_percentage = 101;
	}
	// set it for random dedupe selection too
	td->o.dedupe_percentage = adjusted_dedupe_percentage;

	if (td->o.use_unique_bitmap) {
		bits_needed = td->o.size / td->o.dedupe_block_size;
		bits_to_set = bits_needed * (1 - adjusted_dedupe_percentage / 100.);
		if (generate_bitmap(td, bits_needed, bits_to_set)) {
			return 1;
		}
	}
	return 0;
}

/**
 * initializes the global dedup workset.
 * this needs to be called after all jobs' seeds
 * have been initialized
 */
int init_global_dedupe_working_set_seeds(void)
{
	unsigned long long total_size = calc_total_size();

	unsigned int dedupe_mode = DEDUPE_MODE_WORKING_SET;
	for_each_td(td) {
		if (!td->o.dedupe_global)
			continue;

		switch (td->o.dedupe_mode) {
			case DEDUPE_MODE_WORKING_SET:
			if (init_dedupe_working_set_seeds(td, 1))
				return 1;
			break;
			case DEDUPE_MODE_WORKING_SET2:
			if (init_global_dedupe_buffers(td, 1, total_size))
				return 1;
			dedupe_mode = DEDUPE_MODE_WORKING_SET2;
			break;
			default:
				assert(0);
		}
	} end_for_each();

	if (dedupe_mode == DEDUPE_MODE_WORKING_SET2)
		set_td_start_offset(total_size);

	return 0;
}

int init_dedupe_working_set_seeds(struct thread_data *td, bool global_dedup)
{
	int tindex;
	struct thread_data *td_seed;
	unsigned long long i, j, num_seed_advancements, pages_per_seed;
	struct frand_state dedupe_working_set_state = {0};

	if (!td->o.dedupe_percentage || !(td->o.dedupe_mode == DEDUPE_MODE_WORKING_SET))
		return 0;

	tindex = td->thread_number - 1;
	num_seed_advancements = td->o.min_bs[DDIR_WRITE] /
		min_not_zero(td->o.min_bs[DDIR_WRITE], (unsigned long long) td->o.compress_chunk);
	/*
	 * The dedupe working set keeps seeds of unique data (generated by buf_state).
	 * Dedupe-ed pages will be generated using those seeds.
	 */
	td->num_unique_pages = (td->o.size * (unsigned long long)td->o.dedupe_working_set_percentage / 100) / td->o.min_bs[DDIR_WRITE];
	td->dedupe_working_set_states = malloc(sizeof(struct frand_state) * td->num_unique_pages);

	dprint(FD_DEDUPE, "Dedupe block size %llu, pages %llu\n", td->o.min_bs[DDIR_WRITE], td->num_unique_pages);

	if (!td->dedupe_working_set_states) {
		log_err("fio: could not allocate dedupe working set for %u, dedupe bs %llu, dedupes set pages %llu\n", 
			td->thread_number, td->o.min_bs[DDIR_WRITE], td->num_unique_pages);
		return 1;
	}

	frand_copy(&dedupe_working_set_state, &td->dedupe_buf_state);
	frand_copy(&td->dedupe_working_set_states[0], &dedupe_working_set_state);
	pages_per_seed = max(td->num_unique_pages / thread_number, 1ull);
	for (i = 1; i < td->num_unique_pages; i++) {
		/*
		 * When compression is used the seed is advanced multiple times to
		 * generate the buffer. We want to regenerate the same buffer when
		 * deduping against this page
		 */
		for (j = 0; j < num_seed_advancements; j++)
			__get_next_seed(&dedupe_working_set_state);

		/*
		 * When global dedup is used, we rotate the seeds to allow
		 * generating same buffers across different jobs. Deduplication buffers
		 * are spread evenly across jobs participating in global dedupe
		 */
		if (global_dedup && i % pages_per_seed == 0) {
			td_seed = tnumber_to_td(++tindex % thread_number);
			frand_copy(&dedupe_working_set_state, &td_seed->dedupe_buf_state);
		}

		frand_copy(&td->dedupe_working_set_states[i], &dedupe_working_set_state);
	}

	return 0;
}
