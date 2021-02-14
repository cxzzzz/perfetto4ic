/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef INCLUDE_PERFETTO_TRACING_TRACED_VALUE_H_
#define INCLUDE_PERFETTO_TRACING_TRACED_VALUE_H_

#include "perfetto/base/compiler.h"
#include "perfetto/base/export.h"
#include "perfetto/base/template_util.h"
#include "perfetto/tracing/internal/checked_scope.h"
#include "perfetto/tracing/traced_value_forward.h"
#include "protos/perfetto/trace/track_event/debug_annotation.pbzero.h"

#include <type_traits>
#include <utility>

namespace perfetto {

class DebugAnnotation;

// These classes provide a JSON-inspired way to write structed data into traces.
//
// Each TracedValue can be consumed exactly once to write a value into a trace
// using one of the Write* methods.
//
// Write* methods fall into two categories:
// - Primitive types (int, string, bool, double, etc): they just write the
//   provided value, consuming the TracedValue in the process.
// - Complex types (arrays and dicts): they consume the TracedValue and
//   return a corresponding scoped object (TracedArray or TracedDictionary).
//   This scope then can be used to write multiple items into the container:
//   TracedArray::AppendItem and TracedDictionary::AddItem return a new
//   TracedValue which then can be used to write an element of the
//   dictionary or array.
//
// To define how a custom class should be written into the trace, users should
// define one of the two following functions:
// - Foo::WriteIntoTracedValue(TracedValue) const
//   (preferred for code which depends on perfetto directly)
// - perfetto::TraceFormatTraits<T>::WriteIntoTracedValue(
//       TracedValue, const T&);
//   (should be used if T is defined in a library which doesn't know anything
//   about tracing).
//
//
// After definiting a conversion method, the object can be used directly as a
// TRACE_EVENT argument:
//
// Foo foo;
// TRACE_EVENT("cat", "Event", "arg", foo);
//
// Examples:
//
// TRACE_EVENT("cat", "event", "params", [&](perfetto::TracedValue context)
// {
//   auto dict = std::move(context).WriteDictionary();
//   dict->Add("param1", param1);
//   dict->Add("param2", param2);
//   ...
//   dict->Add("paramN", paramN);
//
//   {
//     auto inner_array = dict->AddArray("inner");
//     inner_array->Append(value1);
//     inner_array->Append(value2);
//   }
// });
//
// template <typename T>
// TraceFormatTraits<std::optional<T>>::WriteIntoTracedValue(
//    TracedValue context, const std::optional<T>& value) {
//  if (!value) {
//    std::move(context).WritePointer(nullptr);
//    return;
//  }
//  perfetto::WriteIntoTracedValue(std::move(context), *value);
// }
//
// template <typename T>
// TraceFormatTraits<std::vector<T>>::WriteIntoTracedValue(
//    TracedValue context, const std::array<T>& value) {
//  auto array = std::move(context).WriteArray();
//  for (const auto& item: value) {
//    array_scope.Append(item);
//  }
// }
//
// class Foo {
//   void WriteIntoTracedValue(TracedValue context) const {
//     auto dict = std::move(context).WriteDictionary();
//     dict->Set("key", 42);
//     dict->Set("foo", "bar");
//     dict->Set("member", member_);
//   }
// }
class TracedArray;
class TracedDictionary;
class TracedValue;

namespace internal {
PERFETTO_EXPORT TracedValue
CreateTracedValueFromProto(protos::pbzero::DebugAnnotation*);
}

class PERFETTO_EXPORT TracedValue {
 public:
  TracedValue(const TracedValue&) = delete;
  TracedValue& operator=(const TracedValue&) = delete;
  TracedValue& operator=(TracedValue&&) = delete;
  TracedValue(TracedValue&&) = default;
  ~TracedValue() = default;

  // TracedValue represents a context into which a single value can be written
  // (either by writing it directly for primitive types, or by creating a
  // TracedArray or TracedDictionary for the complex types). This is enforced
  // by allowing Write* methods to be called only on rvalue references.

  void WriteInt64(int64_t value) &&;
  void WriteUInt64(uint64_t value) &&;
  void WriteDouble(double value) &&;
  void WriteBoolean(bool value) &&;
  void WriteString(const char*) &&;
  void WriteString(const char*, size_t len) &&;
  void WriteString(const std::string&) &&;
  void WritePointer(const void* value) &&;

  // Rules for writing nested dictionaries and arrays:
  // - Only one scope (TracedArray, TracedDictionary or TracedValue) can be
  // active at the same time. It's only allowed to call methods on the active
  // scope.
  // - When a scope creates a nested scope, the new scope becomes active.
  // - When a scope is destroyed, it's parent scope becomes active again.
  //
  // Typically users will have to create a scope only at the beginning of a
  // conversion function and this scope should be destroyed at the end of it.
  // TracedArray::Append and TracedDictionary::Add create, write and complete
  // inner scopes automatically.

  // Scope which allows multiple values to be appended.
  TracedArray WriteArray() && PERFETTO_WARN_UNUSED_RESULT;

  // Scope which allows multiple key-value pairs to be added.
  TracedDictionary WriteDictionary() && PERFETTO_WARN_UNUSED_RESULT;

 private:
  friend class TracedArray;
  friend class TracedDictionary;
  friend TracedValue internal::CreateTracedValueFromProto(
      protos::pbzero::DebugAnnotation*);

  static TracedValue CreateFromProto(protos::pbzero::DebugAnnotation*);

  inline explicit TracedValue(protos::pbzero::DebugAnnotation* root_context,
                              internal::CheckedScope* parent_scope)
      : root_context_(root_context), checked_scope_(parent_scope) {}
  inline explicit TracedValue(
      protos::pbzero::DebugAnnotation::NestedValue* nested_context,
      internal::CheckedScope* parent_scope)
      : nested_context_(nested_context), checked_scope_(parent_scope) {}

  // Temporary support for perfetto::DebugAnnotation C++ class before it's going
  // to be replaced by TracedValue.
  // TODO(altimin): Convert v8 to use TracedValue directly and delete it.
  friend class DebugAnnotation;

  // Only one of them can be null.
  // TODO(altimin): replace DebugAnnotation with something that doesn't require
  // this duplication.
  protos::pbzero::DebugAnnotation* root_context_ = nullptr;
  protos::pbzero::DebugAnnotation::NestedValue* nested_context_ = nullptr;

  internal::CheckedScope checked_scope_;
};

class PERFETTO_EXPORT TracedArray {
 public:
  TracedArray(const TracedArray&) = delete;
  TracedArray& operator=(const TracedArray&) = delete;
  TracedArray& operator=(TracedArray&&) = delete;
  TracedArray(TracedArray&&) = default;
  ~TracedArray() = default;

  TracedValue AppendItem();

  template <typename T>
  void Append(T&& value) {
    WriteIntoTracedValue(AppendItem(), std::forward<T>(value));
  }

  TracedDictionary AppendDictionary() PERFETTO_WARN_UNUSED_RESULT;
  TracedArray AppendArray();

 private:
  friend class TracedValue;

  inline explicit TracedArray(
      protos::pbzero::DebugAnnotation::NestedValue* value,
      internal::CheckedScope* parent_scope)
      : value_(value), checked_scope_(parent_scope) {}

  protos::pbzero::DebugAnnotation::NestedValue* value_;

  internal::CheckedScope checked_scope_;
};

class PERFETTO_EXPORT TracedDictionary {
 public:
  TracedDictionary(const TracedDictionary&) = delete;
  TracedDictionary& operator=(const TracedDictionary&) = delete;
  TracedDictionary& operator=(TracedDictionary&&) = delete;
  TracedDictionary(TracedDictionary&&) = default;
  ~TracedDictionary() = default;

  TracedValue AddItem(const char* key);

  template <typename T>
  void Add(const char* key, T&& value) {
    WriteIntoTracedValue(AddItem(key), std::forward<T>(value));
  }

  TracedDictionary AddDictionary(const char* key);
  TracedArray AddArray(const char* key);

 private:
  friend class TracedValue;

  inline explicit TracedDictionary(
      protos::pbzero::DebugAnnotation::NestedValue* value,
      internal::CheckedScope* parent_scope)
      : value_(value), checked_scope_(parent_scope) {}

  protos::pbzero::DebugAnnotation::NestedValue* value_;

  internal::CheckedScope checked_scope_;
};

namespace internal {

// SFINAE helpers for finding a right overload to convert a given class to
// trace-friendly form, ordered from most to least preferred.

constexpr int kMaxWriteImplPriority = 4;

// If T has WriteIntoTracedValue member function, call it.
template <typename T>
decltype(std::declval<T>().WriteIntoTracedValue(std::declval<TracedValue>()),
         void())
WriteImpl(base::priority_tag<4>, TracedValue context, T&& value) {
  value.WriteIntoTracedValue(std::move(context));
}

// If perfetto::TraceFormatTraits<T>::WriteIntoTracedValue(TracedValue, const
// T&) is available, use it.
template <typename T>
decltype(TraceFormatTraits<base::remove_cvref_t<T>>::WriteIntoTracedValue(
             std::declval<TracedValue>(),
             std::declval<T>()),
         void())
WriteImpl(base::priority_tag<3>, TracedValue context, T&& value) {
  TraceFormatTraits<base::remove_cvref_t<T>>::WriteIntoTracedValue(
      std::move(context), std::forward<T>(value));
}

// If T has operator(), which takes TracedValue, use it.
// Very useful for lambda resolutions.
template <typename T>
decltype(std::declval<T>()(std::declval<TracedValue>()), void())
WriteImpl(base::priority_tag<2>, TracedValue context, T&& value) {
  std::forward<T>(value)(std::move(context));
}

// If T is a container and its elements have tracing support, use it.
//
// Note: a reference to T should be passed to std::begin, otherwise
// for non-reference types const T& will be passed to std::begin, losing
// support for non-const WriteIntoTracedValue methods.
template <typename T>
typename check_traced_value_support<
    decltype(*std::begin(std::declval<T&>()))>::type
WriteImpl(base::priority_tag<1>, TracedValue context, T&& value) {
  auto array = std::move(context).WriteArray();
  for (auto&& item : value) {
    array.Append(item);
  }
}

// std::underlying_type can't be used with non-enum types, so we need this
// indirection.
template <typename T, bool = std::is_enum<T>::value>
struct safe_underlying_type {
  using type = typename std::underlying_type<T>::type;
};

template <typename T>
struct safe_underlying_type<T, false> {
  using type = T;
};

template <typename T>
struct is_incomplete_type {
  static constexpr bool value = sizeof(T) != 0;
};

// sizeof is not available for const char[], but it's still not considered to be
// an incomplete type for our purposes as the size can be determined at runtime
// due to strings being null-terminated.
template <>
struct is_incomplete_type<const char[]> {
  static constexpr bool value = true;
};

}  // namespace internal

// Helper template to determine if a given type can be passed to
// perfetto::WriteIntoTracedValue. These templates will fail to resolve if the
// class does not have it support, so they are useful in SFINAE and in producing
// helpful compiler results.
template <typename T, class Result = void>
using check_traced_value_support_t = decltype(
    internal::WriteImpl(
        std::declval<base::priority_tag<internal::kMaxWriteImplPriority>>(),
        std::declval<TracedValue>(),
        std::declval<T>()),
    std::declval<Result>());

// check_traced_value_support<T, V>::type is defined (and equal to V) iff T
// supports being passed to WriteIntoTracedValue. See the comment in
// traced_value_forward.h for more details.
template <typename T, class Result>
struct check_traced_value_support<T,
                                  Result,
                                  check_traced_value_support_t<T, Result>> {
  static_assert(
      internal::is_incomplete_type<T>::value,
      "perfetto::TracedValue should not be used with incomplete types");

  static constexpr bool value = true;
  using type = Result;
};

namespace internal {

// Helper class to check if a given type can be passed to
// perfetto::WriteIntoTracedValue. This template will always resolve (with
// |value| being set to either true or false depending on presence of the
// support, so this macro is useful in the situation when you want to e.g. OR
// the result with some other conditions.
//
// In this case, compiler will not give you the full deduction chain, so, for
// example, use check_traced_value_support for writing positive static_asserts
// and has_traced_value_support for writing negative.
template <typename T>
class has_traced_value_support {
  using Yes = char[1];
  using No = char[2];

  template <typename V>
  static Yes& check(check_traced_value_support_t<V, int>);
  template <typename V>
  static No& check(...);

 public:
  static constexpr bool value = sizeof(Yes) == sizeof(check<T>(0));
};

}  // namespace internal

template <typename T>
void WriteIntoTracedValue(TracedValue context, T&& value) {
  // TODO(altimin): Add a URL to documentation and a list of common failure
  // patterns.
  static_assert(
      internal::has_traced_value_support<T>::value,
      "The provided type (passed to TRACE_EVENT argument / TracedArray::Append "
      "/ TracedDictionary::Add) does not support being written in a trace "
      "format. Please see the comment in traced_value.h for more details.");

  // Should be kept in sync with check_traced_value_support_t!
  internal::WriteImpl(base::priority_tag<internal::kMaxWriteImplPriority>(),
                      std::move(context), std::forward<T>(value));
}

// Helpers to write a given value into TracedValue even if the given type
// doesn't support conversion (in which case the provided fallback should be
// used). Useful for automatically generating conversions for autogenerated
// code, but otherwise shouldn't be used as non-autogenerated code is expected
// to define WriteIntoTracedValue convertor.
// See WriteWithFallback test in traced_value_unittest.cc for a concrete
// example.
template <typename T>
typename std::enable_if<internal::has_traced_value_support<T>::value>::type
WriteIntoTracedValueWithFallback(TracedValue context,
                                 T&& value,
                                 const std::string&) {
  WriteIntoTracedValue(std::move(context), std::forward<T>(value));
}

template <typename T>
typename std::enable_if<!internal::has_traced_value_support<T>::value>::type
WriteIntoTracedValueWithFallback(TracedValue context,
                                 T&&,
                                 const std::string& fallback) {
  std::move(context).WriteString(fallback);
}

// TraceFormatTraits implementations for primitive types.

// Specialisation for signed integer types (note: it excludes enums, which have
// their own explicit specialisation).
template <typename T>
struct TraceFormatTraits<
    T,
    typename std::enable_if<std::is_integral<T>::value &&
                            !std::is_same<T, bool>::value &&
                            std::is_signed<T>::value>::type> {
  inline static void WriteIntoTracedValue(TracedValue context, T value) {
    std::move(context).WriteInt64(value);
  }
};

// Specialisation for unsigned integer types (note: it excludes enums, which
// have their own explicit specialisation).
template <typename T>
struct TraceFormatTraits<
    T,
    typename std::enable_if<std::is_integral<T>::value &&
                            !std::is_same<T, bool>::value &&
                            std::is_unsigned<T>::value>::type> {
  inline static void WriteIntoTracedValue(TracedValue context, T value) {
    std::move(context).WriteUInt64(value);
  }
};

// Specialisation for bools.
template <>
struct TraceFormatTraits<bool> {
  inline static void WriteIntoTracedValue(TracedValue context, bool value) {
    std::move(context).WriteBoolean(value);
  }
};

// Specialisation for floating point values.
template <typename T>
struct TraceFormatTraits<
    T,
    typename std::enable_if<std::is_floating_point<T>::value>::type> {
  inline static void WriteIntoTracedValue(TracedValue context, T value) {
    std::move(context).WriteDouble(static_cast<double>(value));
  }
};

// Specialisation for signed enums.
template <typename T>
struct TraceFormatTraits<
    T,
    typename std::enable_if<
        std::is_enum<T>::value &&
        std::is_signed<
            typename internal::safe_underlying_type<T>::type>::value>::type> {
  inline static void WriteIntoTracedValue(TracedValue context, T value) {
    std::move(context).WriteInt64(static_cast<int64_t>(value));
  }
};

// Specialisation for unsigned enums.
template <typename T>
struct TraceFormatTraits<
    T,
    typename std::enable_if<
        std::is_enum<T>::value &&
        std::is_unsigned<
            typename internal::safe_underlying_type<T>::type>::value>::type> {
  inline static void WriteIntoTracedValue(TracedValue context, T value) {
    std::move(context).WriteUInt64(static_cast<uint64_t>(value));
  }
};

// Specialisations for C-style strings.
template <>
struct TraceFormatTraits<const char*> {
  inline static void WriteIntoTracedValue(TracedValue context,
                                          const char* value) {
    std::move(context).WriteString(value);
  }
};

template <>
struct TraceFormatTraits<char[]> {
  inline static void WriteIntoTracedValue(TracedValue context,
                                          const char value[]) {
    std::move(context).WriteString(value);
  }
};

template <size_t N>
struct TraceFormatTraits<char[N]> {
  inline static void WriteIntoTracedValue(TracedValue context,
                                          const char value[N]) {
    std::move(context).WriteString(value);
  }
};

// Specialisation for C++ strings.
template <>
struct TraceFormatTraits<std::string> {
  inline static void WriteIntoTracedValue(TracedValue context,
                                          const std::string& value) {
    std::move(context).WriteString(value);
  }
};

// Specialisation for (const) void*, which writes the pointer value.
template <>
struct TraceFormatTraits<void*> {
  inline static void WriteIntoTracedValue(TracedValue context, void* value) {
    std::move(context).WritePointer(value);
  }
};

template <>
struct TraceFormatTraits<const void*> {
  inline static void WriteIntoTracedValue(TracedValue context,
                                          const void* value) {
    std::move(context).WritePointer(value);
  }
};

// Specialisation for std::unique_ptr<>, which writes either nullptr or the
// object it points to.
template <typename T>
struct TraceFormatTraits<std::unique_ptr<T>, check_traced_value_support_t<T>> {
  inline static void WriteIntoTracedValue(TracedValue context,
                                          const std::unique_ptr<T>& value) {
    ::perfetto::WriteIntoTracedValue(std::move(context), value.get());
  }
};

// Specialisation for raw pointer, which writes either nullptr or the object it
// points to.
template <typename T>
struct TraceFormatTraits<T*, check_traced_value_support_t<T>> {
  inline static void WriteIntoTracedValue(TracedValue context, T* value) {
    if (!value) {
      std::move(context).WritePointer(nullptr);
      return;
    }
    ::perfetto::WriteIntoTracedValue(std::move(context), *value);
  }
};

// Specialisation for nullptr.
template <>
struct TraceFormatTraits<std::nullptr_t> {
  inline static void WriteIntoTracedValue(TracedValue context, std::nullptr_t) {
    std::move(context).WritePointer(nullptr);
  }
};

}  // namespace perfetto

#endif  // INCLUDE_PERFETTO_TRACING_TRACED_VALUE_H_
