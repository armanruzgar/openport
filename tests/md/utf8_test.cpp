#include "openport/md/utf8.hpp"

#include <gtest/gtest.h>

namespace {
using namespace openport;

TEST(Utf8, ValidatesAsJsonOutputNeeds) {
  EXPECT_TRUE(md::valid_utf8(""));
  EXPECT_TRUE(md::valid_utf8("CPI \xc3\xa9t\xc3\xa9 \xe2\x80\x93 \xf0\x9f\x93\x88"));
  EXPECT_FALSE(md::valid_utf8("CPI \xe9t\xe9"));        // Latin-1
  EXPECT_FALSE(md::valid_utf8("\xff"));
  EXPECT_FALSE(md::valid_utf8("x\xc3"));               // cut mid-character
  EXPECT_FALSE(md::valid_utf8("\xc0\xaf"));            // overlong
  EXPECT_FALSE(md::valid_utf8("\xed\xa0\x80"));        // a surrogate
  EXPECT_FALSE(md::valid_utf8("\xf4\x90\x80\x80"));    // past U+10FFFF
}

TEST(Utf8, ReplacesEachBadByteAndCutsOnCharacterBoundaries) {
  EXPECT_EQ(md::utf8_replaced("a\xff" "b\xc3"), "a\xef\xbf\xbd" "b\xef\xbf\xbd");
  EXPECT_EQ(md::utf8_replaced("\xc3\xa9"), "\xc3\xa9");
  EXPECT_EQ(md::utf8_prefix("ab\xc3\xa9", 3), "ab");
  EXPECT_EQ(md::utf8_prefix("ab\xc3\xa9", 4), "ab\xc3\xa9");
  EXPECT_EQ(md::utf8_prefix("abc", 2), "ab");
}
}  // namespace
