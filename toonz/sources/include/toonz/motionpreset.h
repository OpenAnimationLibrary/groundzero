#pragma once

#ifndef MOTIONPRESET_INCLUDED
#define MOTIONPRESET_INCLUDED

#include "tcommon.h"
#include "tdoublekeyframe.h"
#include "tfilepath.h"

#include <QString>

#include <string>
#include <vector>

#undef DVAPI
#undef DVVAR
#ifdef TOONZLIB_EXPORTS
#define DVAPI DV_EXPORT_API
#define DVVAR DV_EXPORT_VAR
#else
#define DVAPI DV_IMPORT_API
#define DVVAR DV_IMPORT_VAR
#endif

class TOStream;
class TIStream;
class TDoubleParam;
class TStageObject;

//=============================================================================
//! A saved animation that can be applied to any stage object, starting at any
//! frame and relative to where the object is there.
/*!
  A preset holds, for each of a few animatable channels of a stage object, a
  list of keyframes expressed \a relative to the first key of the channel:

  - the frame of a key is an offset from the first frame of the preset (the
    earliest key of all the channels is at offset 0);
  - the value of a key is, depending on the \b mode of the channel, either
    the \b difference from the value of the first key of the channel (Add) or
    its \b ratio to it (Multiply). So the value of the first key is always 0
    (Add) or 1 (Multiply);
  - the interpolation of the segment starting at the key and the speed
    handles are those of TDoubleKeyframe. In Add channels the handles are
    used as they are; in Multiply channels their value component is a ratio
    too, and is multiplied by the reference value when applied.

  The channels are named as the channels of the stage objects in scene files:
  "x" and "y" (inches), "z", "so", "rot" (degrees), "rotx" and "roty"
  (degrees, tilt), "shx" and "shy" (shear), which are Add channels, and "sx",
  "sy" and "sc" (scale factors, 1 is 100%), which are Multiply channels by
  default. The path position (T_Path) is not part of presets.

  \par Applying a preset at the frame F
  For each channel, with S the value of the curve at the frame of its first
  key (F plus the offset of the key):
  - Add channels (positions, angles, shear): the key i gets S + d_i, where
    d_i is the stored difference. A rotation of 360 therefore turns the object
    once more, whatever its angle was.
  - Multiply channels (scales): the key i gets S * r_i, where r_i is the
    stored ratio, so a "pulse" to 1.2 makes any object 20% bigger than it was.
  - The keys that the curve already has between the first and the last key of
    the preset are replaced by the keys of the preset; the other keys stay.
    The segment after the last key of the preset is linear.
  - If the \e anchor of the preset is \b End the keys are shifted so that the
    \b last key gets the value S the curve has at F (instead of the first one):
    the object arrives where it is, as in "fly in" presets. In a Multiply
    channel the keys are divided by the last ratio.
  - A curve is skipped, and reported, if it is cycled, or has an expression,
    a file or a similar shape segment where the keys would go.

  \par Offset layers
  A preset can also be applied to the curves of an offset layer of the object
  (see TStageObject::OffsetLayer). Those curves are added to the base ones,
  also for the scale channels, so the keys of a Multiply channel get
  S + R * (r_i - 1) where R is the value of the base curve at the key.

  \par File
  A preset is saved as a \c .tnzmotion text file, with the XML-like syntax
  of TOStream:

  \code
  <motionpreset>
    <name> "Fly In From Left" </name>
    <description> "..." </description>      (optional)
    <anchor> end </anchor>                  (optional, "start" by default)
    <channel name="x" mode="add">           (mode: add or multiply)
      frame value type speedInX speedInY speedOutX speedOutY linkedHandles
      ...
    </channel>
  </motionpreset>
  \endcode

  Each key is one line of 8 numbers: type is one of the codes of the scene
  files (n None, C Constant, L Linear, S SpeedInOut, E EaseInOut,
  Ep EaseInOutPercentage, Exp Exponential) and linkedHandles is 0 or 1.
*/
class DVAPI MotionPreset {
public:
  enum Mode { Add, Multiply };
  enum Anchor { AnchorStart, AnchorEnd };

  struct Key {
    double m_frame;  //!< Offset from the start of the preset
    double m_value;  //!< Difference from (Add) or ratio to (Multiply) the
                     //!< value of the first key of the channel
    TDoubleKeyframe::Type m_type;  //!< Interpolation of the segment after it
    TPointD m_speedIn, m_speedOut;
    bool m_linkedHandles;

    Key()
        : m_frame(0)
        , m_value(0)
        , m_type(TDoubleKeyframe::Linear)
        , m_linkedHandles(false) {}
    Key(double frame, double value,
        TDoubleKeyframe::Type type = TDoubleKeyframe::Linear)
        : m_frame(frame)
        , m_value(value)
        , m_type(type)
        , m_linkedHandles(false) {}
  };

  struct Channel {
    std::string m_name;
    Mode m_mode;
    std::vector<Key> m_keys;

    Channel() : m_mode(Add) {}
    Channel(const std::string &name, Mode mode) : m_name(name), m_mode(mode) {}
  };

  //! The result of apply()
  struct ApplyResult {
    int m_channelCount;  //!< The channels that got keys
    int m_keyCount;      //!< The keys that were set
    //! Why a channel was left alone, as "channel: reason"
    std::vector<std::string> m_skipped;

    ApplyResult() : m_channelCount(0), m_keyCount(0) {}
  };

  //! The result of captureChannel()
  enum CaptureResult {
    CaptureNothing,  //!< Less than two keys in the range: nothing to store
    CaptureDone,
    CaptureSkipped  //!< The keys can't be stored (expressions...)
  };

public:
  MotionPreset() : m_anchor(AnchorStart) {}

  const QString &getName() const { return m_name; }
  void setName(const QString &name) { m_name = name; }
  const QString &getDescription() const { return m_description; }
  void setDescription(const QString &description) {
    m_description = description;
  }
  Anchor getAnchor() const { return m_anchor; }
  void setAnchor(Anchor anchor) { m_anchor = anchor; }

  int getChannelCount() const { return (int)m_channels.size(); }
  const Channel &getChannel(int index) const { return m_channels[index]; }
  const std::vector<Channel> &getChannels() const { return m_channels; }
  //! Returns the channel with the name, or 0.
  const Channel *findChannel(const std::string &name) const;
  void addChannel(const Channel &channel) { m_channels.push_back(channel); }
  void clearChannels() { m_channels.clear(); }

  //! Returns the offset of the last key of all the channels, i.e. how many
  //! frames the preset lasts.
  double getDuration() const;

  //! Returns whether the preset can be applied: it has channels with known
  //! names and a few keys at strictly increasing frames, with valid
  //! interpolations, numbers that are finite, and, for Multiply channels,
  //! ratios that are not 0 where they are divided by. If not, \e reason says
  //! why.
  bool isValid(std::string *reason = 0) const;

  //! Returns whether the preset is in the form capture() makes: the first key
  //! of each channel has a null difference (a ratio of 1), no frame is
  //! negative, and some channel starts at offset 0.
  bool isNormalized() const;

  //! Returns whether the channel has at least two keys with different values
  static bool isAnimated(const Channel &channel);

  //! \name Files
  //@{
  bool save(const TFilePath &fp, std::string *error = 0) const;
  static bool load(const TFilePath &fp, MotionPreset &preset,
                   std::string *error = 0);
  //! Writes the content of the \c motionpreset tag
  void saveData(TOStream &os) const;
  //! Reads the content of the \c motionpreset tag. May throw.
  void loadData(TIStream &is);
  //@}

  //! \name Channels of the stage objects
  //@{
  //! Returns the TStageObject::Channel of a channel name, or -1
  static int getStageObjectChannel(const std::string &name);
  //! Returns the name of a TStageObject::Channel, or an empty string
  static std::string getChannelName(int stageObjectChannel);
  //! Returns Multiply for the scales and Add for the other channels
  static Mode getDefaultMode(const std::string &name);
  //! Returns the names of all the channels
  static std::vector<std::string> getChannelNames();
  //@}

  //! \name Capture
  //@{
  //! Stores the keys that \e param has between the frames (included) as a
  //! channel. The frames of the keys are made relative to \e startFrame. The
  //! first key gets no incoming handle, and the last one a linear segment.
  //! Scale channels are stored as ratios, unless their first value is null.
  static CaptureResult captureChannel(const TDoubleParam *param,
                                      const std::string &name,
                                      double firstFrame, double lastFrame,
                                      double startFrame, Channel &channel,
                                      std::string *reason = 0);

  //! Stores the keys that the animated channels of the object have between
  //! the frames (included) in \e preset, replacing its channels. The preset is
  //! normalized: it starts at the earliest key. Channels that can't be stored
  //! are named in \e skipped. Returns false if no channel was stored.
  static bool captureFromKeys(const TStageObject *obj, int firstFrame,
                              int lastFrame, MotionPreset &preset,
                              std::vector<std::string> *skipped = 0);
  //@}

  //! \name Apply
  //@{
  //! Sets the keys of the channel in \e param, with the first key at \e frame
  //! plus its offset. See the class description for the rules. If \e base is
  //! not null, \e param is an offset layer curve added to \e base. Returns
  //! false, and leaves the curve as it is, if the curve can't take the keys;
  //! \e reason says why. It doesn't add an undo.
  bool applyChannel(const Channel &channel, TDoubleParam *param, double frame,
                    const TDoubleParam *base = 0,
                    std::string *reason      = 0) const;

  //! Applies the channels of the preset to the object, at \e frame: to its
  //! base curves, or to the curves of its offset layer \e layerIndex if it
  //! is not negative. If \e addUndo is true an undo is added to the
  //! TUndoManager for each curve; the caller opens the undo block and notifies
  //! the application.
  ApplyResult apply(TStageObject *obj, double frame, int layerIndex = -1,
                    bool addUndo = true) const;
  //@}

private:
  QString m_name;
  QString m_description;
  Anchor m_anchor;
  std::vector<Channel> m_channels;
};

#endif  // MOTIONPRESET_INCLUDED
