#pragma once

#ifndef ATLASPACKER_H
#define ATLASPACKER_H

// Rectangle packer of the Texture Atlas export (see
// doc/animate_texture_atlas.md).
//
// Packs rectangles (sprite sizes in pixels) into one or more square-limited
// pages with the MaxRects algorithm, best short side fit heuristic. It has no
// dependency but the standard library, so that it can be tested standalone
// (toonzlib/tests/atlaspacker_test.cpp).
//
//  - Deterministic: the same sizes and options always give the same layout.
//    The rectangles are packed biggest side first, ties broken by their index.
//  - Padding is the minimum number of empty pixels between two rectangles of
//    a page. Nothing is added between a rectangle and the border of the page.
//  - A rectangle larger than the maximum page size, or with a side which is
//    not positive, is rejected (reported in Result::m_rejected, and the other
//    rectangles are packed anyway), it never throws or crashes.
//  - Several pages are used when the rectangles do not fit in one page of the
//    maximum size. A page which holds everything left is shrunk to the
//    smallest size found; the other pages are cropped to their used area.
//  - Rotating a rectangle by 90 degrees is only done if allowed, which is off
//    by default.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace AtlasPacker {

struct Options {
  int m_maxPageSize    = 4096;   //!< Largest width and height of a page
  int m_padding        = 1;      //!< Empty pixels between rectangles
  bool m_allowRotation = false;  //!< Allow turning a rectangle by 90 degrees
  bool m_powerOfTwo    = false;  //!< Round page sizes up to powers of two
                                 //!< (never above m_maxPageSize)
};

struct Size {
  int m_w = 0, m_h = 0;
  Size() {}
  Size(int w, int h) : m_w(w), m_h(h) {}
};

//! Where a rectangle went. m_page is -1 if it was rejected.
struct Placement {
  int m_page = -1;
  int m_x = 0, m_y = 0;  //!< Top left corner in the page
  int m_w = 0, m_h = 0;  //!< Size in the page: swapped if m_rotated
  bool m_rotated = false;
};

struct PageSize {
  int m_w = 0, m_h = 0;
};

struct Rejected {
  enum Reason {
    TooLarge,  //!< A side is larger than the maximum page size
    EmptySize  //!< A side is not positive
  };
  int m_index;  //!< Index of the rectangle in the input
  Reason m_reason;
};

struct Result {
  std::vector<Placement> m_placements;  //!< One for each input rectangle
  std::vector<PageSize> m_pages;
  std::vector<Rejected> m_rejected;

  //! True if every rectangle was placed
  bool ok() const { return m_rejected.empty(); }
};

//=============================================================================

namespace detail {

struct Rect {
  int m_x, m_y, m_w, m_h;
};

inline bool contains(const Rect &a, const Rect &b) {
  return b.m_x >= a.m_x && b.m_y >= a.m_y && b.m_x + b.m_w <= a.m_x + a.m_w &&
         b.m_y + b.m_h <= a.m_y + a.m_h;
}

//-----------------------------------------------------------------------------

//! The free space of a page, as a list of maximal free rectangles
class MaxRectsBin {
  std::vector<Rect> m_free;

public:
  MaxRectsBin(int w, int h) { m_free.push_back(Rect{0, 0, w, h}); }

  //! Finds the free rectangle which leaves the smallest short side
  //! (then long side) and puts a w x h rectangle at its top left corner.
  bool insert(int w, int h, bool allowRotation, Rect &placed, bool &rotated) {
    bool found    = false;
    int bestShort = 0, bestLong = 0;
    Rect best{0, 0, 0, 0};
    bool bestRotated = false;

    for (size_t i = 0; i < m_free.size(); ++i) {
      const Rect &f = m_free[i];
      for (int r = 0; r < 2; ++r) {
        if (r == 1 && (!allowRotation || w == h)) break;
        int rw = r ? h : w, rh = r ? w : h;
        if (rw > f.m_w || rh > f.m_h) continue;
        int leftW = f.m_w - rw, leftH = f.m_h - rh;
        int shortSide = std::min(leftW, leftH);
        int longSide  = std::max(leftW, leftH);
        if (!found || shortSide < bestShort ||
            (shortSide == bestShort && longSide < bestLong)) {
          found       = true;
          bestShort   = shortSide;
          bestLong    = longSide;
          best        = Rect{f.m_x, f.m_y, rw, rh};
          bestRotated = (r == 1);
        }
      }
    }
    if (!found) return false;

    place(best);
    placed  = best;
    rotated = bestRotated;
    return true;
  }

private:
  //! Removes the area of \e n from the free rectangles
  void place(const Rect &n) {
    std::vector<Rect> next;
    next.reserve(m_free.size() + 8);

    // Rectangles which do not touch n are kept as they are. They are maximal
    // already, and no new rectangle can contain one of them (a new rectangle
    // is part of a rectangle which was maximal).
    std::vector<Rect> added;
    for (const Rect &f : m_free) {
      if (n.m_x >= f.m_x + f.m_w || n.m_x + n.m_w <= f.m_x ||
          n.m_y >= f.m_y + f.m_h || n.m_y + n.m_h <= f.m_y) {
        next.push_back(f);
        continue;
      }
      if (n.m_x > f.m_x)
        added.push_back(Rect{f.m_x, f.m_y, n.m_x - f.m_x, f.m_h});
      if (n.m_x + n.m_w < f.m_x + f.m_w)
        added.push_back(
            Rect{n.m_x + n.m_w, f.m_y, f.m_x + f.m_w - (n.m_x + n.m_w), f.m_h});
      if (n.m_y > f.m_y)
        added.push_back(Rect{f.m_x, f.m_y, f.m_w, n.m_y - f.m_y});
      if (n.m_y + n.m_h < f.m_y + f.m_h)
        added.push_back(
            Rect{f.m_x, n.m_y + n.m_h, f.m_w, f.m_y + f.m_h - (n.m_y + n.m_h)});
    }

    // The new rectangles which are inside another one are dropped. Of two
    // equal ones, the first is dropped.
    std::vector<char> dead(added.size(), 0);
    for (size_t i = 0; i < added.size(); ++i) {
      for (const Rect &k : next)
        if (contains(k, added[i])) {
          dead[i] = 1;
          break;
        }
      if (dead[i]) continue;
      for (size_t j = 0; j < added.size(); ++j) {
        if (j == i || dead[j]) continue;
        if (contains(added[j], added[i])) {
          dead[i] = 1;
          break;
        }
      }
    }
    for (size_t i = 0; i < added.size(); ++i)
      if (!dead[i]) next.push_back(added[i]);

    m_free.swap(next);
  }
};

//-----------------------------------------------------------------------------

struct Item {
  int m_index;  //!< Index in the input
  int m_w, m_h;
};

struct Trial {
  std::vector<Placement> m_placed;  //!< One for each item
  std::vector<char> m_fits;         //!< One for each item
  bool m_all = true;                //!< Whether every item fits
};

//! Packs the items, in order, into a binW x binH bin.
inline Trial runTrial(const std::vector<Item> &items, int binW, int binH,
                      int padding, bool allowRotation, bool stopAtFailure) {
  Trial trial;
  trial.m_placed.resize(items.size());
  trial.m_fits.assign(items.size(), 0);

  // Every item takes padding more pixels to the right and to the bottom, in a
  // bin which has them too, so that two items are at least padding apart, and
  // an item can still touch the right and bottom borders.
  MaxRectsBin bin(binW + padding, binH + padding);
  for (size_t i = 0; i < items.size(); ++i) {
    Rect node{0, 0, 0, 0};
    bool rotated = false;
    if (!bin.insert(items[i].m_w + padding, items[i].m_h + padding,
                    allowRotation, node, rotated)) {
      trial.m_all = false;
      if (stopAtFailure) return trial;
      continue;
    }
    Placement &p    = trial.m_placed[i];
    p.m_x           = node.m_x;
    p.m_y           = node.m_y;
    p.m_rotated     = rotated;
    p.m_w           = rotated ? items[i].m_h : items[i].m_w;
    p.m_h           = rotated ? items[i].m_w : items[i].m_h;
    trial.m_fits[i] = 1;
  }
  return trial;
}

//-----------------------------------------------------------------------------

inline int nextPowerOfTwo(int value) {
  int p = 1;
  while (p < value && p < (1 << 30)) p <<= 1;
  return p;
}

inline int clampInt(int value, int lo, int hi) {
  return std::max(lo, std::min(hi, value));
}

//! The items are packed biggest side first, the index breaking ties, which
//! makes the order (and so the layout) independent of the sort algorithm.
inline bool itemBefore(const Item &a, const Item &b) {
  int sideA = std::max(a.m_w, a.m_h), sideB = std::max(b.m_w, b.m_h);
  if (sideA != sideB) return sideA > sideB;
  long long areaA = (long long)a.m_w * a.m_h, areaB = (long long)b.m_w * b.m_h;
  if (areaA != areaB) return areaA > areaB;
  return a.m_index < b.m_index;
}

}  // namespace detail

//=============================================================================

/*!
Packs rectangles of the given \e sizes. The placement of the rectangle i of the
input is in the placement i of the result.
*/
inline Result pack(const std::vector<Size> &sizes, const Options &options) {
  using namespace detail;

  Result result;
  result.m_placements.assign(sizes.size(), Placement());

  const int maxSize = clampInt(options.m_maxPageSize, 1, 65536);
  const int padding = clampInt(options.m_padding, 0, maxSize);
  const bool rotate = options.m_allowRotation;

  std::vector<Item> remaining;
  for (size_t i = 0; i < sizes.size(); ++i) {
    if (sizes[i].m_w <= 0 || sizes[i].m_h <= 0)
      result.m_rejected.push_back(Rejected{(int)i, Rejected::EmptySize});
    else if (sizes[i].m_w > maxSize || sizes[i].m_h > maxSize)
      result.m_rejected.push_back(Rejected{(int)i, Rejected::TooLarge});
    else
      remaining.push_back(Item{(int)i, sizes[i].m_w, sizes[i].m_h});
  }
  std::sort(remaining.begin(), remaining.end(), itemBefore);

  while (!remaining.empty()) {
    // Every item fits alone in a page of the maximum size, so at least one is
    // placed and the loop ends.
    Trial chosen =
        runTrial(remaining, maxSize, maxSize, padding, rotate, false);

    if (chosen.m_all) {
      // Everything left fits in one page: look for a smaller one
      double inflatedArea = 0.0;
      int largestSide     = 1;
      for (const Item &item : remaining) {
        inflatedArea += (double)(item.m_w + padding) * (item.m_h + padding);
        largestSide = std::max(largestSide, std::max(item.m_w, item.m_h));
      }

      if (options.m_powerOfTwo) {
        // Pages of a power of two size, square or twice as wide as high, by
        // increasing area
        const double minArea = inflatedArea;
        bool done            = false;
        for (int s = 1; s < (1 << 30) && !done; s <<= 1) {
          for (int k = 0; k < 2 && !done; ++k) {
            int w = std::min(maxSize, s * (k ? 2 : 1)),
                h = std::min(maxSize, s);
            if (w == maxSize && h == maxSize) {
              done = true;  // the full size trial already holds everything
              break;
            }
            if ((double)(w + padding) * (h + padding) < minArea) continue;
            Trial t = runTrial(remaining, w, h, padding, rotate, true);
            if (t.m_all) {
              chosen = t;
              done   = true;
            }
          }
        }
      } else {
        // Smallest square side found by bisection. The area gives a lower
        // bound: the bin has to hold the area of all the inflated items.
        int lo = std::max(largestSide,
                          (int)std::floor(std::sqrt(inflatedArea)) - padding);
        lo     = clampInt(lo, 1, maxSize);
        int hi = maxSize;  // known to work: chosen
        while (lo < hi) {
          int mid = lo + (hi - lo) / 2;
          Trial t = runTrial(remaining, mid, mid, padding, rotate, true);
          if (t.m_all) {
            hi     = mid;
            chosen = t;
          } else
            lo = mid + 1;
        }
      }
    }

    // The page is as large as the placed rectangles need
    int pageW = 1, pageH = 1;
    for (size_t i = 0; i < remaining.size(); ++i) {
      if (!chosen.m_fits[i]) continue;
      const Placement &p = chosen.m_placed[i];
      pageW              = std::max(pageW, p.m_x + p.m_w);
      pageH              = std::max(pageH, p.m_y + p.m_h);
    }
    if (options.m_powerOfTwo) {
      pageW = std::min(maxSize, nextPowerOfTwo(pageW));
      pageH = std::min(maxSize, nextPowerOfTwo(pageH));
    }

    int pageIndex = (int)result.m_pages.size();
    PageSize pageSize;
    pageSize.m_w = pageW;
    pageSize.m_h = pageH;
    result.m_pages.push_back(pageSize);

    std::vector<Item> left;
    for (size_t i = 0; i < remaining.size(); ++i) {
      if (!chosen.m_fits[i]) {
        left.push_back(remaining[i]);
        continue;
      }
      Placement p                               = chosen.m_placed[i];
      p.m_page                                  = pageIndex;
      result.m_placements[remaining[i].m_index] = p;
    }
    remaining.swap(left);
  }

  return result;
}

}  // namespace AtlasPacker

#endif  // ATLASPACKER_H
