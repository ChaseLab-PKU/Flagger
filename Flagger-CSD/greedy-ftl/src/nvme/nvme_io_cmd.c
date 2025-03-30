//////////////////////////////////////////////////////////////////////////////////
// nvme_io_cmd.c for Cosmos+ OpenSSD
// Copyright (c) 2016 Hanyang University ENC Lab.
// Contributed by Yong Ho Song <yhsong@enc.hanyang.ac.kr>
//				  Youngjin Jo <yjjo@enc.hanyang.ac.kr>
//				  Sangjin Lee <sjlee@enc.hanyang.ac.kr>
//				  Jaewook Kwak <jwkwak@enc.hanyang.ac.kr>
//
// This file is part of Cosmos+ OpenSSD.
//
// Cosmos+ OpenSSD is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3, or (at your option)
// any later version.
//
// Cosmos+ OpenSSD is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Cosmos+ OpenSSD; see the file COPYING.
// If not, see <http://www.gnu.org/licenses/>.
//////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////
// Company: ENC Lab. <http://enc.hanyang.ac.kr>
// Engineer: Sangjin Lee <sjlee@enc.hanyang.ac.kr>
//			 Jaewook Kwak <jwkwak@enc.hanyang.ac.kr>
//
// Project Name: Cosmos+ OpenSSD
// Design Name: Cosmos+ Firmware
// Module Name: NVMe IO Command Handler
// File Name: nvme_io_cmd.c
//
// Version: v1.0.1
//
// Description:
//   - handles NVMe IO command
//////////////////////////////////////////////////////////////////////////////////

//////////////////////////////////////////////////////////////////////////////////
// Revision History:
//
// * v1.0.1
//   - header file for buffer is changed from "ia_lru_buffer.h" to "lru_buffer.h"
//
// * v1.0.0
//   - First draft
//////////////////////////////////////////////////////////////////////////////////


#include "xil_printf.h"
#include "debug.h"
#include "io_access.h"

#include "nvme.h"
#include "host_lld.h"
#include "nvme_io_cmd.h"
#include "../memory_map.h"

#define IO_NVM_AGGREGATE_START  0x90
#define IO_NVM_AGGREGATE_DONE   0x91
#define AGG_CTRL_REG            (AGG_ACCEL_BASE + 0x00)
#define AGG_STATUS_REG          (AGG_ACCEL_BASE + 0x04)
#define AGG_SRC_ADDR_H          (AGG_ACCEL_BASE + 0x08)
#define AGG_SRC_ADDR_L          (AGG_ACCEL_BASE + 0x0C)
#define AGG_DST_ADDR_H          (AGG_ACCEL_BASE + 0x10)
#define AGG_DST_ADDR_L          (AGG_ACCEL_BASE + 0x14)
#define AGG_LENGTH_REG          (AGG_ACCEL_BASE + 0x18)

typedef struct {
    unsigned int ACTID[2];
    unsigned int startOffset;
    unsigned int endOffset;
} AGGREGATE_COMMAND;

void handle_aggregate_start(unsigned int cmdSlotTag, NVME_IO_COMMAND *nvmeIOCmd) {
    AGGREGATE_COMMAND aggCmd;
    aggCmd.ACTID[0] = nvmeIOCmd->dword[10];
    aggCmd.ACTID[1] = nvmeIOCmd->dword[11];
    aggCmd.startOffset = nvmeIOCmd->dword[12];
    aggCmd.endOffset = nvmeIOCmd->dword[13];

    unsigned long long srcAddr = (unsigned long long)DDR4_BUFFER_BASE_ADDR 
                               + ((unsigned long long)aggCmd.ACTID[0] * BYTES_PER_NVME_BLOCK) 
                               + aggCmd.startOffset;
    unsigned int dataLength = aggCmd.endOffset - aggCmd.startOffset;

    Xil_Out32(AGG_SRC_ADDR_H, (srcAddr >> 32) & 0xFFFFFFFF);
    Xil_Out32(AGG_SRC_ADDR_L, srcAddr & 0xFFFFFFFF);
    Xil_Out32(AGG_LENGTH_REG, dataLength);
    Xil_Out32(AGG_CTRL_REG, 0x1);

    while ((Xil_In32(AGG_STATUS_REG) & 0x1) == 0);
    unsigned int aggStatus = Xil_In32(AGG_STATUS_REG);
    send_aggregate_done(cmdSlotTag, (aggStatus >> 1) & 0x1);
}


void send_aggregate_done(unsigned int cmdSlotTag, unsigned int status) {
    NVME_COMPLETION nvmeCPL;
    nvmeCPL.statusFieldWord = status ? 0x1 : 0x0;
    nvmeCPL.specific = Xil_In32(AGG_STATUS_REG);
    set_auto_nvme_cpl(nvmeCmd->cmdSlotTag, nvmeCPL.specific, nvmeCPL.statusFieldWord);
}


void handle_nvme_io_read(unsigned int cmdSlotTag, NVME_IO_COMMAND *nvmeIOCmd) {
    unsigned int requestedNvmeBlock, dmaIndex, numOfNvmeBlock, devAddrH, devAddrL;
    unsigned long long devAddr;

    IO_READ_COMMAND_DW12 readInfo12;
    unsigned int startACTID[2];
    unsigned int nlb;

    readInfo12.dword = nvmeIOCmd->dword[12];
    startACTID[0] = nvmeIOCmd->dword[10];
    startACTID[1] = nvmeIOCmd->dword[11];
    nlb = readInfo12.NLB;
    ASSERT(startACTID[0] < STORAGE_CAPACITY_L && (startACTID[1] < STORAGE_CAPACITY_H || startACTID[1] == 0));
    ASSERT((nvmeIOCmd->PRP1[0] & 0x3) == 0 && (nvmeIOCmd->PRP2[0] & 0x3) == 0);
    ASSERT(nvmeIOCmd->PRP1[1] < 0x10000 && nvmeIOCmd->PRP2[1] < 0x10000);

    dmaIndex = 0;
    requestedNvmeBlock = nlb + 1;
    devAddr = (unsigned long long)DDR4_BUFFER_BASE_ADDR + (unsigned long long)startACTID[0] * (unsigned long long)BYTES_PER_NVME_BLOCK;
    devAddrH = (unsigned int)(devAddr >> 32);
    devAddrL = (unsigned int)(devAddr & 0xFFFFFFFF);
    numOfNvmeBlock = 0;

    while(numOfNvmeBlock < requestedNvmeBlock) {
        set_auto_tx_dma(cmdSlotTag, dmaIndex, devAddrH, devAddrL, NVME_COMMAND_AUTO_COMPLETION_ON);
        numOfNvmeBlock++;
        dmaIndex++;
        devAddr += BYTES_PER_NVME_BLOCK;
        devAddrH = (unsigned int)(devAddr >> 32);
        devAddrL = (unsigned int)(devAddr & 0xFFFFFFFF);
    }
}

void handle_nvme_io_write(unsigned int cmdSlotTag, NVME_IO_COMMAND *nvmeIOCmd) {
    unsigned int requestedNvmeBlock, dmaIndex, numOfNvmeBlock, devAddrH, devAddrL;
    unsigned long long devAddr;
    
    IO_READ_COMMAND_DW12 writeInfo12;
    unsigned int startACTID[2];
    unsigned int nlb;

    writeInfo12.dword = nvmeIOCmd->dword[12];
    startACTID[0] = nvmeIOCmd->dword[10];
    startACTID[1] = nvmeIOCmd->dword[11];
    nlb = writeInfo12.NLB;

    ASSERT(startACTID[0] < STORAGE_CAPACITY_L && (startACTID[1] < STORAGE_CAPACITY_H || startACTID[1] == 0));
    ASSERT((nvmeIOCmd->PRP1[0] & 0xF) == 0 && (nvmeIOCmd->PRP2[0] & 0xF) == 0);
    ASSERT(nvmeIOCmd->PRP1[1] < 0x10000 && nvmeIOCmd->PRP2[1] < 0x10000);

    dmaIndex = 0;
    requestedNvmeBlock = nlb + 1;
    devAddr = (unsigned long long)DDR4_BUFFER_BASE_ADDR + (unsigned long long)startACTID[0] * (unsigned long long)BYTES_PER_NVME_BLOCK;
    devAddrH = (unsigned int)(devAddr >> 32);
    devAddrL = (unsigned int)(devAddr & 0xFFFFFFFF);
    numOfNvmeBlock = 0;

    while(numOfNvmeBlock < requestedNvmeBlock) {
        set_auto_rx_dma(cmdSlotTag, dmaIndex, devAddrH, devAddrL, NVME_COMMAND_AUTO_COMPLETION_ON);
        numOfNvmeBlock++;
        dmaIndex++;
        devAddr += BYTES_PER_NVME_BLOCK;
        devAddrH = (unsigned int)(devAddr >> 32);
        devAddrL = (unsigned int)(devAddr & 0xFFFFFFFF);
    }
}

void handle_nvme_io_cmd(NVME_COMMAND *nvmeCmd) {
    NVME_IO_COMMAND *nvmeIOCmd;
    NVME_COMPLETION nvmeCPL;
    unsigned int opc;

    nvmeIOCmd = (NVME_IO_COMMAND*)nvmeCmd->cmdDword;
    opc = (unsigned int)nvmeIOCmd->OPC;

    switch(opc) {
        case IO_NVM_FLUSH:
            PRINT("IO Flush Command\r\n");
            nvmeCPL.dword[0] = 0;
            nvmeCPL.specific = 0x0;
            set_auto_nvme_cpl(nvmeCmd->cmdSlotTag, nvmeCPL.specific, nvmeCPL.statusFieldWord);
            break;
        case IO_NVM_WRITE:
            PRINT("IO Write Command\r\n");
            handle_nvme_io_write(nvmeCmd->cmdSlotTag, nvmeIOCmd);
            break;
        case IO_NVM_READ:
            PRINT("IO Read Command\r\n");
            handle_nvme_io_read(nvmeCmd->cmdSlotTag, nvmeIOCmd);
            break;
        case IO_NVM_AGGREGATE_START:
            PRINT("Host requested aggregation start\n");
            handle_aggregate_start(nvmeCmd->cmdSlotTag, nvmeIOCmd);
            break;
        case IO_NVM_AGGREGATE_DONE:
            PRINT("Host acknowledged aggregation completion\n");
            break;
        default:
            xil_printf("Unsupported IO Command OPC: 0x%X\n", opc);
            ASSERT(0);
            break;
    }

#if (__IO_CMD_DONE_MESSAGE_PRINT)
    xil_printf("OPC = 0x%X\r\n", nvmeIOCmd->OPC);
    xil_printf("PRP1[63:32] = 0x%X, PRP1[31:0] = 0x%X\r\n", nvmeIOCmd->PRP1[1], nvmeIOCmd->PRP1[0]);
    xil_printf("PRP2[63:32] = 0x%X, PRP2[31:0] = 0x%X\r\n", nvmeIOCmd->PRP2[1], nvmeIOCmd->PRP2[0]);
    xil_printf("dword10 = 0x%X\r\n", nvmeIOCmd->dword10);
    xil_printf("dword11 = 0x%X\r\n", nvmeIOCmd->dword11);
    xil_printf("dword12 = 0x%X\r\n", nvmeIOCmd->dword12);
#endif
}