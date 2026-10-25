#pragma once

#ifndef POSE_LIBRARY_INCLUDED
#define POSE_LIBRARY_INCLUDED

#include "tcommon.h"
#include "toonz/tstageobjectid.h"

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
class TXsheet;
class TDoubleParam;

//=============================================================================
//! A scene-level library of additive rig poses.
/*!
  A pose stores per-channel \a offsets relative to a \a base pose. Applying
  a pose with weight w at a frame keys each affected channel to
  (current value + w * offset), so several poses can be stacked on top of
  each other.

  Channels are identified by a stage object id and a channel name. Stage
  object channels use the same names TStageObject uses in scene files
  ("x", "y", "z", "so", "sx", "sy", "sc", "rot", "pos", "shx", "shy").
  Plastic skeleton vertex parameters use "plasticAngle:<vertex name>" and
  "plasticDistance:<vertex name>".

  Poses refer to the stage objects of the top-level xsheet.
*/
class DVAPI PoseLibrary {
public:
  struct Entry {
    TStageObjectId m_objectId;
    std::string m_channel;
    double m_value;  //!< Absolute value in the base, offset in a pose.

    Entry() : m_value(0) {}
    Entry(const TStageObjectId &id, const std::string &channel, double value)
        : m_objectId(id), m_channel(channel), m_value(value) {}
  };

  struct Pose {
    QString m_name;
    std::vector<Entry> m_entries;
  };

public:
  PoseLibrary() {}

  bool isEmpty() const { return m_base.empty() && m_poses.empty(); }
  void clear();

  const std::vector<Entry> &getBase() const { return m_base; }
  void setBase(const std::vector<Entry> &base) { m_base = base; }

  int getPoseCount() const { return (int)m_poses.size(); }
  const Pose &getPose(int index) const { return m_poses[index]; }
  void addPose(const Pose &pose) { m_poses.push_back(pose); }
  void removePose(int index);

  void saveData(TOStream &os) const;
  void loadData(TIStream &is);

  //! Returns the animatable parameter of the specified channel, or 0 if the
  //! object or channel does not exist. Stage objects are never created.
  static TDoubleParam *getChannelParam(TXsheet *xsh, const TStageObjectId &id,
                                       const std::string &channel);

  //! Returns the TStageObject::Channel (as an int) of a pose channel name, or
  //! -1 if it is not a stage object channel ("plasticAngle:..." ones).
  static int getStageObjectChannel(const std::string &channel);

  //! Returns the curve of the pose channel in the offset layer of the object
  //! (see TStageObject::OffsetLayer), or 0 if the object, the layer or the
  //! channel do not exist. The Path channel has no offset curve.
  static TDoubleParam *getOffsetLayerParam(TXsheet *xsh,
                                           const TStageObjectId &id,
                                           const std::wstring &layerName,
                                           const std::string &channel);

  //! Appends the current values of every pose channel of the specified
  //! objects at the given frame.
  static void captureValues(TXsheet *xsh,
                            const std::vector<TStageObjectId> &ids,
                            double frame, std::vector<Entry> &values);

private:
  std::vector<Entry> m_base;
  std::vector<Pose> m_poses;
};

#endif  // POSE_LIBRARY_INCLUDED
