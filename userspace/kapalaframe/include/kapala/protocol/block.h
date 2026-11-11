#pragma once

// Kapalaframe: the block device protocol, between a disk driver (ahci) and
// its one client (the file system server).
//
//   client                                        driver
//   Attach + shared memory (BUFFER_SIZE bytes) ->
//                                              <- Reply{status, sectors}
//   Read{lba, count}                           -> (the disk writes into the buffer)
//                                              <- Reply{status}
//   (fills the buffer) Write{lba, count}       ->
//                                              <- Reply{status}
//   Flush                                      -> (the disk's write cache reaches the platters)
//                                              <- Reply{status}
//
// Data always starts at the beginning of the shared buffer.

namespace Kapala::Protocol::Block {

constexpr unsigned long SECTOR_SIZE = 512;
constexpr unsigned long BUFFER_SIZE = 64 * 1024;
constexpr unsigned long MAX_SECTORS = BUFFER_SIZE / SECTOR_SIZE; // per request

enum class Op : unsigned char {
    Attach = 1,
    Read = 2,
    Write = 3,
    Flush = 4,
};

struct Request {
    Op op;
    unsigned count; // sectors, 1..MAX_SECTORS
    unsigned long lba;
};

struct Reply {
    long status;           // Sys::OK or a negative Sys::Error
    unsigned long sectors; // Attach: the disk's size
};

} // namespace Kapala::Protocol::Block
