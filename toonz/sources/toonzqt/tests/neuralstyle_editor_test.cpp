#include "toonzqt/colormatcheditor.h"
#include "toonz/neuralstyle.h"
#include "toonzqt/paramfield.h"
#include "tnotanimatableparam.h"
#include "tundo.h"
#include <QApplication>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QDynamicPropertyChangeEvent>
#include <QFileDialog>
#include <QPushButton>
#include <QLineEdit>
#include <QPixmap>
#include <QVariant>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok,const char *message){if(!ok)throw std::runtime_error(message);}
QPushButton *button(QWidget *dialog,const char *name){return dialog->findChild<QPushButton *>(name);}
class InferenceObserver final:public QObject {
public:
  std::function<void(QDialog *)> action;
  bool eventFilter(QObject *object,QEvent *event)override {
    if(event->type()==QEvent::DynamicPropertyChange &&
       static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName()=="neuralInferenceActive" &&
       object->property("neuralInferenceActive").toBool()) {
      if(auto dialog=qobject_cast<QDialog *>(object))if(action)action(dialog);
    }
    return false;
  }
};
}
int main(int argc,char **argv) {
  qputenv("QT_QPA_PLATFORM","offscreen");
  QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
  QApplication application(argc,argv);
  try {
    require(argc==2,"Expected visual report directory");const QDir report(application.arguments()[1]);QDir().mkpath(report.absolutePath());
    {
      NeuralStyle::Profile dense;dense.reference=QImage(256,256,QImage::Format_RGB32);quint32 random=2026;
      for(int y=0;y<256;++y)for(int x=0;x<256;++x) {
        random=random*1664525u+1013904223u;dense.reference.setPixel(x,y,qRgb((random>>16)&255,(random>>8)&255,random&255));
      }
      const QString document=NeuralStyle::serialize(dense);require(document.size()>32767,"Dense reference did not exercise text limit");
      TStringParamP param(L"");param->setName("profile");StringParamField field(nullptr,"Texture Reference",param);
      field.enableNeuralStyleControls();field.setParam(param,param,0);
      auto text=field.findChild<QLineEdit *>();require(text!=nullptr,"Hidden profile field missing");text->setText(document);
      require(text->text()==document,"Embedded reference truncated by parameter field");
      require(QMetaObject::invokeMethod(&field,"onChange",Qt::DirectConnection),"Cannot apply profile parameter");
      require(param->getValue()==document.toStdWString(),"Profile apply truncated embedded image");
      TUndoManager::manager()->undo();require(param->getValue().empty(),"Texture profile undo failed");
      TUndoManager::manager()->redo();require(param->getValue()==document.toStdWString(),"Texture profile redo failed");
      TUndoManager::manager()->reset();
    }
    QImage reference(256,256,QImage::Format_RGB32),source(384,384,QImage::Format_RGB32);
    for(int y=0;y<256;++y)for(int x=0;x<256;++x)reference.setPixel(x,y,qRgb(x,y,100));
    for(int y=0;y<384;++y)for(int x=0;x<384;++x)source.setPixel(x,y,qRgb((x/8%2)*160+40,(y/8%2)*120+70,150));
    NeuralStyle::Profile captured;captured.reference=reference;
    const QString initial=NeuralStyle::serialize(captured);QString profile=initial;
    bool started=false,cancelledDuringInference=false,failed=false;
    InferenceObserver cancelObserver;
    cancelObserver.action=[&](QDialog *dialog){
      cancelledDuringInference=true;
      failed=button(dialog,"neuralUseReference")->isEnabled() || button(dialog,"neuralExport")->isEnabled();
      dialog->reject();
    };
    QElapsedTimer deadline;deadline.start();QTimer cancelPoll;cancelPoll.setInterval(5);
    QObject::connect(&cancelPoll,&QTimer::timeout,[&]{
      auto dialog=qobject_cast<QDialog *>(QApplication::activeModalWidget());if(!dialog)return;
      if(deadline.elapsed()>60000){failed=true;dialog->reject();return;}
      auto preview=button(dialog,"neuralPreview");if(!preview)return;
      if(!started){started=true;dialog->installEventFilter(&cancelObserver);preview->click();return;}
    });
    cancelPoll.start();const bool accepted=editNeuralStyleProfile(nullptr,profile,{},source);cancelPoll.stop();
    require(!accepted && cancelledDuringInference && !failed && profile==initial,"Editor Cancel changed profile or exposed in-flight output");

    QTemporaryDir temporary;require(temporary.isValid(),"Reference replacement directory unavailable");
    QImage replacement(256,256,QImage::Format_RGB32);replacement.fill(QColor(20,60,230));
    const QString replacementPath=temporary.filePath("replacement.png");require(replacement.save(replacementPath),"Cannot save replacement fixture");
    profile=initial;started=false;failed=false;bool replacing=false,replaced=false,recaptured=false;int settle=0;
    QTimer picker;picker.setInterval(5);
    QObject::connect(&picker,&QTimer::timeout,[&]{
      if(auto file=qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
        file->selectFile(replacementPath);QMetaObject::invokeMethod(file,"accept",Qt::QueuedConnection);
      }
    });
    InferenceObserver replaceObserver;
    replaceObserver.action=[&](QDialog *dialog){
      if(replacing)return;
      replacing=true;picker.start();button(dialog,"neuralLoad1")->click();picker.stop();replaced=true;
    };
    deadline.restart();QTimer replacePoll;replacePoll.setInterval(5);
    QObject::connect(&replacePoll,&QTimer::timeout,[&]{
      auto dialog=qobject_cast<QDialog *>(QApplication::activeModalWidget());if(!dialog)return;
      if(deadline.elapsed()>60000){failed=true;dialog->reject();return;}
      if(auto file=qobject_cast<QFileDialog *>(dialog)) {
        file->selectFile(replacementPath);QMetaObject::invokeMethod(file,"accept",Qt::QueuedConnection);return;
      }
      auto preview=button(dialog,"neuralPreview");if(!preview)return;
      auto use=button(dialog,"neuralUseReference"),exportButton=button(dialog,"neuralExport");
      if(!started){started=true;dialog->installEventFilter(&replaceObserver);preview->click();return;}
      if(replaced && !recaptured && preview->isEnabled()) {
        if(use->isEnabled() || exportButton->isEnabled()){failed=true;dialog->reject();return;}
        // Let queued completion/progress messages settle; none may reactivate
        // stale output after the reference changed during inference.
        if(++settle<12)return;
        recaptured=true;preview->click();return;
      }
      if(recaptured && use->isEnabled()) {
        if(!dialog->grab().save(report.filePath("texture-editor-replacement.png")))failed=true;
        use->click();
      }
    });
    replacePoll.start();const bool committed=editNeuralStyleProfile(nullptr,profile,{},source);replacePoll.stop();
    NeuralStyle::Profile parsed;QString error;
    require(committed && replaced && recaptured && !failed && NeuralStyle::parse(profile,parsed,error),"Editor reference replacement lifecycle failed");
    require(parsed.reference==replacement && parsed.sampleName=="replacement.png","Stale reference preview was applied after replacement");
    std::cout<<"Editor inference cancellation and reference replacement passed\n";return 0;
  }catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
