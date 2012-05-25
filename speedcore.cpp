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

std::atomic_long p_pipe;
std::atomic_long c_pipe;

std::atomic_bool barrier;

const char * const BOLD  = "\E[1m";
const char * const RESET = "\E[0m";

int
main(int argc, char *argv[])
{
    const size_t core  = hardware_concurrency();
    const size_t trans = 10000000;

    std::vector<double> TS(core*core);

    std::cout << "SpeedCore:" << std::endl;

    for(size_t i = 0; i < (core-1); i++)
    {
        for(size_t j = i+1; j < core; j++)
        {
            int n = i*core+j;

            std::cout << "\rRunning test " << (i+1) << "/" << core << " " << "/-\\|"[n&3] << std::flush;

            p_pipe.store(0);
            c_pipe.store(0);

            barrier.store(true, std::memory_order_release);

            std::thread c ([&] {
                while (barrier.load(std::memory_order_acquire)) {}
                for(unsigned int i = 1; i < trans; i++)
                {
                    p_pipe.store(i, std::memory_order_release);
                    while (c_pipe.load(std::memory_order_acquire) != i)
                    {}
                }});

            std::thread p ([&] {
                for(unsigned int i = 1; i < trans; i++)
                {
                    while(p_pipe.load(std::memory_order_acquire) != i)
                    {}
                    c_pipe.store(i,std::memory_order_release);
                }});

            set_affinity(c, i);
            set_affinity(p, j);

            auto begin = std::chrono::system_clock::now();
            
            barrier.store(false, std::memory_order_release);

            c.join();
            p.join();
            
            auto end = std::chrono::system_clock::now();

            TS[n] = static_cast<uint64_t>(trans)*1000/std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count();
        }
    }

    auto it = std::max_element(TS.begin(), TS.end());
    auto max_ = *it;

    for(auto &elem : TS)
    {
        elem /= max_;
    }

    std::cout << "\nMax speed " << (max_*2) << " T/S (core " << 
                    std::distance(TS.begin(), it)/core << " <-> " << 
                    std::distance(TS.begin(), it)%core << ")" << std::endl;

    std::cout << "*\t";
    for(size_t i = 0; i < core; i++) {
        std::cout << i << '\t';
    }
    std::cout << std::endl;

    for(size_t i = 0; i < core; i++) {
        std::cout << i  << '\t';
        for(size_t j = 0; j < core; j++)
        {
            auto & elem = TS[i * core + j];
            if (elem != 0.0) {
                if (elem >  0.96)
                    std::cout << BOLD;
                std::cout << std::ceil(elem*100)/100 << RESET << '\t';
            }
            else
                std::cout << "-\t";

        }
        std::cout << std::endl;
    }

    return 0;
}
 
