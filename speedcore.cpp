/*
 *  Copyright (c) 2011 Bonelli Nicola <bonelli@antifork.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include <iostream>
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <iterator>
#include <string>
#include <thread>
#include <chrono>
#include <limits>
#include <cmath>
#include <atomic>

#include <pthread.h> // pthread_setaffinity_np

struct queue_header
{
    struct 
    {
        volatile unsigned long index;
    } producer __attribute__((aligned(64)));

    struct 
    {
        volatile unsigned long index;
    } consumer __attribute__((aligned(64)));

    unsigned long size;             /* number of slots */
    unsigned long size_mask;        /* number of slots */

} queue_hdr  __attribute__((aligned(64)));


int queue_slot[1<<10];


#define wmb() asm   __volatile__ ("" ::: "memory") 
#define likely(x)   __builtin_expect((x),1)
#define unlikely(x) __builtin_expect((x),0)


static inline void
spsc_init(struct queue_header *that, unsigned long s)
{
    that->producer.index  = 0;
    that->consumer.index  = 0;

    that->size = s;
    that->size_mask = (s-1);
}

/* producer */

static inline
int spsc_write_index(struct queue_header *q)
{
    auto size = (q->consumer.index - q->producer.index + q->size_mask) & q->size_mask;
    if (unlikely(size == 0)) {
        return -1;
    }
    return q->producer.index;
}


static inline
void spsc_write_commit(struct queue_header *q)
{
    wmb();
    auto size = (q->consumer.index - q->producer.index + q->size_mask) & q->size_mask;
    if (size != 0) {
        q->producer.index = (q->producer.index + 1) & q->size_mask;
    }
    wmb();
}


static inline
int spsc_read_index(struct queue_header *q)
{   
    auto size = (q->producer.index - q->consumer.index + q->size) & q->size_mask;
    if(unlikely(size == 0)) {
        return -1;
    }
    return q->consumer.index;
}


static inline
void spsc_read_commit(struct queue_header *q)
{
    wmb();
    auto size = (q->producer.index - q->consumer.index + q->size) & q->size_mask;
    if (size != 0) {
        q->consumer.index = (q->consumer.index + 1) & q->size_mask;
    }
    wmb();
}


static inline void 
set_affinity(std::thread &t, int n) 
{
    if(t.get_id() == std::thread::id())
        throw std::runtime_error("thread not running");

    cpu_set_t cpuset;
    CPU_ZERO(&cpuset); CPU_SET(n, &cpuset);

    auto pth = t.native_handle();
    if ( ::pthread_setaffinity_np(pth, sizeof(cpuset), &cpuset) != 0)
        throw std::runtime_error("pthread_setaffinity_np");
}


unsigned int hardware_concurrency()
{
    auto proc = []() {
        std::ifstream cpuinfo("/proc/cpuinfo");
        return std::count(std::istream_iterator<std::string>(cpuinfo),
                          std::istream_iterator<std::string>(),
                          std::string("processor"));
    };
   
    return std::thread::hardware_concurrency() ? : proc();
}


std::atomic_bool barrier;

void producer()
{
    while(barrier.load());

    for(int i = 0; i < 100000000; i++)
    {
        int w;
        do {   
            w = spsc_write_index(&queue_hdr);
        }
        while(w == -1);
        queue_slot[w] = i;
        spsc_write_commit(&queue_hdr);
    }
}


void consumer()
{
    int sum = 0;
    for(int i = 0; i < 100000000; i++)
    {
        int r;
        do {   
            r = spsc_read_index(&queue_hdr);
        }
        while(r == -1);
        sum += queue_slot[r];
        spsc_read_commit(&queue_hdr);
    }
}


int
main(int argc, char *argv[])
{
    const size_t core = hardware_concurrency();

    std::vector<double> elapsed(core*core, std::numeric_limits<double>::max());

    for(size_t i = 0; i < (core-1); i++)
    {
        for(size_t j = i+1; j < core; j++)
        {
            int n = i*core +j;

            std::cout << "SpeedCore " << "/-\\|"[n&3] << '\r' << std::flush;

            spsc_init(&queue_hdr, sizeof(queue_slot)/sizeof(queue_slot[0]));


            barrier.store(true);

            std::thread c (consumer);
            std::thread p (producer);

            set_affinity(c, i);
            set_affinity(p, j);

            auto begin = std::chrono::system_clock::now();
            barrier.store(false);

            c.join();
            p.join();
            
            auto end = std::chrono::system_clock::now();

            elapsed[n] = std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
        }
    }

    auto min_ = * std::min_element(elapsed.begin(), elapsed.end());
    for(auto &elem : elapsed)
    {
        if (elem != std::numeric_limits<double>::max())
            elem /= min_;
    }

    std::cout << std::endl << "*\t";
    for(size_t i = 0; i < core; i++) {
        std::cout << i << '\t';
    }
    std::cout << std::endl;

    for(size_t i = 0; i < core; i++) {
        std::cout << i  << '\t';
        for(size_t j = 0; j < core; j++)
        {
            auto & elem = elapsed[i * core + j];
            if (elem != std::numeric_limits<double>::max())
                std::cout << std::ceil(elem*100)/100 << '\t';
            else
                std::cout << "-\t";

        }
        std::cout << std::endl;
    }

    return 0;
}
 
