package common

import (
	"golang.org/x/crypto/sha3"
	"testing"
	"unsafe"
)

func TestHashPassThrough(t *testing.T) {
	cacheSize := 1000
	hasher := NewCachedHasher[Address](cacheSize, AddressSerializer{})
	var adr Address
	for i := 0; i < 2*cacheSize; i++ {
		if got, want := hasher.Hash(adr), GetHash(sha3.NewLegacyKeccak256(), adr[:]); got != want {
			t.Errorf("hashes do not match: %v != %v", got, want)
		}
		adr[i%20]++
	}
}

func TestHashPassThroughRunParallel(t *testing.T) {
	cacheSize := 100
	hasher := NewCachedHasher[Address](cacheSize, AddressSerializer{})
	for i := 0; i < 10_000*cacheSize; i++ {
		var addr Address
		go func(addr Address) {
			if got, want := hasher.Hash(addr), GetHash(sha3.NewLegacyKeccak256(), addr[:]); got != want {
				t.Errorf("hashes do not match: %v != %v", got, want)
			}
		}(addr)
		addr[i%20]++
	}
}

func TestHasherPool(t *testing.T) {
	pool := newHasherPool()

	hasher := pool.getHasher()

	// hasher created as new - the pool is empty
	if len(pool.pool) != 0 {
		t.Errorf("pool is not empty: %d", len(pool.pool))
	}

	// hasher returned to the pool
	pool.returnHasher(hasher)

	if len(pool.pool) != 1 {
		t.Errorf("pool is empty: %d", len(pool.pool))
	}

	if pool.pool[0] != hasher {
		t.Errorf("hasher not the same as the one in the pool")
	}

	// get again from the pool
	hasher2 := pool.getHasher()

	if hasher2 != hasher {
		t.Errorf("hasher not the same as the one in the pool")
	}

	// hasher returned - the pool is empty

	if len(pool.pool) != 0 {
		t.Errorf("pool capacity should change: %d", len(pool.pool))
	}
}

func TestMemoryFootprint(t *testing.T) {
	cacheSize := 1000
	hasher := NewCachedHasher[Address](cacheSize, AddressSerializer{})

	// fully utilised cache
	var adr Address
	for i := 0; i < cacheSize; i++ {
		if got, want := hasher.Hash(adr), GetHash(sha3.NewLegacyKeccak256(), adr[:]); got != want {
			t.Errorf("hashes do not match: %v != %v", got, want)
		}
		adr[i%20]++
	}

	var h Hash
	if got, want := hasher.GetMemoryFootprint().Total(), uintptr(cacheSize)*(unsafe.Sizeof(h)+unsafe.Sizeof(adr)); got < want {
		t.Errorf("no memory foodprint provided")
	}

	if hasher.GetMemoryFootprint().GetChild("cache") == nil {
		t.Errorf("memory footprint missing")
	}

	if got, want := hasher.GetMemoryFootprint().GetChild("cache").Total(), uintptr(cacheSize)*(unsafe.Sizeof(h)+unsafe.Sizeof(adr)); got < want {
		t.Errorf("no memory foodprint provided")
	}

	if hasher.GetMemoryFootprint().GetChild("hashersPool") == nil {
		t.Errorf("memory footprint missing")
	}

	if got, want := hasher.GetMemoryFootprint().GetChild("hashersPool").Total(), uintptr(0); got <= want {
		t.Errorf("no memory foodprint provided")
	}

}