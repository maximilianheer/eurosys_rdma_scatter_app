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
const int NUM_GPUS = 4;

#define DEFAULT_GPU_ID 0

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
    int *mem, int* dest_buffers[], uint transfers, uint n_runs, bool operation
) {
    // When writing, the server asserts the written payload is correct (which the client sets)
    // When reading, the client asserts the read payload is correct (which the server sets)
    for (int i = 0; i < sg.len / sizeof(int); i++) {
        mem[i] = operation ? 0 : i;        
    }

    hipStream_t streams[NUM_GPUS];
    hipEvent_t events[NUM_GPUS];

    for(int i = 0; i < NUM_GPUS; i++) {
                if (hipSetDevice(i)) { throw std::runtime_error("Couldn't select GPU!"); } 
                if (hipStreamCreate(&streams[i]) != hipSuccess) { throw std::runtime_error("Couldn't create stream!"); }
                if (hipEventCreate(&events[i]) != hipSuccess) { throw std::runtime_error("Couldn't create event!"); }
                printf("Created stream and event for GPU %d\n", i);
            }

    for (int i = 0; i < n_runs; i++) {
        // Clear previous completion flags and sync with client
        coyote_thread.clearCompleted();
        coyote_thread.connSync(IS_CLIENT);


        // For writes, wait until client has written the targer number of messages; then write them back
        if (operation) {
            while (coyote_thread.checkCompleted(coyote::CoyoteOper::LOCAL_WRITE) != transfers) {}

            // Create Streams and Events for checking 
            /* hipStream_t streams[NUM_GPUS];
            hipEvent_t events[NUM_GPUS]; */ 

            // Copy received payload from host memory to the four GPU buffers and record every copy as event 
            for(int i = 0; i < sg.len / 4096; i++) {
                // printf("Copying chunk %d/%d to GPU %d\n", i+1, sg.len/4096, i%4);
                if (hipSetDevice(i%4)) { throw std::runtime_error("Couldn't select GPU!"); }
                hipMemcpyAsync(dest_buffers[i % 4], &mem[i * 4096], 4096, hipMemcpyHostToDevice);

                // if (hipSetDevice(0)) { throw std::runtime_error("Couldn't select GPU!"); }
                // hipMemcpyAsync(dest_buffers[0], &mem[0], sg.len, hipMemcpyHostToDevice);
                // hipMemcpy(dest_buffers[i % 4], &mem[i * 4096], 4096, hipMemcpyHostToDevice);
                // hipEventRecord(events[i % 4], streams[i % 4]);
            }

        
            // hipMemcpy(dest_buffers[0], &mem[0], sg.len, hipMemcpyHostToDevice);

            // Check events non-blocking until all copies are done 
            /* bool all_done = false; 
            bool done[NUM_GPUS] = {false, false, false, false};
            while(!all_done) {
                all_done = true; 
                for(int i = 0; i < NUM_GPUS; i++) {
                    if(!done[i]) { 
                        if (hipSetDevice(i)) { throw std::runtime_error("Couldn't select GPU!"); }
                        hipError_t event_status = hipEventQuery(events[i]);
                        if (event_status == hipErrorNotReady) {
                            all_done = false; 
                            printf("Copy to GPU %d not done yet!\n", i);
                        } else if (event_status == hipSuccess) {
                            done[i] = true;
                            printf("Copy to GPU %d done!\n", i);
                        } else {
                            throw std::runtime_error("Error while querying the hipEvent: " + std::to_string(event_status));
                        }
                    }
                }
                printf("Continue Checking...\n");
            } */ 

            // Sync mem copy for all the GPUs 
            for(int i = 0; i < NUM_GPUS; i++) {
                if (hipSetDevice(i)) { throw std::runtime_error("Couldn't select GPU!"); }
                if (hipDeviceSynchronize() != hipSuccess) { throw std::runtime_error("Couldn't synchronize stream!"); }

                // if (hipSetDevice(0)) { throw std::runtime_error("Couldn't select GPU!"); }
                // if (hipDeviceSynchronize() != hipSuccess) { throw std::runtime_error("Couldn't synchronize stream!"); }

                // printf("Synchronized stream for GPU %d\n", i);
            }

            

            for (int i = 0; i < transfers; i++) {
                coyote_thread.invoke(coyote::CoyoteOper::REMOTE_RDMA_WRITE, sg);
            }
        // For reads, the server is completely passive 
        } else { 

        }
    }

    printf("Checking done!\n");
            for(int i = 0; i < NUM_GPUS; i++) {
                hipStreamDestroy(streams[i]); 
                hipEventDestroy(events[i]); 
                printf("Destroyed stream and event for GPU %d\n", i);
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

    // Allocate four buffers on four different GPUs for the scatter operation 
    if (hipSetDevice(0)) { throw std::runtime_error("Couldn't select GPU!"); } 
    // if (hipSetDevice(1)) { throw std::runtime_error("Couldn't select GPU!"); }
    // if (hipSetDevice(1)) { throw std::runtime_error("Couldn't select GPU!"); }
    // if (hipSetDevice(1)) { throw std::runtime_error("Couldn't select GPU!"); }   

    int* vaddr_1 = (int *) coyote_thread.getMem({coyote::CoyoteAllocType::GPU, max_size, false, 0}); 
    
    int* vaddr_2 = (int *) coyote_thread.getMem({coyote::CoyoteAllocType::GPU, max_size, false, 1}); 
    
    int* vaddr_3 = (int *) coyote_thread.getMem({coyote::CoyoteAllocType::GPU, max_size, false, 2});
    
    int* vaddr_4 = (int *) coyote_thread.getMem({coyote::CoyoteAllocType::GPU, max_size, false, 3});

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
    /* coyote_thread.setCSR(reinterpret_cast<uint64_t>(mem), static_cast<uint32_t>(ScatterRegisters::VADDR_1));
    coyote_thread.setCSR(reinterpret_cast<uint64_t>(mem), static_cast<uint32_t>(ScatterRegisters::VADDR_2));
    coyote_thread.setCSR(reinterpret_cast<uint64_t>(mem), static_cast<uint32_t>(ScatterRegisters::VADDR_3));
    coyote_thread.setCSR(reinterpret_cast<uint64_t>(mem), static_cast<uint32_t>(ScatterRegisters::VADDR_4));
    coyote_thread.setCSR(static_cast<uint64_t>(true), static_cast<uint32_t>(ScatterRegisters::VADDR_VALID)); */ 

    // Pack the destination buffers into an array for passing them to the benchmark function 
    int* destination_buffers [4] = {vaddr_1, vaddr_2, vaddr_3, vaddr_4};

    if (hipSetDevice(0)) { throw std::runtime_error("Couldn't select GPU!"); }

    // Benchmark sweep; exactly like done in the client code
    HEADER("RDMA BENCHMARK: SERVER");
    unsigned int curr_size = min_size;
    while(curr_size <= max_size) {
        coyote::rdmaSg sg = { .len = curr_size };
        // run_bench(coyote_thread, sg, mem, N_THROUGHPUT_REPS, n_runs, operation);
        run_bench(coyote_thread, sg, mem, destination_buffers, N_LATENCY_REPS, n_runs+10, operation);
        curr_size *= 2;
    }

    // Final sync and exit
    if (hipSetDevice(0)) { throw std::runtime_error("Couldn't select GPU!"); }
    hipFree(vaddr_1);
    if (hipSetDevice(1)) { throw std::runtime_error("Couldn't select GPU!"); }
    hipFree(vaddr_2);
    if (hipSetDevice(2)) { throw std::runtime_error("Couldn't select GPU!"); }
    hipFree(vaddr_3);
    if (hipSetDevice(3)) { throw std::runtime_error("Couldn't select GPU!"); }
    hipFree(vaddr_4);

    coyote_thread.connSync(IS_CLIENT);
    return EXIT_SUCCESS;
}