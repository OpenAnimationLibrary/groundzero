#pragma once

#include <QColor>

#include <array>
#include <map>
#include <optional>

#include "tpalettekeyinterpolation.h"
#include <vector>

class QMenu;

// The editor uses scalar curves for display. A track owns edits to the
// underlying grouped keys, including their validation, notifications and undo.
class FunctionKeyTrack {
public:
  using Value = std::array<double, 4>;

  struct Snapshot {
    std::map<double, Value> keys;  // Xsheet frames, including subframes.
    std::map<double, TPaletteKeyInterpolation> interpolation;
    Value defaults{};
    bool available = true;

    bool operator==(const Snapshot &other) const {
      return keys == other.keys && interpolation == other.interpolation &&
             defaults == other.defaults && available == other.available;
    }
    bool operator!=(const Snapshot &other) const { return !(*this == other); }
  };

  struct Edit {
    std::optional<double> from;   // Empty when creating a key.
    std::optional<double> to;     // Empty when deleting a key.
    std::optional<double> value;  // Empty to keep the complete original value.
  };

  virtual ~FunctionKeyTrack()   = default;
  virtual Snapshot read() const = 0;
  virtual bool canEdit() const  = 0;
  virtual QColor swatch() const { return {}; }
  virtual std::optional<double> valueAt(int component, double frame) const = 0;
  virtual void addInterpolationActions(QMenu &menu, double frame) const    = 0;
  virtual bool edit(int component, const std::vector<Edit> &edits)         = 0;
};
