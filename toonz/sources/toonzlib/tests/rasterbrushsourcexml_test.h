#pragma once
// Included beside the preset lifecycle groups; fixtures exercise source XML
// admission without treating local-name matches as a qualified source schema.
void testSourceGeneratedScatterXmlAdmission() {
  const auto rewrite = [](const QByteArray &source,
                          const std::vector<std::pair<QString, QString>> &changes) {
    QImage image = QImage::fromData(source, "PNG");
    require(!image.isNull(), "Cannot read source XML fixture");
    QString xml = image.text("preset");
    for (const auto &change : changes) {
      const auto before = change.first.toHtmlEscaped();
      require(xml.count(before) == 1, "Source XML mutation is not unique");
      xml.replace(before, change.second.toHtmlEscaped());
    }
    image.setText("preset", xml);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    require(buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "PNG"),
            "Cannot encode source XML fixture");
    return bytes;
  };
  const auto generated = [&](const QString &generator) {
    return rewrite(hardPixelKpp("buildUp"), {
        {"id=\"default\"", "id=\"" + generator + "\"" +
             (generator == "soft" ? " softness_curve=\"0,0;1,1;\"" : "")},
        {"diameter=\"32\"", "diameter=\"16\""},
        {"spacing=\"0.1\"", "spacing=\"1\""},
        {"antialiasEdges=\"0\"", "antialiasEdges=\"1\""}});
  };
  const QByteArray originals[] = {generated("soft"), generated("gauss"),
                                  sourceScatterKpp(true, true, false)};
  for (int index = 0; index < 3; ++index) {
    QString error;
    const auto original = importBrush(originals[index], "kpp", error);
    const std::string expected = index == 2
        ? "krita-5.3.4.1-scatter-candidate-1"
        : "krita-5.3.4.1-generated-mask-candidate-1";
    require(error.isEmpty() && original.complete && original.records.size() == 1 &&
                original.records.front().preset.profile == expected,
            "Valid generated/scatter source XML lost candidate admission");
    const auto whitespace = rewrite(originals[index], {
        {"<MaskGenerator ", "\n  <MaskGenerator "},
        {"</Brush>", "\n</Brush>\n"}});
    const auto spaced = importBrush(whitespace, "kpp", error);
    require(error.isEmpty() && spaced.complete && spaced.records.size() == 1 &&
                spaced.records.front().preset.profile == expected,
            "Ordinary source brush whitespace changed candidate admission");
    const std::vector<std::vector<std::pair<QString, QString>>> mutations = {
        {{"<Brush ", "<Brush xmlns=\"urn:unmeasured:brush\" "}},
        {{"<MaskGenerator ",
          "<MaskGenerator xmlns=\"urn:unmeasured:mask\" "}},
        {{"<MaskGenerator ",
          "<other:MaskGenerator xmlns:other=\"urn:unmeasured:mask\" "}},
        {{"<Brush ", "<Brush xmlns:other=\"urn:unmeasured:attribute\" "},
         {"BrushVersion=\"2\"", "other:BrushVersion=\"2\""}},
        {{"<Brush ", "<Brush xmlns:unused=\"urn:unmeasured:unused\" "}},
        {{"<MaskGenerator ", "unsupported<MaskGenerator "}},
        {{"</Brush>", "unsupported</Brush>"}},
        {{"/></Brush>", ">unsupported</MaskGenerator></Brush>"}}};
    for (const auto &mutation : mutations) {
      const auto rejected = importBrush(rewrite(originals[index], mutation),
                                        "kpp", error);
      require(!error.isEmpty() && !rejected.complete && rejected.records.empty(),
              "Unknown source brush XML acquired generated/scatter behavior");
    }
  }
}
