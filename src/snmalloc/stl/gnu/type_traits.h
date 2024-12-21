#pragma once

#include <stddef.h>

namespace snmalloc
{
  namespace stl
  {
    /**
     * Type identity metafunction.
     */
    template<typename T>
    struct type_identity
    {
      using type = T;
    };

    template<typename T>
    using type_identity_t = typename type_identity<T>::type;

    /**
     * Integral constant.
     */
    template<typename T, T v>
    struct integral_constant
    {
      using value_type = T;
      static constexpr T value = v;
    };

    template<bool B>
    using bool_constant = integral_constant<bool, B>;

    using true_type = bool_constant<true>;
    using false_type = bool_constant<false>;

    /**
     * Remove CV qualifiers.
     */
    template<class T>
    struct remove_cv : type_identity<T>
    {};

    template<class T>
    struct remove_cv<const T> : type_identity<T>
    {};

    template<class T>
    struct remove_cv<volatile T> : type_identity<T>
    {};

    template<class T>
    struct remove_cv<const volatile T> : type_identity<T>
    {};

    template<class T>
    using remove_cv_t = typename remove_cv<T>::type;

    /**
     * Type equality.
     */
    template<typename T, typename U>
    struct is_same : false_type
    {};

    template<typename T>
    struct is_same<T, T> : true_type
    {};

    template<typename T, typename U>
    inline constexpr bool is_same_v = is_same<T, U>::value;

    /**
     * Is integral type.
     */
    template<typename T>
    struct is_integral
    {
    private:
      template<typename Head, typename... Args>
      static constexpr bool is_unqualified_any_of()
      {
        return (... || is_same_v<remove_cv_t<Head>, Args>);
      }

    public:
      static constexpr bool value = is_unqualified_any_of<
        T,
#ifdef __SIZEOF_INT128__
        __int128_t,
        __uint128_t,
#endif
        char,
        signed char,
        unsigned char,
        short,
        unsigned short,
        int,
        unsigned int,
        long,
        unsigned long,
        long long,
        unsigned long long,
        bool>();
    };

    template<typename T>
    inline constexpr bool is_integral_v = is_integral<T>::value;

    /**
     * Remove all array extents.
     */
#if __has_builtin(__remove_all_extents)
    template<typename T>
    using remove_all_extents_t = __remove_all_extents(T);
#else
    template<class T>
    struct remove_all_extents
    {
      using type = T;
    };

    template<class T>
    struct remove_all_extents<T[]>
    {
      using type = typename remove_all_extents<T>::type;
    };

    template<class T, size_t N>
    struct remove_all_extents<T[N]>
    {
      using type = typename remove_all_extents<T>::type;
    };

    template<class T>
    using remove_all_extents_t = typename remove_all_extents<T>::type;
#endif

    /**
     * void_t
     */
    template<typename... Ts>
    using void_t = typename type_identity<void>::type;

    /**
     * Has unique object representations.
     */
    /* remove_all_extents_t is needed due to clang's behavior */
    template<class T>
    inline constexpr bool has_unique_object_representations_v =
      __has_unique_object_representations(remove_all_extents_t<T>);

    /**
     * enable_if
     */
    template<bool B, typename T = void>
    struct enable_if;

    template<typename T>
    struct enable_if<true, T> : type_identity<T>
    {};

    template<bool B, typename T = void>
    using enable_if_t = typename enable_if<B, T>::type;

    /**
     * conditional
     */

    template<bool B, typename T, typename F>
    struct conditional : type_identity<T>
    {};

    template<typename T, typename F>
    struct conditional<false, T, F> : type_identity<F>
    {};

    template<bool B, typename T, typename F>
    using conditional_t = typename conditional<B, T, F>::type;

    /**
     * add_lvalue_reference/add_rvalue_reference
     */
#if __has_builtin(__add_lvalue_reference)
    template<class T>
    using add_lvalue_reference_t = __add_lvalue_reference(T);
#else
    template<class T> // Note that `cv void&` is a substitution failure
    auto __add_lvalue_reference_impl(int) -> type_identity<T&>;
    template<class T> // Handle T = cv void case
    auto __add_lvalue_reference_impl(...) -> type_identity<T>;

    template<class T>
    struct add_lvalue_reference : decltype(__add_lvalue_reference_impl<T>(0))
    {};
    template<class T>
    using add_lvalue_reference_t = typename add_lvalue_reference<T>::type;
#endif

#if __has_builtin(__add_rvalue_reference)
    template<class T>
    using add_rvalue_reference_t = __add_rvalue_reference(T);
#else
    template<class T>
    auto __add_rvalue_reference_impl(int) -> type_identity<T&&>;
    template<class T>
    auto __add_rvalue_referenc_impl(...) -> type_identity<T>;

    template<class T>
    struct add_rvalue_reference : decltype(__add_rvalue_reference_impl<T>(0))
    {};

    template<class T>
    using add_rvalue_reference_t = typename add_rvalue_reference<T>::type;
#endif

    /**
     * remove_reference
     */
    template<class T>
    struct remove_reference : type_identity<T>
    {};

    template<class T>
    struct remove_reference<T&> : type_identity<T>
    {};

    template<class T>
    struct remove_reference<T&&> : type_identity<T>
    {};
    template<class T>
    using remove_reference_t = typename remove_reference<T>::type;

    /**
     * add_pointer
     */
#if __has_builtin(__add_pointer)
    template<class T>
    using add_pointer_t = __add_pointer(T);
#else
    template<class T>
    auto __add_pointer_impl(int) -> type_identity<remove_reference_t<T>*>;
    template<class T>
    auto __add_pointer_impl(...) -> type_identity<T>;

    template<class T>
    struct add_pointer : decltype(__add_pointer_impl<T>(0))
    {};

    template<class T>
    using add_pointer_t = typename add_pointer<T>::type;
#endif
    /**
     * is_array
     */
    template<class T>
    inline constexpr bool is_array_v = __is_array(T);

    /**
     * is_function
     */
    template<typename T>
    inline constexpr bool is_function_v = __is_function(T);

    /**
     * remove_extent
     */

#if __has_builtin(__remove_extent)
    template<class T>
    using remove_extent_t = __remove_extent(T);
#else
    template<class T>
    struct remove_extent
    {
      using type = T;
    };

    template<class T>
    struct remove_extent<T[]>
    {
      using type = T;
    };

    template<class T, size_t N>
    struct remove_extent<T[N]>
    {
      using type = T;
    };

    template<class T>
    using remove_extent_t = typename remove_extent<T>::type;
#endif

    /**
     * decay
     */
#if __has_builtin(__decay)
    template<class T>
    using decay_t = __decay(T);
#else
    template<class T>
    class decay
    {
      using U = remove_reference_t<T>;

    public:
      using type = conditional_t<
        is_array_v<U>,
        add_pointer_t<remove_extent_t<U>>,
        conditional_t<is_function_v<U>, add_pointer_t<U>, remove_cv_t<U>>>;
    };

    template<class T>
    using decay_t = typename decay<T>::type;
#endif

    /**
     * is_copy_assignable
     */
    template<class T>
    constexpr bool is_copy_assignable_v = __is_assignable(
      add_lvalue_reference_t<T>, add_lvalue_reference_t<const T>);

    /**
     * is_copy_constructible
     */
    template<class T>
    inline constexpr bool is_copy_constructible_v =
      __is_constructible(T, add_lvalue_reference_t<const T>);

    /**
     * is_move_assignable
     */
    template<class T>
    constexpr bool is_move_assignable_v =
      __is_assignable(add_lvalue_reference_t<T>, add_lvalue_reference_t<T>);

    /**
     * is_move_constructible
     */
    template<class T>
    inline constexpr bool is_move_constructible_v =
      __is_constructible(T, add_rvalue_reference_t<T>);

    /**
     * is_convertible
     */
    template<class From, class To>
    inline constexpr bool is_convertible_v = __is_convertible(From, To);

    /**
     * is_base_of
     */
    template<class Base, class Derived>
    inline constexpr bool is_base_of_v = __is_base_of(Base, Derived);

    /**
     * is_trivially_copyable
     */
    template<class T>
    inline constexpr bool is_trivially_copyable_v = __is_trivially_copyable(T);

    /**
     * remove_const
     */
#if __has_builtin(__remove_const)
    template<class T>
    using remove_const_t = __remove_const(T);
#else
    template<class T>
    struct remove_const
    {
      using type = T;
    };

    template<class T>
    struct remove_const<const T>
    {
      using type = T;
    };

    template<class T>
    using remove_const_t = typename remove_const<T>::type;
#endif

    /**
     * add_const
     */
    template<class T>
    using add_const_t = const T;

#if __has_builtin(__is_lvalue_reference)
    template<class T>
    inline constexpr bool is_lvalue_reference_v = __is_lvalue_reference(T);
#else
    template<typename T>
    struct is_lvalue_reference : public false_type
    {};

    template<typename T>
    struct is_lvalue_reference<T&> : public true_type
    {};

    template<typename T>
    inline constexpr bool is_lvalue_reference_v = is_lvalue_reference<T>::value;
#endif

  } // namespace stl
} // namespace snmalloc
