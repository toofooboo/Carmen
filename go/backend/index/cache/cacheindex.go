package cache

import (
	"github.com/Fantom-foundation/Carmen/go/backend/index"
	"github.com/Fantom-foundation/Carmen/go/common"
)

// Index wraps another index and a cache
type Index[K comparable, I common.Identifier] struct {
	wrapped index.Index[K, I]
	cache   *common.Cache[K, I]
}

// NewIndex constructs a new Index instance, which either delegates to the wrapped index or gets data from the cache if it has them.
func NewIndex[K comparable, I common.Identifier](wrapped index.Index[K, I], cacheCapacity int) *Index[K, I] {
	return &Index[K, I]{wrapped, common.NewCache[K, I](cacheCapacity)}
}

// GetOrAdd returns an index mapping for the key, or creates the new index
func (m *Index[K, I]) GetOrAdd(key K) (idx I, err error) {
	idx, exists := m.cache.Get(key)
	if !exists {
		idx, err = m.wrapped.GetOrAdd(key)
		m.cache.Set(key, idx)
	}
	return
}

// Get returns an index mapping for the key, returns index.ErrNotFound if not exists
func (m *Index[K, I]) Get(key K) (idx I, err error) {
	idx, exists := m.cache.Get(key)
	if !exists {
		idx, err = m.wrapped.Get(key)
		m.cache.Set(key, idx)
	}
	return
}

// Contains returns whether the key exists in the mapping or not.
func (m *Index[K, I]) Contains(key K) (exists bool) {
	_, exists = m.cache.Get(key)
	if !exists {
		if idx, err := m.wrapped.Get(key); err != index.ErrNotFound {
			m.cache.Set(key, idx)
		}
	}

	return
}

// GetStateHash returns the index hash.
func (m *Index[K, I]) GetStateHash() (common.Hash, error) {
	return m.wrapped.GetStateHash()
}

// Close closes the storage and clean-ups all possible dirty values
func (m *Index[K, I]) Close() error {
	return m.wrapped.Close()
}