#pragma once

#include <cstdint>
#include <ostream>

namespace carmen {

// An AccountState models the life-cycle of accounts.
// Note: the assigned values need to be kept in sync with the Go counterpart.
enum class AccountState : std::uint8_t {
  // An unknown account.
  kUnknown = 0,
  // A active account.
  kExists = 1,
  // A deleted account.
  kDeleted = 2,
};

std::ostream& operator<<(std::ostream&, AccountState);

}  // namespace carmen