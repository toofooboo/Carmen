package mpt

import (
	"fmt"
	"os"
	"path/filepath"

	"github.com/Fantom-foundation/Carmen/go/common"
)

const lockFileName = "~lock"

// LockDirectory acquires a lock on the given directory. If needed,
// the directory is implicitly created. The operation fails if the
// lock can not be acquired due to some other thread or process holding
// the lock or due to an IO error.
//
// Note: if successful, the acquired lock needs to be explicitly released.
// The lock is not automatically released when the process is terminated.
func LockDirectory(directory string) (common.LockFile, error) {
	if err := os.MkdirAll(directory, 0700); err != nil {
		return nil, err
	}
	lock, err := common.CreateLockFile(filepath.Join(directory, lockFileName))
	if err != nil {
		return nil, fmt.Errorf("unable to gain exclusive access to %s: %w", directory, err)
	}
	return lock, nil
}