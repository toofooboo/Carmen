#include "backend/index/test_util.h"

#include "backend/index/index.h"
#include "gtest/gtest.h"

namespace carmen::backend::index {
namespace {

// The generic index tests are not explictily tested here.
GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(IndexTest);

// Check that the MockIndex implementation is a valid Index.
TEST(MockIndexTest, IsIndex) { EXPECT_TRUE((Index<MockIndex<int, int>>)); }

// Check that the MockIndexWrapper implementation is a valid Index.
TEST(MockIndexWrapperTest, IsIndex) {
  EXPECT_TRUE((Index<MockIndexWrapper<int, int>>));
}

}  // namespace
}  // namespace carmen::backend::index