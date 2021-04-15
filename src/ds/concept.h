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

  template<typename D, typename B>
  concept ConceptSubtype = std::derived_from<D, B>;
#  else
  template<typename T, typename U>
  concept ConceptSame = std::is_same<T, U>::value;

  template<typename D, typename B>
  concept ConceptSubtype = std::is_base_of<B, D>::value;
#  endif
} // namespace snmalloc
#endif
