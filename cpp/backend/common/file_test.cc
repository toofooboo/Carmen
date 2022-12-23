#include "backend/common/file.h"

#include <filesystem>
#include <sstream>

#include "backend/common/page.h"
#include "common/file_util.h"
#include "gtest/gtest.h"

namespace carmen::backend::store {
namespace {

// A page format used for the tests.
template <std::size_t page_size>
class alignas(kFileSystemPageSize) Page
    : public std::array<std::byte, page_size> {
 public:
  constexpr static const auto kPageSize = page_size;

  operator std::span<const std::byte, kPageSize>() const {
    return std::span<const std::byte, kPageSize>{&(*this)[0], kPageSize};
  }

  operator std::span<std::byte, kPageSize>() {
    return std::span<std::byte, kPageSize>{&(*this)[0], kPageSize};
  }
};

TEST(TestPageTest, IsPage) {
  EXPECT_TRUE(carmen::backend::Page<Page<kFileSystemPageSize>>);
  EXPECT_TRUE(carmen::backend::Page<Page<2 * kFileSystemPageSize>>);
}

TEST(InMemoryFileTest, IsFile) {
  EXPECT_TRUE((File<InMemoryFile<4>>));
  EXPECT_TRUE((File<InMemoryFile<8>>));
  EXPECT_TRUE((File<InMemoryFile<16>>));
  EXPECT_TRUE((File<InMemoryFile<32>>));
}

TEST(InMemoryFileTest, InitialFileIsEmpty) {
  InMemoryFile<32> file;
  EXPECT_EQ(0, file.GetNumPages());
}

TEST(InMemoryFileTest, PagesCanBeWrittenAndRead) {
  using Page = Page<8>;
  InMemoryFile<Page::kPageSize> file;

  Page page_a{std::byte{0x01}, std::byte{0x02}};
  file.StorePage(0, page_a);
  EXPECT_EQ(1, file.GetNumPages());

  Page restored;
  file.LoadPage(0, restored);
  EXPECT_EQ(page_a, restored);
}

TEST(InMemoryFileTest, PagesAreDifferentiated) {
  using Page = Page<4>;
  InMemoryFile<Page::kPageSize> file;

  Page page_a{std::byte{0x01}, std::byte{0x02}};
  Page page_b{std::byte{0x03}, std::byte{0x04}};

  file.StorePage(0, page_a);
  file.StorePage(1, page_b);
  EXPECT_EQ(2, file.GetNumPages());

  Page restored;
  file.LoadPage(0, restored);
  EXPECT_EQ(page_a, restored);
  file.LoadPage(1, restored);
  EXPECT_EQ(page_b, restored);
}

TEST(InMemoryFileTest, WritingPagesCreatesImplicitEmptyPages) {
  using Page = Page<8>;
  InMemoryFile<Page::kPageSize> file;

  // Storing a page at position 2 implicitly creates pages 0 and 1.
  Page page_a{std::byte{0x01}, std::byte{0x02}};
  file.StorePage(2, page_a);
  EXPECT_EQ(3, file.GetNumPages());

  Page zero{};
  Page restored;
  file.LoadPage(0, restored);
  EXPECT_EQ(zero, restored);
  file.LoadPage(1, restored);
  EXPECT_EQ(zero, restored);
  file.LoadPage(2, restored);
  EXPECT_EQ(page_a, restored);
}

TEST(InMemoryFileTest, LoadingUninitializedPagesLeadsToZeros) {
  using Page = Page<4>;
  InMemoryFile<Page::kPageSize> file;
  Page zero{};
  Page loaded;
  loaded.fill(std::byte{1});
  file.LoadPage(0, loaded);
  EXPECT_EQ(zero, loaded);
}

template <typename F>
class SingleFileTest : public testing::Test {};

TYPED_TEST_SUITE_P(SingleFileTest);

TYPED_TEST_P(SingleFileTest, IsFile) {
  using RawFile = TypeParam;
  EXPECT_TRUE((File<SingleFileBase<8, RawFile>>));
  EXPECT_TRUE((File<SingleFileBase<32, RawFile>>));
}

TYPED_TEST_P(SingleFileTest, ExistingFileCanBeOpened) {
  TempFile temp_file;
  ASSERT_TRUE(std::filesystem::exists(temp_file.GetPath()));
  SingleFileBase<32, TypeParam> file(temp_file.GetPath());
  EXPECT_EQ(0, file.GetNumPages());
}

TYPED_TEST_P(SingleFileTest, NonExistingFileIsCreated) {
  TempFile temp_file;
  ASSERT_TRUE(std::filesystem::exists(temp_file.GetPath()));
  std::filesystem::remove(temp_file.GetPath());
  ASSERT_FALSE(std::filesystem::exists(temp_file.GetPath()));
  SingleFileBase<32, TypeParam> file(temp_file.GetPath());
  EXPECT_TRUE(std::filesystem::exists(temp_file.GetPath()));
  EXPECT_EQ(0, file.GetNumPages());
}

TYPED_TEST_P(SingleFileTest, NestedDirectoryIsCreatedIfNeeded) {
  TempDir temp_dir;
  SingleFileBase<32, TypeParam> file(temp_dir.GetPath() / "some" / "dir" /
                                     "file.dat");
  EXPECT_TRUE(std::filesystem::exists(temp_dir.GetPath()));
  EXPECT_TRUE(std::filesystem::exists(temp_dir.GetPath() / "some"));
  EXPECT_TRUE(std::filesystem::exists(temp_dir.GetPath() / "some" / "dir"));
  EXPECT_TRUE(std::filesystem::exists(temp_dir.GetPath() / "some" / "dir" /
                                      "file.dat"));
  EXPECT_EQ(0, file.GetNumPages());
}

TYPED_TEST_P(SingleFileTest, InitialFileIsEmpty) {
  TempFile temp_file;
  SingleFileBase<32, TypeParam> file(temp_file.GetPath());
  EXPECT_EQ(0, file.GetNumPages());
}

TYPED_TEST_P(SingleFileTest, PagesCanBeWrittenAndRead) {
  using Page = Page<kFileSystemPageSize>;
  TempFile temp_file;
  SingleFileBase<Page::kPageSize, TypeParam> file(temp_file.GetPath());

  Page page_a{std::byte{0x01}, std::byte{0x02}};
  file.StorePage(0, page_a);
  EXPECT_EQ(1, file.GetNumPages());

  Page restored;
  file.LoadPage(0, restored);
  EXPECT_EQ(page_a, restored);
}

TYPED_TEST_P(SingleFileTest, PagesAreDifferentiated) {
  TempFile temp_file;
  using Page = Page<kFileSystemPageSize>;
  SingleFileBase<Page::kPageSize, TypeParam> file(temp_file.GetPath());

  Page page_a{std::byte{0x01}, std::byte{0x02}};
  Page page_b{std::byte{0x03}, std::byte{0x04}};

  file.StorePage(0, page_a);
  file.StorePage(1, page_b);
  EXPECT_EQ(2, file.GetNumPages());

  Page restored;
  file.LoadPage(0, restored);
  EXPECT_EQ(page_a, restored);
  file.LoadPage(1, restored);
  EXPECT_EQ(page_b, restored);
}

TYPED_TEST_P(SingleFileTest, WritingPagesCreatesImplicitEmptyPages) {
  TempFile temp_file;
  using Page = Page<kFileSystemPageSize>;
  SingleFileBase<Page::kPageSize, TypeParam> file(temp_file.GetPath());

  // Storing a page at position 2 implicitly creates pages 0 and 1.
  Page page_a{std::byte{0x01}, std::byte{0x02}};
  file.StorePage(2, page_a);
  EXPECT_EQ(3, file.GetNumPages());

  Page zero{};
  Page restored;
  file.LoadPage(0, restored);
  EXPECT_EQ(zero, restored);
  file.LoadPage(1, restored);
  EXPECT_EQ(zero, restored);
  file.LoadPage(2, restored);
  EXPECT_EQ(page_a, restored);
}

TYPED_TEST_P(SingleFileTest, LoadingUninitializedPagesLeadsToZeros) {
  TempFile temp_file;
  using Page = Page<kFileSystemPageSize>;
  SingleFileBase<Page::kPageSize, TypeParam> file(temp_file.GetPath());
  Page zero{};
  Page loaded;
  loaded.fill(std::byte{1});
  file.LoadPage(0, loaded);
  EXPECT_EQ(zero, loaded);
}

REGISTER_TYPED_TEST_SUITE_P(SingleFileTest, IsFile, ExistingFileCanBeOpened,
                            NonExistingFileIsCreated,
                            NestedDirectoryIsCreatedIfNeeded,
                            InitialFileIsEmpty, PagesCanBeWrittenAndRead,
                            PagesAreDifferentiated,
                            WritingPagesCreatesImplicitEmptyPages,
                            LoadingUninitializedPagesLeadsToZeros);

using RawFileTypes = ::testing::Types<internal::FStreamFile, internal::CFile,
                                      internal::PosixFile>;
INSTANTIATE_TYPED_TEST_SUITE_P(My, SingleFileTest, RawFileTypes);

}  // namespace
}  // namespace carmen::backend::store
