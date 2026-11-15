#include "../burstlinesfx.cpp"
#include "comiclinesfx_uitest.h"

int main(int argc, char **argv) {
  return ComicLinesFxUiTest::run<BurstLinesFx>(
      argc, argv, "PASS: every burst lines control, keys and undo");
}
