#pragma once
#include "../pal/pal.h"
#include "concept.h"
#include "defines.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace snmalloc
{
#ifdef __cpp_concepts
  template<typename Rep>
  concept RBRepTypes = requires()
  {
    typename Rep::Holder;
    typename Rep::Contents;
  };

  template<typename Rep>
  concept RBRepMethods =
    requires(typename Rep::Holder* hp, typename Rep::Contents k, bool b)
  {
    {
      Rep::get(hp)
    }
    ->ConceptSame<typename Rep::Contents>;
    {
      Rep::set(hp, k)
    }
    ->ConceptSame<void>;
    {
      Rep::is_red(k)
    }
    ->ConceptSame<bool>;
    {
      Rep::set_red(k, b)
    }
    ->ConceptSame<void>;
    {
      Rep::ref(b, k)
    }
    ->ConceptSame<typename Rep::Holder&>;
  };

  template<typename Rep>
  concept RBRep = //
    RBRepTypes<Rep> //
      && RBRepMethods<Rep> //
        && ConceptSame<
          decltype(Rep::null),
          std::add_const_t<typename Rep::Contents>>;
#endif

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
    bool run_checks = DEBUG,
    bool TRACE = false>
  class RBTree
  {
    using H = typename Rep::Holder;
    using K = typename Rep::Contents;

    // Container that behaves like a C++ Ref type to enable assignment
    // to treat left, right and root uniformly.
    class ChildRef
    {
      H* ptr;

    public:
      ChildRef() : ptr(nullptr) {}

      ChildRef(H& p) : ptr(&p) {}

      operator K()
      {
        return Rep::get(ptr);
      }

      ChildRef& operator=(const K& t)
      {
        // Use representations assigment, so we update the correct bits
        // color and other things way also be stored in the Holder.
        Rep::set(ptr, t);
        return *this;
      }

      bool operator==(const ChildRef t) const
      {
        return ptr == t.ptr;
      }

      bool operator!=(const ChildRef t) const
      {
        return ptr != t.ptr;
      }

      H* addr()
      {
        return ptr;
      }

      bool is_null()
      {
        return Rep::get(ptr) == Rep::null;
      }
    };

    // Root field of the tree
    H root{};

    static ChildRef get_dir(bool direction, K k)
    {
      return {Rep::ref(direction, k)};
    }

    ChildRef get_root()
    {
      return {root};
    }

    void invariant()
    {
      invariant(get_root());
    }

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
          Rep::is_red(curr) &&
          (Rep::is_red(get_dir(true, curr)) ||
           Rep::is_red(get_dir(false, curr))))
        {
          report_fatal_error(
            "Invariant failed: {} is red and has red child",
            Rep::printable(curr));
        }

        int left_inv = invariant(get_dir(true, curr), lower, curr);
        int right_inv = invariant(get_dir(false, curr), curr, upper);

        if (left_inv != right_inv)
        {
          report_fatal_error(
            "Invariant failed: {} has different black depths",
            Rep::printable(curr));
        }

        if (Rep::is_red(curr))
          return left_inv;

        return left_inv + 1;
      }
    }

    struct RBStep
    {
      ChildRef node;
      bool dir = false;
    };

  public:
    // Internal representation of a path in the tree.
    // Exposed to allow for some composite operations to be defined
    // externally.
    class RBPath
    {
      friend class RBTree;

      std::array<RBStep, 128> path;
      size_t length = 0;

      RBPath(typename Rep::Holder& root) : path{}
      {
        path[0] = {root, false};
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
        auto next = get_dir(direction, curr());
        if (next.is_null())
          return false;
        path[length] = {next, direction};
        length++;
        return true;
      }

      // Extend path in `direction`.
      // If `direction` contains zero, do not extend the path.
      // Returns false if path is extended with null.
      bool move_inc_null(bool direction)
      {
        auto next = get_dir(direction, curr());
        path[length] = {next, direction};
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
          auto actual = get_dir(dir, parent);
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
              K(path[i].node),
              path[i].node.addr(),
              path[i].dir);
          }
        }
      }
    };

  private:
    void debug_log(const char* msg, RBPath& path)
    {
      debug_log(msg, path, get_root());
    }

    void debug_log(const char* msg, RBPath& path, ChildRef base)
    {
      if constexpr (TRACE)
      {
        message<100>("-------");
        message<1024>(msg);
        path.print();
        print(base);
      }
      else
      {
        UNUSED(msg, path, base);
      }
    }

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
        auto colour = Rep::is_red(curr) ? "R-" : "B-";
        auto reset = "";
#else
        auto colour = Rep::is_red(curr) ? "\e[1;31m" : "\e[1;34m";
        auto reset = "\e[0m";
#endif

        message<1024>(
          "{}\\_{}{}{}@{} ({})",
          indent,
          colour,
          curr,
          reset,
          curr.addr(),
          depth);
        if ((get_dir(true, curr) != 0) || (get_dir(false, curr) != 0))
        {
          auto s_indent = std::string(indent);
          print(get_dir(true, curr), (s_indent + "|").c_str(), depth + 1);
          print(get_dir(false, curr), (s_indent + " ").c_str(), depth + 1);
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

    bool remove_path(RBPath& path)
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
        K child = get_dir(true, path.curr());
        // Unlink target replacing with possible child.
        path.curr() = child;
      }

      bool leaf_red = Rep::is_red(curr);

      if (path.curr() != splice)
      {
        // If we had a left child, replace ourselves with the extracted value
        // from above
        Rep::set_red(curr, Rep::is_red(splice));
        get_dir(true, curr) = K(get_dir(true, splice));
        get_dir(false, curr) = K(get_dir(false, splice));
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
      // shorter than satisfies the invariant. The following algorithm moves up
      // the path until it finds a close red element or the root. If we convert
      // the tree to one, in which the root is double black, then the algorithm
      // is complete, as there is nothing to be out of balance with. Otherwise,
      // we are searching for nearby red elements so we can rotate the tree to
      // rebalance. The following slides nicely cover the case analysis below
      //   https://www.cs.purdue.edu/homes/ayg/CS251/slides/chap13c.pdf
      while (path.curr() != ChildRef(root))
      {
        K parent = path.parent();
        bool cur_dir = path.curr_dir();
        K sibling = get_dir(!cur_dir, parent);

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
        if (Rep::is_red(sibling))
        {
          debug_log("Red sibling", path, path.parent());
          K nibling = get_dir(cur_dir, sibling);
          get_dir(!cur_dir, parent) = nibling;
          get_dir(cur_dir, sibling) = parent;
          Rep::set_red(parent, true);
          Rep::set_red(sibling, false);
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
        if (Rep::is_red(get_dir(!cur_dir, sibling)))
        {
          debug_log("Red nibling 1", path, path.parent());
          K r_nibling = get_dir(!cur_dir, sibling);
          K o_nibling = get_dir(cur_dir, sibling);
          get_dir(cur_dir, sibling) = parent;
          get_dir(!cur_dir, parent) = o_nibling;
          path.parent() = sibling;
          Rep::set_red(r_nibling, false);
          Rep::set_red(sibling, Rep::is_red(parent));
          Rep::set_red(parent, false);
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
        if (Rep::is_red(get_dir(cur_dir, sibling)))
        {
          debug_log("Red nibling 2", path, path.parent());
          K r_nibling = get_dir(cur_dir, sibling);
          K r_nibling_same = get_dir(cur_dir, r_nibling);
          K r_nibling_opp = get_dir(!cur_dir, r_nibling);
          get_dir(!cur_dir, parent) = r_nibling_same;
          get_dir(cur_dir, sibling) = r_nibling_opp;
          get_dir(cur_dir, r_nibling) = parent;
          get_dir(!cur_dir, r_nibling) = sibling;
          path.parent() = r_nibling;
          Rep::set_red(r_nibling, Rep::is_red(parent));
          Rep::set_red(parent, false);
          debug_log("Red nibling 2 - done", path, path.parent());
          break;
        }

        // Handle black sibling and niblings, and red parent.
        if (Rep::is_red(parent))
        {
          debug_log("Black sibling and red parent case", path, path.parent());
          Rep::set_red(parent, false);
          Rep::set_red(sibling, true);
          debug_log(
            "Black sibling and red parent case - done", path, path.parent());
          break;
        }
        // Handle black sibling and niblings and black parent.
        debug_log(
          "Black sibling, niblings and black parent case", path, path.parent());
        Rep::set_red(sibling, true);
        path.pop();
        invariant(path.curr());
        debug_log(
          "Black sibling, niblings and black parent case - done",
          path,
          path.curr());
      }
      return true;
    }

    // Insert an element at the given path.
    void insert_path(RBPath path, K value)
    {
      SNMALLOC_ASSERT(path.curr().is_null());
      path.curr() = value;
      get_dir(true, path.curr()) = Rep::null;
      get_dir(false, path.curr()) = Rep::null;
      Rep::set_red(value, true);

      debug_log("Insert ", path);

      // Propogate double red up to rebalance.
      // These notes were particularly clear for explaining insert
      // https://www.cs.cmu.edu/~fp/courses/15122-f10/lectures/17-rbtrees.pdf
      while (path.curr() != get_root())
      {
        SNMALLOC_ASSERT(Rep::is_red(path.curr()));
        if (!Rep::is_red(path.parent()))
        {
          invariant();
          return;
        }
        bool curr_dir = path.curr_dir();
        K curr = path.curr();
        K parent = path.parent();
        K grand_parent = path.grand_parent();
        SNMALLOC_ASSERT(!Rep::is_red(grand_parent));
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
          K sibling = get_dir(!curr_dir, parent);
          Rep::set_red(curr, false);
          get_dir(curr_dir, grand_parent) = sibling;
          get_dir(!curr_dir, parent) = grand_parent;
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
          K child_g = get_dir(curr_dir, curr);
          K child_p = get_dir(!curr_dir, curr);

          Rep::set_red(parent, false);
          path.grand_parent() = curr;
          get_dir(curr_dir, curr) = grand_parent;
          get_dir(!curr_dir, curr) = parent;
          get_dir(curr_dir, parent) = child_p;
          get_dir(!curr_dir, grand_parent) = child_g;
          debug_log(
            "Insert - double red case 2 - done", path, path.grand_parent());
        }

        // Move to what replaced grand parent.
        path.pop();
        path.pop();
        invariant(path.curr());
      }
      Rep::set_red(get_root(), false);
      invariant();
    }

    K remove_min()
    {
      if (get_root().is_null())
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
      if (get_root().is_null())
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

      insert_path(path, value);
      return true;
    }

    RBPath get_root_path()
    {
      return RBPath(root);
    }
  };
} // namespace snmalloc
