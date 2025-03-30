/**********
Copyright (c) 2019, Xilinx, Inc.
All rights reserved.

Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors
may be used to endorse or promote products derived from this software
without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**********/
#include "xcl2.hpp"
#include "libunvme.h"
#include <vector>
#include <chrono>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <cstring>
#include <iostream>
#include <limits>
#include <algorithm>
#include <time.h>

#define DATA_SIZE 62500000
#define NVME_PAGESIZE 4096
#define NVME_QDEPTH   32

//Set IP address of FPGA
// #define IP_ADDR 0x0A01D498
// #define BOARD_NUMBER 0
// #define ARP 0x0A01D498

void wait_for_enter(const std::string &msg) {
    std::cout << msg << std::endl;
    std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <XCLBIN File> [<#RxByte> <Port> <local_IP> <boardNum>]" << std::endl;
        return EXIT_FAILURE;
    }

    std::string binaryFile = argv[1];

    cl_int err;
    cl::CommandQueue q;
    cl::Context context;

    cl::Kernel user_kernel;
    cl::Kernel network_kernel;

    uint32_t local_IP = 0x0A01D498;
    uint32_t boardNum = 1;
    
    if (argc >= 5) {
        std::string s = argv[4];
        std::string delimiter = ".";
        int ip[4];
        size_t pos = 0;
        std::string token;
        int i = 0;
        while ((pos = s.find(delimiter)) != std::string::npos) {
            token = s.substr(0, pos);
            ip[i] = stoi(token);
            s.erase(0, pos + delimiter.length());
            i++;
        }
        ip[i] = stoi(s); 
        local_IP = ip[3] | (ip[2] << 8) | (ip[1] << 16) | (ip[0] << 24);
    }

    if (argc >= 6) {
        boardNum = strtol(argv[5], NULL, 10);
    }

    printf("local_IP:%x, boardNum:%d\n", local_IP, boardNum);

    auto size = DATA_SIZE;
    auto vector_size_bytes = sizeof(int) * size;
    std::vector<int, aligned_allocator<int>> network_ptr0(size);
    std::vector<int, aligned_allocator<int>> network_ptr1(size);

    // OPENCL HOST CODE AREA START
    auto devices = xcl::get_xil_devices();
    auto fileBuf = xcl::read_binary_file(binaryFile);
    cl::Program::Binaries bins{{fileBuf.data(), fileBuf.size()}};
    int valid_device = 0;
    for (unsigned int i = 0; i < devices.size(); i++) {
        auto device = devices[i];
        OCL_CHECK(err, context = cl::Context({device}, NULL, NULL, NULL, &err));
        OCL_CHECK(err, q = cl::CommandQueue(context, {device}, CL_QUEUE_PROFILING_ENABLE, &err));

        std::cout << "Trying to program device[" << i
                  << "]: " << device.getInfo<CL_DEVICE_NAME>() << std::endl;
        cl::Program program(context, {device}, bins, NULL, &err);
        if (err != CL_SUCCESS) {
            std::cout << "Failed to program device[" << i << "] with xclbin file!\n";
        } else {
            std::cout << "Device[" << i << "]: program successful!\n";
            OCL_CHECK(err, network_kernel = cl::Kernel(program, "network_krnl", &err));
            OCL_CHECK(err, user_kernel = cl::Kernel(program, "hls_recv_krnl", &err));
            valid_device++;
            break;
        }
    }
    if (valid_device == 0) {
        std::cout << "Failed to program any device found, exit!\n";
        exit(EXIT_FAILURE);
    }
    
    wait_for_enter("\nPress ENTER to continue after setting up ILA trigger...");

    OCL_CHECK(err, err = network_kernel.setArg(0, local_IP));
    OCL_CHECK(err, err = network_kernel.setArg(1, boardNum));
    OCL_CHECK(err, err = network_kernel.setArg(2, local_IP));

    OCL_CHECK(err, cl::Buffer buffer_r1(context, CL_MEM_USE_HOST_PTR | CL_MEM_READ_WRITE, vector_size_bytes, network_ptr0.data(), &err));
    OCL_CHECK(err, cl::Buffer buffer_r2(context, CL_MEM_USE_HOST_PTR | CL_MEM_READ_WRITE, vector_size_bytes, network_ptr1.data(), &err));

    OCL_CHECK(err, err = network_kernel.setArg(3, buffer_r1));
    OCL_CHECK(err, err = network_kernel.setArg(4, buffer_r2));

    printf("enqueue network kernel...\n");
    OCL_CHECK(err, err = q.enqueueTask(network_kernel));
    OCL_CHECK(err, err = q.finish());
    
    uint32_t connection = 1;
    uint32_t basePort = 5001; 
    uint32_t rxByteCnt = 320000;

    if (argc >= 3)
        rxByteCnt = strtol(argv[2], NULL, 10);

    if (argc >= 4)
        basePort = strtol(argv[3], NULL, 10);

    OCL_CHECK(err, err = user_kernel.setArg(16, connection));
    OCL_CHECK(err, err = user_kernel.setArg(17, basePort));
    OCL_CHECK(err, err = user_kernel.setArg(18, rxByteCnt));

    auto start = std::chrono::high_resolution_clock::now();
    printf("enqueue user kernel...\n");
    OCL_CHECK(err, err = q.enqueueTask(user_kernel));
    OCL_CHECK(err, err = q.finish());
    auto end = std::chrono::high_resolution_clock::now();
    double durationUs = (std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count() / 1000.0);
    printf("durationUs:%f\n", durationUs);
    // OPENCL HOST CODE AREA END    

    
    std::cout << "Network/kernel operations completed." << std::endl;

    const char* pciName = "0000:01:00.0"; 
    int nsid = 1;
    const unvme_ns_t* ns = unvme_open(pciName, nsid, 4, NVME_QDEPTH);
    if (!ns) {
        std::cerr << "unvme_open failed: " << strerror(errno) << std::endl;
        exit(EXIT_FAILURE);
    }
    size_t total_bytes = rxByteCnt;
    int num_pages = (total_bytes + NVME_PAGESIZE - 1) / NVME_PAGESIZE;
    unvme_page_t* pages = unvme_alloc(ns, 0, num_pages);
    if (!pages) {
        std::cerr << "unvme_alloc failed for " << num_pages << " pages" << std::endl;
        unvme_close(ns);
        exit(EXIT_FAILURE);
    }
    for (int i = 0; i < num_pages; ++i) {
        pages[i].buf = network_ptr0.data() + i * NVME_PAGESIZE;
        pages[i].actid = (i * NVME_PAGESIZE) / ns->actid_blocksize;
        pages[i].nlb = NVME_PAGESIZE / ns->actid_blocksize;
        pages[i].qid = i % ns->maxppq;
    }
    if (unvme_awrite(ns, pages, num_pages)) {
        std::cerr << "Batch write failed: " << strerror(errno) << std::endl;
        unvme_free(ns, pages);
        unvme_close(ns);
        exit(EXIT_FAILURE);
    }
    uint64_t start_lba = 0;
    uint64_t end_lba = (total_bytes + ns->actid_blocksize - 1) / ns->actid_blocksize;
    if (unvme_aggregate_start(ns, pages, start_lba, end_lba)) {
        std::cerr << "Aggregation failed: " << strerror(errno) << std::endl;
        unvme_free(ns, pages);
        unvme_close(ns);
        exit(EXIT_FAILURE);
    }
    unvme_page_t* read_pages = unvme_alloc(ns, 1, num_pages);
    if (!read_pages) {
        std::cerr << "Alloc read buffer failed" << std::endl;
        unvme_free(ns, pages);
        unvme_close(ns);
        exit(EXIT_FAILURE);
    }
    memcpy(read_pages, pages, num_pages * sizeof(unvme_page_t));
    if (unvme_aread(ns, read_pages, num_pages)) {
        std::cerr << "Batch read failed" << std::endl;
        unvme_free(ns, read_pages);
        unvme_free(ns, pages);
        unvme_close(ns);
        exit(EXIT_FAILURE);
    }
    time_t poll_start = time(NULL);
    while (true) {
        int completed = unvme_apoll(ns, 0, 1);
        if (completed == num_pages) break;
        if (time(NULL) - poll_start > 5) {
            std::cerr << "Operation timeout" << std::endl;
            break;
        }
    }
    if (memcmp(network_ptr1.data(), read_pages[0].buf, total_bytes) == 0) {
        std::cout << "Data verification passed" << std::endl;
    }
    unvme_free(ns, read_pages);
    unvme_free(ns, pages);
    unvme_close(ns);

    std::cout << "SSD operations completed." << std::endl;
    std::cout << "EXIT recorded" << std::endl;
    return 0;
}
