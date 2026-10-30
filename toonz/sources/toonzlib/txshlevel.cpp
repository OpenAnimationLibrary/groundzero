

#include "toonz/txshlevel.h"
#include "toonz/txshleveltypes.h"
#include "toonz/hook.h"
#include "tstream.h"
#include <QJsonArray>
#include <QJsonDocument>

//-----------------------------------------------------------------------------

TXshLevel::TXshLevel(ClassCode code, std::wstring name)
    : TSmartObject(code)
    , m_name(name)
    , m_type(UNKNOWN_XSHLEVEL)
    , m_hookSet(new HookSet())
    , m_scene(0) {
  updateShortName();
}

//-----------------------------------------------------------------------------

TXshLevel::~TXshLevel() { delete m_hookSet; }

void TXshLevel::loadCastTags(TIStream &is) {
  m_castTags.clear();
  m_castCoverSet = false;
  std::string encoded;
  if (is.getTagParam("castCover", encoded) && encoded.size() <= 1024) {
    const auto cover = QJsonDocument::fromJson(QByteArray::fromStdString(encoded)).array();
    if (cover.size() == 2 && cover[0].isDouble() && cover[1].isString()) {
      const int number = cover[0].toInt(TFrameId::EMPTY_FRAME);
      const QString letter = cover[1].toString();
      if ((number >= 0 || number == TFrameId::NO_FRAME) && letter.size() <= 32)
        setCastCover(TFrameId(number, letter));
    }
  }
  encoded.clear();
  if (!is.getTagParam("castTags", encoded) || encoded.size() > 16384) return;
  const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(encoded));
  if (!doc.isArray()) return;
  for (const QJsonValue &value : doc.array()) {
    const QString tag = value.toString().trimmed().left(80);
    if (!tag.isEmpty() && !m_castTags.contains(tag, Qt::CaseInsensitive))
      m_castTags.append(tag);
    if (m_castTags.size() == 32) break;
  }
}

void TXshLevel::saveCastPath(TOStream &os, const TFilePath &path) const {
  if (m_castTags.isEmpty() && !m_castCoverSet) { os.child("path") << path; return; }
  std::map<std::string, std::string> attributes;
  if (!m_castTags.isEmpty()) attributes["castTags"] = QJsonDocument(QJsonArray::fromStringList(m_castTags))
                               .toJson(QJsonDocument::Compact).toStdString();
  if (m_castCoverSet)
    attributes["castCover"] = QJsonDocument(QJsonArray{m_castCover.getNumber(), m_castCover.getLetter()})
                                  .toJson(QJsonDocument::Compact).toStdString();
  os.openChild("path", attributes);
  os << path;
  os.closeChild();
}

//-----------------------------------------------------------------------------

void TXshLevel::setScene(ToonzScene *scene) { m_scene = scene; }

//-----------------------------------------------------------------------------

void TXshLevel::setName(std::wstring name) {
  m_name = name;
  updateShortName();
}

//-----------------------------------------------------------------------------

void TXshLevel::updateShortName() {
  if (m_name.size() < 5)
    m_shortName = m_name;
  else
    m_shortName = m_name.substr(0, 4) + L"~";
}
