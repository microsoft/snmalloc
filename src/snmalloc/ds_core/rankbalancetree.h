#pragma once

#include "snmalloc/ds_core/concept.h"
#include "snmalloc/ds_core/defines.h"
#include "snmalloc/stl/array.h"

#include <stddef.h>
#include <stdint.h>

// This file was designed for red-black trees but later migrated to support
// rank-balanced trees. We abuse the "RB" acronym to mean "rank-balanced".

namespace snmalloc
{
#ifndef SNMALLOC_DEFAULT_RBTREE_POLICY
#  define SNMALLOC_DEFAULT_RBTREE_POLICY WeakAVLPolicy
#endif

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

  namespace rankbalancetree
  {
    // Container that behaves like a C++ Ref type to enable assignment
    // to treat left, right and root uniformly.
    template<SNMALLOC_CONCEPT(RBRep) Rep>
    class ChildRef
    {
      using H = typename Rep::Handle;
      using K = typename Rep::Contents;

      H ptr;

    public:
      constexpr ChildRef() = default;

      ChildRef(H p) : ptr(p) {}

      ChildRef(const ChildRef& other) = default;

      operator K()
      {
        return Rep::get(ptr);
      }

      ChildRef& operator=(const ChildRef& other) = default;

      ChildRef& operator=(const K t)
      {
        // Use representations assigment, so we update the correct bits
        // color and other things way also be stored in the Handle.
        Rep::set(ptr, t);
        return *this;
      }

      /**
       * Comparison operators.  Note that these are nominal comparisons:
       * they compare the identities of the references rather than the values
       * referenced.
       * comparison of the values held in these child references.
       * @{
       */
      bool operator==(const ChildRef t) const
      {
        return ptr == t.ptr;
      }

      bool operator!=(const ChildRef t) const
      {
        return ptr != t.ptr;
      }

      ///@}

      bool is_null()
      {
        return Rep::get(ptr) == Rep::null;
      }

      /**
       * Return the reference in some printable format defined by the
       * representation.
       */
      auto printable()
      {
        return Rep::printable(ptr);
      }

      static ChildRef get_dir(bool direction, K k)
      {
        return {Rep::ref(direction, k)};
      }
    };

    template<SNMALLOC_CONCEPT(RBRep) Rep>
    struct RBStep
    {
      ChildRef<Rep> node;
      bool dir;

      // Default constructor needed for Array<RBStep, 128>.
      constexpr RBStep() = default;

      // Remove copy constructors to avoid accidentally copying and mutating the
      // path.
      RBStep(const RBStep& other) = delete;
      RBStep& operator=(const RBStep& other) = delete;

      /**
       * Update the step to point to a new node and direction.
       */
      void set(ChildRef<Rep> r, bool direction)
      {
        node = r;
        dir = direction;
      }

      /**
       * Update the step to point to a new node and direction.
       */
      void set(typename Rep::Handle r, bool direction)
      {
        set(ChildRef<Rep>(r), direction);
      }
    };

    // Internal representation of a path in the tree.
    // Exposed to allow for some composite operations to be defined
    // externally.
    template<SNMALLOC_CONCEPT(RBRep) Rep, bool run_checks, bool TRACE>
    struct RBPath
    {
      using ChildRef = rankbalancetree::ChildRef<Rep>;
      using RBStep = rankbalancetree::RBStep<Rep>;

      stl::Array<RBStep, 128> path;
      size_t length = 0;

      RBPath(typename Rep::Handle root)
      {
        path[0].set(root, false);
        length = 1;
      }

      ChildRef ith(size_t n)
      {
        SNMALLOC_ASSERT(length >= n);
        return path[length - n - 1].node;
      }

      bool ith_dir(size_t n)
      {
        SNMALLOC_ASSERT(length >= n);
        return path[length - n - 1].dir;
      }

      ChildRef curr()
      {
        return ith(0);
      }

      bool curr_dir()
      {
        return ith_dir(0);
      }

      ChildRef parent()
      {
        return ith(1);
      }

      bool parent_dir()
      {
        return ith_dir(1);
      }

      ChildRef grand_parent()
      {
        return ith(2);
      }

      // Extend path in `direction`.
      // If `direction` contains `Rep::null`, do not extend the path.
      // Returns false if path is not extended.
      bool move(bool direction)
      {
        auto next = ChildRef::get_dir(direction, curr());
        if (next.is_null())
          return false;
        path[length].set(next, direction);
        length++;
        return true;
      }

      // Extend path in `direction`.
      // If `direction` contains zero, do not extend the path.
      // Returns false if path is extended with null.
      bool move_inc_null(bool direction)
      {
        auto next = ChildRef::get_dir(direction, curr());
        path[length].set(next, direction);
        length++;
        return !(next.is_null());
      }

      // Remove top element from the path.
      void pop()
      {
        SNMALLOC_ASSERT(length > 0);
        length--;
      }

      // If a path is changed in place, then some references can be stale.
      // This rewalks the updated path, and corrects any internal references.
      // `expected` is used to run the update, or if `false` used to check
      // that no update is required.
      void fixup(bool expected = true)
      {
        if (!run_checks && !expected)
          return;

        // During a splice in remove the path can be invalidated,
        // this refreshs the path so that the it refers to the spliced
        // nodes fields.
        // TODO optimise usage to avoid traversing whole path.
        for (size_t i = 1; i < length; i++)
        {
          auto parent = path[i - 1].node;
          auto& curr = path[i].node;
          auto dir = path[i].dir;
          auto actual = ChildRef::get_dir(dir, parent);
          if (actual != curr)
          {
            if (!expected)
            {
              snmalloc::error("Performed an unexpected fixup.");
            }
            curr = actual;
          }
        }
      }

      void print()
      {
        if constexpr (TRACE)
        {
          for (size_t i = 0; i < length; i++)
          {
            message<1024>(
              "  -> {} @ {} ({})",
              Rep::printable(typename Rep::Contents(path[i].node)),
              path[i].node.printable(),
              path[i].dir);
          }
        }
      }
    };
  } // namespace rankbalancetree

  namespace rankbalancetree
  {
    template<SNMALLOC_CONCEPT(RBRep) Rep, bool run_checks, bool TRACE>
    struct RedBlackPolicy
    {
      using K = typename Rep::Contents;
      using H = typename Rep::Handle;
      using ChildRef = rankbalancetree::ChildRef<Rep>;
      using RBPath = rankbalancetree::RBPath<Rep, run_checks, TRACE>;

      /*
       * Verify structural invariants.  Returns the black depth of the `curr`ent
       * node.
       */
      int invariant(K curr, K lower = Rep::null, K upper = Rep::null)
      {
        if constexpr (!run_checks)
        {
          UNUSED(curr, lower, upper);
          return 0;
        }
        else
        {
          if (curr == Rep::null)
            return 1;

          if (
            ((lower != Rep::null) && Rep::compare(lower, curr)) ||
            ((upper != Rep::null) && Rep::compare(curr, upper)))
          {
            report_fatal_error(
              "Invariant failed: {} is out of bounds {}..{}",
              Rep::printable(curr),
              Rep::printable(lower),
              Rep::printable(upper));
          }

          if (
            Rep::tree_tag(curr) &&
            (Rep::tree_tag(ChildRef::get_dir(true, curr)) ||
             Rep::tree_tag(ChildRef::get_dir(false, curr))))
          {
            report_fatal_error(
              "Invariant failed: {} is red and has red child",
              Rep::printable(curr));
          }

          int left_inv = invariant(ChildRef::get_dir(true, curr), lower, curr);
          int right_inv =
            invariant(ChildRef::get_dir(false, curr), curr, upper);

          if (left_inv != right_inv)
          {
            report_fatal_error(
              "Invariant failed: {} has different black depths",
              Rep::printable(curr));
          }

          if (Rep::tree_tag(curr))
            return left_inv;

          return left_inv + 1;
        }
      }

      // Insert an element at the given path.
      template<typename DebugLogger, typename RootGetter>
      void insert_path(
        RBPath& path, K value, DebugLogger debug_log, RootGetter get_root)
      {
        SNMALLOC_ASSERT(path.curr().is_null());
        path.curr() = value;
        ChildRef::get_dir(true, path.curr()) = Rep::null;
        ChildRef::get_dir(false, path.curr()) = Rep::null;
        Rep::set_tree_tag(value, true);

        debug_log("Insert ", path);

        // Propogate double red up to rebalance.
        // These notes were particularly clear for explaining insert
        // https://www.cs.cmu.edu/~fp/courses/15122-f10/lectures/17-rbtrees.pdf
        while (path.curr() != get_root())
        {
          SNMALLOC_ASSERT(Rep::tree_tag(path.curr()));
          if (!Rep::tree_tag(path.parent()))
          {
            invariant(get_root());
            return;
          }
          bool curr_dir = path.curr_dir();
          K curr = path.curr();
          K parent = path.parent();
          K grand_parent = path.grand_parent();
          SNMALLOC_ASSERT(!Rep::tree_tag(grand_parent));
          if (path.parent_dir() == curr_dir)
          {
            debug_log("Insert - double red case 1", path, path.grand_parent());
            /* Same direction case
             * G - grand parent
             * P - parent
             * C - current
             * S - sibling
             *
             *    G                 P
             *   / \               / \
             *  A   P     -->     G   C
             *     / \           / \
             *    S   C         A   S
             */
            K sibling = ChildRef::get_dir(!curr_dir, parent);
            Rep::set_tree_tag(curr, false);
            ChildRef::get_dir(curr_dir, grand_parent) = sibling;
            ChildRef::get_dir(!curr_dir, parent) = grand_parent;
            path.grand_parent() = parent;
            debug_log(
              "Insert - double red case 1 - done", path, path.grand_parent());
          }
          else
          {
            debug_log("Insert - double red case 2", path, path.grand_parent());
            /* G - grand parent
             * P - parent
             * C - current
             * Cg - Current child for grand parent
             * Cp - Current child for parent
             *
             *    G                  C
             *   / \               /   \
             *  A   P             G     P
             *     / \    -->    / \   / \
             *    C   B         A  Cg Cp  B
             *   / \
             *  Cg  Cp
             */
            K child_g = ChildRef::get_dir(curr_dir, curr);
            K child_p = ChildRef::get_dir(!curr_dir, curr);

            Rep::set_tree_tag(parent, false);
            path.grand_parent() = curr;
            ChildRef::get_dir(curr_dir, curr) = grand_parent;
            ChildRef::get_dir(!curr_dir, curr) = parent;
            ChildRef::get_dir(curr_dir, parent) = child_p;
            ChildRef::get_dir(!curr_dir, grand_parent) = child_g;
            debug_log(
              "Insert - double red case 2 - done", path, path.grand_parent());
          }

          // Move to what replaced grand parent.
          path.pop();
          path.pop();
          invariant(path.curr());
        }
        Rep::set_tree_tag(get_root(), false);
        invariant(get_root());
      }

      template<typename DebugLogger, typename RootGetter>
      bool remove_path(RBPath& path, DebugLogger debug_log, RootGetter get_root)
      {
        ChildRef splice = path.curr();
        SNMALLOC_ASSERT(!(splice.is_null()));

        debug_log("Removing", path);

        /*
         * Find immediately smaller leaf element (rightmost descendant of left
         * child) to serve as the replacement for this node.  We may not have a
         * left subtree, so this may not move the path at all.
         */
        path.move(true);
        while (path.move(false))
        {
        }

        K curr = path.curr();

        {
          // Locally extract right-child-less replacement, replacing it with its
          // left child, if any
          K child = ChildRef::get_dir(true, path.curr());
          // Unlink target replacing with possible child.
          path.curr() = child;
        }

        bool leaf_red = Rep::tree_tag(curr);

        if (path.curr() != splice)
        {
          // If we had a left child, replace ourselves with the extracted value
          // from above
          Rep::set_tree_tag(curr, Rep::tree_tag(splice));
          ChildRef::get_dir(true, curr) = K{ChildRef::get_dir(true, splice)};
          ChildRef::get_dir(false, curr) = K{ChildRef::get_dir(false, splice)};
          splice = curr;
          path.fixup();
        }

        debug_log("Splice done", path);

        // TODO: Clear node contents?

        // Red leaf removal requires no rebalancing.
        if (leaf_red)
          return true;

        // Now in the double black case.
        // End of path is considered double black, that is, one black element
        // shorter than satisfies the invariant. The following algorithm moves
        // up the path until it finds a close red element or the root. If we
        // convert the tree to one, in which the root is double black, then the
        // algorithm is complete, as there is nothing to be out of balance with.
        // Otherwise, we are searching for nearby red elements so we can rotate
        // the tree to rebalance. The following slides nicely cover the case
        // analysis below
        //   https://www.cs.purdue.edu/homes/ayg/CS251/slides/chap13c.pdf
        while (path.curr() != get_root())
        {
          K parent = path.parent();
          bool cur_dir = path.curr_dir();
          K sibling = ChildRef::get_dir(!cur_dir, parent);

          /* Handle red sibling case.
           * This performs a rotation to give a black sibling.
           *
           *         p                           s(b)
           *        / \                         /   \
           *       c   s(r)        -->         p(r)  m
           *          /  \                    / \
           *         n    m                  c   n
           *
           * By invariant we know that p, n and m are all initially black.
           */
          if (Rep::tree_tag(sibling))
          {
            debug_log("Red sibling", path, path.parent());
            K nibling = ChildRef::get_dir(cur_dir, sibling);
            ChildRef::get_dir(!cur_dir, parent) = nibling;
            ChildRef::get_dir(cur_dir, sibling) = parent;
            Rep::set_tree_tag(parent, true);
            Rep::set_tree_tag(sibling, false);
            path.parent() = sibling;
            // Manually fix path.  Using path.fixup would alter the complexity
            // class.
            path.pop();
            path.move(cur_dir);
            path.move_inc_null(cur_dir);
            path.fixup(false);
            debug_log("Red sibling - done", path, path.parent());
            continue;
          }

          /* Handle red nibling case 1.
           *          <p>                  <s>
           *          / \                  / \
           *         c   s         -->    p   rn
           *            / \              / \
           *          on   rn           c   on
           */
          if (Rep::tree_tag(ChildRef::get_dir(!cur_dir, sibling)))
          {
            debug_log("Red nibling 1", path, path.parent());
            K r_nibling = ChildRef::get_dir(!cur_dir, sibling);
            K o_nibling = ChildRef::get_dir(cur_dir, sibling);
            ChildRef::get_dir(cur_dir, sibling) = parent;
            ChildRef::get_dir(!cur_dir, parent) = o_nibling;
            path.parent() = sibling;
            Rep::set_tree_tag(r_nibling, false);
            Rep::set_tree_tag(sibling, Rep::tree_tag(parent));
            Rep::set_tree_tag(parent, false);
            debug_log("Red nibling 1 - done", path, path.parent());
            break;
          }

          /* Handle red nibling case 2.
           *         <p>                   <rn>
           *         / \                  /    \
           *        c   s         -->    p      s
           *           / \              / \    / \
           *         rn   on           c  rno rns on
           *         / \
           *       rno  rns
           */
          if (Rep::tree_tag(ChildRef::get_dir(cur_dir, sibling)))
          {
            debug_log("Red nibling 2", path, path.parent());
            K r_nibling = ChildRef::get_dir(cur_dir, sibling);
            K r_nibling_same = ChildRef::get_dir(cur_dir, r_nibling);
            K r_nibling_opp = ChildRef::get_dir(!cur_dir, r_nibling);
            ChildRef::get_dir(!cur_dir, parent) = r_nibling_same;
            ChildRef::get_dir(cur_dir, sibling) = r_nibling_opp;
            ChildRef::get_dir(cur_dir, r_nibling) = parent;
            ChildRef::get_dir(!cur_dir, r_nibling) = sibling;
            path.parent() = r_nibling;
            Rep::set_tree_tag(r_nibling, Rep::tree_tag(parent));
            Rep::set_tree_tag(parent, false);
            debug_log("Red nibling 2 - done", path, path.parent());
            break;
          }

          // Handle black sibling and niblings, and red parent.
          if (Rep::tree_tag(parent))
          {
            debug_log("Black sibling and red parent case", path, path.parent());
            Rep::set_tree_tag(parent, false);
            Rep::set_tree_tag(sibling, true);
            debug_log(
              "Black sibling and red parent case - done", path, path.parent());
            break;
          }
          // Handle black sibling and niblings and black parent.
          debug_log(
            "Black sibling, niblings and black parent case",
            path,
            path.parent());
          Rep::set_tree_tag(sibling, true);
          path.pop();
          invariant(path.curr());
          debug_log(
            "Black sibling, niblings and black parent case - done",
            path,
            path.curr());
        }
        return true;
      }
    };

    template<SNMALLOC_CONCEPT(RBRep) Rep, bool run_checks, bool TRACE>
    struct WeakAVLPolicy
    {
      using K = typename Rep::Contents;
      using H = typename Rep::Handle;
      using ChildRef = rankbalancetree::ChildRef<Rep>;
      using RBPath = rankbalancetree::RBPath<Rep, run_checks, TRACE>;

      // Null nodes have conceptual rank -1 and therefore odd parity.
      static constexpr bool null_rank_parity = true;

      static bool rank_parity(K node)
      {
        if (node == Rep::null)
          return null_rank_parity;
        return Rep::tree_tag(node);
      }

      static void toggle_rank_parity(K node)
      {
        SNMALLOC_ASSERT(node != Rep::null);
        Rep::set_tree_tag(node, !Rep::tree_tag(node));
      }

      // if parent and child have the same parity, parent is 2-level above the
      // child
      static bool edge_is_even(K parent, bool dir)
      {
        K child = ChildRef::get_dir(dir, parent);
        return rank_parity(parent) == rank_parity(child);
      }

      // If parent and child have different parity, parent is 1-level above the
      // child
      static bool edge_is_odd(K parent, bool dir)
      {
        return !edge_is_even(parent, dir);
      }

      // A node is a leaf if both of its children are null.
      // A leaf node always has rank 1.
      static bool is_leaf(K node)
      {
        return ChildRef::get_dir(true, node).is_null() &&
          ChildRef::get_dir(false, node).is_null();
      }

      // Check if a node have both children 2-levels lower in rank.
      // These nodes can only be created during deletion.
      static bool is_22(K node)
      {
        return edge_is_even(node, true) && edge_is_even(node, false);
      }

      // Do rotation using the child at the given direction as the pivot.
      // This is the normal rotation in binary search tree: the pivot
      // transfer an opposite child to its parent then becomes the parent
      // of the old parent.
      static K rotate_subtree(ChildRef subtree, bool direction)
      {
        K root = subtree;
        K pivot = ChildRef::get_dir(direction, root);
        SNMALLOC_ASSERT(pivot != Rep::null);

        K transfer = ChildRef::get_dir(!direction, pivot);
        ChildRef::get_dir(direction, root) = transfer;
        ChildRef::get_dir(!direction, pivot) = root;
        subtree = pivot;
        return pivot;
      }

      /*
       * Verify structural invariants. Returns the rank of `curr`, using null
       * nodes at rank -1.
       */
      int invariant(K curr, K lower = Rep::null, K upper = Rep::null)
      {
        if constexpr (!run_checks)
        {
          UNUSED(curr, lower, upper);
          return 0;
        }
        else
        {
          if (curr == Rep::null)
            return -1;

          if (
            ((lower != Rep::null) && Rep::compare(lower, curr)) ||
            ((upper != Rep::null) && Rep::compare(curr, upper)))
          {
            report_fatal_error(
              "Invariant failed: {} is out of bounds {}..{}",
              Rep::printable(curr),
              Rep::printable(lower),
              Rep::printable(upper));
          }

          K left = ChildRef::get_dir(true, curr);
          K right = ChildRef::get_dir(false, curr);
          int left_rank = invariant(left, lower, curr);
          int right_rank = invariant(right, curr, upper);

          // The rank computed from either side should be the same.
          int left_from_edge = left_rank + (edge_is_odd(curr, true) ? 1 : 2);
          int right_from_edge = right_rank + (edge_is_odd(curr, false) ? 1 : 2);
          if (left_from_edge != right_from_edge)
          {
            report_fatal_error(
              "Invariant failed: {} computes different ranks from each side",
              Rep::printable(curr));
          }

          if (is_leaf(curr) && (left_from_edge != 0))
          {
            report_fatal_error(
              "Invariant failed: leaf {} has rank {} (expected 0)",
              Rep::printable(curr),
              left_from_edge);
          }

          if ((left_from_edge & 1) != static_cast<int>(Rep::tree_tag(curr)))
          {
            report_fatal_error(
              "Invariant failed: {} parity bit disagrees with computed rank {}",
              Rep::printable(curr),
              left_from_edge);
          }

          return left_from_edge;
        }
      }

      template<typename DebugLogger, typename RootGetter>
      void insert_path(
        RBPath& path, K value, DebugLogger debug_log, RootGetter get_root)
      {
        // Insert an external node (rank 0, even parity). Null children are
        // conceptual rank -1.
        SNMALLOC_ASSERT(path.curr().is_null());
        path.curr() = value;
        // Create a leaf node.
        ChildRef::get_dir(true, path.curr()) = Rep::null;
        ChildRef::get_dir(false, path.curr()) = Rep::null;
        Rep::set_tree_tag(value, false);

        debug_log("Insert", path);

        while (path.curr() != get_root())
        {
          K node = path.curr();
          K parent = path.parent();
          bool node_dir = path.curr_dir();

          // If parent and inserted node have opposite parity, this edge is
          // rank-diff 1 and insertion is already valid as parent does not
          // need to be promoted.
          //   (P)              (P)
          //  ╱ ╲       =>      ╱ ╲
          // *  (S)           (N) (S)
          if (edge_is_odd(parent, node_dir))
            break;

          bool sibling_dir = !node_dir;
          if (edge_is_odd(parent, sibling_dir))
          {
            /*
             * Case 1: parent has two 1-children before insertion and now has
             * a 0-child to the inserted node. Promote parent and continue.
             *
             *          (GP)                    (GP)
             *           │ x                     │ x-1
             *           │                     (P)
             *     0     │               1    ╱   ╲
             *   (N) ─── (P)       =>       (N)   (S)
             *                ╲ 1
             *                (S)
             */
            debug_log("Insert - promote parent", path, path.parent());
            toggle_rank_parity(parent);
            path.pop();
            continue;
          }

          if (edge_is_even(node, !node_dir))
          {
            /*
             * Case 2: sibling edge from node is 2-level lower in rank. And node
             * has a 1-node along the same direction. Rotate parent once.
             *
             *                 (GP)                          (GP)
             *             0    │ x                        x  │
             *         (N) ─── (P)          =>               (N)
             *      1   ╱   ╲ 2   ╲ 2                       ╱   ╲    1
             *      (C1)   (C2)    ╲                   (C1)     (P)
             *                      (S)                      1 ╱   ╲  1
             *                                                (C2) (S)
             */
            debug_log("Insert - single rotation", path, path.parent());
            rotate_subtree(path.parent(), node_dir);
            // Parent is demoted by one rank while node is at the same rank.
            toggle_rank_parity(parent);
          }
          else
          {
            /*
             * Case 3: sibling edge from node is 2-level lower in rank. And node
             * has a 1-node along the opposite direction. Do zig-zag rotation.
             *
             *                 (GP)                             (GP)
             *             0    │ x                               │ x
             *         (N) ─── (P)            =>                (C1)
             *      2   ╱   ╲ 1   ╲                          1 ╱    ╲
             *         ╱   (C1)    ╲ 2                        (N)    (P)
             *      (C2)   ╱   ╲    ╲                       1 ╱ ╲   ╱ ╲
             *           (A)  (B)    (S)                    (C2)(A)(B)(S)
             *
             */
            debug_log("Insert - double rotation", path, path.parent());
            K middle = ChildRef::get_dir(!node_dir, node);

            rotate_subtree(path.curr(), !node_dir);
            rotate_subtree(path.parent(), node_dir);
            // Middle is promoted, parent and node are demoted.
            toggle_rank_parity(middle);
            toggle_rank_parity(parent);
            toggle_rank_parity(node);
          }

          invariant(get_root());
          return;
        }

        invariant(get_root());
      }

      template<typename DebugLogger, typename RootGetter>
      bool remove_path(RBPath& path, DebugLogger debug_log, RootGetter get_root)
      {
        ChildRef splice = path.curr();
        SNMALLOC_ASSERT(!(splice.is_null()));

        debug_log("Removing", path);

        // Extract predecessor (rightmost descendant in left subtree), if any.
        path.move(true);
        while (path.move(false))
        {
        }

        K curr = path.curr();

        {
          // Extract predecessor node from its current location.
          K child = ChildRef::get_dir(true, path.curr());
          path.curr() = child;
        }

        if (path.curr() != splice)
        {
          // Move extracted predecessor into the splice location.
          Rep::set_tree_tag(curr, Rep::tree_tag(splice));
          ChildRef::get_dir(true, curr) = K{ChildRef::get_dir(true, splice)};
          ChildRef::get_dir(false, curr) = K{ChildRef::get_dir(false, splice)};
          splice = curr;
          path.fixup();
        }

        debug_log("Splice done", path);

        while (path.curr() != get_root())
        {
          K cursor = path.parent();
          bool dir = path.curr_dir();
          bool sibling_dir = !dir;

          /*
           * Case 0: deleted-side edge changes 1 -> 2. This can stop immediately
           * unless we created a 2-2 leaf, which must be demoted and bubbled up.
           *
           *         (C)                      (C)
           *      X ╱   ╲ 1               X ╱   ╲
           *      (*)   (D)        =>      (*)   ╲ 2
           *                                     (D)
           */
          if (edge_is_even(cursor, dir))
          {
            if (!(is_leaf(cursor) && is_22(cursor)))
            {
              invariant(get_root());
              return true;
            }

            // 2-2 leaf must be demoted and we continue upwards.
            toggle_rank_parity(cursor);
            path.pop();
            continue;
          }

          K sibling = ChildRef::get_dir(sibling_dir, cursor);
          SNMALLOC_ASSERT(sibling != Rep::null);

          /*
           * Case 1: deleted-side edge is now 3-level lower and sibling edge is
           * 2-level lower. Demote cursor and continue upward.
           *
           *           (P)                    (P)
           *            │ X                    │ X+1
           *           (C)                     │
           *        2 ╱   ╲ 3        =>       (C)
           *         ╱     ╲               1 ╱   ╲ 2
           *       (*)      ╲              (*)    ╲
           *                 (D)                   (D)
           */
          if (edge_is_even(cursor, sibling_dir))
          {
            toggle_rank_parity(cursor);
            path.pop();
            continue;
          }

          /*
           * Case 2: sibling is 1-level lower and it is a 2-2 node. Demote
           * sibling and cursor, then continue upward.
           *
           *           (P)                    (P)
           *            │ X                    │ X+1
           *           (C)                     │
           *        1 ╱   ╲ 3        =>       (C)
           *         (S)   ╲               1 ╱   ╲ 2
           *       2╱  ╲2   ╲              (S)    ╲
           *       ╱    ╲    (D)         1 ╱  ╲ 1  (D)
           *     (*)    (*)              (*)  (*)
           */
          if (is_22(sibling))
          {
            toggle_rank_parity(sibling);
            toggle_rank_parity(cursor);
            path.pop();
            continue;
          }

          /*
           * Case 3: sibling cannot be demoted since it has a 1-edge. if sibling
           * has a 1-child on the same side. Single rotation at cursor.
           *
           *           (P)                              (P)
           *            │ X                              │ X
           *           (C)                              (S)
           *        1 ╱   ╲ 3             =>         2 ╱   ╲ 1
           *         (S)   ╲                          ╱    (C)
           *      1 ╱  ╲ Y  ╲                      (T)   Y ╱  ╲ 2
           *       (T)  ╲    (D)                          ╱    ╲
           *             (*)                            (*)    (D)
           */
          if (edge_is_odd(sibling, sibling_dir))
          {
            bool inner_is_2 = edge_is_even(sibling, !sibling_dir);

            rotate_subtree(path.parent(), sibling_dir);
            // sibling is promoted, cursor demoted.
            toggle_rank_parity(sibling);
            toggle_rank_parity(cursor);

            // Special leaf case requires one extra demotion of cursor.
            if (inner_is_2 && is_leaf(cursor))
              toggle_rank_parity(cursor);

            invariant(get_root());
            return true;
          }

          /*
           * Case 4: the 1-child is on the opposite side of the sibling.
           *
           *              (P)                                  (P)
           *               │ X                                  │ X
           *              (C)                                  (T)
           *           1 ╱   ╲ 3               =>           2 ╱   ╲ 2
           *            (S)   ╲                              ╱     ╲
           *         2 ╱  ╲ 1  ╲                            (S)     (C)
           *          ╱   (T)   (D)                       1 ╱ ╲     ╱ ╲ 1
           *       (*) y ╱   ╲ z                          (*) (A) (B) (D)
           *            (A) (B)
           */
          auto sibling_ref = ChildRef::get_dir(sibling_dir, cursor);
          rotate_subtree(sibling_ref, !sibling_dir);
          rotate_subtree(path.parent(), sibling_dir);

          // Sibling is demoted by one. Cursor and sibling's 1-child's rank
          // changes are two, so no further toggle is needed.
          toggle_rank_parity(sibling);

          invariant(get_root());
          return true;
        }

        invariant(get_root());
        return true;
      }
    };
  } // namespace rankbalancetree

  /**
   * Contains a self balancing binary tree.
   *
   * The template parameter Rep provides the representation of the nodes as a
   * collection of functions and types that are requires.  See the associated
   * test for an example.
   *
   * run_checks enables invariant checking on the tree. Enabled in Debug.
   * TRACE prints all the sets of the rebalancing operations. Only enabled by
   * the test when debugging a specific failure.
   */
  template<
    SNMALLOC_CONCEPT(RBRep) Rep,
    bool run_checks = Debug,
    bool TRACE = false,
    template<typename, bool, bool> class Policy =
      rankbalancetree::SNMALLOC_DEFAULT_RBTREE_POLICY>
  class RBTree : public Policy<Rep, run_checks, TRACE>
  {
    using H = typename Rep::Handle;
    using K = typename Rep::Contents;
    using ChildRef = rankbalancetree::ChildRef<Rep>;
    using RBStep = rankbalancetree::RBStep<Rep>;
    using Base = Policy<Rep, run_checks, TRACE>;

    // Root field of the tree
    typename stl::remove_const_t<stl::remove_reference_t<decltype(Rep::root)>>
      root{Rep::root};

    ChildRef get_root()
    {
      return {H{&root}};
    }

    void invariant()
    {
      Base::invariant(get_root());
    }

  public:
    using RBPath = rankbalancetree::RBPath<Rep, run_checks, TRACE>;

  private:
    struct DebugLogger
    {
      RBTree* context;

      void operator()(const char* msg, RBPath& path)
      {
        this->operator()(msg, path, context->get_root());
      }

      void operator()(const char* msg, RBPath& path, ChildRef base)
      {
        if constexpr (TRACE)
        {
          message<100>("------- {}", Rep::name());
          message<1024>(msg);
          path.print();
          context->print(base);
        }
        else
        {
          UNUSED(msg, path, base);
        }
      }
    };

  public:
    constexpr RBTree() = default;

    void print()
    {
      print(get_root());
    }

    void print(ChildRef curr, const char* indent = "", size_t depth = 0)
    {
      if constexpr (TRACE)
      {
        if (curr.is_null())
        {
          message<1024>("{}\\_null", indent);
          return;
        }

#ifdef _MSC_VER
        auto colour = Rep::tree_tag(curr) ? "R-" : "B-";
        auto reset = "";
#else
        auto colour = Rep::tree_tag(curr) ? "\e[1;31m" : "\e[1;34m";
        auto reset = "\e[0m";
#endif

        message<1024>(
          "{}\\_{}{}{}@{} ({})",
          indent,
          colour,
          Rep::printable((K(curr))),
          reset,
          curr.printable(),
          depth);
        if (!(ChildRef::get_dir(true, curr).is_null() &&
              ChildRef::get_dir(false, curr).is_null()))
        {
          // As the tree should be balanced, the depth should not exceed 128 if
          // there are 2^64 elements in the tree. This is a debug feature, and
          // it would be impossible to debug something of this size, so this is
          // considerably larger than required.
          // If there is a bug that leads to an unbalanced tree, this might be
          // insufficient to accurately display the tree, but it will still be
          // memory safe as the search code is bounded by the string size.
          static constexpr size_t max_depth = 128;
          char s_indent[max_depth];
          size_t end = 0;
          for (; end < max_depth - 1; end++)
          {
            if (indent[end] == 0)
              break;
            s_indent[end] = indent[end];
          }
          s_indent[end] = '|';
          s_indent[end + 1] = 0;
          print(ChildRef::get_dir(true, curr), s_indent, depth + 1);
          s_indent[end] = ' ';
          print(ChildRef::get_dir(false, curr), s_indent, depth + 1);
        }
      }
    }

    bool find(RBPath& path, K value)
    {
      bool dir;

      if (path.curr().is_null())
        return false;

      do
      {
        if (Rep::equal(path.curr(), value))
          return true;
        dir = Rep::compare(path.curr(), value);
      } while (path.move_inc_null(dir));

      return false;
    }

    bool is_empty()
    {
      return get_root().is_null();
    }

    K remove_min()
    {
      if (is_empty())
        return Rep::null;

      auto path = get_root_path();
      while (path.move(true))
      {
      }

      K result = path.curr();

      remove_path(path);
      return result;
    }

    bool remove_elem(K value)
    {
      if (is_empty())
        return false;

      auto path = get_root_path();
      if (!find(path, value))
        return false;

      remove_path(path);
      return true;
    }

    bool insert_elem(K value)
    {
      auto path = get_root_path();

      if (find(path, value))
        return false;

      Base::insert_path(
        path, value, DebugLogger{this}, [this]() { return get_root(); });
      return true;
    }

    RBPath get_root_path()
    {
      return RBPath(H{&root});
    }

    void insert_path(RBPath& path, K value)
    {
      Base::insert_path(
        path, value, DebugLogger{this}, [this]() { return get_root(); });
    }

    bool remove_path(RBPath& path)
    {
      return Base::remove_path(
        path, DebugLogger{this}, [this]() { return get_root(); });
    }
  };
} // namespace snmalloc
