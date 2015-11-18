// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include <stdio.h>
#include <string.h>
#include <iostream>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include "global/global_init.h"
#include "common/ceph_argparse.h"
#include "include/stringify.h"
#include "common/errno.h"
#include <gtest/gtest.h>

#include "os/bluestore/BlueFS.h"

string get_temp_bdev(uint64_t size)
{
  static int n = 0;
  string fn = "ceph_test_bluefs.tmp.block." + stringify(getpid())
    + "." + stringify(++n);
  int fd = ::open(fn.c_str(), O_CREAT|O_RDWR|O_TRUNC, 0644);
  assert(fd >= 0);
  int r = ::ftruncate(fd, size);
  assert(r >= 0);
  ::close(fd);
  return fn;
}

void rm_temp_bdev(string f)
{
  ::unlink(f.c_str());
}

TEST(BlueFS, mkfs) {
  uint64_t size = 1048476 * 128;
  string fn = get_temp_bdev(size);
  BlueFS fs;
  fs.add_block_device(0, fn);
  fs.add_block_extent(0, 1048576, size - 1048576);
  fs.mkfs(0, 4096);
  rm_temp_bdev(fn);
}

TEST(BlueFS, mkfs_mount) {
  uint64_t size = 1048476 * 128;
  string fn = get_temp_bdev(size);
  BlueFS fs;
  ASSERT_EQ(0, fs.add_block_device(0, fn));
  fs.add_block_extent(0, 1048576, size - 1048576);
  ASSERT_EQ(0, fs.mkfs(0, 4096));
  ASSERT_EQ(0, fs.mount(0, 4096));
  ASSERT_EQ(fs.get_total(0), size - 1048576);
  ASSERT_LT(fs.get_free(0), size - 1048576);  
  fs.umount();
  rm_temp_bdev(fn);
}

TEST(BlueFS, write_read) {
  uint64_t size = 1048476 * 128;
  string fn = get_temp_bdev(size);
  BlueFS fs;
  ASSERT_EQ(0, fs.add_block_device(0, fn));
  fs.add_block_extent(0, 1048576, size - 1048576);
  ASSERT_EQ(0, fs.mkfs(0, 4096));
  ASSERT_EQ(0, fs.mount(0, 4096));
  {
    BlueFS::FileWriter *h;
    ASSERT_EQ(0, fs.create_and_open_for_write("dir", "file", &h));
    bufferlist bl;
    bl.append("foo");
    h->append(bl);
    bl.append("bar");
    h->append(bl);
    bl.append("baz");
    h->append(bl);
    fs.fsync(h);
    delete h;
  }
  {
    BlueFS::FileReader *h;
    ASSERT_EQ(0, fs.open_for_read("dir", "file", &h));
    bufferptr bp;
    ASSERT_EQ(9, fs.read(h, 1024, &bp, NULL));
    ASSERT_EQ(0, strncmp("foobarbaz", bp.c_str(), 9));
    delete h;
  }
  fs.umount();
  rm_temp_bdev(fn);
}

int main(int argc, char **argv) {
  vector<const char*> args;
  argv_to_vec(argc, (const char **)argv, args);
  env_to_vec(args);

  global_init(NULL, args, CEPH_ENTITY_TYPE_CLIENT, CODE_ENVIRONMENT_UTILITY, 0);
  common_init_finish(g_ceph_context);
  g_ceph_context->_conf->set_val(
    "enable_experimental_unrecoverable_data_corrupting_features",
    "*");
  g_ceph_context->_conf->apply_changes(NULL);

  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
