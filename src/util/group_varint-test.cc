// Copyright (c) 2013, Cloudera, inc.

#include <gtest/gtest.h>
#include "util/group_varint-inl.h"

namespace kudu {
namespace coding {

extern void DumpSSETable();

// Encodes the given four ints as group-varint, then
// decodes and ensures the result is the same.
static void DoTestRoundTripGVI32(
  uint32_t a, uint32_t b, uint32_t c, uint32_t d,
  bool use_sse=false) {

  faststring buf;
  AppendGroupVarInt32(&buf, a, b, c, d);

  uint32_t ret[4];

  const uint8_t *end;

  if (use_sse) {
    end = DecodeGroupVarInt32_SSE(
      reinterpret_cast<const uint8_t *>(buf.data()),
      &ret[0], &ret[1], &ret[2], &ret[3]);
  } else {
    end = DecodeGroupVarInt32(
      reinterpret_cast<const uint8_t *>(buf.data()),
      &ret[0], &ret[1], &ret[2], &ret[3]);
  }

  ASSERT_EQ(a, ret[0]);
  ASSERT_EQ(b, ret[1]);
  ASSERT_EQ(c, ret[2]);
  ASSERT_EQ(d, ret[3]);
  ASSERT_EQ(reinterpret_cast<const char *>(end),
            buf.data() + buf.size());
}


TEST(TestGroupVarInt, TestSSETable) {
  DumpSSETable();
  faststring buf;
  AppendGroupVarInt32(&buf, 0, 0, 0, 0);
  DoTestRoundTripGVI32(0, 0, 0, 0, true);
  DoTestRoundTripGVI32(1, 2, 3, 4, true);
  DoTestRoundTripGVI32(1, 2000, 3, 200000, true);
}

TEST(TestGroupVarInt, TestGroupVarInt) {
  faststring buf;
  AppendGroupVarInt32(&buf, 0, 0, 0, 0);
  ASSERT_EQ(5UL, buf.size());
  ASSERT_EQ(0, memcmp("\x00\x00\x00\x00\x00", buf.data(), 5));
  buf.clear();

  // All 1-byte
  AppendGroupVarInt32(&buf, 1, 2, 3, 254);
  ASSERT_EQ(5UL, buf.size());
  ASSERT_EQ(0, memcmp("\x00\x01\x02\x03\xfe", buf.data(), 5));
  buf.clear();

  // Mixed 1-byte and 2-byte
  AppendGroupVarInt32(&buf, 256, 2, 3, 65535);
  ASSERT_EQ(7UL, buf.size());
  ASSERT_EQ( BOOST_BINARY( 01 00 00 01 ), buf.at(0));
  ASSERT_EQ(256, *reinterpret_cast<const uint16_t *>(&buf[1]));
  ASSERT_EQ(2, *reinterpret_cast<const uint8_t *>(&buf[3]));
  ASSERT_EQ(3, *reinterpret_cast<const uint8_t *>(&buf[4]));
  ASSERT_EQ(65535, *reinterpret_cast<const uint16_t *>(&buf[5]));
}


// Round-trip encode/decodes using group varint
TEST(TestGroupVarInt, TestRoundTrip) {
  // A few simple tests.
  DoTestRoundTripGVI32(0, 0, 0, 0);
  DoTestRoundTripGVI32(1, 2, 3, 4);
  DoTestRoundTripGVI32(1, 2000, 3, 200000);

  // Then a randomized test.
  for (int i = 0; i < 10000; i++) {
    DoTestRoundTripGVI32(random(), random(), random(), random());
  }
}



}
}
