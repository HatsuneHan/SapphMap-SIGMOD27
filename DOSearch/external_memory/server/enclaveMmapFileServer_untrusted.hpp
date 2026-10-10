#pragma once
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>

#include "common/cpp_extended.hpp"

#ifndef DEFAULT_FILENAME
#define DEFAULT_FILENAME "storage.bin"
#endif

static size_t mapped_size = 0;
static int fd = -1;
static uint8_t* data = nullptr;
uint8_t* ocall_InitServer(uint64_t sizeOfT, uint64_t N_) {
  if (sizeOfT == 0 || N_ == 0 || N_ > SIZE_MAX / sizeOfT) {
    std::cerr << "Invalid file mapping size." << std::endl;
    std::exit(EXIT_FAILURE);
  }
  mapped_size = static_cast<size_t>(N_ * sizeOfT);
  fd = open(DEFAULT_FILENAME, O_RDWR | O_CREAT | O_TRUNC, 0644);
  if (fd == -1) {
    std::cerr << "Error opening file for writing." << std::endl;
    std::exit(EXIT_FAILURE);
  }
  int allocRes = fallocate64(fd, 0, 0, mapped_size);
  if (allocRes != 0) {
    std::cerr << "Error allocating space for the file." << std::endl;
    close(fd);
    fd = -1;
    std::exit(EXIT_FAILURE);
  }
  data = static_cast<uint8_t*>(
      mmap(nullptr, mapped_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
  if (data == MAP_FAILED) {
    std::cerr << "Error memory mapping the file." << std::endl;
    close(fd);
    fd = -1;
    data = nullptr;
    std::exit(EXIT_FAILURE);
  }
  return NULL;
}

void ocall_Read(size_t pos, uint64_t length, uint8_t* page) {
  std::memcpy(page, data + pos, length);
}

void ocall_Write(uint64_t pos, uint64_t length, const uint8_t* page) {
  std::memcpy(data + pos, page, length);
}

uint64_t compressChunks(uint64_t* offsets, uint64_t* sizes, uint64_t chunkNum) {
  if (!chunkNum) {
    return 0;
  }
  uint64_t endOffset = *offsets + *sizes;
  uint64_t j = 0;
  for (uint64_t i = 1; i < chunkNum; ++i) {
    uint64_t offset = *(offsets + i);
    uint64_t size = *(sizes + i);
    if (offset != endOffset) {
      sizes[j] = endOffset - offsets[j];
      offsets[++j] = offset;
    }
    endOffset = offset + size;
  }
  sizes[j] = endOffset - offsets[j];
  return j + 1;
}

void ocall_Read_Batch(uint64_t* offsets, uint64_t* sizes, uint8_t* tmp,
                      uint64_t chunkNum, uint64_t totalSize) {
  // printf("batch read %ld chunks of total size %ld\n", chunkNum, totalSize);
  uint8_t* pos = tmp;
  chunkNum = compressChunks(offsets, sizes, chunkNum);
  for (uint64_t i = 0; i < chunkNum; ++i) {
    uint64_t offset = *(offsets + i);
    uint64_t size = *(sizes + i);
    ocall_Read(offset, size, pos);
    pos += size;
  }
}
void ocall_Write_Batch(uint64_t* offsets, uint64_t* sizes, uint8_t* tmp,
                       uint64_t chunkNum, uint64_t totalSize) {
  //  printf("batch write %ld chunks of total size %ld\n", chunkNum, totalSize);
  uint8_t* pos = tmp;
  chunkNum = compressChunks(offsets, sizes, chunkNum);
  for (uint64_t i = 0; i < chunkNum; ++i) {
    uint64_t offset = *(offsets + i);
    uint64_t size = *(sizes + i);
    ocall_Write(offset, size, pos);
    pos += size;
  }
}

void ocall_DeleteServer() {
  if (data != nullptr && munmap(data, mapped_size) == -1) {
    std::cerr << "Error unmapping the file." << std::endl;
  }
  data = nullptr;
  mapped_size = 0;
  if (fd != -1) close(fd);
  fd = -1;
}
