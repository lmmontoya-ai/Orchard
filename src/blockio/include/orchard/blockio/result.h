#pragma once

#include <utility>
#include <variant>

#include "orchard/blockio/error.h"

namespace orchard::blockio {

template <typename T> class Result {
public:
  Result(T value) : storage_(std::move(value)) {}
  Result(const Error& error) : storage_(error) {}
  Result(Error&& error) : storage_(std::move(error)) {}

  [[nodiscard]] bool ok() const noexcept {
    return std::holds_alternative<T>(storage_);
  }
  [[nodiscard]] explicit operator bool() const noexcept {
    return ok();
  }

  [[nodiscard]] const T& value() const& {
    return std::get<T>(storage_);
  }
  [[nodiscard]] T& value() & {
    return std::get<T>(storage_);
  }
  [[nodiscard]] T&& value() && {
    return std::get<T>(std::move(storage_));
  }

  [[nodiscard]] const Error& error() const {
    return std::get<Error>(storage_);
  }

private:
  std::variant<T, Error> storage_;
};

} // namespace orchard::blockio
