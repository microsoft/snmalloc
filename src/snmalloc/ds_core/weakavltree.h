#pragma once

#include "snmalloc/stl/array.h"

#include <stddef.h>
#include <stdint.h>

namespace snmalloc
{
  /**
   * Weak AVL tree implementation using 1-bit rank parity per node.
   *
   * The representation must provide:
   * - types `Handle`, `Contents`
   * - `get(Handle)`, `set(Handle, Contents)`
   * - `ref(bool is_left, Contents)`
   * - `tree_tag(Contents)`, `set_tree_tag(Contents, uint8_t)` (bit0 = parity)
   * - `compare(Contents, Contents)`, `equal(Contents, Contents)`
   * - constants `null` and `root`
   */
  template<typename Rep, bool run_checks = Debug, bool TRACE = false>
  class WeakAVLTree
  {
    using H = typename Rep::Handle;
    using K = typename Rep::Contents;
    using RootStorage =
      stl::remove_const_t<stl::remove_reference_t<decltype(Rep::root)>>;

    RootStorage root{Rep::root};

    static constexpr bool Left = false;
    static constexpr bool Right = true;
    static constexpr uint8_t ParityMask = 0b1;
    static constexpr bool use_checks = run_checks;

    class RBPath
    {
      friend class WeakAVLTree;

      K parent{Rep::null};
      K curr{Rep::null};
      bool dir{Left};
    };

    H root_ref()
    {
      return H{&root};
    }

    K get_root() const
    {
      return Rep::get(H{const_cast<RootStorage*>(&root)});
    }

    void set_root(K n)
    {
      set(root_ref(), n);
    }

    static K get(H p)
    {
      return Rep::get(p);
    }

    static void set(H p, K v)
    {
      Rep::set(p, v);
    }

    static H child_ref(K n, bool dir)
    {
      // Rep uses true for left, false for right.
      return Rep::ref(!dir, n);
    }

    static K child(K n, bool dir)
    {
      return get(child_ref(n, dir));
    }

    static void set_child(K n, bool dir, K v)
    {
      set(child_ref(n, dir), v);
    }

    K parent(K n) const
    {
      if (is_null(n))
        return Rep::null;

      K p = Rep::null;
      K cur = get_root();
      while (!is_null(cur) && !Rep::equal(cur, n))
      {
        p = cur;
        bool dir = Rep::compare(cur, n) ? Left : Right;
        cur = child(cur, dir);
      }
      return is_null(cur) ? Rep::null : p;
    }

    static void set_parent(K n, K p)
    {
      UNUSED(n, p);
    }

    static bool is_null(K n)
    {
      return Rep::equal(n, Rep::null);
    }

    static bool parity(K n)
    {
      if (is_null(n))
        return true;
      return (Rep::tree_tag(n) & ParityMask) != 0;
    }

    static void set_parity(K n, bool p)
    {
      if (!is_null(n))
        Rep::set_tree_tag(n, p ? 1 : 0);
    }

    static void toggle_parity(K n)
    {
      if (!is_null(n))
        Rep::set_tree_tag(n, static_cast<uint8_t>(Rep::tree_tag(n) ^ 1));
    }

    static void promote(K n)
    {
      toggle_parity(n);
    }

    static void demote(K n)
    {
      toggle_parity(n);
    }

    static void double_promote(K n)
    {
      UNUSED(n);
    }

    static void double_demote(K n)
    {
      UNUSED(n);
    }

    static bool is_leaf(K n)
    {
      return !is_null(n) && is_null(child(n, Left)) && is_null(child(n, Right));
    }

    static bool is_right_child(K p, K n)
    {
      return Rep::equal(child(p, Right), n);
    }

    static bool is_2_child(K n, K p)
    {
      return parity(n) == parity(p);
    }

    K sibling(K n) const
    {
      K p = parent(n);
      if (is_null(p))
        return Rep::null;
      return Rep::equal(child(p, Left), n) ? child(p, Right) : child(p, Left);
    }

    void rotate_right_at(K x)
    {
      K z = parent(x);
      K y = child(x, Right);
      K p_z = parent(z);

      set_parent(x, p_z);
      if (!is_null(p_z))
      {
        if (Rep::equal(child(p_z, Left), z))
          set_child(p_z, Left, x);
        else
          set_child(p_z, Right, x);
      }
      else
      {
        set_root(x);
      }

      set_child(x, Right, z);
      set_parent(z, x);

      set_child(z, Left, y);
      if (!is_null(y))
        set_parent(y, z);
    }

    void rotate_left_at(K x)
    {
      K z = parent(x);
      K y = child(x, Left);
      K p_z = parent(z);

      set_parent(x, p_z);
      if (!is_null(p_z))
      {
        if (Rep::equal(child(p_z, Left), z))
          set_child(p_z, Left, x);
        else
          set_child(p_z, Right, x);
      }
      else
      {
        set_root(x);
      }

      set_child(x, Left, z);
      set_parent(z, x);

      set_child(z, Right, y);
      if (!is_null(y))
        set_parent(y, z);
    }

    void double_rotate_right_at(K y)
    {
      K x = parent(y);
      K z = parent(x);
      K p_z = parent(z);

      set_parent(y, p_z);
      if (!is_null(p_z))
      {
        if (Rep::equal(child(p_z, Left), z))
          set_child(p_z, Left, y);
        else
          set_child(p_z, Right, y);
      }
      else
      {
        set_root(y);
      }

      set_child(x, Right, child(y, Left));
      if (!is_null(child(y, Left)))
        set_parent(child(y, Left), x);

      set_child(y, Left, x);
      set_parent(x, y);

      set_child(z, Left, child(y, Right));
      if (!is_null(child(y, Right)))
        set_parent(child(y, Right), z);

      set_child(y, Right, z);
      set_parent(z, y);
    }

    void double_rotate_left_at(K y)
    {
      K x = parent(y);
      K z = parent(x);
      K p_z = parent(z);

      set_parent(y, p_z);
      if (!is_null(p_z))
      {
        if (Rep::equal(child(p_z, Left), z))
          set_child(p_z, Left, y);
        else
          set_child(p_z, Right, y);
      }
      else
      {
        set_root(y);
      }

      set_child(z, Right, child(y, Left));
      if (!is_null(child(y, Left)))
        set_parent(child(y, Left), z);

      set_child(y, Left, z);
      set_parent(z, y);

      set_child(x, Left, child(y, Right));
      if (!is_null(child(y, Right)))
        set_parent(child(y, Right), x);

      set_child(y, Right, x);
      set_parent(x, y);
    }

    void insert_rebalance(K at)
    {
      K x = at;
      K p_x = parent(x);
      bool par_x = false;
      bool par_p_x = false;
      bool par_s_x = false;

      do
      {
        promote(p_x);
        x = p_x;
        p_x = parent(x);

        if (is_null(p_x))
          return;

        par_x = parity(x);
        par_p_x = parity(p_x);
        par_s_x = parity(sibling(x));
      } while (
        (!par_x && !par_p_x && par_s_x) || (par_x && par_p_x && !par_s_x));

      if (!((par_x && par_p_x && par_s_x) || (!par_x && !par_p_x && !par_s_x)))
        return;

      K z = parent(x);
      if (Rep::equal(x, child(p_x, Left)))
      {
        K y = child(x, Right);
        if (is_null(y) || parity(y) == par_x)
        {
          rotate_right_at(x);
          if (!is_null(z))
            demote(z);
        }
        else
        {
          double_rotate_right_at(y);
          promote(y);
          demote(x);
          if (!is_null(z))
            demote(z);
        }
      }
      else
      {
        K y = child(x, Left);
        if (is_null(y) || parity(y) == par_x)
        {
          rotate_left_at(x);
          if (!is_null(z))
            demote(z);
        }
        else
        {
          double_rotate_left_at(y);
          promote(y);
          demote(x);
          if (!is_null(z))
            demote(z);
        }
      }
    }

    static K minimum_at(K n)
    {
      K cur = n;
      while (!is_null(child(cur, Left)))
        cur = child(cur, Left);
      return cur;
    }

    void swap_in_node_at(K old_node, K new_node)
    {
      K l = child(old_node, Left);
      K r = child(old_node, Right);
      K p = parent(old_node);

      set_parent(new_node, p);
      if (!is_null(p))
      {
        if (Rep::equal(child(p, Left), old_node))
          set_child(p, Left, new_node);
        else
          set_child(p, Right, new_node);
      }
      else
      {
        set_root(new_node);
      }

      set_child(new_node, Right, r);
      if (!is_null(r))
        set_parent(r, new_node);

      set_child(new_node, Left, l);
      if (!is_null(l))
        set_parent(l, new_node);

      set_parity(new_node, parity(old_node));

      set_child(old_node, Left, Rep::null);
      set_child(old_node, Right, Rep::null);
      set_parent(old_node, Rep::null);
    }

    void delete_rebalance_3_child(K n, K p_n)
    {
      K x = n;
      K p_x = p_n;
      if (is_null(p_x))
        return;
      K y = Rep::null;
      bool creates_3_node = false;
      bool done = true;

      do
      {
        K p_p_x = parent(p_x);
        y = Rep::equal(child(p_x, Left), x) ? child(p_x, Right) : child(p_x, Left);

        creates_3_node = !is_null(p_p_x) && (parity(p_x) == parity(p_p_x));

        if (is_2_child(y, p_x))
        {
          demote(p_x);
        }
        else
        {
          bool y_parity = parity(y);
          if (
            y_parity == parity(child(y, Left)) &&
            y_parity == parity(child(y, Right)))
          {
            demote(p_x);
            demote(y);
          }
          else
          {
            done = false;
            break;
          }
        }

        x = p_x;
        p_x = p_p_x;
      } while (!is_null(p_x) && creates_3_node);

      if (done)
        return;

      K z = p_x;
      if (Rep::equal(x, child(p_x, Left)))
      {
        K w = child(y, Right);
        if (parity(w) != parity(y))
        {
          rotate_left_at(y);
          promote(y);
          demote(z);
          if (is_leaf(z))
            demote(z);
        }
        else
        {
          K v = child(y, Left);
          if constexpr (use_checks)
            SNMALLOC_ASSERT(parity(y) != parity(v));
          double_rotate_left_at(v);
          double_promote(v);
          demote(y);
          double_demote(z);
        }
      }
      else
      {
        K w = child(y, Left);
        if (parity(w) != parity(y))
        {
          rotate_right_at(y);
          promote(y);
          demote(z);
          if (is_leaf(z))
            demote(z);
        }
        else
        {
          K v = child(y, Right);
          if constexpr (use_checks)
            SNMALLOC_ASSERT(parity(y) != parity(v));
          double_rotate_right_at(v);
          double_promote(v);
          demote(y);
          double_demote(z);
        }
      }
    }

    void delete_rebalance_2_2_leaf(K leaf)
    {
      K x = leaf;
      K p = parent(x);
      if (is_null(p))
      {
        demote(x);
        return;
      }
      if (parity(p) == parity(x))
      {
        demote(x);
        delete_rebalance_3_child(x, p);
      }
      else
      {
        demote(x);
      }
    }

    void erase_node(K node)
    {
      K y = Rep::null;
      K x = Rep::null;
      K p_y = Rep::null;
      bool was_2_child = false;

      if (is_null(child(node, Left)) || is_null(child(node, Right)))
      {
        y = node;
      }
      else
      {
        y = minimum_at(child(node, Right));
      }

      x = is_null(child(y, Left)) ? child(y, Right) : child(y, Left);

      if (!is_null(x))
        set_parent(x, parent(y));

      p_y = parent(y);
      if (is_null(p_y))
      {
        set_root(x);
      }
      else
      {
        was_2_child = is_2_child(y, p_y);
        if (Rep::equal(y, child(p_y, Left)))
          set_child(p_y, Left, x);
        else
          set_child(p_y, Right, x);
      }

      if (!Rep::equal(y, node))
      {
        swap_in_node_at(node, y);
        if (Rep::equal(node, p_y))
          p_y = y;
      }

      if (!is_null(p_y))
      {
        if (was_2_child)
        {
          delete_rebalance_3_child(x, p_y);
        }
        else if (is_null(x) && Rep::equal(child(p_y, Left), child(p_y, Right)))
        {
          delete_rebalance_2_2_leaf(p_y);
        }

        if constexpr (use_checks)
        {
          SNMALLOC_ASSERT(!(is_leaf(p_y) && parity(p_y)));
        }
      }

      set_child(node, Left, Rep::null);
      set_child(node, Right, Rep::null);
      set_parent(node, Rep::null);
      set_parity(node, false);
    }

    K find_node(K value) const
    {
      K next = get_root();
      while (!is_null(next))
      {
        if (Rep::equal(next, value))
          return next;
        bool dir = Rep::compare(next, value) ? Left : Right;
        next = child(next, dir);
      }
      return Rep::null;
    }

    void insert_known_absent(K value, K parent_node, bool dir)
    {
      if (is_null(parent_node))
      {
        set_parent(value, Rep::null);
        set_child(value, Left, Rep::null);
        set_child(value, Right, Rep::null);
        set_parity(value, false);
        set_root(value);
        return;
      }

      bool was_leaf =
        is_null(child(parent_node, Left)) && is_null(child(parent_node, Right));
      set_child(value, Left, Rep::null);
      set_child(value, Right, Rep::null);
      set_parity(value, false);
      set_child(parent_node, dir, value);
      set_parent(value, parent_node);

      if (was_leaf)
        insert_rebalance(value);
    }

  public:
    constexpr WeakAVLTree() = default;

    bool is_empty()
    {
      return is_null(get_root());
    }

    bool insert_elem(K value)
    {
      auto path = get_root_path();
      if (find(path, value))
        return false;
      insert_path(path, value);
      return true;
    }

    bool remove_elem(K value)
    {
      auto path = get_root_path();
      if (!find(path, value))
        return false;
      remove_path(path);
      return true;
    }

    K remove_min()
    {
      K r = get_root();
      if (is_null(r))
        return Rep::null;

      K n = minimum_at(r);
      erase_node(n);
      return n;
    }

    bool find(RBPath& path, K value)
    {
      K parent_node = Rep::null;
      K cursor = get_root();
      bool dir = Left;

      while (!is_null(cursor))
      {
        if (Rep::equal(cursor, value))
        {
          path.parent = parent_node;
          path.curr = cursor;
          path.dir = dir;
          return true;
        }
        parent_node = cursor;
        dir = Rep::compare(cursor, value) ? Left : Right;
        cursor = child(cursor, dir);
      }

      path.parent = parent_node;
      path.curr = Rep::null;
      path.dir = dir;
      return false;
    }

    bool remove_path(RBPath& path)
    {
      if (is_null(path.curr))
        return false;
      erase_node(path.curr);
      return true;
    }

    void insert_path(RBPath& path, K value)
    {
      if constexpr (use_checks)
        SNMALLOC_ASSERT(is_null(path.curr));
      insert_known_absent(value, path.parent, path.dir);
      path.curr = value;
    }

    RBPath get_root_path()
    {
      return RBPath{};
    }
  };
} // namespace snmalloc
