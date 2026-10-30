#pragma once

// Original generated fixture shared by parser and installed-application tests.
// No third-party brush assets are embedded.
#include <QBuffer>
#include <QImage>
#include <QString>
#include <stdexcept>

namespace RasterBrushTest {
inline QByteArray hardPixelKpp(const QString &mutation = {},
                               const QString &model    = {}) {
  QImage image(2, 2, QImage::Format_ARGB32);
  image.fill(Qt::transparent);
  image.setText("version", "2.2");
  const QString engine = model.isEmpty() ? "paintbrush" : "colorsmudge";
  QString xml =
      "<Preset name=\"Original fixed fixture\" paintopid=\"" + engine + "\">";
  auto param = [&](const QString &name, const QString &value) {
    xml += "<param name=\"" + name + "\">" + value.toHtmlEscaped() + "</param>";
  };
  param("paintop", engine);
  param("CompositeOp",
        mutation.startsWith("blend-") ? mutation.mid(6) : "normal");
  for (const QString &key : {"OpacityValue", "FlowValue"})
    param(key, mutation == "halfOpacity" && key == "OpacityValue" ? "0.5"
               : mutation == "zeroFlow" && key == "FlowValue"     ? "0"
               : mutation == "halfFlow" && key == "FlowValue"     ? "0.5"
                                                                  : "1");
  if (mutation == "wrongModeKey")
    param("PaintingMode", "1");
  else
    param("PaintOpAction", mutation == "buildUp" || !model.isEmpty() ? "1"
                           : mutation == "freehandWash"              ? "2"
                                                                     : "0");
  const bool pressureSize =
      mutation == "pressure" || mutation == "pressureRange" ||
      mutation == "pressureSensorRange" || mutation == "pressureSpline";
  for (const QString &key :
       {"EraserMode", "OpacityUseCurve", "FlowUseCurve", "PressureSize",
        "PressureRotation", "PressureRatio", "PressureScatter",
        "PressureSpacing", "Texture/Pattern/Enabled", "MergedPaint",
        "PaintOpSettings/isAirbrushing", "PaintOpSettings/ignoreSpacing",
        "PaintOpSettings/updateSpacingBetweenDabs",
        "KisPrecisionOption/AutoPrecisionEnabled"})
    param(key, (pressureSize && key == "PressureSize") ||
                       (mutation == "wetOpacityPressure" &&
                        key == "OpacityUseCurve") ||
                       ((mutation == "wetFlowPressure" ||
                         mutation == "wetFlowDisabled") &&
                        key == "FlowUseCurve")
                   ? "true"
                   : "false");
  if (mutation == "wetOpacityPressure" || mutation == "wetFlowPressure" ||
      mutation == "wetFlowDisabled") {
    const QString prefix =
        mutation == "wetOpacityPressure" ? "Opacity" : "Flow";
    param("Pressure" + prefix,
          mutation == "wetFlowDisabled" ? "false" : "true");
    param(prefix + "UseSameCurve", "true");
    param(prefix + "commonCurve", "0,0;1,1;");
    param(prefix + "Sensor",
          "<params id=\"pressure\"><curve>0,0;1,1;</curve></params>");
  }
  if (pressureSize) {
    const QString curve = mutation == "pressure"         ? "0,0;1,1;"
                          : mutation == "pressureSpline" ? "0,0;0.5,0.25;1,1;"
                                                         : "0,0.25;1,0.75;";
    param("SizeUseCurve", "true");
    param("SizeUseSameCurve",
          mutation == "pressureSensorRange" ? "false" : "true");
    // A deliberately different inactive common curve proves that the
    // sensor-local serialization does not accidentally use the shared curve.
    param("SizecommonCurve",
          mutation == "pressureSensorRange" ? "0,0;1,1;" : curve);
    param("SizeSensor",
          "<params id=\"pressure\"><curve>" + curve + "</curve></params>");
  }
  if (mutation != "missing") param("KisPrecisionOption/precisionLevel", "5");
  if (mutation == "unknown") param("UnclassifiedActiveSetting", "true");
  if (!model.isEmpty()) {
    const bool wetPressure =
        mutation == "wetPressure" || mutation == "radiusPressure";
    for (const QString &key :
         {"PressureColorRate", "PressureSmudgeRate", "PressureSmudgeRadius"})
      param(key, (mutation == "lengthOff" && key == "PressureSmudgeRate") ||
                         (mutation == "colorOff" && key == "PressureColorRate")
                     ? "false"
                     : "true");
    for (const QString &key :
         {"ColorRateUseCurve", "SmudgeRateUseCurve", "SmudgeRadiusUseCurve"})
      param(key,
            wetPressure && key != "SmudgeRadiusUseCurve" ? "true" : "false");
    if (wetPressure) {
      for (const QString &prefix : {"SmudgeRate", "ColorRate"}) {
        param(prefix + "UseSameCurve", "true");
        param(prefix + "commonCurve", "0,0;1,1;");
        param(prefix + "Sensor",
              "<params id=\"pressure\"><curve>0,0;1,1;</curve></params>");
      }
    }
    param("SmudgeRateSmearAlpha", mutation == "alphaOff" ? "false" : "true");
    param("SmudgeRadiusValue", mutation == "radius"           ? "1"
                               : mutation == "radiusPressure" ? "100"
                                                              : "0");
    param("SmudgeRateValue", "0.5");
    param("ColorRateValue", "0.25");
    param("SmudgeRateMode", model.endsWith("smear") ? "0" : "1");
    param("SmudgeRateUseNewEngine", model.startsWith("new") ? "true" : "false");
  }
  QString brush =
      "<Brush type=\"auto_brush\" BrushVersion=\"2\" spacing=\"0.1\" "
      "angle=\"0\" randomness=\"0\" density=\"1\" "
      "useAutoSpacing=\"0\"><MaskGenerator id=\"default\" type=\"circle\" "
      "diameter=\"32\" ratio=\"1\" hfade=\"1\" vfade=\"1\" "
      "antialiasEdges=\"0\" spikes=\"2\"/></Brush>";
  if (mutation == "soft" || mutation == "unequalFade") {
    brush.replace("hfade=\"1\"", "hfade=\"0.5\"");
    if (mutation == "soft") brush.replace("vfade=\"1\"", "vfade=\"0.5\"");
  }
  if (mutation == "rect") brush.replace("type=\"circle\"", "type=\"rect\"");
  if (mutation.startsWith("smallAA")) {
    brush.replace("diameter=\"32\"", mutation == "smallAAfractional"
                                         ? "diameter=\"4.5\""
                                         : "diameter=\"4\"");
    if (mutation == "smallAAsoft") {
      brush.replace("hfade=\"1\"", "hfade=\"0.5\"");
      brush.replace("vfade=\"1\"", "vfade=\"0.5\"");
    }
  }
  if (mutation == "aa" || mutation.startsWith("smallAA"))
    brush.replace("antialiasEdges=\"0\"", "antialiasEdges=\"1\"");
  if (mutation == "attribute")
    brush.replace("<Brush ", "<Brush unknown=\"1\" ");
  if (mutation == "ellipse") {
    brush.replace("ratio=\"1\"", "ratio=\"0.5\"");
    brush.replace("angle=\"0\"", "angle=\"0.7853981633974483\"");
  }
  if (mutation == "numericSpelling") {
    brush.replace("hfade=\"1\"", "hfade=\"1.0\"");
    brush.replace("vfade=\"1\"", "vfade=\"1e0\"");
    brush.replace("angle=\"0\"", "angle=\"-0.0\"");
    brush.replace("ratio=\"1\"", "ratio=\"1.00\"");
  }
  param("brush_definition", brush);
  xml += "</Preset>";
  image.setText("preset", xml);
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  if (!image.save(&buffer, "PNG"))
    throw std::runtime_error("cannot encode original KPP fixture");
  return bytes;
}
}  // namespace RasterBrushTest
