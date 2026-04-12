#pragma once

#include <exception>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace orchard_test {

class Failure : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

struct TestCase {
  std::string_view name;
  void (*function)();
};

inline void Require(bool condition, std::string_view expression, std::string_view file, int line) {
  if (condition) {
    return;
  }

  std::ostringstream message;
  message << file << ":" << line << ": assertion failed: " << expression;
  throw Failure(message.str());
}

inline int RunTests(std::initializer_list<TestCase> test_cases) {
  std::size_t passed = 0;

  for (const auto& test_case : test_cases) {
    try {
      test_case.function();
      ++passed;
      std::cout << "[PASS] " << test_case.name << '\n';
    } catch (const std::exception& exception) {
      std::cerr << "[FAIL] " << test_case.name << ": " << exception.what() << '\n';
      return 1;
    }
  }

  std::cout << "Executed " << passed << " test(s).\n";
  return 0;
}

} // namespace orchard_test

#define ORCHARD_TEST_REQUIRE(expression)                                                           \
  ::orchard_test::Require((expression), #expression, __FILE__, __LINE__)
