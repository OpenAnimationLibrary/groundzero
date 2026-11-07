#pragma once
// Included inside the comparator anonymous namespace after generatedControlXml.
// Include QCryptographicHash and zlib.h globally, not inside this header.
bool isScatterHoldoutGroup(const QString &group) {
  return QStringList{"scatterholdout-origin-x", "scatterholdout-origin-y",
      "scatterholdout-west-xy", "scatterholdout-half-xy", "scatterholdout-double-xy",
      "scatterholdout-small-xy", "scatterholdout-large-xy", "scatterholdout-pressure-half",
      "scatterholdout-pressure-full", "scatterholdout-pressure-ramp"}.contains(group);
}
bool scatterHoldoutCandidate(const QString &group) {
  require(isScatterHoldoutGroup(group), "Unknown focused scatter holdout group");
  return group == "scatterholdout-origin-x" || group == "scatterholdout-origin-y" ||
         group == "scatterholdout-west-xy";
}
QString scatterHoldoutName(const QString &group, int index) {
  return "GZ Original dry " + group + " " + QString("%1").arg(index, 3, 10, QLatin1Char('0'));
}
QJsonObject scatterHoldoutAuthored(const QString &group, int index) {
  require(isScatterHoldoutGroup(group) && index >= 0 && index < 102,
          "Focused scatter holdout index is outside its authored matrix");
  int diameter = 8, spacing = 4;
  double amount = 1;
  QString axes = "xy", direction = "east", pressureTrace;
  QJsonArray start{256, 1024}, end{1249, 1024}, rect{128, 896, 1280, 256};
  if (group == "scatterholdout-origin-x" || group == "scatterholdout-origin-y") {
    axes = group.endsWith("-x") ? "x" : "y";
    start = {768, 256}; end = {1761, 256}; rect = {640, 128, 1280, 256};
  } else if (group == "scatterholdout-west-xy") {
    direction = "west"; start = {1632, 1024}; end = {639, 1024}; rect = {512, 896, 1280, 256};
  } else if (group == "scatterholdout-half-xy") amount = .5;
  else if (group == "scatterholdout-double-xy") {
    amount = 2; spacing = 5; end = {1497, 1024}; rect = {128, 896, 1536, 256};
  } else if (group == "scatterholdout-small-xy") {
    diameter = 4; end = {753, 1024}; rect = {128, 896, 768, 256};
  } else if (group == "scatterholdout-large-xy") {
    diameter = 16; start = {32, 1024}; end = {2017, 1024}; rect = {0, 896, 2048, 256};
  } else if (group.startsWith("scatterholdout-pressure-")) {
    pressureTrace = group.mid(QString("scatterholdout-pressure-").size());
    start = {896, 1024}; end = {1152, 1024}; rect = {768, 896, 512, 256};
  }
  const bool enabled = index < 100;
  const QString curve = pressureTrace.isEmpty() ? "0,1;1,1;" : "0,0;1,1;";
  QJsonObject options;
  for (const auto &field : QStringList{"PressureSize", "PressureRatio", "PressureRotation",
           "PressureSpacing", "PressureScatter", "PressureMirror", "PressureOpacity", "PressureFlow",
           "Pressureh", "Pressures", "Pressurev", "OpacityUseCurve", "FlowUseCurve", "Texture/Pattern/Enabled"})
    options[field] = "false";
  options["PaintOpAction"] = "1"; options["Spacing/Isotropic"] = "false";
  options["brush_definition"] = QString(
      "<Brush type=\"auto_brush\" BrushVersion=\"2\" spacing=\"%1\" angle=\"0\" randomness=\"0\" "
      "density=\"1\" useAutoSpacing=\"0\" autoSpacingCoeff=\"1\"><MaskGenerator id=\"default\" "
      "type=\"circle\" diameter=\"%2\" ratio=\"1\" hfade=\"1\" vfade=\"1\" "
      "antialiasEdges=\"0\" spikes=\"2\" /></Brush>").arg(spacing).arg(diameter);
  options["PressureScatter"] = enabled ? "true" : "false";
  options["ScatterValue"] = QString::number(amount, 'g', 17);
  options["ScatterUseCurve"] = pressureTrace.isEmpty() ? "false" : "true";
  options["ScatterUseSameCurve"] = "true"; options["ScattercurveMode"] = "0";
  options["ScattercommonCurve"] = curve;
  options["ScatterSensor"] = "<params id=\"pressure\"><curve>" + curve + "</curve></params>";
  options["Scattering/AxisX"] = axes.contains('x') ? "true" : "false";
  options["Scattering/AxisY"] = axes.contains('y') ? "true" : "false";
  QJsonObject spec{{"version", 1}, {"configuration", group}, {"diameter", diameter},
      {"spacing", spacing}, {"amount", amount}, {"axes", axes}, {"direction", direction},
      {"start", start}, {"end", end}, {"rect", rect},
      {"nominalSlots", pressureTrace.isEmpty() ? QJsonValue(32) : QJsonValue(QJsonValue::Null)},
      {"enabled", enabled}, {"replicate", enabled ? index : index - 100},
      {"replicates", enabled ? 100 : 2}, {"bands", 3}, {"physicalTabletClaimed", false},
      {"sourceProfileVerified", false}, {"sourceRngLawIdentified", false}, {"pixelParityClaimed", false},
      {"candidateScope", scatterHoldoutCandidate(group) ? "current-static-component" : "not-admitted-scaling-or-dynamics"},
      {"qualification", "Original holdout authored for acquisition; no implementation admission or successful observation inferred"}};
  QJsonObject result{{"preset", scatterHoldoutName(group, index)}, {"sourceEngine", "paintbrush"},
      {"variant", "baseline"}, {"diameter", diameter}, {"spacing", spacing}, {"opacity", 1}, {"flow", 1},
      {"pressure", QJsonArray{1, 1}}, {"start", start}, {"end", end}, {"background", "transparent"},
      {"foregroundRGBA8", QJsonArray{173, 91, 47, 255}}, {"overrides", options}, {"expectUnchanged", false},
      {"delivery", "Node.paintLine"}, {"canvasCenter", QJsonArray{1024, 1024}},
      {"sourcePressureFlag", !pressureTrace.isEmpty()}};
  if (!pressureTrace.isEmpty()) {
    spec["pressureTrace"] = pressureTrace;
    QJsonArray events;
    for (int i = 0; i < 33; ++i) {
      const double pressure = pressureTrace == "half" ? .5 : pressureTrace == "full" ? 1 : .25 + .75 * i / 32.;
      events.append(QJsonObject{{"x", 896 + i * 8}, {"y", 1024}, {"pressure", pressure},
          {"tiltX", 0}, {"tiltY", 0}, {"rotation", 0}, {"timeUs", std::floor(i * 1000000. / 240 + .5)}});
    }
    result["delivery"] = "Qt tablet events"; result["events"] = events;
    result["pressure"] = QJsonArray{events.first().toObject().value("pressure"), events.last().toObject().value("pressure")};
    result["sensorProbe"] = QJsonObject{{"target", "Scatter"}, {"sensor", "pressure"}, {"enabled", enabled},
        {"label", "scatter holdout " + pressureTrace}, {"isotropic", false}, {"rect", rect}, {"verifyForeground", true},
        {"qualification", "Actual delivered QTabletEvent pressure and effective curve required; physical tablet and source normalization unqualified"}};
  }
  result["scatterHoldoutProbe"] = spec;
  return result;
}
void validateScatterHoldoutCase(const QString &group, int index, const QJsonObject &source) {
  require(source == scatterHoldoutAuthored(group, index),
          "Focused scatter holdout differs from its complete authored inputs: " + group + "/" + QString::number(index));
}

// Qt JSON normalizes 1.0 to 1. Preserve Python's original number and
// string lexemes while sorting object keys for its compact delivery digest.
struct ScatterHoldoutCanonicalJson {
  const QByteArray &input;
  QString context;
  int at = 0, nodes = 0;
  QJsonObject deliveryHashes;
  void whitespace() {
    while (at < input.size() && (input[at] == ' ' || input[at] == '\r' || input[at] == '\n' || input[at] == '\t')) ++at;
  }
  QByteArray string() {
    const int first = at;
    require(at < input.size() && input[at++] == '"', "Expected original JSON string: " + context);
    while (at < input.size()) {
      const unsigned char c = static_cast<unsigned char>(input[at++]);
      if (c == '"') return input.mid(first, at - first);
      require(c >= 32, "Unescaped original JSON control character: " + context);
      if (c == '\\') {
        require(at < input.size(), "Truncated original JSON escape: " + context);
        const char escape = input[at++];
        require(QByteArray("\"\\/bfnrtu").contains(escape), "Invalid original JSON escape: " + context);
        if (escape == 'u') for (int i = 0; i < 4; ++i) {
          require(at < input.size() && QByteArray("0123456789abcdefABCDEF").contains(input[at]),
                  "Invalid original JSON Unicode escape: " + context);
          ++at;
        }
      }
    }
    require(false, "Unclosed original JSON string: " + context); return {};
  }
  QByteArray value(int depth = 0) {
    whitespace();
    require(depth <= 48 && ++nodes <= 1000000 && at < input.size(), "Original JSON structural budget: " + context);
    const char token = input[at];
    if (token == '"') return string();
    if (token == '[') {
      ++at; whitespace(); QByteArray result("[");
      if (at < input.size() && input[at] == ']') { ++at; return result + ']'; }
      bool first = true;
      for (;;) {
        if (!first) result += ',';
        result += value(depth + 1); first = false; whitespace();
        require(at < input.size(), "Unclosed original JSON array: " + context);
        const char separator = input[at++];
        if (separator == ']') return result + ']';
        require(separator == ',', "Invalid original JSON array separator: " + context);
      }
    }
    if (token == '{') {
      ++at; whitespace();
      struct Field { QString name; QByteArray key, data; };
      std::vector<Field> fields; std::set<QString> names;
      if (at < input.size() && input[at] == '}') { ++at; return "{}"; }
      for (;;) {
        whitespace(); const QByteArray key = string();
        QJsonParseError error;
        const auto decoded = QJsonDocument::fromJson(QByteArray("[") + key + ']', &error).array();
        require(error.error == QJsonParseError::NoError && decoded.size() == 1 && decoded[0].isString(),
                "Invalid original JSON object key: " + context);
        const QString name = decoded[0].toString();
        require(names.insert(name).second, "Duplicate original JSON object key: " + context);
        whitespace(); require(at < input.size() && input[at++] == ':', "Missing original JSON colon: " + context);
        fields.push_back({name, key, value(depth + 1)}); whitespace();
        require(at < input.size(), "Unclosed original JSON object: " + context);
        const char separator = input[at++];
        if (separator == '}') break;
        require(separator == ',', "Invalid original JSON object separator: " + context);
      }
      std::sort(fields.begin(), fields.end(), [](const Field &a, const Field &b) { return a.name.toUtf8() < b.name.toUtf8(); });
      QByteArray result("{"), delivery, preset;
      for (const auto &field : fields) {
        if (result.size() > 1) result += ',';
        result += field.key + ':' + field.data;
        if (field.name == "deliveryReport") delivery = field.data;
        if (field.name == "preset") preset = field.data;
      }
      result += '}';
      if (!delivery.isEmpty() && !preset.isEmpty()) {
        const auto decoded = QJsonDocument::fromJson(QByteArray("[") + preset + ']').array();
        require(decoded.size() == 1 && decoded[0].isString() && !deliveryHashes.contains(decoded[0].toString()),
                "Duplicate original delivery identity: " + context);
        deliveryHashes[decoded[0].toString()] =
            QString::fromLatin1(QCryptographicHash::hash(delivery, QCryptographicHash::Sha256).toHex());
      }
      return result;
    }
    const int first = at;
    while (at < input.size() && input[at] != ',' && input[at] != ']' && input[at] != '}' &&
           input[at] != ' ' && input[at] != '\r' && input[at] != '\n' && input[at] != '\t') ++at;
    const auto primitive = input.mid(first, at - first);
    static const QRegularExpression number("^-?(?:0|[1-9][0-9]*)(?:\\.[0-9]+)?(?:[eE][+-]?[0-9]+)?$");
    require(primitive == "true" || primitive == "false" || primitive == "null" ||
                (primitive.size() <= 128 && number.match(QString::fromLatin1(primitive)).hasMatch()),
            "Invalid original JSON primitive: " + context);
    return primitive;
  }
};
struct ScatterHoldoutReportSnapshot {
  QString path; QByteArray digest; QJsonObject report, deliveryHashes;
};
const ScatterHoldoutReportSnapshot &scatterHoldoutReport(const QDir &root) {
  static ScatterHoldoutReportSnapshot snapshot;
  const QString path = root.filePath("output/oracle-status.json");
  QFile file(path);
  require(file.open(QIODevice::ReadOnly) && file.size() > 0 && file.size() <= 64 * 1024 * 1024,
          "Missing or oversized original scatter report");
  const QByteArray bytes = file.read(64 * 1024 * 1024 + 1);
  require(file.atEnd() && file.error() == QFile::NoError && bytes.size() <= 64 * 1024 * 1024,
          "Cannot read complete original scatter report");
  const auto digest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
  if (snapshot.path != path || snapshot.digest != digest) {
    ScatterHoldoutCanonicalJson parser{bytes, "focused scatter report"};
    parser.value(); parser.whitespace();
    require(parser.at == bytes.size(), "Trailing original scatter JSON input");
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(bytes, &error);
    require(error.error == QJsonParseError::NoError && document.isObject(), "Malformed original scatter report");
    snapshot = {path, digest, document.object(), parser.deliveryHashes};
  }
  return snapshot;
}
bool scatterHoldoutInteger(const QJsonValue &value, int expected) {
  return value.isDouble() && value.toDouble() == expected;
}

void scatterHoldoutDelivery(const QJsonObject &authored, const QJsonObject &delivery,
                            const QJsonObject &evidence, const QString &context) {
  const auto api = authored.value("delivery");
  require(delivery.value("mode") == api && evidence.value("deliveryInterface") == api &&
              (!delivery.contains("physicalTabletClaimed") || delivery.value("physicalTabletClaimed") == QJsonValue(false)),
          "Focused scatter delivery API or physical-tablet claim changed: " + context);
  require(delivery.value("events").isArray(), "Focused scatter delivered events are missing: " + context);
  const auto events = delivery.value("events").toArray();
  QJsonArray contacts;
  if (api.toString() == "Node.paintLine") {
    const auto pressure = authored.value("pressure").toArray();
    const QJsonArray expected{
        QJsonObject{{"type", "line-start"}, {"position", authored.value("start")}, {"pressure", pressure[0]}},
        QJsonObject{{"type", "line-end"}, {"position", authored.value("end")}, {"pressure", pressure[1]}}};
    require(events == expected, "Focused scatter Node endpoints or pressure differ: " + context);
    contacts = events;
  } else {
    require(api.toString() == "Qt tablet events" && events.size() == 37,
            "Focused scatter tablet sequence is incomplete: " + context);
    for (int i = 0; i < events.size(); ++i) {
      const int type = i == 0 ? 171 : i == 2 ? 92 : i == 35 ? 93 : i == 36 ? 172 : 87;
      require(events[i].isObject() && scatterHoldoutInteger(events[i].toObject().value("type"), type),
              "Focused scatter enter/hover/contact/release types changed: " + context);
      if (i >= 2 && i < 35) contacts.append(events[i]);
      else require(events[i].toObject().value("pressure").isDouble() &&
                       events[i].toObject().value("pressure").toDouble() == 0,
                   "Focused scatter non-contact pressure changed: " + context);
    }
    for (const auto &phase : QStringList{"stateBeforeProximity", "stateAfterProximity", "stateBeforeContact"}) {
      const auto state = delivery.value(phase).toObject();
      require(state.value("preset") == authored.value("preset") &&
                  state.value("disablePressure") == authored.value("sourcePressureFlag") &&
                  state.value("size").isDouble() && std::isfinite(state.value("size").toDouble()) &&
                  std::abs(state.value("size").toDouble() - authored.value("diameter").toDouble()) <= 1e-5,
              "Focused scatter source state changed before contact: " + context);
    }
    const auto wanted = authored.value("events").toArray();
    require(wanted.size() == 33 && contacts.size() == wanted.size(), "Focused scatter contact count differs: " + context);
    const double base = contacts[0].toObject().value("timeMs").toDouble();
    for (int i = 0; i < wanted.size(); ++i) {
      const auto observed = contacts[i].toObject(), expected = wanted[i].toObject();
      const auto image = observed.value("image").toArray(), tilt = observed.value("tilt").toArray();
      require(image.size() == 2 && tilt.size() == 2, "Focused scatter contact geometry missing: " + context);
      const QJsonArray values{image[0], image[1], observed.value("pressure"), tilt[0], tilt[1], observed.value("rotation")};
      const QJsonArray targets{expected.value("x"), expected.value("y"), expected.value("pressure"),
          expected.value("tiltX"), expected.value("tiltY"), expected.value("rotation")};
      for (int j = 0; j < values.size(); ++j)
        require(values[j].isDouble() && std::isfinite(values[j].toDouble()) &&
                    std::abs(values[j].toDouble() - targets[j].toDouble()) <= 1e-7,
                "Focused scatter actual tablet values changed: " + context);
      const auto clock = observed.value("timeMs");
      require(clock.isDouble() && std::isfinite(clock.toDouble()) && std::isfinite(base) &&
                  std::abs(clock.toDouble() - base - expected.value("timeUs").toDouble() / 1000) <= .500001,
              "Focused scatter actual tablet clock changed: " + context);
    }
  }
  require(evidence.value("deliveredContactEvents").isArray() &&
              evidence.value("deliveredContactEvents").toArray() == contacts,
          "Focused scatter evidence does not contain the actual contacts: " + context);
}

QJsonArray scatterHoldoutBrush(const QString &text, const QString &context) {
  require(text.size() <= 65536 && !text.contains("<!"), "Unsafe focused scatter brush XML: " + context);
  QXmlStreamReader reader(text); QJsonArray result;
  const QStringList numeric{"spacing", "angle", "randomness", "density",
      "diameter", "ratio", "hfade", "vfade", "antialiasEdges", "spikes"};
  while (!reader.atEnd()) {
    const auto token = reader.readNext();
    if (token == QXmlStreamReader::StartElement) {
      require(reader.namespaceUri().isEmpty() && reader.namespaceDeclarations().isEmpty(),
              "Focused scatter brush namespace changed: " + context);
      QJsonObject attributes;
      for (const auto &attribute : reader.attributes()) {
        require(attribute.namespaceUri().isEmpty(), "Focused scatter brush attribute namespace changed: " + context);
        const QString key = attribute.name().toString(), value = attribute.value().toString();
        if (numeric.contains(key)) {
          bool ok = false; const double number = value.toDouble(&ok);
          require(ok && std::isfinite(number), "Focused scatter brush numeric field changed: " + context);
          attributes[key] = number;
        } else attributes[key] = value;
      }
      result.append(QJsonArray{"start", reader.name().toString(), attributes});
    } else if (token == QXmlStreamReader::EndElement) result.append(QJsonArray{"end"});
    else if (token == QXmlStreamReader::Characters)
      require(reader.isWhitespace(), "Focused scatter brush contains active text: " + context);
    else require(token == QXmlStreamReader::StartDocument || token == QXmlStreamReader::EndDocument,
                 "Unexpected focused scatter brush XML token: " + context);
  }
  require(!reader.hasError() && !result.isEmpty(), "Malformed focused scatter brush XML: " + context);
  return result;
}

QByteArray scatterHoldoutPixels(const QJsonObject &evidence, const QJsonArray &rect, const QString &context) {
  const int size = rect[2].toInt() * rect[3].toInt() * 4;
  const QString text = evidence.value("data").toString();
  require(evidence.value("data").isString() && text.size() <= size * 2 + 1024 &&
              text.size() % 4 == 0 && QRegularExpression("^[A-Za-z0-9+/]*={0,2}$").match(text).hasMatch(),
          "Invalid focused scatter compressed pixel encoding: " + context);
  const auto encoded = text.toLatin1(), compressed = QByteArray::fromBase64(encoded);
  require(compressed.toBase64() == encoded && !compressed.isEmpty(),
          "Noncanonical focused scatter base64: " + context);
  QByteArray pixels(size + 1, '\0'); z_stream stream{};
  stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(compressed.constData()));
  stream.avail_in = uInt(compressed.size());
  stream.next_out = reinterpret_cast<Bytef *>(pixels.data()); stream.avail_out = uInt(pixels.size());
  require(inflateInit(&stream) == Z_OK, "Cannot initialize focused scatter decoder: " + context);
  const int decoded = inflate(&stream, Z_FINISH);
  const bool complete = decoded == Z_STREAM_END && stream.total_out == uLong(size) && stream.avail_in == 0;
  const int ended = inflateEnd(&stream);
  require(complete && ended == Z_OK,
          "Incomplete, oversized or concatenated focused scatter pixels: " + context);
  pixels.resize(size); return pixels;
}
QImage scatterHoldoutImage(const QString &path, const QString &context) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly) && file.size() >= 33 && file.size() <= 64 * 1024 * 1024,
          "Missing or oversized focused scatter PNG: " + context);
  const auto data = file.read(64 * 1024 * 1024 + 1);
  auto word = [&](int offset) {
    unsigned value = 0;
    for (int i = 0; i < 4; ++i) value = value * 256 + static_cast<unsigned char>(data[offset + i]);
    return value;
  };
  require(file.atEnd() && file.error() == QFile::NoError && data.size() >= 33 &&
              data.left(8) == QByteArray::fromHex("89504e470d0a1a0a") &&
              data.mid(12, 4) == "IHDR" && word(16) == 2048 && word(20) == 2048,
          "Focused scatter PNG geometry changed before decoding: " + context);
  const auto image = QImage::fromData(data, "PNG");
  require(!image.isNull() && image.width() == 2048 && image.height() == 2048,
          "Cannot decode complete focused scatter source PNG: " + context);
  return image.convertToFormat(QImage::Format_ARGB32);
}
void validateScatterHoldoutSource(const QDir &root, const QString &group, const QJsonObject &source,
                                  const QJsonObject &authored, const QString &context) {
  const QString name = authored.value("preset").toString();
  bool indexOk = false; const int index = name.right(3).toInt(&indexOk);
  require(indexOk, "Focused scatter canonical index missing: " + context);
  validateScatterHoldoutCase(group, index, authored);
  for (auto field = authored.begin(); field != authored.end(); ++field)
    require(source.value(field.key()) == field.value(),
            "Focused scatter authored input changed: " + context + "/" + field.key());
  const auto &snapshot = scatterHoldoutReport(root);
  const auto report = snapshot.report;
  const auto records = report.value("records").toArray(), planned = report.value("plannedExecutionOrder").toArray();
  QJsonArray names, activeOrder; std::set<QString> unique;
  for (int i = 0; i < 102; ++i) names.append(scatterHoldoutName(group, i));
  require(report.value("status") == QJsonValue("complete") && report.value("version") == QJsonValue("5.3.4.1") &&
              records.size() == 102 && planned.size() == 102 &&
              report.value("intendedPresetOrder").toArray() == names &&
              report.value("actualExecutionOrder").toArray() == planned && records[index].toObject() == source,
          "Focused scatter complete original report or canonical row changed: " + context);
  for (const auto &entry : planned) {
    require(entry.isString() && names.contains(entry) && unique.insert(entry.toString()).second,
            "Focused scatter plan is not a complete unique permutation: " + context);
    if (entry.toString().right(3).toInt() < 100) activeOrder.append(entry);
  }
  QJsonArray canonicalActive; for (int i = 0; i < 100; ++i) canonicalActive.append(names[i]);
  require(activeOrder != canonicalActive, "Focused scatter enabled strokes were not randomized: " + context);
  static const QRegularExpression digest("^[0-9a-f]{64}$");
  for (int i = 0; i < records.size(); ++i) {
    const auto row = records[i].toObject(); const QString id = names[i].toString();
    int ordinal = -1; for (int j = 0; j < planned.size(); ++j) if (planned[j].toString() == id) ordinal = j;
    const int repeats = i < 100 ? 1 : 2;
    const auto raw = row.value("repeatSha256Pixels").toArray();
    const QString canonical = row.value("sha256RepeatPixels").toString();
    QJsonArray expectedCanonical; for (int j = 0; j < repeats; ++j) expectedCanonical.append(canonical);
    require(row.value("preset") == names[i] && scatterHoldoutInteger(row.value("intendedOrdinal"), i) &&
                scatterHoldoutInteger(row.value("executionOrdinal"), ordinal) && raw.size() == repeats &&
                raw[0] == row.value("sha256Pixels") && digest.match(canonical).hasMatch() &&
                row.value("repeatSemantics") == QJsonValue("transparent-rgb-zero-v1") &&
                row.value("repeatSha256CanonicalPixels").toArray() == expectedCanonical &&
                row.value("repeatImages").isArray() && row.value("repeatImages").toArray().size() == repeats - 1 &&
                row.value("repeatDifferences").isArray() && row.value("repeatDifferences").toArray().size() == repeats - 1,
            "Focused scatter independent/control repeat or execution policy changed: " + context);
    for (const auto &hash : raw)
      require(hash.isString() && digest.match(hash.toString()).hasMatch(), "Invalid focused scatter repeat digest: " + context);
    for (const auto &difference : row.value("repeatDifferences").toArray())
      require(scatterHoldoutInteger(difference.toObject().value("visibleOrAlphaChangedPixels"), 0),
              "Focused scatter disabled control did not repeat: " + context);
  }
  const auto evidence = source.value("scatterHoldoutEvidence").toObject();
  const QStringList evidenceKeys{"version", "implementationStatus", "candidateExpected",
      "effectiveControls", "effectiveBrushDefinition", "deliveredContactEvents", "deliveryInterface", "deliverySha256",
      "sourceReferenceVerified", "sourceSemanticallySupported", "sourceParityQualified", "sourceRngLawIdentified",
      "pixelParityClaimed", "physicalTabletClaimed", "rect", "exteriorAlphaChecked", "encoding", "data",
      "sha256Pixels", "nonzeroAlphaPixels"};
  require(evidence.size() == evidenceKeys.size(), "Focused scatter evidence schema changed: " + context);
  for (const auto &key : evidenceKeys)
    require(evidence.contains(key), "Missing focused scatter evidence field: " + context + "/" + key);
  const bool candidate = scatterHoldoutCandidate(group);
  require(evidence.value("version") == QJsonValue(1) &&
              evidence.value("candidateExpected") == QJsonValue(candidate) &&
              evidence.value("implementationStatus") == QJsonValue(candidate
                  ? "measured-static-scatter-candidate" : "open-scatter-domain-acquisition-only") &&
              evidence.value("exteriorAlphaChecked") == QJsonValue(true),
          "Focused scatter implementation/evidence flags changed: " + context);
  for (const auto &flag : QStringList{"sourceReferenceVerified", "sourceSemanticallySupported", "sourceParityQualified",
           "sourceRngLawIdentified", "pixelParityClaimed", "physicalTabletClaimed"})
    require(evidence.value(flag) == QJsonValue(false),
            "Focused scatter observation gained qualification: " + context + "/" + flag);
  const auto delivery = source.value("deliveryReport").toObject();
  require(evidence.value("deliverySha256").isString() &&
              evidence.value("deliverySha256") == snapshot.deliveryHashes.value(name) &&
              digest.match(evidence.value("deliverySha256").toString()).hasMatch(),
          "Focused scatter canonical actual delivery digest changed: " + context);
  scatterHoldoutDelivery(authored, delivery, evidence, context);

  const QString stem = QString(name).replace(' ', '_');
  require(source.value("effectivePresetXml") == QJsonValue(stem + ".effective.xml") &&
              source.value("image") == QJsonValue(stem + ".png"),
          "Focused scatter capture filename changed: " + context);
  QFile xml(root.filePath("output/" + stem + ".effective.xml"));
  require(xml.open(QIODevice::ReadOnly) && xml.size() > 0 && xml.size() <= 1024 * 1024,
          "Missing bounded focused scatter effective preset: " + context);
  const auto bytes = xml.read(1024 * 1024 + 1);
  require(xml.atEnd() && xml.error() == QFile::NoError && bytes.size() <= 1024 * 1024 &&
              source.value("effectivePresetSha256") ==
                  QJsonValue(QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())),
          "Focused scatter actual preset digest changed: " + context);
  const auto fields = sourceEffectivePresetXml(bytes, name, context);
  auto expectedControls = authored.value("overrides").toObject();
  expectedControls.remove("brush_definition");
  for (const auto &entry : std::initializer_list<std::pair<QString, QString>>{
      {"paintop", "paintbrush"}, {"CompositeOp", "normal"}, {"EraserMode", "false"},
      {"OpacityValue", "1"}, {"FlowValue", "1"}, {"MergedPaint", "false"},
      {"PaintOpSettings/isAirbrushing", "false"}, {"PaintOpSettings/ignoreSpacing", "false"},
      {"PaintOpSettings/updateSpacingBetweenDabs", "false"}, {"KisPrecisionOption/AutoPrecisionEnabled", "false"},
      {"KisPrecisionOption/precisionLevel", "5"}}) expectedControls[entry.first] = entry.second;
  const auto actualControls = evidence.value("effectiveControls").toObject();
  require(evidence.value("effectiveControls").isObject() && actualControls.size() == expectedControls.size() &&
              evidence.value("effectiveBrushDefinition") == fields.value("brush_definition") &&
              scatterHoldoutBrush(fields.value("brush_definition").toString(), context) ==
                  scatterHoldoutBrush(authored.value("overrides").toObject().value("brush_definition").toString(), context),
          "Focused scatter actual brush or field set changed: " + context);
  for (auto field = expectedControls.begin(); field != expectedControls.end(); ++field) {
    const auto actual = fields.value(field.key());
    require(actual.isString() && actual.toString().size() <= 65536 && actualControls.value(field.key()) == actual,
            "Focused scatter controls are not bound to actual retained XML: " + context + "/" + field.key());
    if (field.key().endsWith("Sensor")) {
      require(!actual.toString().contains("xmlns") &&
                  generatedControlXml(actual.toString(), context) ==
                      generatedControlXml(field.value().toString(), context),
              "Focused scatter actual sensor XML changed: " + context);
    } else if (field.key().endsWith("Value") || field.key() == "PaintOpAction" ||
               field.key() == "KisPrecisionOption/precisionLevel") {
      bool ok = false, expectedOk = false;
      const double value = actual.toString().toDouble(&ok), target = field.value().toString().toDouble(&expectedOk);
      require(ok && expectedOk && std::isfinite(value) && std::isfinite(target) && std::abs(value - target) <= 1e-7,
              "Focused scatter actual numeric option changed: " + context + "/" + field.key());
    } else require(actual == field.value(), "Focused scatter actual option changed: " + context + "/" + field.key());
  }
  const auto rect = authored.value("scatterHoldoutProbe").toObject().value("rect").toArray();
  require(rect.size() == 4 && evidence.value("rect").isArray() &&
              evidence.value("rect").toArray() == rect &&
              evidence.value("encoding") == QJsonValue("zlib-base64-bgra8-row-major"),
          "Focused scatter pixel geometry changed: " + context);
  const auto pixels = scatterHoldoutPixels(evidence, rect, context);
  require(evidence.value("sha256Pixels") ==
              QJsonValue(QString::fromLatin1(QCryptographicHash::hash(pixels, QCryptographicHash::Sha256).toHex())),
          "Focused scatter retained crop digest changed: " + context);
  const auto image = scatterHoldoutImage(root.filePath("output/" + stem + ".png"), context);
  QByteArray actualPixels; actualPixels.reserve(pixels.size()); int nonzero = 0;
  const int left = rect[0].toInt(), top = rect[1].toInt(), width = rect[2].toInt(), height = rect[3].toInt();
  for (int y = 0; y < 2048; ++y) for (int x = 0; x < 2048; ++x) {
    const auto pixel = image.pixel(x, y);
    if (x >= left && x < left + width && y >= top && y < top + height) {
      actualPixels.append(char(qBlue(pixel))); actualPixels.append(char(qGreen(pixel)));
      actualPixels.append(char(qRed(pixel))); actualPixels.append(char(qAlpha(pixel)));
      nonzero += qAlpha(pixel) != 0;
    } else require(qAlpha(pixel) == 0, "Focused scatter source alpha escaped its authored capture: " + context);
  }
  require(actualPixels == pixels && nonzero > 0 &&
              scatterHoldoutInteger(evidence.value("nonzeroAlphaPixels"), nonzero),
          "Focused scatter crop does not match actual complete source PNG: " + context);
  if (index >= 100) {
    const auto repeated = source.value("repeatImages").toArray()[0].toObject();
    const QString expectedName = stem + ".repeat-1.png";
    require(repeated.value("image") == QJsonValue(expectedName),
            "Focused scatter disabled repeat filename changed: " + context);
    QFile repeatFile(root.filePath("output/" + expectedName));
    require(repeatFile.open(QIODevice::ReadOnly) && repeatFile.size() >= 33 &&
                repeatFile.size() <= 64 * 1024 * 1024,
            "Missing focused scatter disabled repeat PNG: " + context);
    const auto repeatedBytes = repeatFile.read(64 * 1024 * 1024 + 1);
    require(repeatFile.atEnd() && repeatFile.error() == QFile::NoError &&
                repeatedBytes.size() <= 64 * 1024 * 1024 &&
                repeated.value("sha256File") == QJsonValue(QString::fromLatin1(
                    QCryptographicHash::hash(repeatedBytes, QCryptographicHash::Sha256).toHex())),
            "Focused scatter disabled repeat file digest changed: " + context);
    const auto repeatImage = scatterHoldoutImage(root.filePath("output/" + expectedName), context);
    for (int y = 0; y < 2048; ++y) for (int x = 0; x < 2048; ++x) {
      const auto first = image.pixel(x, y), second = repeatImage.pixel(x, y);
      require(qAlpha(first) == qAlpha(second) &&
                  (qAlpha(first) == 0 || first == second),
              "Focused scatter disabled control visible pixels did not repeat: " + context);
    }
  }
}
