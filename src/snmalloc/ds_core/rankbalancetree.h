#pragma once

#include "snmalloc/stl/array.h"

#include <stddef.h>
#include <stdint.h>

namespace snmalloc
{
#ifdef __cpp_concepts
  /**
   * The representation must define two types.  `Contents` defines some
   * identifier that can be mapped to a node as a value type.  `Handle` defines
   * a reference to the storage, which can be used to update it.
   *
   * Conceptually, `Contents` is a node ID and `Handle` is a pointer to a node
   * ID.
   */
  template<typename Rep>
  concept RBRepTypes = requires() {
    typename Rep::Handle;
    typename Rep::Contents;
  };

  /**
   * The representation must define operations on the holder and contents
   * types.  It must be able to 'dereference' a holder with `get`, assign to it
   * with `set`, set and query the red/black colour of a node with
   * `set_tree_tag` and `tree_tag`.
   *
   * The `ref` method provides uniform access to the children of a node,
   * returning a holder pointing to either the left or right child, depending on
   * the direction parameter.
   *
   * The backend must also provide two constant values.
   * `Rep::null` defines a value that, if returned from `get`, indicates a null
   * value. `Rep::root` defines a value that, if constructed directly, indicates
   * a null value and can therefore be used as the initial raw bit pattern of
   * the root node.
   */
  template<typename Rep>
  concept RBRepMethods =
    requires(typename Rep::Handle hp, typename Rep::Contents k, bool b) {
      { Rep::get(hp) } -> ConceptSame<typename Rep::Contents>;
      { Rep::set(hp, k) } -> ConceptSame<void>;
      { Rep::tree_tag(k) } -> ConceptSame<bool>;
      { Rep::set_tree_tag(k, b) } -> ConceptSame<void>;
      { Rep::ref(b, k) } -> ConceptSame<typename Rep::Handle>;
      { Rep::null } -> ConceptSameModRef<const typename Rep::Contents>;
      {
        typename Rep::Handle{const_cast<
          stl::remove_const_t<stl::remove_reference_t<decltype(Rep::root)>>*>(
          &Rep::root)}
      } -> ConceptSame<typename Rep::Handle>;
    };

  template<typename Rep>
  concept RBRep = //
    RBRepTypes<Rep> //
    && RBRepMethods<Rep> //
    &&
    ConceptSame<decltype(Rep::null), stl::add_const_t<typename Rep::Contents>>;
#endif
} // namespace snmalloc

#include "redblacktree.h"
#include "weakavltree.h"

namespace snmalloc
{
  template<typename Rep, bool run_checks = Debug, bool TRACE = false>
  using DefaultRBTree = WeakAVLTree<Rep, run_checks, TRACE>;
}