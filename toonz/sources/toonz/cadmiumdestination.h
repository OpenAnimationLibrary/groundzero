#pragma once

#include <QString>

class ToonzScene;
class TXshSimpleLevel;

namespace Cadmium {
TXshSimpleLevel *createResultLevel(ToonzScene *scene,
                                  const TXshSimpleLevel *source,
                                  QString &error);
}
