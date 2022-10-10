package common

import "math/rand"

// Distribution wraps a Label of the distribution and a function to get a next value withing the given distribution
type Distribution struct {
	Label   string
	GetNext func() uint32
}

// GetDistributions return a set of distributions
func GetDistributions(size int) []Distribution {
	expRate := float64(10) / float64(size)
	it := 0
	return []Distribution{
		{
			Label: "Sequential",
			GetNext: func() uint32 {
				it = (it + 1) % size
				return uint32(it)
			},
		},
		{
			Label: "Uniform",
			GetNext: func() uint32 {
				return uint32(rand.Intn(size))
			},
		},
		{
			Label: "Exponential",
			GetNext: func() uint32 {
				return uint32(rand.ExpFloat64() / expRate)
			},
		},
	}
}