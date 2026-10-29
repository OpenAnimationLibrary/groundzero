// Regression test of the rectangle packer of the Texture Atlas export
// (toonz/atlaspacker.h): no overlaps, everything inside its page, padding,
// determinism, page splitting, oversize rectangles and the other options.

#include "toonz/atlaspacker.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace AtlasPacker;

namespace {

void require(bool condition, const std::string &message) {
  if (!condition) throw std::runtime_error(message);
}

//------------------------------------------------------------------

// Small deterministic generator, so that the test does not depend on the
// standard library implementation
struct Lcg {
  unsigned m_state;
  explicit Lcg(unsigned seed) : m_state(seed) {}
  int next(int lo, int hi) {  // inclusive
    m_state = m_state * 1664525u + 1013904223u;
    return lo + (int)((m_state >> 8) % (unsigned)(hi - lo + 1));
  }
};

std::vector<Size> randomSizes(int count, int lo, int hi, unsigned seed) {
  Lcg rng(seed);
  std::vector<Size> sizes;
  for (int i = 0; i < count; ++i) {
    int w = rng.next(lo, hi);
    int h = rng.next(lo, hi);
    sizes.push_back(Size(w, h));
  }
  return sizes;
}

//------------------------------------------------------------------

bool isPowerOfTwo(int v) { return v > 0 && (v & (v - 1)) == 0; }

bool samePlacements(const Result &a, const Result &b) {
  if (a.m_placements.size() != b.m_placements.size() ||
      a.m_pages.size() != b.m_pages.size() ||
      a.m_rejected.size() != b.m_rejected.size())
    return false;
  for (size_t i = 0; i < a.m_placements.size(); ++i) {
    const Placement &p = a.m_placements[i], &q = b.m_placements[i];
    if (p.m_page != q.m_page || p.m_x != q.m_x || p.m_y != q.m_y ||
        p.m_w != q.m_w || p.m_h != q.m_h || p.m_rotated != q.m_rotated)
      return false;
  }
  for (size_t i = 0; i < a.m_pages.size(); ++i)
    if (a.m_pages[i].m_w != b.m_pages[i].m_w ||
        a.m_pages[i].m_h != b.m_pages[i].m_h)
      return false;
  return true;
}

//------------------------------------------------------------------

// Checks every rule of a result which has no rejected rectangle: all placed,
// the stored size is the input one (swapped if rotated), inside the page, the
// page not larger than the maximum, and two rectangles of a page at least
// padding apart.
void checkResult(const std::vector<Size> &sizes, const Options &options,
                 const Result &result, const std::string &what) {
  require(result.m_placements.size() == sizes.size(),
          what + ": one placement per input");

  const int padding = options.m_padding < 0 ? 0 : options.m_padding;

  for (size_t i = 0; i < sizes.size(); ++i) {
    const Placement &p = result.m_placements[i];
    if (p.m_page < 0) continue;  // rejected, checked by the caller

    require(p.m_page < (int)result.m_pages.size(), what + ": page index");
    const PageSize &page = result.m_pages[p.m_page];

    if (p.m_rotated) {
      require(options.m_allowRotation, what + ": rotated when not allowed");
      require(p.m_w == sizes[i].m_h && p.m_h == sizes[i].m_w,
              what + ": rotated size");
    } else
      require(p.m_w == sizes[i].m_w && p.m_h == sizes[i].m_h,
              what + ": stored size");

    require(p.m_x >= 0 && p.m_y >= 0, what + ": negative position");
    require(p.m_x + p.m_w <= page.m_w && p.m_y + p.m_h <= page.m_h,
            what + ": outside its page");
  }

  for (const PageSize &page : result.m_pages) {
    require(page.m_w >= 1 && page.m_h >= 1, what + ": empty page");
    require(
        page.m_w <= options.m_maxPageSize && page.m_h <= options.m_maxPageSize,
        what + ": page larger than the maximum");
    if (options.m_powerOfTwo)
      require(isPowerOfTwo(page.m_w) && isPowerOfTwo(page.m_h),
              what + ": page size is not a power of two");
  }

  for (size_t i = 0; i < sizes.size(); ++i) {
    const Placement &p = result.m_placements[i];
    if (p.m_page < 0) continue;
    for (size_t j = i + 1; j < sizes.size(); ++j) {
      const Placement &q = result.m_placements[j];
      if (q.m_page != p.m_page) continue;
      bool apart = p.m_x + p.m_w + padding <= q.m_x ||
                   q.m_x + q.m_w + padding <= p.m_x ||
                   p.m_y + p.m_h + padding <= q.m_y ||
                   q.m_y + q.m_h + padding <= p.m_y;
      if (!apart)
        throw std::runtime_error(what + ": overlap or padding not respected (" +
                                 std::to_string(i) + ", " + std::to_string(j) +
                                 ")");
    }
  }
}

//------------------------------------------------------------------

void testEmpty() {
  Options options;
  Result result = pack(std::vector<Size>(), options);
  require(result.ok(), "empty: ok");
  require(result.m_pages.empty(), "empty: no page");
  require(result.m_placements.empty(), "empty: no placement");
  require(result.m_rejected.empty(), "empty: nothing rejected");
}

//------------------------------------------------------------------

void testSingle() {
  Options options;
  std::vector<Size> sizes = {Size(37, 21)};
  Result result           = pack(sizes, options);
  require(result.ok(), "single: ok");
  require(result.m_pages.size() == 1, "single: one page");
  require(result.m_pages[0].m_w == 37 && result.m_pages[0].m_h == 21,
          "single: the page is as large as the sprite");
  const Placement &p = result.m_placements[0];
  require(p.m_page == 0 && p.m_x == 0 && p.m_y == 0 && !p.m_rotated,
          "single: at the corner, not rotated");
  checkResult(sizes, options, result, "single");

  options.m_powerOfTwo = true;
  result               = pack(sizes, options);
  require(result.m_pages[0].m_w == 64 && result.m_pages[0].m_h == 32,
          "single: power of two page");
  checkResult(sizes, options, result, "single pow2");
}

//------------------------------------------------------------------

void testNoOverlapAndPadding() {
  for (int padding : {0, 1, 3, 8}) {
    for (int pow2 = 0; pow2 < 2; ++pow2) {
      for (int rotation = 0; rotation < 2; ++rotation) {
        Options options;
        options.m_maxPageSize   = 512;
        options.m_padding       = padding;
        options.m_powerOfTwo    = pow2 != 0;
        options.m_allowRotation = rotation != 0;

        std::vector<Size> sizes = randomSizes(150, 4, 120, 1234u + padding);
        Result result           = pack(sizes, options);
        std::string what        = "random padding " + std::to_string(padding) +
                           (pow2 ? " pow2" : "") + (rotation ? " rot" : "");
        require(result.ok(), what + ": ok");
        for (const Placement &p : result.m_placements)
          require(p.m_page >= 0, what + ": every sprite is placed");
        checkResult(sizes, options, result, what);
      }
    }
  }
}

//------------------------------------------------------------------

void testNoRotationByDefault() {
  Options options;
  require(!options.m_allowRotation, "default: rotation off");
  require(!options.m_powerOfTwo, "default: power of two off");
  require(options.m_maxPageSize == 4096, "default: 4096 pages");

  std::vector<Size> sizes = randomSizes(80, 5, 90, 99u);
  Result result           = pack(sizes, options);
  for (const Placement &p : result.m_placements)
    require(!p.m_rotated, "default: no sprite is rotated");
}

//------------------------------------------------------------------

void testPaddingBetweenTouchingSprites() {
  // Two sprites which fill a page exactly side by side need the padding
  Options options;
  options.m_maxPageSize   = 10;
  options.m_padding       = 2;
  std::vector<Size> sizes = {Size(4, 10), Size(4, 10)};
  Result result           = pack(sizes, options);
  require(result.ok() && result.m_pages.size() == 1, "padding: one page");
  checkResult(sizes, options, result, "padding");

  // 5 + 2 + 5 fits a page of 12, not a page of 11
  sizes                 = {Size(5, 10), Size(5, 10)};
  options.m_maxPageSize = 12;
  result                = pack(sizes, options);
  require(result.m_pages.size() == 1, "padding: fits side by side in 12");
  checkResult(sizes, options, result, "padding 12");
  options.m_maxPageSize = 11;
  result                = pack(sizes, options);
  require(result.m_pages.size() == 2, "padding: 5 + 2 + 5 > 11, two pages");
  checkResult(sizes, options, result, "padding two pages");
}

//------------------------------------------------------------------

void testDeterministic() {
  Options options;
  options.m_maxPageSize   = 256;
  options.m_padding       = 2;
  std::vector<Size> sizes = randomSizes(120, 3, 100, 7u);

  Result a = pack(sizes, options);
  Result b = pack(sizes, options);
  require(samePlacements(a, b), "deterministic: same input, same layout");

  options.m_allowRotation = true;
  options.m_powerOfTwo    = true;
  a                       = pack(sizes, options);
  b                       = pack(sizes, options);
  require(samePlacements(a, b), "deterministic: with rotation and pow2");
}

//------------------------------------------------------------------

void testMultiPage() {
  Options options;
  options.m_maxPageSize = 256;
  options.m_padding     = 2;

  // Only four 100 x 100 sprites fit a 256 x 256 page: 50 sprites, 13 pages
  std::vector<Size> sizes(50, Size(100, 100));
  Result result = pack(sizes, options);
  require(result.ok(), "multipage: ok");
  require(result.m_pages.size() == 13, "multipage: 13 pages");
  checkResult(sizes, options, result, "multipage");

  std::vector<int> perPage(result.m_pages.size(), 0);
  for (const Placement &p : result.m_placements) ++perPage[p.m_page];
  for (int count : perPage)
    require(count >= 1 && count <= 4, "multipage: at most four per page");

  // A single page is enough with a larger limit
  options.m_maxPageSize = 1024;
  result                = pack(sizes, options);
  require(result.m_pages.size() == 1, "multipage: one page of 1024");
  checkResult(sizes, options, result, "multipage 1024");
}

//------------------------------------------------------------------

void testOversize() {
  Options options;
  options.m_maxPageSize   = 4096;
  std::vector<Size> sizes = {Size(5000, 10), Size(20, 20), Size(10, 4097),
                             Size(0, 5), Size(4096, 4096)};
  Result result           = pack(sizes, options);

  require(!result.ok(), "oversize: reported");
  require(result.m_rejected.size() == 3, "oversize: three rejected");
  bool found0 = false, found2 = false, found3 = false;
  for (const Rejected &r : result.m_rejected) {
    if (r.m_index == 0) found0 = (r.m_reason == Rejected::TooLarge);
    if (r.m_index == 2) found2 = (r.m_reason == Rejected::TooLarge);
    if (r.m_index == 3) found3 = (r.m_reason == Rejected::EmptySize);
  }
  require(found0 && found2 && found3, "oversize: indices and reasons");

  require(result.m_placements[0].m_page < 0 &&
              result.m_placements[2].m_page < 0 &&
              result.m_placements[3].m_page < 0,
          "oversize: rejected sprites have no page");
  require(
      result.m_placements[1].m_page >= 0 && result.m_placements[4].m_page >= 0,
      "oversize: the others are packed anyway");
  checkResult(sizes, options, result, "oversize");

  // The size of a page is the limit, with rotation too
  options.m_allowRotation = true;
  options.m_maxPageSize   = 100;
  sizes                   = {Size(101, 5), Size(5, 101), Size(100, 100)};
  result                  = pack(sizes, options);
  require(result.m_rejected.size() == 2, "oversize: rotation does not help");
  require(result.m_placements[2].m_page == 0, "oversize: exact fit");
}

//------------------------------------------------------------------

void testManySmall() {
  Options options;
  options.m_maxPageSize = 4096;
  options.m_padding     = 1;

  std::vector<Size> sizes(1500, Size(16, 16));
  Result result = pack(sizes, options);
  require(result.ok(), "many: ok");
  require(result.m_pages.size() == 1, "many: one page");
  checkResult(sizes, options, result, "many");

  // The page is about as large as the sprites need, far below the limit
  long long area = (long long)result.m_pages[0].m_w * result.m_pages[0].m_h;
  require(area < 1500LL * 17 * 17 * 2, "many: the page is compact");

  // Different small sizes
  sizes  = randomSizes(800, 2, 40, 5u);
  result = pack(sizes, options);
  require(result.ok() && result.m_pages.size() == 1, "many: random one page");
  checkResult(sizes, options, result, "many random");
}

//------------------------------------------------------------------

void testBigAndSmall() {
  Options options;
  options.m_maxPageSize = 4096;
  options.m_padding     = 2;

  std::vector<Size> sizes;
  sizes.push_back(Size(3000, 3000));
  for (int i = 0; i < 100; ++i) sizes.push_back(Size(64, 64));
  Result result = pack(sizes, options);

  require(result.ok(), "big: ok");
  require(result.m_pages.size() == 1, "big: the small ones go beside it");
  const Placement &big = result.m_placements[0];
  require(big.m_page == 0 && big.m_x == 0 && big.m_y == 0,
          "big: the biggest sprite is packed first");
  checkResult(sizes, options, result, "big");

  // A second big one needs a second page
  sizes.push_back(Size(3000, 3000));
  result = pack(sizes, options);
  require(result.ok() && result.m_pages.size() == 2, "big: two big, two pages");
  require(result.m_placements[0].m_page != result.m_placements.back().m_page,
          "big: the big sprites are on different pages");
  checkResult(sizes, options, result, "big two");
}

//------------------------------------------------------------------

void testPowerOfTwo() {
  Options options;
  options.m_maxPageSize   = 1000;  // not a power of two: it is the limit
  options.m_padding       = 1;
  options.m_powerOfTwo    = true;
  std::vector<Size> sizes = randomSizes(60, 10, 200, 21u);
  Result result           = pack(sizes, options);
  require(result.ok(), "pow2: ok");
  for (const PageSize &page : result.m_pages)
    require(page.m_w <= 1000 && page.m_h <= 1000, "pow2: limit kept");

  options.m_maxPageSize = 1024;
  result                = pack(sizes, options);
  require(result.ok(), "pow2 1024: ok");
  checkResult(sizes, options, result, "pow2 1024");
}

//------------------------------------------------------------------

void testRotation() {
  Options options;
  options.m_maxPageSize   = 64;
  options.m_padding       = 0;
  options.m_allowRotation = true;

  // Mixed tall and wide sprites, rotation is allowed whatever the result
  std::vector<Size> sizes;
  for (int i = 0; i < 12; ++i) sizes.push_back(Size(8, 40));
  for (int i = 0; i < 12; ++i) sizes.push_back(Size(40, 8));
  Result result = pack(sizes, options);
  require(result.ok(), "rotation: ok");
  checkResult(sizes, options, result, "rotation");
}

}  // namespace

int main() {
  try {
    testEmpty();
    testSingle();
    testNoOverlapAndPadding();
    testNoRotationByDefault();
    testPaddingBetweenTouchingSprites();
    testDeterministic();
    testMultiPage();
    testOversize();
    testManySmall();
    testBigAndSmall();
    testPowerOfTwo();
    testRotation();
  } catch (const std::exception &e) {
    std::cerr << "atlaspacker_test failed: " << e.what() << std::endl;
    return 1;
  }
  std::cout << "atlaspacker_test passed" << std::endl;
  return 0;
}
