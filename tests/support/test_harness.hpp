#pragma once

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

#include "ensemble_fabric/ids.hpp"

namespace ef_test {

struct Case {
  std::string name;
  std::function<void()> body;
};

inline std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

inline int& failure_count() {
  static int failures = 0;
  return failures;
}

inline int& check_count() {
  static int checks = 0;
  return checks;
}

inline std::string& current_case() {
  static std::string name;
  return name;
}

/// Thrown by EF_REQUIRE_OK so that a case stops at the first unmet precondition
/// instead of dereferencing an Outcome that never held a value.
struct RequirementFailure {};

struct Registrar {
  Registrar(const char* name, std::function<void()> body) {
    Case entry;
    entry.name = name;
    entry.body = std::move(body);
    registry().push_back(std::move(entry));
  }
};

inline void fail(const char* file, int line, const std::string& message) {
  ++failure_count();
  std::cout << "  FAIL " << current_case() << " (" << file << ":" << line << "): " << message
            << std::endl;
}

template <typename T>
inline std::string describe(const T& value) {
  if constexpr (std::is_enum_v<T>) {
    // Every enumeration in the runtime has a stable spelling; reusing it keeps
    // failure messages readable without teaching the harness about each enum.
    return std::string(to_string(value));
  } else {
    std::ostringstream stream;
    stream << value;
    return stream.str();
  }
}

inline std::string describe(bool value) { return value ? "true" : "false"; }
inline std::string describe(const std::string& value) { return value; }

template <typename Tag, typename Rep>
inline std::string describe(const ensemble_fabric::StrongId<Tag, Rep>& value) {
  return value.to_string();
}

inline int run_all(int argc, char** argv) {
  std::string filter;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--filter" && index + 1 < argc) {
      filter = argv[++index];
    }
  }
  int executed = 0;
  for (const Case& entry : registry()) {
    if (!filter.empty() && entry.name.find(filter) == std::string::npos) {
      continue;
    }
    current_case() = entry.name;
    const int before = failure_count();
    try {
      entry.body();
    } catch (const RequirementFailure&) {
      // The unmet requirement has already been recorded as a failure; the case
      // simply stops here.
    } catch (const std::exception& error) {
      fail(__FILE__, __LINE__, std::string("unhandled exception: ") + error.what());
    } catch (...) {
      fail(__FILE__, __LINE__, "unhandled non-standard exception");
    }
    ++executed;
    if (failure_count() == before) {
      std::cout << "  ok   " << entry.name << std::endl;
    }
  }
  std::cout << executed << " case(s), " << check_count() << " check(s), " << failure_count()
            << " failure(s)" << std::endl;
  return failure_count() == 0 ? 0 : 1;
}

}  // namespace ef_test

#define EF_TEST_MAIN                                    \
  int main(int argc, char** argv) { return ef_test::run_all(argc, argv); }

#define EF_TEST(name)                                                     \
  static void ef_case_##name();                                           \
  static const ef_test::Registrar ef_registrar_##name(#name, ef_case_##name); \
  static void ef_case_##name()

#define EF_CHECK(condition)                                                          \
  do {                                                                               \
    ++ef_test::check_count();                                                        \
    if (!(condition)) {                                                              \
      ef_test::fail(__FILE__, __LINE__, "expected: " #condition);                    \
    }                                                                                \
  } while (false)

#define EF_CHECK_EQ(actual, expected)                                                \
  do {                                                                               \
    ++ef_test::check_count();                                                        \
    const auto& ef_actual_value = (actual);                                          \
    const auto& ef_expected_value = (expected);                                      \
    if (!(ef_actual_value == ef_expected_value)) {                                   \
      ef_test::fail(__FILE__, __LINE__,                                              \
                    std::string("expected " #actual " == " #expected " but got ") +  \
                        ef_test::describe(ef_actual_value) + " vs " +                \
                        ef_test::describe(ef_expected_value));                       \
    }                                                                                \
  } while (false)

#define EF_CHECK_OK(expression)                                                      \
  do {                                                                               \
    ++ef_test::check_count();                                                        \
    const auto& ef_outcome = (expression);                                           \
    if (!ef_outcome.ok()) {                                                          \
      ef_test::fail(__FILE__, __LINE__,                                              \
                    std::string(#expression " failed: ") +                           \
                        std::string(ensemble_fabric::to_string(ef_outcome.code())) + \
                        " - " + ef_outcome.error().message);                         \
    }                                                                                \
  } while (false)

#define EF_REQUIRE_OK(expression)                                                    \
  do {                                                                               \
    ++ef_test::check_count();                                                        \
    const auto& ef_outcome = (expression);                                           \
    if (!ef_outcome.ok()) {                                                          \
      ef_test::fail(__FILE__, __LINE__,                                              \
                    std::string(#expression " failed: ") +                           \
                        std::string(to_string(ef_outcome.code())) + " - " +          \
                        ef_outcome.error().message);                                 \
      throw ef_test::RequirementFailure{};                                           \
    }                                                                                \
  } while (false)

#define EF_REQUIRE(condition)                                                        \
  do {                                                                               \
    ++ef_test::check_count();                                                        \
    if (!(condition)) {                                                              \
      ef_test::fail(__FILE__, __LINE__, "required: " #condition);                    \
      throw ef_test::RequirementFailure{};                                           \
    }                                                                                \
  } while (false)

#define EF_CHECK_ERR(expression, expected_code)                                      \
  do {                                                                               \
    ++ef_test::check_count();                                                        \
    const auto& ef_outcome = (expression);                                           \
    if (ef_outcome.ok()) {                                                           \
      ef_test::fail(__FILE__, __LINE__, std::string(#expression " unexpectedly succeeded")); \
    } else if (ef_outcome.code() != (expected_code)) {                               \
      ef_test::fail(__FILE__, __LINE__,                                              \
                    std::string(#expression " produced ") +                          \
                        std::string(ensemble_fabric::to_string(ef_outcome.code())) + \
                        " instead of " +                                             \
                        std::string(ensemble_fabric::to_string(expected_code)));     \
    }                                                                                \
  } while (false)
