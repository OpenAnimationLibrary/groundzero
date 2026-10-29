#include "toonzqt/colormatcheditor.h"
#include "toonz/colormatch.h"
#include "toonz/palettecmd.h"
#include "toonz/tpalettehandle.h"
#include "tcolorstyles.h"
#include <QCheckBox>
#include <QCoreApplication>
#include <QPixmap>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSaveFile>
#include <QTableWidget>
#include <QHeaderView>
#include <QTimer>
#include <QVBoxLayout>
#include <future>
#include <chrono>

namespace {
QString tr(const char *s) { return QCoreApplication::translate("ReferenceColorMatch",s); }
QImage bounded(const QImage &image) {
  if (image.width() <= 1024 && image.height() <= 1024) return image;
  return image.scaled(1024,1024,Qt::KeepAspectRatio,Qt::SmoothTransformation);
}
struct AnalysisResult { ColorMatch::Profile profile; QImage preview; QString error; };
class Editor final : public QDialog {
  QImage images[2], masks[2];
  QDoubleSpinBox *crop[2][4], *strength;
  QCheckBox *lightness;
  QLabel *before, *after, *referencePreview, *status;
  QPushButton *apply, *analyze, *exportProfile, *exportLut;
  ColorMatch::Profile profile;
  bool valid = false, busy = false;
  std::atomic_bool cancelled{false};
  std::future<AnalysisResult> task;
  QTimer poll;
  void invalidate() {
    cancelled = true;
    valid = false;
    apply->setEnabled(false); exportProfile->setEnabled(false); exportLut->setEnabled(false);
    after->clear();
    status->setText(tr("Inputs changed. Preview again before applying."));
  }
  void showImage(QLabel *label,const QImage &image) {
    label->setPixmap(QPixmap::fromImage(image.scaled(280,220,Qt::KeepAspectRatio,Qt::SmoothTransformation)));
  }
  void load(int side,bool mask) {
    QString path=QFileDialog::getOpenFileName(this,mask?tr("Load Sampling Mask"):tr("Load Image"),QString(),tr("Raster images (*.png *.jpg *.jpeg *.bmp *.tif *.tiff)"));
    if(path.isEmpty()) return;
    QString error;
    auto image=ColorMatch::loadSample(path,error);
    if(image.isNull()) { QMessageBox::warning(this,tr("Reference Color Match"),error); return; }
    if(mask) {
      if(images[side].isNull() || image.size()!=images[side].size()) {
        QMessageBox::warning(this,tr("Mask Size"),tr("Load the image first. Its mask must have the same aspect ratio and sampled dimensions.")); return;
      }
      masks[side]=image;
    } else {
      images[side]=image; masks[side]=QImage();
      showImage(side?referencePreview:before,image);
    }
    invalidate();
  }
  void start() {
    if(busy) return;
    invalidate();
    cancelled=false; busy=true; analyze->setEnabled(false);
    status->setText(tr("Analyzing locally... Cancel closes without applying."));
    auto a=images[0], b=images[1], ma=masks[0], mb=masks[1];
    QRectF rects[2];
    for(int i=0;i<2;++i) rects[i]=QRectF(crop[i][0]->value()/100,crop[i][1]->value()/100,
                                      crop[i][2]->value()/100,crop[i][3]->value()/100);
    const QRectF ra=rects[0], rb=rects[1];
    ColorMatch::Profile p; p.strength=strength->value()/100;
    p.preserveLightness=lightness->isChecked();
    task=std::async(std::launch::async,[this,a,b,ma,mb,ra,rb,p]() mutable {
      AnalysisResult result; result.profile=p;
      if(ColorMatch::analyze(a,ma,ra,result.profile.source,result.error,&cancelled) &&
         ColorMatch::analyze(b,mb,rb,result.profile.reference,result.error,&cancelled))
        result.preview=ColorMatch::preview(a,result.profile,&cancelled);
      return result;
    });
    poll.start(30);
  }
  void adjustProfile() {
    if (!valid || busy) { invalidate(); return; }
    profile.strength=strength->value()/100;
    profile.preserveLightness=lightness->isChecked();
    if(images[0].isNull()) return;
    cancelled=false;busy=true;analyze->setEnabled(false);apply->setEnabled(false);
    const auto image=images[0];const auto p=profile;
    task=std::async(std::launch::async,[this,image,p] {
      AnalysisResult result;result.profile=p;
      result.preview=ColorMatch::preview(image,p,&cancelled);return result;
    });
    poll.start(30);
  }
  void save(bool lut) {
    if(!valid) return;
    QString path=QFileDialog::getSaveFileName(this,lut?tr("Export Creative LUT"):tr("Export Color Profile"),QString(),lut?tr("Cube LUT (*.cube)"):tr("Color profile (*.gzcolor)"));
    if(path.isEmpty()) return;
    QString error;
    if(lut) { if(ColorMatch::exportCube(path,profile,error)) return; }
    else {
      QSaveFile file(path); QByteArray bytes=ColorMatch::serialize(profile).toUtf8();
      if(file.open(QIODevice::WriteOnly) && file.write(bytes)==bytes.size() && file.commit()) return;
      error=file.errorString();
    }
    QMessageBox::warning(this,tr("Export Failed"),error);
  }
public:
  Editor(QWidget *parent,const QString &initial,const QImage &reference,const QImage &source)
      : QDialog(parent) {
    setWindowTitle(tr("Match Reference Colors"));
    auto layout=new QVBoxLayout(this);
    auto explanation=new QLabel(tr("Capture one color mapping for the whole shot. SDR sRGB images only.\nSampling masks use white to include and black to exclude. Crops are percentages from the top left.\nThis changes colors, not texture or drawing geometry."),this);
    explanation->setWordWrap(true); layout->addWidget(explanation);
    auto inputs=new QHBoxLayout(); layout->addLayout(inputs);
    for(int i=0;i<2;++i) {
      auto box=new QGroupBox(i?tr("Reference"):tr("Source Sample"),this);
      auto form=new QFormLayout(box); inputs->addWidget(box);
      auto choose=new QPushButton(tr("Load / Replace Image..."),box);
      auto mask=new QPushButton(tr("Sampling Mask..."),box);
      auto clear=new QPushButton(tr("Clear Mask"),box);
      form->addRow(choose); form->addRow(mask); form->addRow(clear);
      connect(choose,&QPushButton::clicked,this,[this,i]{load(i,false);});
      connect(mask,&QPushButton::clicked,this,[this,i]{load(i,true);});
      connect(clear,&QPushButton::clicked,this,[this,i]{masks[i]=QImage();invalidate();});
      const char *labels[]={"Left %","Top %","Width %","Height %"};
      for(int j=0;j<4;++j) {
        crop[i][j]=new QDoubleSpinBox(box); crop[i][j]->setRange(0,100);
        crop[i][j]->setValue(j<2?0:100); form->addRow(tr(labels[j]),crop[i][j]);
      }
    }
    auto form=new QFormLayout(); layout->addLayout(form);
    strength=new QDoubleSpinBox(this); strength->setRange(0,100); strength->setValue(100);
    lightness=new QCheckBox(tr("Preserve lightness"),this); lightness->setChecked(true);
    form->addRow(tr("Strength %"),strength); form->addRow(lightness);
    auto previews=new QHBoxLayout(); layout->addLayout(previews);
    before=new QLabel(tr("Source"),this); referencePreview=new QLabel(tr("Reference"),this); after=new QLabel(tr("Result"),this);
    for(auto label:{before,referencePreview,after}) {label->setMinimumSize(200,160); previews->addWidget(label);}
    status=new QLabel(this); status->setWordWrap(true); layout->addWidget(status);
    auto actions=new QHBoxLayout();layout->addLayout(actions);
    analyze=new QPushButton(tr("Preview"),this);
    auto import=new QPushButton(tr("Import Profile..."),this);
    exportProfile=new QPushButton(tr("Export Profile..."),this);
    exportLut=new QPushButton(tr("Export LUT..."),this);
    for(auto button:{analyze,import,exportProfile,exportLut}) actions->addWidget(button);
    auto buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,this);
    apply=buttons->button(QDialogButtonBox::Ok); apply->setText(tr("Use Profile"));layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::accepted,this,&QDialog::accept);
    connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    connect(analyze,&QPushButton::clicked,this,[this]{start();});
    connect(exportProfile,&QPushButton::clicked,this,[this]{save(false);});
    connect(exportLut,&QPushButton::clicked,this,[this]{save(true);});
    connect(import,&QPushButton::clicked,this,[this]{
      if(busy) return;
      QString path=QFileDialog::getOpenFileName(this,tr("Import Profile"),QString(),tr("Color profile (*.gzcolor)"));
      if(path.isEmpty())return;
      QFile file(path); QString error;
      if(!file.open(QIODevice::ReadOnly)||file.size()>4096) {QMessageBox::warning(this,tr("Import Failed"),tr("Profile is unreadable or exceeds 4 KiB."));return;}
      if(!setProfile(QString::fromUtf8(file.readAll()),error)) QMessageBox::warning(this,tr("Import Failed"),error);
    });
    for(int i=0;i<2;++i)for(int j=0;j<4;++j)
      connect(crop[i][j],QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this]{invalidate();});
    connect(strength,QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this]{adjustProfile();});
    connect(lightness,&QCheckBox::toggled,this,[this]{adjustProfile();});
    connect(&poll,&QTimer::timeout,this,[this]{
      if(task.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready)return;
      poll.stop();busy=false;analyze->setEnabled(true);
      AnalysisResult result;
      try {result=task.get();} catch(const std::exception &e) {result.error=QString::fromUtf8(e.what());}
      if(cancelled.load())return;
      if(!result.error.isEmpty()||result.preview.isNull()) {status->setText(result.error);return;}
      profile=result.profile;valid=true;showImage(after,result.preview);
      apply->setEnabled(true);exportProfile->setEnabled(true);exportLut->setEnabled(true);
      status->setText(tr("Preview ready. The profile is embedded in the scene; original files are not needed for rendering."));
    });
    images[0]=bounded(source);images[1]=bounded(reference);
    showImage(before,images[0]);showImage(referencePreview,images[1]);
    invalidate();QString error;
    if(!initial.isEmpty()&&!setProfile(initial,error))status->setText(error);
  }
  ~Editor() override {cancelled=true;if(task.valid())task.wait();}
  void reject() override {cancelled=true;QDialog::reject();}
  bool setProfile(const QString &text,QString &error) {
    ColorMatch::Profile next;
    if(!ColorMatch::parse(text,next,error))return false;
    strength->blockSignals(true);lightness->blockSignals(true);
    strength->setValue(next.strength*100);lightness->setChecked(next.preserveLightness);
    strength->blockSignals(false);lightness->blockSignals(false);
    profile=next;valid=true;apply->setEnabled(true);exportProfile->setEnabled(true);exportLut->setEnabled(true);
    showImage(after,ColorMatch::preview(images[0],profile));
    status->setText(tr("Stored profile loaded. Preview recomputes from the selected source and reference."));return true;
  }
  QString result() const {return valid?ColorMatch::serialize(profile):QString();}
};
}

bool editColorMatchProfile(QWidget *parent,QString &profile,const QImage &reference,const QImage &source) {
  Editor editor(parent,profile,reference,source);
  if(editor.exec()!=QDialog::Accepted || editor.result().isEmpty())return false;
  profile=editor.result();return true;
}

void matchPaletteToReference(QWidget *parent,TPaletteHandle *handle,const QImage &reference) {
  TPaletteP palette=handle?handle->getPalette():nullptr;
  if(!palette || palette->isLocked() || palette->isAnimated()) {
    QMessageBox::warning(parent,tr("Palette Unavailable"),tr("Choose an unlocked, non-animated palette."));return;
  }
  std::vector<int> ids;
  for(int i=1;i<palette->getStyleCount();++i) {
    auto style=palette->getStyle(i);
    if(style->getTagId()==3 && style->getGlobalName().empty())ids.push_back(i);
  }
  if(ids.empty()) {QMessageBox::information(parent,tr("No Editable Colors"),tr("Only unlinked solid-color styles can be mapped."));return;}
  QImage source(int(ids.size()),1,QImage::Format_ARGB32);
  for(int i=0;i<int(ids.size());++i) {auto c=palette->getStyle(ids[i])->getMainColor();source.setPixel(i,0,qRgba(c.r,c.g,c.b,c.m));}
  QString text;
  if(!editColorMatchProfile(parent,text,reference,source))return;
  ColorMatch::Profile profile;QString error;
  if(!ColorMatch::parse(text,profile,error))return;
  QDialog review(parent);review.setWindowTitle(tr("Review Palette Mapping"));
  auto layout=new QVBoxLayout(&review);
  auto label=new QLabel(tr("Select only styles to change. Leave ink and approved character colors unchecked.\nShared palette changes affect every drawing using these style IDs."),&review);label->setWordWrap(true);layout->addWidget(label);
  auto table=new QTableWidget(int(ids.size()),4,&review);
  table->setHorizontalHeaderLabels({tr("Apply"),tr("Style ID"),tr("Before"),tr("After")});
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  std::vector<TPixel32> colors;
  for(int i=0;i<int(ids.size());++i) {
    auto c=palette->getStyle(ids[i])->getMainColor();
    auto v=ColorMatch::transform({{c.r/255.0,c.g/255.0,c.b/255.0}},profile);
    colors.emplace_back(int(v[0]*255+.5),int(v[1]*255+.5),int(v[2]*255+.5),c.m);
    auto check=new QTableWidgetItem();check->setFlags(Qt::ItemIsEnabled|Qt::ItemIsUserCheckable);check->setCheckState(Qt::Unchecked);table->setItem(i,0,check);
    table->setItem(i,1,new QTableWidgetItem(QString::number(ids[i])));
    for(int j=0;j<2;++j) {auto p=j?colors.back():c;auto item=new QTableWidgetItem(QColor(p.r,p.g,p.b).name());item->setBackground(QColor(p.r,p.g,p.b));table->setItem(i,j+2,item);}
  }
  table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);layout->addWidget(table);
  auto buttons=new QDialogButtonBox(QDialogButtonBox::Apply|QDialogButtonBox::Cancel,&review);layout->addWidget(buttons);
  QObject::connect(buttons->button(QDialogButtonBox::Apply),&QPushButton::clicked,&review,&QDialog::accept);
  QObject::connect(buttons,&QDialogButtonBox::rejected,&review,&QDialog::reject);
  if(review.exec()!=QDialog::Accepted)return;
  std::vector<std::pair<int,TPixel32>> mapping;
  for(int i=0;i<int(ids.size());++i)if(table->item(i,0)->checkState()==Qt::Checked)mapping.emplace_back(ids[i],colors[i]);
  if(handle->getPalette()!=palette.getPointer() || !PaletteCmd::applyReferenceColors(handle,mapping))
    QMessageBox::warning(parent,tr("Palette Changed"),tr("The palette is no longer editable. No colors were changed."));
}
