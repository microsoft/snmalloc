#pragma once

#include <type_traits>

/**
 * C++20 concepts are referenced as if they were types in declarations within
 * template parameters (e.g. "template<FooConcept Foo> ...").  That is, they
 * take the place of the "typename"/"class" keyword on template parameters.
 * If the compiler understands concepts, this macro expands as its argument;
 * otherwise, it expands to the keyword "typename", so snmalloc templates that
 * use concept-qualified parameters should use this to remain compatible across
 * C++ versions: "template<SNMALLOC_CONCEPT(FooConcept) Foo>"
 */
#ifdef __cpp_concepts
#  define SNMALLOC_CONCEPT(c) c
#else
#  define SNMALLOC_CONCEPT(c) typename
#endif

#ifdef __cpp_concepts
namespace snmalloc
{
  /**
   * C++20 concepts are more than just new syntax; there's a new support
   * library specified as well.  As C++20 is quite new, however, there are some
   * environments, notably Clang, that understand the syntax but do not yet
   * offer the library.  Fortunately, alternate pronouciations are possible.
   */
#  ifdef _cpp_lib_concepts
  /**
   * ConceptSame<T,U> is true if T and U are the same type and false otherwise.
   * When specifying a concept, use ConceptSame<U> to indicate that an
   * expression must evaluate precisely to the type U.
   */
  template<typename T, typename U>
  concept ConceptSame = std::same_as<T, U>;
#  else
  template<typename T, typename U>
  concept ConceptSame = std::is_same<T, U>::value;
#  endif

  /**
   * Equivalence mod std::remove_reference
   */
  template<typename T, typename U>
  concept ConceptSameModRef =
    ConceptSame<std::remove_reference_t<T>, std::remove_reference_t<U>>;

  /**
   * Some of the types in snmalloc are circular in their definition and use
   * templating as a lazy language to carefully tie knots and only pull on the
   * whole mess once it's assembled.  Unfortunately, concepts amount to eagerly
   * demanding the result of the computation.  If concepts come into play during
   * the circular definition, they may see an incomplete type and so fail (with
   * "incomplete type ... used in type trait expression" or similar).  However,
   * it turns out that SFINAE gives us a way to detect whether a template
   * parameter refers to an incomplete type, and short circuit evaluation means
   * we can bail on concept checking if we find ourselves in this situation.
   *
   * See https://devblogs.microsoft.com/oldnewthing/20190710-00/?p=102678
   *
   * Unfortunately, C++20 concepts are not first-order things and, in
   * particular, cannot themselves be template parameters.  So while we would
   * love to write a generic Lazy combinator,
   *
   *   template<template<typename> concept C, typename T>
   *   concept Lazy = !is_type_complete_v<T> || C<T>();
   *
   * this will instead have to be inlined at every definition (and referred to
   * explicitly at call sites) until C++23 or later.
   */
  template<typename, typename = void>
  constexpr bool is_type_complete_v = false;
  template<typename T>
  constexpr bool is_type_complete_v<T, std::void_t<decltype(sizeof(T))>> = true;

} // namespace snmalloc
#endif
