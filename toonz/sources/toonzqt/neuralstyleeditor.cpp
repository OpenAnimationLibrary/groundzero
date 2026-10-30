#include "toonzqt/colormatcheditor.h"
#include "toonz/colormatch.h"
#include "toonz/neuralstyle.h"
#include <QCoreApplication>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QSaveFile>
#include <QTimer>
#include <QVBoxLayout>
#include <chrono>
#include <future>

namespace {
QString tr(const char *text){return QCoreApplication::translate("NeuralStyleEditor",text);}
QImage bounded(const QImage &image) {
  return image.width()<=1024 && image.height()<=1024?image:
      image.scaled(1024,1024,Qt::KeepAspectRatio,Qt::SmoothTransformation);
}
QString sampleName(const QString &path) {
  return QFileInfo(path).fileName().left(128).replace(':','_').replace('\\','_');
}
struct Preview {NeuralStyle::Profile profile;QImage result;QString error;};
class Editor final:public QDialog {
  QImage reference,source,mask;
  NeuralStyle::Profile profile;
  QLabel *before,*referenceView,*after,*status;
  QDoubleSpinBox *crop[4];
  QPushButton *preview,*apply,*exportProfile;
  bool valid=false,busy=false;
  std::atomic_bool cancelled{false};
  std::future<Preview> task;
  QTimer poll;
  void show(QLabel *view,const QImage &image) {
    view->setPixmap(QPixmap::fromImage(image.scaled(260,220,Qt::KeepAspectRatio,Qt::SmoothTransformation)));
  }
  void invalidate() {
    cancelled=true;valid=false;apply->setEnabled(false);exportProfile->setEnabled(false);
    after->clear();status->setText(tr("Inputs changed. Preview before using the reference."));
  }
  void load(int kind) {
    const auto path=QFileDialog::getOpenFileName(this,tr("Load Image"),QString(),tr("Raster images (*.png *.jpg *.jpeg *.bmp *.tif *.tiff)"));
    if(path.isEmpty())return;
    QString error;const auto image=ColorMatch::loadSample(path,error);
    if(image.isNull()){QMessageBox::warning(this,tr("Neural Style"),error);return;}
    if(kind==2 && (reference.isNull() || image.size()!=reference.size())) {
      QMessageBox::warning(this,tr("Reference Mask"),tr("Load the original reference first. The mask must have matching sampled dimensions."));return;
    }
    invalidate();
    if(kind==0) {source=image;show(before,source);}
    if(kind==1) {
      if(ColorMatch::sampleDigest(image)!=profile.sampleDigest) {
        mask=QImage();profile.maskName.clear();profile.maskDigest.clear();
      }
      reference=image;show(referenceView,reference);
      profile.sampleName=sampleName(path);
      if(!mask.isNull() && mask.size()!=reference.size())mask=QImage();
    }
    if(kind==2){mask=image;profile.maskName=sampleName(path);}
  }
  void start() {
    if(busy)return;
    if(source.isNull()){status->setText(tr("Load a source sample for the preview."));return;}
    if(reference.isNull() && profile.reference.isNull()) {
      status->setText(tr("Load a reference image or import a captured profile."));return;
    }
    if(!reference.isNull() && !profile.maskDigest.isEmpty() && mask.isNull()) {
      status->setText(tr("Relink the recorded reference mask or explicitly Clear Mask before recapturing."));return;
    }
    const QRectF area(crop[0]->value()/100,crop[1]->value()/100,crop[2]->value()/100,crop[3]->value()/100);
    if(reference.isNull() && (std::abs(area.x()-profile.crop.x())>1e-6 ||
        std::abs(area.y()-profile.crop.y())>1e-6 || std::abs(area.width()-profile.crop.width())>1e-6 ||
        std::abs(area.height()-profile.crop.height())>1e-6)) {
      status->setText(tr("Relink the original reference to change its crop. The embedded look remains available."));return;
    }
    const auto captured=profile;const auto image=source,style=reference,selection=mask;
    cancelled=false;busy=true;preview->setEnabled(false);apply->setEnabled(false);exportProfile->setEnabled(false);
    status->setText(tr("Styling locally on the CPU. Cancel closes without applying."));
    task=std::async(std::launch::async,[this,image,style,selection,area,captured]{
      Preview result;result.profile=captured;
      try {
        if(!style.isNull() && !NeuralStyle::capture(style,selection,area,result.profile,result.error))return result;
        QRect contentArea;const auto content=NeuralStyle::prepare(image,contentArea,result.error);
        if(content.isNull())return result;
        NeuralStyle::Session session(QDir(QCoreApplication::applicationDirPath()).filePath("neural-style"));
        QImage styled;
        if(session.style(result.profile.reference,content,styled,result.error,&cancelled)) {
          result.result=NeuralStyle::restoreSize(styled,image.size()).convertToFormat(QImage::Format_ARGB32);
          const auto original=image.convertToFormat(QImage::Format_ARGB32);
          for(int y=0;y<image.height();++y) {
            if(cancelled){result.result=QImage();return result;}
            auto out=reinterpret_cast<QRgb *>(result.result.scanLine(y));
            const auto in=reinterpret_cast<const QRgb *>(original.constScanLine(y));
            for(int x=0;x<image.width();++x)out[x]=qRgba(qRed(out[x]),qGreen(out[x]),qBlue(out[x]),qAlpha(in[x]));
          }
        }
      }catch(const std::exception &error){result.error=QString::fromUtf8(error.what());}
      return result;
    });
    poll.start(30);
  }
  void importProfile() {
    const auto path=QFileDialog::getOpenFileName(this,tr("Import Texture Profile"),QString(),tr("Texture profile (*.gzstyle)"));
    if(path.isEmpty())return;
    QFile file(path);QString error;NeuralStyle::Profile loaded;
    if(!file.open(QIODevice::ReadOnly) || file.size()>400000 ||
       !NeuralStyle::parse(QString::fromUtf8(file.readAll()),loaded,error)) {
      QMessageBox::warning(this,tr("Import Failed"),error.isEmpty()?tr("Cannot read this texture profile."):error);return;
    }
    invalidate();profile=loaded;reference=QImage();mask=QImage();setCrop();
    valid=true;apply->setEnabled(!busy);exportProfile->setEnabled(!busy);show(referenceView,profile.reference);
    status->setText(tr("Captured reference loaded. It works without the original image. Relink originals to change the crop or mask."));
  }
  void setCrop() {
    const double values[]={profile.crop.x(),profile.crop.y(),profile.crop.width(),profile.crop.height()};
    for(int i=0;i<4;++i){crop[i]->blockSignals(true);crop[i]->setValue(values[i]*100);crop[i]->blockSignals(false);}
  }
  void save() {
    if(!valid || busy)return;
    const auto path=QFileDialog::getSaveFileName(this,tr("Export Texture Profile"),QString(),tr("Texture profile (*.gzstyle)"));
    if(path.isEmpty())return;
    QSaveFile file(path);const auto bytes=NeuralStyle::serialize(profile).toUtf8();
    if(!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit())
      QMessageBox::warning(this,tr("Export Failed"),file.errorString());
  }
public:
  Editor(QWidget *parent,const QString &initial,const QImage &initialReference,const QImage &initialSource):QDialog(parent) {
    setWindowTitle(tr("Neural Texture Reference"));auto layout=new QVBoxLayout(this);
    auto explanation=new QLabel(tr("Transfer texture and color from a reference using local CPU inference.\nThe model works at 384 pixels; original alpha and protected ink are preserved by the FX.\nThis sample preview shows full strength. Set shot boundaries, strength and temporal history in the FX."),this);
    explanation->setWordWrap(true);layout->addWidget(explanation);
    auto row=new QHBoxLayout();layout->addLayout(row);
    before=new QLabel(this);referenceView=new QLabel(this);after=new QLabel(this);
    for(auto view:{before,referenceView,after}){view->setMinimumSize(260,220);row->addWidget(view);}
    auto form=new QFormLayout();layout->addLayout(form);
    const char *names[]={"Source Sample...","Load / Relink Reference...","Reference Sampling Mask..."};
    for(int i=0;i<3;++i){auto button=new QPushButton(tr(names[i]),this);form->addRow(button);connect(button,&QPushButton::clicked,this,[this,i]{load(i);});}
    auto clearMask=new QPushButton(tr("Clear Mask"),this);form->addRow(clearMask);
    connect(clearMask,&QPushButton::clicked,this,[this]{
      if(reference.isNull() && !profile.maskDigest.isEmpty()) {
        status->setText(tr("Relink the original reference before removing its captured mask."));return;
      }
      mask=QImage();profile.maskName.clear();profile.maskDigest.clear();invalidate();
    });
    const char *labels[]={"Reference left %","Reference top %","Reference width %","Reference height %"};
    for(int i=0;i<4;++i) {
      crop[i]=new QDoubleSpinBox(this);crop[i]->setDecimals(4);crop[i]->setRange(i<2?0:.0001,100);crop[i]->setValue(i<2?0:100);
      form->addRow(tr(labels[i]),crop[i]);connect(crop[i],QOverload<double>::of(&QDoubleSpinBox::valueChanged),this,[this]{invalidate();});
    }
    auto actions=new QHBoxLayout();layout->addLayout(actions);
    auto import=new QPushButton(tr("Import Profile..."),this);exportProfile=new QPushButton(tr("Export Profile..."),this);
    preview=new QPushButton(tr("Preview Texture"),this);actions->addWidget(import);actions->addWidget(exportProfile);actions->addWidget(preview);
    connect(import,&QPushButton::clicked,this,[this]{importProfile();});connect(exportProfile,&QPushButton::clicked,this,[this]{save();});connect(preview,&QPushButton::clicked,this,[this]{start();});
    status=new QLabel(this);status->setWordWrap(true);layout->addWidget(status);
    auto buttons=new QDialogButtonBox(QDialogButtonBox::Cancel,this);apply=buttons->addButton(tr("Use Reference"),QDialogButtonBox::AcceptRole);layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::rejected,this,&QDialog::reject);
    connect(apply,&QPushButton::clicked,this,[this]{if(valid && !busy)accept();});
    connect(&poll,&QTimer::timeout,this,[this]{
      if(task.wait_for(std::chrono::seconds(0))!=std::future_status::ready)return;
      auto result=task.get();poll.stop();busy=false;preview->setEnabled(true);
      if(!cancelled) {
        valid=!result.result.isNull() && result.error.isEmpty();
        if(valid){profile=result.profile;show(after,result.result);show(referenceView,profile.reference);status->setText(tr("Reference captured and embedded. Sample preview complete; check motion and ink in the scene preview."));}
        else status->setText(result.error.isEmpty()?tr("Preview cancelled."):result.error);
      }
      apply->setEnabled(valid);exportProfile->setEnabled(valid);
    });
    QString error;
    if(!initial.isEmpty()) {
      valid=NeuralStyle::parse(initial,profile,error);
      if(valid){setCrop();show(referenceView,profile.reference);status->setText(tr("The captured reference is embedded. Load a source sample to preview it."));}
      else status->setText(error);
    }else status->setText(tr("Load a source sample and reference, then Preview Texture."));
    if(!initialReference.isNull()) {
      reference=bounded(initialReference);show(referenceView,reference);
      if(!initial.isEmpty()){profile.maskName.clear();profile.maskDigest.clear();invalidate();}
    }
    if(!initialSource.isNull()){source=bounded(initialSource);show(before,source);}
    apply->setEnabled(valid);exportProfile->setEnabled(valid);
  }
  ~Editor(){cancelled=true;if(task.valid())task.wait();}
  QString value()const{return NeuralStyle::serialize(profile);}
  void reject()override{cancelled=true;QDialog::reject();}
};
}
bool editNeuralStyleProfile(QWidget *parent,QString &profile,const QImage &reference,const QImage &source) {
  Editor editor(parent,profile,reference,source);
  if(editor.exec()!=QDialog::Accepted)return false;
  profile=editor.value();return true;
}
void createNeuralStyleProfile(QWidget *parent,const QImage &reference) {
  QString profile;
  if(!editNeuralStyleProfile(parent,profile,reference))return;
  const auto path=QFileDialog::getSaveFileName(parent,tr("Save Profile for Neural Texture Style FX"),QString(),tr("Texture profile (*.gzstyle)"));
  if(path.isEmpty())return;
  QSaveFile file(path);const auto bytes=profile.toUtf8();
  if(!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit())
    QMessageBox::warning(parent,tr("Export Failed"),file.errorString());
}
