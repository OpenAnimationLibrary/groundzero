#include "../motionlinesfx.cpp"
#include "comiclinesfx_uitest.h"

int main(int argc, char **argv) {
  return ComicLinesFxUiTest::run<MotionLinesFx>(
      argc, argv, "PASS: every motion lines control, keys and undo");
}
