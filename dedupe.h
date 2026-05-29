#ifndef DEDUPE_H
#define DEDUPE_H

int init_dedupe_working_set_seeds(struct thread_data *td, bool global_dedupe);
int init_global_dedupe_working_set_seeds(void);

int init_global_dedupe_buffers(struct thread_data *td, bool global_dedup, unsigned long long total_size);
unsigned long long calc_total_size(void);

void dedupe_encode_dedupe_set(char *buf, unsigned long long page_size, unsigned long long dedupe_set);
unsigned long long dedupe_get_dedupe_set(struct thread_data *td, unsigned long long io_offset_in_dedupe_blocks);

#endif
