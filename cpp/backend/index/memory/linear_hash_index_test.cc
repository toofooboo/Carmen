#include "backend/index/memory/linear_hash_index.h"

#include "backend/index/test_util.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace carmen::backend::index {
namespace {

using ::testing::IsOkAndHolds;
using ::testing::Optional;

using TestIndex = InMemoryLinearHashIndex<int, int, 16>;

// Instantiates common index tests for the InMemory index type.
INSTANTIATE_TYPED_TEST_SUITE_P(InMemory, IndexTest, TestIndex);

TEST(LinearHashingIndexTest, LoadTest) {
  constexpr int N = 1000;
  TestIndex index;
  for (int i = 0; i < N; i++) {
    EXPECT_THAT(index.GetOrAdd(i), IsOkAndHolds(std::pair{i, true}));
  }
  for (int i = 0; i < N; i++) {
    EXPECT_THAT(index.GetOrAdd(i), IsOkAndHolds(std::pair{i, false}));
  }
  for (int i = 0; i < N; i++) {
    EXPECT_THAT(index.Get(i), IsOkAndHolds(i));
  }
}

}  // namespace
}  // namespace carmen::backend::index
