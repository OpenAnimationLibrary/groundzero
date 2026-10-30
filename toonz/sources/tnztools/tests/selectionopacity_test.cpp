#include "tools/selectionopacity.h"
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}
int main() {
  try {
    TVectorImageP image = new TVectorImage;
    std::vector<TThickPoint> points = {{0, 0, 2}, {5, 0, 2}, {10, 0, 2}};
    auto *first = new TStroke(points), *second = new TStroke(points);
    first->setOpacity(.327); second->setOpacity(.327);
    image->addStroke(first); image->addStroke(second);
    auto one = SelectionOpacity::summarize(image, {0});
    require(one.count == 1 && !one.mixed && one.value == .327, "precise single readback");
    auto same = SelectionOpacity::summarize(image, {0, 1});
    require(same.count == 2 && !same.mixed, "homogeneous readback");
    second->setOpacity(.7);
    auto mixed = SelectionOpacity::summarize(image, {0, 1});
    require(mixed.count == 2 && mixed.mixed, "mixed readback");
    for (int i = 0; i < 100; ++i) SelectionOpacity::summarize(image, {0, 1, -1, 10});
    require(first->getOpacity() == .327 && second->getOpacity() == .7,
            "readback changed artwork");
    require(!SelectionOpacity::summarize(image, {}).count, "empty selection");
    require(SelectionOpacity::summarize(image, {-1, 0, 99}).count == 1, "stale indices");
    std::cout << "Native stroke opacity readback passed\n";
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
