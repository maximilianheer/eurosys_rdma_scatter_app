/**
 * This file is part of the Coyote <https://github.com/fpgasystems/Coyote>
 *
 * MIT Licence
 * Copyright (c) 2021-2025, Systems Group, ETH Zurich
 * All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <iostream>
#include <cstdlib>

// AMD GPU management & run-time libraries
#include <hip/hip_runtime.h>

// External library for easier parsing of CLI arguments by the executable
#include <boost/program_options.hpp>

// Coyote-specific includes
#include "cThread.hpp"
#include "constants.hpp"

constexpr bool const IS_CLIENT = false;

// Registers, corresponding to the registers defined in the vFPGA
enum class ScatterRegisters: uint32_t {
    VADDR_1 = 0, 
    VADDR_2 = 1, 
    VADDR_3 = 2, 
    VADDR_4 = 3, 
    VADDR_VALID = 4
}; 

// Note, how the Coyote thread is passed by reference; to avoid creating a copy of 
// the thread object which can lead to undefined behaviour and bugs. 
void run_bench(
    coyote::cThread &coyote_thread, coyote::rdmaSg &sg, 
    int *mem, uint transfers, uint n_runs, bool operation
) {
    // When writing, the server asserts the written payload is correct (which the client sets)
    // When reading, the client asserts the read payload is correct (which the server sets)
    for (int i = 0; i < sg.len / sizeof(int); i++) {
        mem[i] = operation ? 0 : i;        
    }

    for (int i = 0; i < n_runs; i++) {
        // Clear previous completion flags and sync with client
        coyote_thread.clearCompleted();
        coyote_thread.connSync(IS_CLIENT);

        // For writes, wait until client has written the targer number of messages; then write them back
        if (operation) {
            while (coyote_thread.checkCompleted(coyote::CoyoteOper::LOCAL_WRITE) != transfers) {}

            for (int i = 0; i < transfers; i++) {
                coyote_thread.invoke(coyote::CoyoteOper::REMOTE_RDMA_WRITE, sg);
            }
        // For reads, the server is completely passive 
        } else { 

        }
    }
    
    // Functional correctness check
    if (operation) {
        for (int i = 0; i < sg.len / sizeof(int); i++) {
            // assert(mem[i] == i);                        
        }
    }
}

int main(int argc, char *argv[])  {
    // CLI arguments
    bool operation;
    unsigned int min_size, max_size, n_runs;

    boost::program_options::options_description runtime_options("Coyote Perf RDMA Options");
    runtime_options.add_options()
        ("operation,o", boost::program_options::value<bool>(&operation)->default_value(false), "Benchmark operation: READ(0) or WRITE(1)")
        ("runs,r", boost::program_options::value<unsigned int>(&n_runs)->default_value(N_RUNS_DEFAULT), "Number of times to repeat the test")
        ("min_size,x", boost::program_options::value<unsigned int>(&min_size)->default_value(MIN_TRANSFER_SIZE_DEFAULT), "Starting (minimum) transfer size")
        ("max_size,X", boost::program_options::value<unsigned int>(&max_size)->default_value(MAX_TRANSFER_SIZE_DEFAULT), "Ending (maximum) transfer size");
    boost::program_options::variables_map command_line_arguments;
    boost::program_options::store(boost::program_options::parse_command_line(argc, argv, runtime_options), command_line_arguments);
    boost::program_options::notify(command_line_arguments);

    HEADER("CLI PARAMETERS:");
    std::cout << "Benchmark operation: " << (operation ? "WRITE" : "READ") << std::endl;
    std::cout << "Number of test runs: " << n_runs << std::endl;
    std::cout << "Starting transfer size: " << min_size << std::endl;
    std::cout << "Ending transfer size: " << max_size << std::endl << std::endl;

    // Allocate Coyothe threa and set-up RDMA connections, buffer etc.
    // initRDMA is explained in more detail in client/main.cpp
    coyote::cThread coyote_thread(DEFAULT_VFPGA_ID, getpid());
    int *mem = (int *) coyote_thread.initRDMA(max_size, coyote::DEF_PORT);
    if (!mem) { throw std::runtime_error("Could not allocate memory; exiting..."); }

    if (hipSetDevice(DEFAULT_GPU_ID)) { throw std::runtime_error("Couldn't select GPU!"); }

    // Allocate four buffers for the scatter operation 
    int* vaddr_1 = (int *) coyote_thread.getMem({coyote::CoyoteAllocType::GPU, max_size, false, DEFAULT_GPU_ID}); 
    int* vaddr_2 = (int *) coyote_thread.getMem({coyote::CoyoteAllocType::GPU, max_size, false, DEFAULT_GPU_ID}); 
    int* vaddr_3 = (int *) coyote_thread.getMem({coyote::CoyoteAllocType::GPU, max_size, false, DEFAULT_GPU_ID});
    int* vaddr_4 = (int *) coyote_thread.getMem({coyote::CoyoteAllocType::GPU, max_size, false, DEFAULT_GPU_ID});

    // Print all the new buffer addresses
    std::cout << "Scatter buffer addresses:" << std::endl;
    std::cout << "Buffer 1: " << std::hex << reinterpret_cast<uint64_t>(vaddr_1) << std::endl;
    std::cout << "Buffer 2: " << std::hex << reinterpret_cast<uint64_t>(vaddr_2) << std::endl;
    std::cout << "Buffer 3: " << std::hex << reinterpret_cast<uint64_t>(vaddr_3) << std::endl;
    std::cout << "Buffer 4: " << std::hex << reinterpret_cast<uint64_t>(vaddr_4) << std::endl << std::dec;

    // Throw an exception if any of the buffers could not be allocated 
    if(!vaddr_1 || !vaddr_2 || !vaddr_3 || !vaddr_4) {
        throw std::runtime_error("Could not allocate memory for scatter buffers; exiting...");
    }

    // Write the buffer addresses to the vFPGA registers
    coyote_thread.setCSR(reinterpret_cast<uint64_t>(vaddr_1), static_cast<uint32_t>(ScatterRegisters::VADDR_1));
    coyote_thread.setCSR(reinterpret_cast<uint64_t>(vaddr_2), static_cast<uint32_t>(ScatterRegisters::VADDR_2));
    coyote_thread.setCSR(reinterpret_cast<uint64_t>(vaddr_3), static_cast<uint32_t>(ScatterRegisters::VADDR_3));
    coyote_thread.setCSR(reinterpret_cast<uint64_t>(vaddr_4), static_cast<uint32_t>(ScatterRegisters::VADDR_4));
    coyote_thread.setCSR(static_cast<uint64_t>(true), static_cast<uint32_t>(ScatterRegisters::VADDR_VALID));

    // Benchmark sweep; exactly like done in the client code
    HEADER("RDMA BENCHMARK: SERVER");
    unsigned int curr_size = min_size;
    while(curr_size <= max_size) {
        coyote::rdmaSg sg = { .len = curr_size };
        run_bench(coyote_thread, sg, mem, N_THROUGHPUT_REPS, n_runs, operation);
        run_bench(coyote_thread, sg, mem, N_LATENCY_REPS, n_runs, operation);
        curr_size *= 2;
    }

    // Final sync and exit
    coyote_thread.connSync(IS_CLIENT);
    return EXIT_SUCCESS;
}