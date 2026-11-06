// Included after the integration fixture's existing anonymous helpers.
namespace {
void runRasterBrushSourceEditorIntegration(TTool *tool, TEnumProperty *property,
    TXshSimpleLevel *level, TUndoManager *undo) {
  checkpoint("qualifying admitted source editor state, cancel and portable persistence");
  for (int kind = 0; kind < 9; ++kind) {
    RasterBrush::Record fixture;
    auto &p = fixture.preset;
    p.name = "Original source editor fixture " + std::to_string(kind);
    p.profile = "krita-5.3.4.1-hard-pixel-candidate-1";
    p.size.value = 16; p.spacing = 1;
    if (kind < 2) {
      p.profile = "krita-5.3.4.1-generated-mask-candidate-1";
      RasterBrush::SourceGeneratedDefinitionV1 definition;
      definition.generator = kind ? RasterBrush::SourceGeneratedGenerator::Gaussian : RasterBrush::SourceGeneratedGenerator::Soft;
      definition.shape = kind ? RasterBrush::SourceGeneratedShape::Rectangle : RasterBrush::SourceGeneratedShape::Circle;
      definition.diameter = p.size.value = kind ? 64 : 16;
      definition.horizontalFade = .1; definition.verticalFade = .9; definition.antialiasEdges = true;
      if (!kind) definition.softnessCurve = {{0, 0}, {.25, .75}, {1, 1}};
      p.tip.shape = kind ? RasterBrush::Shape::Square : RasterBrush::Shape::Round;
      p.tip.antialiasEdges = true;
      p.sourceGeneratedV1 = fixture.sourceGeneratedV1 = definition;
    } else if (kind == 2) {
      p.profile = "krita-5.3.4.1-scatter-candidate-1";
      p.size.value = 8; p.spacing = 4; p.sourceScatterV1 = RasterBrush::SourceScatterV1{};
    } else if (kind == 3) {
      p.wash = true; p.sourceMasked.enabled = true;
    } else if (kind == 4) {
      p.profile = "krita-5.3.4.1-fixed-bitmap-candidate-1";
      p.size.value = 4; p.tip.shape = RasterBrush::Shape::Bitmap;
      auto image = std::make_shared<RasterBrush::Image>(); image->width = image->height = 2;
      image->pixels = {{0, 0, 0, 1}, {0, 0, 0, .2}, {0, 0, 0, .6}, {0, 0, 0, 0}};
      p.tip.images = {image}; p.sourceMirror.enabled = p.sourceMirror.horizontal = true;
      p.sourceMirror.control.mappings.push_back({RasterBrush::Sensor::Pressure, {{0, 0}, {1, 1}}, 1});
    } else if (kind == 5) {
      p.sourceHsvV2[0].enabled = true; p.sourceHsvV2[0].control.value = .75;
    } else if (kind == 6) {
      p.profile = "krita-5.3.4.1-legacy-dulling-candidate-1";
      p.wet = RasterBrush::Wet::Dulling; p.smudgeRadius = 0;
    } else if (kind == 7) {
      p.profile = "krita-5.3.4.1-airbrush-candidate-1";
      p.opacity.value = .1; p.airbrushRate = 20; p.airbrushIgnoreSpacing = true;
    } else {
      p.profile = "krita-5.3.4.1-default-circle-candidate-1";
      p.tip.hardness = .5; p.tip.antialiasEdges = true;
    }
    const QString folder = ToonzFolder::getLibraryFolder().getQString() + "/rasterbrushes";
    QString error;
    const auto originalPath = RasterBrush::install(folder, fixture, error);
    require(error.isEmpty() && !originalPath.isEmpty(), "cannot install source editor fixture");
    const auto originalBytes = RasterBrush::readBoundedRegularFile(originalPath, 64 * 1024 * 1024, error);
    require(error.isEmpty(), "cannot read source editor original");
    const auto selection = QDir(folder).relativeFilePath(originalPath).toStdWString();
    const auto baseline = raster(level, 1)->clone();
    for (bool accept : {false, true}) {
      tool->onActivate(); property->setValue(selection);
      require(tool->onPropertyChanged("Native Brush:"), "cannot select source editor fixture");
      bool edited = false, guarded = false, expired = false;
      QObject context;
      QElapsedTimer deadline; deadline.start();
      QTimer watchdog;
      QObject::connect(&watchdog, &QTimer::timeout, &context, [&] {
        if (deadline.elapsed() < 15000) return;
        expired = true;
        if (auto modal = qobject_cast<QDialog *>(QApplication::activeModalWidget())) modal->reject();
      });
      watchdog.start(25);
      QTimer::singleShot(0, &context, [&] {
        auto editor = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!editor) return;
        auto preview = editor->findChild<QWidget *>("nativeBrushScratchpad");
        auto native = editor->findChild<QWidget *>("nativeBrushNativeControls");
        auto source = editor->findChild<QWidget *>("nativeBrushSourceControls");
        auto message = editor->findChild<QLabel *>("sourceBrushEditStatus");
        auto choose = [&](const char *name, int value) {
          auto control = editor->findChild<QComboBox *>(name);
          if (!control || !control->isEnabled()) return false;
          control->setCurrentIndex(value); return control->currentIndex() == value;
        };
        auto number = [&](const char *name, double value) {
          auto control = editor->findChild<QDoubleSpinBox *>(name);
          if (!control || !control->isEnabled()) return false;
          control->setValue(value); return std::abs(control->value() - value) < 1e-12;
        };
        auto flag = [&](const char *name, bool value) {
          auto control = editor->findChild<QCheckBox *>(name);
          if (!control || !control->isEnabled()) return false;
          control->setChecked(value); return control->isChecked() == value;
        };
        if (!preview || !native || native->isEnabled() || !source || !source->isVisible() || !message) {
          editor->reject(); return;
        }
        const auto before = preview->grab().toImage();
        edited = true;
        if (kind < 2) {
          edited &= choose("sourceBrushGenerator", kind ? 0 : 1);
          edited &= choose("sourceBrushShape", kind ? 0 : 1);
          edited &= choose("sourceBrushGeneratedSize", kind ? 0 : 1);
          if (!kind) edited &= number("sourceBrushHorizontalFade", .2) && number("sourceBrushVerticalFade", .8);
          else {
            auto curve = editor->findChild<QLineEdit *>("sourceBrushSoftnessCurve");
            auto apply = editor->findChild<QPushButton *>("sourceBrushSoftnessCurveApply");
            if (curve && apply) { curve->setText("0,0.25;1,0.25;"); apply->click(); }
            else edited = false;
          }
          if (!kind) edited &= choose("sourceBrushGenerator", 0);
          auto invalidCurve = editor->findChild<QLineEdit *>("sourceBrushSoftnessCurve");
          auto applyCurve = editor->findChild<QPushButton *>("sourceBrushSoftnessCurveApply");
          if (invalidCurve && applyCurve) {
            const auto validPreview = preview->grab().toImage();
            invalidCurve->setText("0,1;0.5,0.4;0.5,0.3;1,0;");
            applyCurve->click();
            guarded = !message->text().isEmpty() && preview->grab().toImage() == validPreview;
          }
          if (!kind) edited &= choose("sourceBrushGenerator", 1);
        } else if (kind == 2) edited &= choose("sourceBrushScatterAxes", 1);
        else if (kind == 3) {
          edited &= flag("sourceBrushMaskMaster", false) && number("sourceBrushMaskDiameter", 10) &&
              number("sourceBrushMaskAspect", .75) && number("sourceBrushMaskSpacing", .3) &&
              number("sourceBrushMaskOpacity", .6) && number("sourceBrushMaskFlow", .4) && choose("sourceBrushMaskMode", 2);
          auto wash = editor->findChild<QCheckBox *>("sourceBrushWash");
          if (wash) { wash->setChecked(false); guarded = wash->isChecked() && !message->text().isEmpty(); }
        } else if (kind == 4) edited &= choose("sourceBrushMirrorAxes", 2);
        else if (kind == 5) edited &= choose("sourceBrushHsv0Input", 2) && choose("sourceBrushHsv2Input", 1) &&
            number("sourceBrushHsv2Value", .8);
        else if (kind == 6) edited &= number("sourceBrushPickup", .3) && number("sourceBrushColorRate", .2) &&
            number("sourceBrushRadius", .5) && flag("sourceBrushLengthEnabled", false) &&
            flag("sourceBrushColorEnabled", false) && flag("sourceBrushSmearAlpha", false) &&
            choose("sourceBrushPickupInput", 1) && choose("sourceBrushColorInput", 1);
        else if (kind == 7) edited &= choose("sourceBrushAirbrushRate", 0) && flag("sourceBrushAirbrushIgnoreSpacing", false);
        else edited &= number("sourceBrushDefaultFade", .25) && flag("sourceBrushDefaultAA", false);
        if (kind != 3 && kind >= 2) guarded = true;
        edited = edited && guarded && preview->grab().toImage() != before;
        auto buttons = editor->findChild<QDialogButtonBox *>();
        if (!accept || !edited || !buttons) { editor->reject(); return; }
        QTimer::singleShot(0, &context, [] {
          if (auto name = qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) name->accept();
        });
        buttons->button(QDialogButtonBox::Ok)->click();
      });
      property->setValue(L"<edit>");
      const bool handled = tool->onPropertyChanged("Native Brush:");
      watchdog.stop();
      require(handled && edited && !expired, "source controls failed guarded editing, real-engine preview or bounded modal completion");
      require(RasterBrush::readBoundedRegularFile(originalPath, 64 * 1024 * 1024, error) == originalBytes &&
                  error.isEmpty() && same(baseline, raster(level, 1)) && undo->getHistoryCount() == 0,
              "source editor changed its original or document");
      if (!accept) { require(property->getValue() == selection, "cancelled source editor selected a copy"); continue; }
      const auto savedPath = QDir(folder).filePath(QString::fromStdWString(property->getValue()));
      const auto saved = RasterBrush::importBrushProcess(QCoreApplication::applicationFilePath(), savedPath, error);
      require(error.isEmpty() && saved.complete && saved.records.size() == 1, "cannot reopen edited source copy");
      auto correct = [&](const RasterBrush::Record &record) {
        const auto &q = record.preset;
        if (q.profile != p.profile || record.referenceVerified || !RasterBrush::validate(q).empty()) return false;
        if (kind < 2) return record.sourceGeneratedV1 && q.sourceGeneratedV1 &&
            q.sourceGeneratedV1->generator == (kind ? RasterBrush::SourceGeneratedGenerator::Soft : RasterBrush::SourceGeneratedGenerator::Gaussian) &&
            q.sourceGeneratedV1->shape == (kind ? RasterBrush::SourceGeneratedShape::Circle : RasterBrush::SourceGeneratedShape::Rectangle) &&
            q.size.value == (kind ? 16 : 64) && q.sourceGeneratedV1->diameter == q.size.value &&
            record.sourceGeneratedV1->diameter == q.size.value &&
            (kind ? q.sourceGeneratedV1->softnessCurve.size() == 2 && q.sourceGeneratedV1->softnessCurve.front().y == .25 &&
                       record.sourceGeneratedV1->softnessCurve.back().y == .25
                  : q.sourceGeneratedV1->horizontalFade == .2 && q.sourceGeneratedV1->verticalFade == .8 &&
                       record.sourceGeneratedV1->horizontalFade == .2 && record.sourceGeneratedV1->verticalFade == .8);
        if (kind == 2) return q.sourceScatterV1 && !q.sourceScatterV1->axisX && q.sourceScatterV1->axisY && q.scatter.value == 0;
        if (kind == 3) return q.wash && q.sourceMasked.enabled && !q.sourceMasked.useMasterSize && q.sourceMasked.diameter == 10 &&
            q.sourceMasked.aspect == .75 && q.sourceMasked.spacing == .3 && q.sourceMasked.opacity == .6 &&
            q.sourceMasked.flow == .4 && q.sourceMasked.mode == RasterBrush::MaskMode::Subtract;
        if (kind == 4) return q.sourceMirror.enabled && !q.sourceMirror.horizontal && q.sourceMirror.vertical;
        if (kind == 5) return q.sourceHsvV2[0].enabled && q.sourceHsvV2[0].control.mappings.size() == 1 &&
            q.sourceHsvV2[2].enabled && q.sourceHsvV2[2].control.value == .8;
        if (kind == 6) return q.pickup == .3 && q.colorRate == .2 && q.smudgeRadius == .5 && !q.sourceSmudgeLengthEnabled &&
            !q.sourceColorRateEnabled && !q.sourceSmearAlpha && q.pickupControl.mappings.size() == 1 && q.colorRateControl.mappings.size() == 1;
        if (kind == 7) return q.airbrushRate == 5 && !q.airbrushIgnoreSpacing;
        return q.tip.hardness == .25 && !q.tip.antialiasEdges;
      };
      require(correct(saved.records.front()), "source editor lost active or preserved state on save");
      const auto portable = QDir::current().filePath("source-editor-copy.gzbrush");
      require(RasterBrush::savePortable(portable, saved.records.front(), RasterBrush::ResourcePackaging::PrivateCopy, error),
              "cannot export edited source copy");
      const auto reopened = RasterBrush::importBrushProcess(QCoreApplication::applicationFilePath(), portable, error);
      require(error.isEmpty() && reopened.complete && reopened.records.size() == 1 && correct(reopened.records.front()),
              "portable source copy lost edited state or activated native replacement semantics");
    }
  }
}
} // namespace
