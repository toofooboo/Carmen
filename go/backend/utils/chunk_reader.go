package utils

import "io"

// chunkReader is an io.Reader that reads from the stored data,
// while returning them by parts of the configured size.
// It is a rather testing utility to verify certain code
// that uses the io.Reader is capable of handling situation
// where the reader does not return all expected data
// by a single read.
type chunkReader struct {
	data []byte
	size int
}

// NewChunkReader returns an io.Reader that reads from the stored data,
// while returning them by parts of the configured size.
// It is a rather testing utility to verify certain code
// that uses the io.Reader is capable of handling situation
// where the reader does not return all expected data
// by a single read.
func NewChunkReader(data []byte, chunkSize int) io.Reader {
	return &chunkReader{data, chunkSize}
}

func (r *chunkReader) Read(p []byte) (int, error) {
	if len(r.data) == 0 {
		return 0, io.EOF
	}

	if r.size >= len(r.data) {
		n := copy(p, r.data)
		r.data = r.data[n:]
		return n, nil
	}

	n := copy(p, r.data[0:r.size-1])
	r.data = r.data[n:]
	return n, nil
}