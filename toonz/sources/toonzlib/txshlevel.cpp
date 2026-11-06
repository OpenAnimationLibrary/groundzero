

#include "toonz/txshlevel.h"
#include "toonz/txshleveltypes.h"
#include "toonz/hook.h"
#include "tstream.h"
#include "toonz/toonzscene.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

//-----------------------------------------------------------------------------

TXshLevel::TXshLevel(ClassCode code, std::wstring name)
    : TSmartObject(code)
    , m_name(name)
    , m_type(UNKNOWN_XSHLEVEL)
    , m_hookSet(new HookSet())
    , m_scene(0) {
  updateShortName();
  renewCastId();
}

//-----------------------------------------------------------------------------

TXshLevel::~TXshLevel() { delete m_hookSet; }

void TXshLevel::renewCastId() {
  m_castId = QUuid::createUuid().toString(QUuid::WithoutBraces);
}

TXshLevel::LibraryProvenance::Relationship TXshLevel::getLibraryRelationship() const {
  const auto &origin = m_libraryProvenance;
  if (origin.relationship != LibraryProvenance::Linked) return origin.relationship;
  if (getPath().isEmpty() || origin.sourceAssetPath.isEmpty()) return LibraryProvenance::Replaced;
  auto normalized = [](const QString &path) {
    QFileInfo file(path);
    QString result = file.canonicalFilePath();
    if (result.isEmpty()) result = QDir::cleanPath(file.absoluteFilePath());
#ifdef Q_OS_WIN
    result = result.toCaseFolded();
#endif
    return result;
  };
  const auto actual = m_scene ? m_scene->decodeFilePath(getPath()) : getPath();
  return normalized(actual.getQString()) == normalized(origin.sourceAssetPath)
             ? LibraryProvenance::Linked : LibraryProvenance::Replaced;
}

void TXshLevel::detachLibrarySource(LibraryProvenance::Relationship relationship) {
  // Copying an already replaced source must not claim it is still Library data.
  if (relationship == LibraryProvenance::LocalCopy &&
      m_libraryProvenance.relationship == LibraryProvenance::Replaced) return;
  if (m_libraryProvenance.relationship != LibraryProvenance::NotRecorded)
    m_libraryProvenance.relationship = relationship;
}

void TXshLevel::loadCastTags(TIStream &is) {
  m_castTags.clear();
  m_castCoverSet = false;
  m_libraryProvenance = LibraryProvenance();
  m_sourceExpectedOnDisk = true;  // Legacy native scenes describe existing sources.
  std::string encoded;
  if (is.getTagParam("castId", encoded) && encoded.size() <= 64 &&
      !QUuid(QString::fromStdString(encoded)).isNull())
    m_castId = QUuid(QString::fromStdString(encoded)).toString(QUuid::WithoutBraces);
  encoded.clear();
  if (is.getTagParam("castUnsaved", encoded)) m_sourceExpectedOnDisk = encoded != "1";
  encoded.clear();
  if (is.getTagParam("libraryOrigin", encoded) && encoded.size() <= 32768) {
    const auto object = QJsonDocument::fromJson(QByteArray::fromStdString(encoded)).object();
    LibraryProvenance origin;
    origin.itemId = object.value("id").toString();
    origin.itemPath = object.value("item").toString();
    origin.kind = object.value("kind").toString();
    origin.sourceAssetPath = object.value("source").toString();
    origin.revision = object.value("revision").toInt();
    const int relation = object.value("relationship").toInt();
    if (!origin.itemId.isEmpty() && origin.itemId.size() <= 256 &&
        origin.itemPath.size() <= 4096 && origin.sourceAssetPath.size() <= 4096 &&
        (origin.kind == "rig" || origin.kind == "symbol") && origin.revision > 0 &&
        relation >= LibraryProvenance::Linked && relation <= LibraryProvenance::Replaced) {
      origin.relationship = LibraryProvenance::Relationship(relation);
      m_libraryProvenance = origin;
    }
  }
  encoded.clear();
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

std::map<std::string, std::string> TXshLevel::getCastAttributes() const {
  std::map<std::string, std::string> attributes;
  attributes["castId"] = m_castId.toStdString();
  if (!m_sourceExpectedOnDisk) attributes["castUnsaved"] = "1";
  const auto &origin = m_libraryProvenance;
  if (origin.relationship != LibraryProvenance::NotRecorded) {
    QJsonObject object{{"id", origin.itemId}, {"item", origin.itemPath},
                       {"kind", origin.kind}, {"source", origin.sourceAssetPath},
                       {"revision", origin.revision}, {"relationship", int(origin.relationship)}};
    attributes["libraryOrigin"] = QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString();
  }
  if (!m_castTags.isEmpty()) attributes["castTags"] = QJsonDocument(QJsonArray::fromStringList(m_castTags))
                               .toJson(QJsonDocument::Compact).toStdString();
  if (m_castCoverSet)
    attributes["castCover"] = QJsonDocument(QJsonArray{m_castCover.getNumber(), m_castCover.getLetter()})
                                  .toJson(QJsonDocument::Compact).toStdString();
  return attributes;
}

void TXshLevel::saveCastPath(TOStream &os, const TFilePath &path) const {
  os.openChild("path", getCastAttributes());
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
