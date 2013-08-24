/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "expect.h"
#include "hdfs.h"
#include "native_mini_dfs.h"

#include <errno.h>
#include <inttypes.h>
#include <semaphore.h>
#include <pthread.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define TO_STR_HELPER(X) #X
#define TO_STR(X) TO_STR_HELPER(X)

#define TEST_FILE_NAME_LENGTH 128
#define TEST_ZEROCOPY_FULL_BLOCK_SIZE 4096
#define TEST_ZEROCOPY_LAST_BLOCK_SIZE 3215
#define TEST_ZEROCOPY_NUM_BLOCKS 6
#define SMALL_READ_LEN 16

#define ZC_BUF_LEN 32768

static uint8_t *getZeroCopyBlockData(int blockIdx)
{
    uint8_t *buf = malloc(TEST_ZEROCOPY_FULL_BLOCK_SIZE);
    int i;
    if (!buf) {
        fprintf(stderr, "malloc(%d) failed\n", TEST_ZEROCOPY_FULL_BLOCK_SIZE);
        exit(1);
    }
    for (i = 0; i < TEST_ZEROCOPY_FULL_BLOCK_SIZE; i++) {
      buf[i] = blockIdx + (i % 17);
    }
    return buf;
}

static int getZeroCopyBlockLen(int blockIdx)
{
    if (blockIdx >= TEST_ZEROCOPY_NUM_BLOCKS) {
        return 0;
    } else if (blockIdx == (TEST_ZEROCOPY_NUM_BLOCKS - 1)) {
        return TEST_ZEROCOPY_LAST_BLOCK_SIZE;
    } else {
        return TEST_ZEROCOPY_FULL_BLOCK_SIZE;
    }
}

static void printBuf(const uint8_t *buf, size_t len) __attribute__((unused));

static void printBuf(const uint8_t *buf, size_t len)
{
  size_t i;

  for (i = 0; i < len; i++) {
    fprintf(stderr, "%02x", buf[i]);
  }
  fprintf(stderr, "\n");
}

static int doTestZeroCopyReads(hdfsFS fs, const char *fileName)
{
    hdfsFile file = NULL;
    struct hadoopZeroCopyCursor *zcursor = NULL;
    uint8_t *backingBuffer = NULL, *block;
    const void *zcPtr;

    file = hdfsOpenFile(fs, fileName, O_RDONLY, 0, 0, 0);
    EXPECT_NONNULL(file);
    zcursor = hadoopZeroCopyCursorAlloc(file);
    EXPECT_NONNULL(zcursor);
    /* haven't read anything yet */
    EXPECT_ZERO(expectFileStats(file, 0LL, 0LL, 0LL, 0LL));
    block = getZeroCopyBlockData(0);
    EXPECT_NONNULL(block);
    /* first read is half of a block. */
    EXPECT_INT_EQ(TEST_ZEROCOPY_FULL_BLOCK_SIZE / 2,
          hadoopZeroCopyRead(zcursor,
                  TEST_ZEROCOPY_FULL_BLOCK_SIZE / 2, &zcPtr));
    EXPECT_ZERO(memcmp(zcPtr, block, TEST_ZEROCOPY_FULL_BLOCK_SIZE / 2));
    /* read the next half of the block */
    EXPECT_INT_EQ(TEST_ZEROCOPY_FULL_BLOCK_SIZE / 2,
          hadoopZeroCopyRead(zcursor,
                  TEST_ZEROCOPY_FULL_BLOCK_SIZE / 2, &zcPtr));
    EXPECT_ZERO(memcmp(zcPtr, block + (TEST_ZEROCOPY_FULL_BLOCK_SIZE / 2),
                       TEST_ZEROCOPY_FULL_BLOCK_SIZE / 2));
    free(block);
    EXPECT_ZERO(expectFileStats(file, TEST_ZEROCOPY_FULL_BLOCK_SIZE, 
              TEST_ZEROCOPY_FULL_BLOCK_SIZE,
              TEST_ZEROCOPY_FULL_BLOCK_SIZE,
              TEST_ZEROCOPY_FULL_BLOCK_SIZE));
    /* Now let's read just a few bytes. */
    EXPECT_INT_EQ(SMALL_READ_LEN,
                  hadoopZeroCopyRead(zcursor, SMALL_READ_LEN, &zcPtr));
    block = getZeroCopyBlockData(1);
    EXPECT_NONNULL(block);
    EXPECT_ZERO(memcmp(block, zcPtr, SMALL_READ_LEN));
    EXPECT_INT_EQ(TEST_ZEROCOPY_FULL_BLOCK_SIZE + SMALL_READ_LEN,
                  hdfsTell(fs, file));
    EXPECT_ZERO(expectFileStats(file,
          TEST_ZEROCOPY_FULL_BLOCK_SIZE + SMALL_READ_LEN,
          TEST_ZEROCOPY_FULL_BLOCK_SIZE + SMALL_READ_LEN,
          TEST_ZEROCOPY_FULL_BLOCK_SIZE + SMALL_READ_LEN,
          TEST_ZEROCOPY_FULL_BLOCK_SIZE + SMALL_READ_LEN));

    /* Try to read a full block's worth of data.  This will cross the block
     * boundary, which means we have to fall back to non-zero-copy reads.
     * However, because we don't have a backing buffer, the fallback will fail
     * with EPROTONOSUPPORT. */
    EXPECT_INT_EQ(-1, 
          hadoopZeroCopyRead(zcursor, TEST_ZEROCOPY_FULL_BLOCK_SIZE, &zcPtr));
    EXPECT_INT_EQ(EPROTONOSUPPORT, errno);

    /* Now set a backing buffer and try again.  It should succeed this time. */
    backingBuffer = malloc(ZC_BUF_LEN);
    EXPECT_NONNULL(backingBuffer);
    EXPECT_ZERO(hadoopZeroCopyCursorSetFallbackBuffer(zcursor,
                                          backingBuffer, ZC_BUF_LEN));
    EXPECT_INT_EQ(TEST_ZEROCOPY_FULL_BLOCK_SIZE, 
          hadoopZeroCopyRead(zcursor, TEST_ZEROCOPY_FULL_BLOCK_SIZE, &zcPtr));
    EXPECT_ZERO(expectFileStats(file,
          (2 * TEST_ZEROCOPY_FULL_BLOCK_SIZE) + SMALL_READ_LEN,
          (2 * TEST_ZEROCOPY_FULL_BLOCK_SIZE) + SMALL_READ_LEN,
          (2 * TEST_ZEROCOPY_FULL_BLOCK_SIZE) + SMALL_READ_LEN,
          TEST_ZEROCOPY_FULL_BLOCK_SIZE + SMALL_READ_LEN));
    EXPECT_ZERO(memcmp(block + SMALL_READ_LEN, zcPtr,
        TEST_ZEROCOPY_FULL_BLOCK_SIZE - SMALL_READ_LEN));
    free(block);
    block = getZeroCopyBlockData(2);
    EXPECT_NONNULL(block);
    EXPECT_ZERO(memcmp(block, zcPtr +
        (TEST_ZEROCOPY_FULL_BLOCK_SIZE - SMALL_READ_LEN), SMALL_READ_LEN));
    free(block);
    hadoopZeroCopyCursorFree(zcursor);
    EXPECT_ZERO(hdfsCloseFile(fs, file));
    free(backingBuffer);
    return 0;
}

static int createZeroCopyTestFile(hdfsFS fs, char *testFileName,
                                  size_t testFileNameLen)
{
    int blockIdx, blockLen;
    hdfsFile file;
    uint8_t *data;

    snprintf(testFileName, testFileNameLen, "/zeroCopyTestFile.%d.%d",
             getpid(), rand());
    file = hdfsOpenFile(fs, testFileName, O_WRONLY, 0, 1,
                        TEST_ZEROCOPY_FULL_BLOCK_SIZE);
    EXPECT_NONNULL(file);
    for (blockIdx = 0; blockIdx < TEST_ZEROCOPY_NUM_BLOCKS; blockIdx++) {
        blockLen = getZeroCopyBlockLen(blockIdx);
        data = getZeroCopyBlockData(blockIdx);
        EXPECT_NONNULL(data);
        EXPECT_INT_EQ(blockLen, hdfsWrite(fs, file, data, blockLen));
    }
    EXPECT_ZERO(hdfsCloseFile(fs, file));
    return 0;
}

/**
 * Test that we can write a file with libhdfs and then read it back
 */
int main(void)
{
    int port;
    struct NativeMiniDfsConf conf = {
        .doFormat = 1,
        .configureShortCircuit = 1,
    };
    char testFileName[TEST_FILE_NAME_LENGTH];
    hdfsFS fs;
    struct NativeMiniDfsCluster* cl;
    struct hdfsBuilder *bld;

    cl = nmdCreate(&conf);
    EXPECT_NONNULL(cl);
    EXPECT_ZERO(nmdWaitClusterUp(cl));
    port = nmdGetNameNodePort(cl);
    if (port < 0) {
        fprintf(stderr, "TEST_ERROR: test_zerocopy: "
                "nmdGetNameNodePort returned error %d\n", port);
        return EXIT_FAILURE;
    }
    bld = hdfsNewBuilder();
    EXPECT_NONNULL(bld);
    EXPECT_ZERO(nmdConfigureHdfsBuilder(cl, bld));
    hdfsBuilderSetForceNewInstance(bld);
    hdfsBuilderConfSetStr(bld, "dfs.block.size",
                          TO_STR(TEST_ZEROCOPY_FULL_BLOCK_SIZE));
    /* ensure that we'll always get our mmaps */
    hdfsBuilderConfSetStr(bld, "dfs.client.read.shortcircuit.skip.checksum",
                          "true");
    fs = hdfsBuilderConnect(bld);
    EXPECT_NONNULL(fs);
    EXPECT_ZERO(createZeroCopyTestFile(fs, testFileName,
          TEST_FILE_NAME_LENGTH));
    EXPECT_ZERO(doTestZeroCopyReads(fs, testFileName));
    EXPECT_ZERO(hdfsDisconnect(fs));
    EXPECT_ZERO(nmdShutdown(cl));
    nmdFree(cl);
    fprintf(stderr, "TEST_SUCCESS\n"); 
    return EXIT_SUCCESS;
}