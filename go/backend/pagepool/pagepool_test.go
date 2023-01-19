package pagepool

import (
	"github.com/Fantom-foundation/Carmen/go/common"
	"testing"
)

var (
	pageA = 0
	pageB = 1
	pageC = 2
	pageD = 3

	data = []byte{0xAA}
)

func TestEmptyPagePool(t *testing.T) {
	pagePool := initPagePool()

	if page, _ := pagePool.Get(pageA); page.Size() != 0 {
		t.Errorf("Pool should be empty")
	}
}

func TestPageGetSet(t *testing.T) {
	pagePool := initPagePool()

	// create pages
	newPage1, _ := pagePool.Get(pageA)
	newPage2, _ := pagePool.Get(pageD)

	if actPage, _ := pagePool.Get(pageD); actPage != newPage2 {
		t.Errorf("Wrong page returned")
	}

	if actPage, _ := pagePool.Get(pageA); actPage != newPage1 {
		t.Errorf("Wrong page returned")
	}
}

func TestPageOverflows(t *testing.T) {
	pagePool := initPagePool()

	evictedPage := NewRawPage(common.PageSize)
	evictedPage.FromBytes(data[:]) // to track a non-empty page

	page := NewRawPage(common.PageSize)
	// 3 pages with 4 items each
	_ = pagePool.put(pageA, evictedPage)
	_ = pagePool.put(pageB, page)
	_ = pagePool.put(pageC, page)

	_ = pagePool.put(pageD, page)

	// Here a page is loaded from the persistent storage.
	// If the page exists there, it verifies the page was evicted from the page pool
	testPage := NewRawPage(common.PageSize)
	if err := pagePool.pageStore.Load(pageA, testPage); testPage.Size() == 0 || err != nil {
		t.Errorf("One page should be evicted")
	}
}

func TestRemovedPageDoesNotExist(t *testing.T) {
	pagePool := initPagePool()

	_, _ = pagePool.Get(pageA) // create the page
	_, _ = pagePool.Remove(pageA)

	if actualPage, _ := pagePool.Get(pageA); actualPage.Size() != 0 {
		t.Errorf("Page was not deleted: %v", actualPage)
	}
}

func TestPageRemoveOverflow(t *testing.T) {
	evictedPage := NewRawPage(common.PageSize)
	evictedPage.FromBytes(data[:]) // to track a non-empty page
	page := NewRawPage(common.PageSize)

	pagePool := initPagePool()

	_ = pagePool.put(pageA, evictedPage)
	_ = pagePool.put(pageB, page)
	_ = pagePool.put(pageC, page)
	_ = pagePool.put(pageD, page)

	// Here a page is loaded from the persistent storage.
	// If the page exists there, it verifies the page was evicted from the page pool
	testPage := NewRawPage(common.PageSize)
	if err := pagePool.pageStore.Load(pageA, testPage); testPage.Size() == 0 || err != nil {
		t.Errorf("Page is not evicted. ")
	}

	_, _ = pagePool.Remove(pageA)

	// removed from the page pool
	if removed, _ := pagePool.Get(pageA); removed.Size() != 0 {
		t.Errorf("Page is not removed: %v", removed)
	}

	// and from the store
	testPage = NewRawPage(common.PageSize)
	if err := pagePool.pageStore.Load(pageA, testPage); testPage.Size() != 0 || err != nil {
		t.Errorf("Page is not removed. ")
	}

	// remove from the pool only
	if exists, _ := pagePool.Remove(pageB); exists {
		t.Errorf("Page is not removed")
	}

	// removed from the page pool - repeat
	if removed, _ := pagePool.Get(pageB); removed.Size() != 0 {
		t.Errorf("Page should not exist: %v", removed)
	}
}

func TestPageClose(t *testing.T) {
	pagePool := initPagePool()

	newPage, _ := pagePool.Get(pageA)
	newPage.FromBytes(data) // to track a non-empty page

	if actualPage, _ := pagePool.Get(pageA); actualPage.Size() == 0 || actualPage != newPage {
		t.Errorf("Page was not created, %v != %v", actualPage, newPage)
	}

	_ = pagePool.Close()

	// close must persist the page
	// try to get the page from the storage, and it must exist there
	page := NewRawPage(common.PageSize)
	if err := pagePool.pageStore.Load(pageA, page); err != nil || page.Size() == 0 {
		t.Errorf("Page is not persisted, %v ", page)
	}
}

func initPagePool() *PagePool[int, *RawPage] {
	poolSize := 3
	pageFactory := func() *RawPage { return NewRawPage(common.PageSize) }
	return NewPagePool[int, *RawPage](poolSize, NewMemoryPageStore[int](nextIdGenerator()), pageFactory)
}

func nextIdGenerator() func() int {
	var id int
	return func() int {
		id += 1
		return id
	}
}
