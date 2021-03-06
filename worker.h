/* Copyright (c) 2021, Evgeny Baskov. All rights reserved */

#ifndef WORKER_H_
#define WORKER_H_ 1

#include <stddef.h>

void submit_work(void (*func)(int, void *), const void *data, size_t data_size);
void init_workers(void);
void drain_work(void);
void fini_workers(_Bool force);

extern int nproc;

#endif

