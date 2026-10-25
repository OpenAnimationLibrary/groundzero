#include "toonz/poselibrary.h"

// TnzLib includes
#include "toonz/txsheet.h"
#include "toonz/tstageobject.h"
#include "toonz/tstageobjecttree.h"

// TnzExt includes
#include "ext/plasticskeletondeformation.h"

// TnzCore includes
#include "tdoubleparam.h"
#include "texception.h"
#include "tstream.h"

namespace {

//-----------------------------------------------------------------------------

// Stage object channel names, matching the tags TStageObject::saveData uses.
const struct {
  TStageObject::Channel m_channel;
  const char *m_name;
} stageObjectChannels[] = {
    {TStageObject::T_X, "x"},        {TStageObject::T_Y, "y"},
    {TStageObject::T_Z, "z"},        {TStageObject::T_SO, "so"},
    {TStageObject::T_ScaleX, "sx"},  {TStageObject::T_ScaleY, "sy"},
    {TStageObject::T_Scale, "sc"},   {TStageObject::T_Angle, "rot"},
    {TStageObject::T_Path, "pos"},   {TStageObject::T_ShearX, "shx"},
    {TStageObject::T_ShearY, "shy"},
};

const std::string plasticAnglePrefix    = "plasticAngle:";
const std::string plasticDistancePrefix = "plasticDistance:";

//-----------------------------------------------------------------------------

bool startsWith(const std::string &s, const std::string &prefix) {
  return s.compare(0, prefix.size(), prefix) == 0;
}

//-----------------------------------------------------------------------------

void saveEntries(TOStream &os, const std::vector<PoseLibrary::Entry> &entries) {
  for (const PoseLibrary::Entry &entry : entries) {
    os << entry.m_objectId.toString() << entry.m_channel << entry.m_value;
    os.cr();
  }
}

//-----------------------------------------------------------------------------

void loadEntries(TIStream &is, std::vector<PoseLibrary::Entry> &entries) {
  entries.clear();
  while (!is.eos()) {
    std::string idStr, channel;
    double value = 0;
    is >> idStr >> channel >> value;
    TStageObjectId id = toStageObjectId(idStr);
    if (id != TStageObjectId::NoneId && !channel.empty())
      entries.push_back(PoseLibrary::Entry(id, channel, value));
  }
}

}  // namespace

//=============================================================================
// PoseLibrary
//-----------------------------------------------------------------------------

void PoseLibrary::clear() {
  m_base.clear();
  m_poses.clear();
}

//-----------------------------------------------------------------------------

void PoseLibrary::removePose(int index) {
  if (0 <= index && index < (int)m_poses.size())
    m_poses.erase(m_poses.begin() + index);
}

//-----------------------------------------------------------------------------
/*! Writes the library as:
    <base> id channel value ... </base>
    <pose> <name> "name" </name> <entries> id channel offset ... </entries>
    </pose> ...
*/
void PoseLibrary::saveData(TOStream &os) const {
  if (!m_base.empty()) {
    os.openChild("base");
    saveEntries(os, m_base);
    os.closeChild();
  }
  for (const Pose &pose : m_poses) {
    os.openChild("pose");
    os.child("name") << pose.m_name.toStdString();
    os.openChild("entries");
    saveEntries(os, pose.m_entries);
    os.closeChild();
    os.closeChild();
  }
}

//-----------------------------------------------------------------------------

void PoseLibrary::loadData(TIStream &is) {
  clear();
  std::string tagName;
  while (is.matchTag(tagName)) {
    if (tagName == "base") {
      loadEntries(is, m_base);
    } else if (tagName == "pose") {
      Pose pose;
      while (is.matchTag(tagName)) {
        if (tagName == "name") {
          std::string name;
          is >> name;
          pose.m_name = QString::fromStdString(name);
        } else if (tagName == "entries") {
          loadEntries(is, pose.m_entries);
        } else {
          is.skipCurrentTag();
          continue;
        }
        if (!is.matchEndTag()) throw TException(tagName + " : missing end tag");
      }
      m_poses.push_back(pose);
    } else {
      // Ignore data written by a newer version.
      is.skipCurrentTag();
      continue;
    }
    if (!is.matchEndTag()) throw TException(tagName + " : missing end tag");
  }
}

//-----------------------------------------------------------------------------

TDoubleParam *PoseLibrary::getChannelParam(TXsheet *xsh,
                                           const TStageObjectId &id,
                                           const std::string &channel) {
  if (!xsh) return 0;
  TStageObject *obj = xsh->getStageObjectTree()->getStageObject(id, false);
  if (!obj) return 0;

  for (const auto &soChannel : stageObjectChannels)
    if (channel == soChannel.m_name) return obj->getParam(soChannel.m_channel);

  int param = -1;
  std::string vxName;
  if (startsWith(channel, plasticAnglePrefix)) {
    param  = SkVD::ANGLE;
    vxName = channel.substr(plasticAnglePrefix.size());
  } else if (startsWith(channel, plasticDistancePrefix)) {
    param  = SkVD::DISTANCE;
    vxName = channel.substr(plasticDistancePrefix.size());
  }
  if (param < 0) return 0;

  const PlasticSkeletonDeformationP &sd = obj->getPlasticSkeletonDeformation();
  if (!sd) return 0;
  SkVD *vd = sd->vertexDeformation(QString::fromStdString(vxName));
  return vd ? vd->m_params[param].getPointer() : 0;
}

//-----------------------------------------------------------------------------

int PoseLibrary::getStageObjectChannel(const std::string &channel) {
  for (const auto &soChannel : stageObjectChannels)
    if (channel == soChannel.m_name) return (int)soChannel.m_channel;
  return -1;
}

//-----------------------------------------------------------------------------

TDoubleParam *PoseLibrary::getOffsetLayerParam(TXsheet *xsh,
                                               const TStageObjectId &id,
                                               const std::wstring &layerName,
                                               const std::string &channel) {
  if (!xsh) return 0;
  TStageObject *obj = xsh->getStageObjectTree()->getStageObject(id, false);
  if (!obj) return 0;

  int stageChannel = getStageObjectChannel(channel);
  if (stageChannel < 0) return 0;
  return obj->getOffsetParam(obj->findOffsetLayer(layerName),
                             (TStageObject::Channel)stageChannel);
}

//-----------------------------------------------------------------------------

void PoseLibrary::captureValues(TXsheet *xsh,
                                const std::vector<TStageObjectId> &ids,
                                double frame, std::vector<Entry> &values) {
  if (!xsh) return;
  TStageObjectTree *tree = xsh->getStageObjectTree();
  for (const TStageObjectId &id : ids) {
    TStageObject *obj = tree->getStageObject(id, false);
    if (!obj) continue;

    for (const auto &soChannel : stageObjectChannels) {
      TDoubleParam *param = obj->getParam(soChannel.m_channel);
      if (param)
        values.push_back(Entry(id, soChannel.m_name, param->getValue(frame)));
    }

    const PlasticSkeletonDeformationP &sd =
        obj->getPlasticSkeletonDeformation();
    if (!sd) continue;
    SkD::vd_iterator vdt, vdEnd;
    sd->vertexDeformations(vdt, vdEnd);
    for (; vdt != vdEnd; ++vdt) {
      const std::pair<const QString *, SkVD *> &vd = *vdt;
      std::string vxName                           = vd.first->toStdString();
      values.push_back(
          Entry(id, plasticAnglePrefix + vxName,
                vd.second->m_params[SkVD::ANGLE]->getValue(frame)));
      values.push_back(
          Entry(id, plasticDistancePrefix + vxName,
                vd.second->m_params[SkVD::DISTANCE]->getValue(frame)));
    }
  }
}
