#include "tools/styleanimationcopy.h"
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char *message) {
  if (!ok) throw std::runtime_error(message);
}
int main() {
  try {
    TPaletteP palette = new TPalette;
    const int source = palette->addStyle(TPixel32::Red);
    palette->setKeyframe(source, 0);
    palette->setStyle(source, TPixel32::Blue);
    palette->setKeyframe(source, 10);
    palette->setFrame(5);
    const TPixel32 atFive = palette->getStyle(source)->getMainColor();
    auto *copy = palette->getStyle(source)->clone();
    copy->setName(L"Independent");
    copy->setGlobalName(L"");
    const auto initialAppearance = std::shared_ptr<TColorStyle>(copy->clone());
    const int target = palette->addStyle(copy);
    const auto keys = StyleAnimationCopy::capture(palette.getPointer(), source, *initialAppearance);
    require(keys.size() == 2 && palette->getFrame() == 5 &&
                palette->getStyle(source)->getMainColor() == atFive,
            "capture mutated source time or appearance");
    StyleAnimationCopy::install(palette.getPointer(), target, keys, *initialAppearance);
    require(palette->getKeyframeCount(target) == 2, "target keys missing");
    for (int frame = 0; frame <= 10; ++frame) {
      palette->setFrame(frame);
      require(palette->getStyle(source)->getMainColor() ==
                  palette->getStyle(target)->getMainColor(), "animation differs");
      require(palette->getStyle(target)->getName() == L"Independent", "target identity changed");
    }
    auto appearance = std::shared_ptr<TColorStyle>(palette->getStyle(target)->clone());
    StyleAnimationCopy::install(palette.getPointer(), target, {}, *appearance);
    require(!palette->getKeyframeCount(target) && palette->getKeyframeCount(source) == 2,
            "clearing copied animation changed source");
    StyleAnimationCopy::install(palette.getPointer(), target, keys, *appearance);
    require(palette->getKeyframeCount(target) == 2, "redo copied animation");
    std::cout << "Independent native palette animation copy passed\n";
  } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
