#include "../neuralstylefx.cpp"
#include "../neuraltemporal.cpp"
#include "neuraltemporal_cases.h"
#include "tparamcontainer.h"
#include "tthread.h"
#include "trenderer.h"
#include "tenv.h"
#include "timagecache.h"
#include "tfxutil.h"
#include "toonz/toonzscene.h"
#include "toonz/tproject.h"
#include "toonz/txsheet.h"
#include "toonz/fxdag.h"
#include "toonz/tcolumnfxset.h"
#include "toonz/preferences.h"
#include <QTemporaryDir>
#include <QPainter>
#include <QPainterPath>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <future>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value,const char *message){if(!value)throw std::runtime_error(message);}
// Full TRasterFx::compute uses a render-scoped resource/cache manager.
// Match the application's lifecycle rather than calling it outside a render.
struct RenderContext {
  TRenderer renderer;
  unsigned long id;
  RenderContext():renderer(1),id(TRenderer::buildRenderId()) {
    renderer.install(id);renderer.declareRenderStart(id);renderer.declareFrameStart(0);
  }
  ~RenderContext() {
    renderer.declareFrameEnd(0);renderer.declareRenderEnd(id);renderer.uninstall();
  }
};
QImage fixture(const QString &path,int side) {
  QFile file(path);require(file.open(QIODevice::ReadOnly),"Missing RGB fixture");
  const auto bytes=file.readAll();require(bytes.size()==side*side*3,"Invalid RGB fixture length");
  return QImage(reinterpret_cast<const uchar *>(bytes.constData()),side,side,side*3,QImage::Format_RGB888).convertToFormat(QImage::Format_RGB32);
}
void profiles() {
  QImage image(8,4,QImage::Format_ARGB32);image.fill(qRgb(200,50,20));
  QImage mask(image.size(),QImage::Format_ARGB32);mask.fill(qRgb(128,128,128));
  NeuralStyle::Profile profile;QString error;
  require(NeuralStyle::capture(image,mask,QRectF(.25,0,.5,1),profile,error),"Capture cropped reference");
  require(qRed(profile.reference.pixel(128,128))==227,"Reference mask coverage not applied");
  const auto serialized=NeuralStyle::serialize(profile);NeuralStyle::Profile restored;
  require(NeuralStyle::parse(serialized,restored,error),"Neural profile roundtrip");
  require(profile.reference==restored.reference && profile.crop==restored.crop &&
          profile.sampleDigest==restored.sampleDigest,"Captured reference changed in persistence");
  for(const auto &bad:{QString("{}"),QString(400001,'x'),QString(serialized).replace("srgb-white-letterbox-v1","unknown")}) {
    require(!NeuralStyle::parse(bad,restored,error),"Malformed profile accepted");
    require(restored.reference==profile.reference,"Failed parse changed existing profile");
  }
  mask.fill(Qt::black);require(!NeuralStyle::capture(image,mask,QRectF(0,0,1,1),restored,error),"Excluded reference accepted");
  require(!NeuralStyle::capture(image,{},QRectF(.9,0,.2,1),restored,error),"Crop outside image accepted");
  for(const auto &size:{QSize(800,400),QSize(400,800),QSize(1,1024)}) {
    QImage transparent(size,QImage::Format_ARGB32);transparent.fill(qRgba(0,0,0,0));QRect area;
    const auto prepared=NeuralStyle::prepare(transparent,area,error);
    require(prepared.size()==QSize(384,384) && area.width()>0 && area.height()>0,"Invalid model letterbox");
    require(prepared.pixel(192,192)==qRgb(255,255,255),"Transparent RGB leaked into inference");
    require(NeuralStyle::restoreSize(prepared,size).size()==size,"Neural restoration changed geometry");
  }
}
void temporal(){NeuralTemporalCases::run();}

class Source final:public TStandardRasterFx {
  template<class P>void fill(TRasterPT<P> raster,const TTile &tile,double frame,const TRenderSettings &info) {
    const auto &image=images[std::min(size_t(std::max(0,int(frame))),images.size()-1)];
    const auto inverse=info.m_affine.inv();
    for(int y=0;y<raster->getLy();++y)for(int x=0;x<raster->getLx();++x) {
      const auto p=inverse*TPointD(tile.m_pos.x+x+.5,tile.m_pos.y+y+.5);
      const int xx=int(std::floor(p.x)),yy=383-int(std::floor(p.y));
      const auto color=xx>=0 && xx<384 && yy>=0 && yy<384?image.pixel(xx,yy):QRgb(0);
      const double alpha=qAlpha(color)/255.0,maximum=P::maxChannelValue;
      const auto c=[maximum](double value){return typename P::Channel(value*maximum+(P::maxChannelValue==1?0:.5));};
      raster->pixels(y)[x]=P(c(qRed(color)/255.0*alpha),c(qGreen(color)/255.0*alpha),c(qBlue(color)/255.0*alpha),c(alpha));
    }
  }
public:
  std::vector<QImage> images;
  explicit Source(std::vector<QImage> values):images(std::move(values)){}
  const TPersistDeclaration *getDeclaration() const override{return nullptr;}
  bool canHandle(const TRenderSettings &,double) override{return true;}
  bool doGetBBox(double,TRectD &box,const TRenderSettings &) override{box=TRectD(0,0,384,384);return true;}
  std::string getAlias(double frame,const TRenderSettings &) const override {
    QCryptographicHash hash(QCryptographicHash::Sha256);hashImage(hash,images[std::min(size_t(std::max(0,int(frame))),images.size()-1)]);
    return hash.result().toHex().toStdString();
  }
  void compute(TTile &tile,double frame,const TRenderSettings &info) override {
    if(TRaster32P raster=tile.getRaster())fill<TPixel32>(raster,tile,frame,info);
    else if(TRaster64P raster=tile.getRaster())fill<TPixel64>(raster,tile,frame,info);
    else if(TRasterFP raster=tile.getRaster())fill<TPixelF>(raster,tile,frame,info);
    else throw std::runtime_error("Unsupported source fixture raster");
  }
  void doCompute(TTile &tile,double frame,const TRenderSettings &info) override{compute(tile,frame,info);}
};
void configure(NeuralStyleFx &fx,const QImage &reference,Source *source,int temporal=0) {
  NeuralStyle::Profile profile;profile.reference=reference;
  TStringParamP p=TParamP(fx.getParams()->getParam("profile"));p->setValue(NeuralStyle::serialize(profile).toStdWString());
  TIntParamP history=TParamP(fx.getParams()->getParam("temporal"));history->setValue(temporal);
  fx.getInputPort(0)->setFx(source);
}
QImage render(NeuralStyleFx &fx,int frame,const TRenderSettings &info,int x=0,int y=0,int width=384,int height=384) {
  TRaster32P raster(width,height);TTile tile(raster,TPointD(x,y));fx.doCompute(tile,frame,info);
  QImage result(width,height,QImage::Format_ARGB32);
  for(int yy=0;yy<height;++yy)for(int xx=0;xx<width;++xx) {
    const auto p=raster->pixels(yy)[xx];
    const auto straight=[&](int value){return p.m?std::min(255,int(value*255.0/p.m+.5)):0;};
    result.setPixel(xx,height-1-yy,qRgba(straight(p.r),straight(p.g),straight(p.b),p.m));
  }
  return result;
}
QImage renderHost(NeuralStyleFx &fx,const TRenderSettings &info,double x,double y,int width,int height) {
  TRaster32P raster(width,height);TTile tile(raster,TPointD(x,y));fx.compute(tile,0,info);
  QImage result(width,height,QImage::Format_RGB32);
  for(int yy=0;yy<height;++yy)for(int xx=0;xx<width;++xx) {
    const auto p=raster->pixels(yy)[xx];result.setPixel(xx,height-1-yy,qRgb(p.r,p.g,p.b));
  }
  return result;
}
template<class P>void preservation() {
  TRasterPT<P> raster(2,1),ink(2,1);const double maximum=P::maxChannelValue;
  const auto c=[maximum](double value){return typename P::Channel(value*maximum);};
  const P original(c(.2),c(.1),c(.05),c(.5));raster->fill(original);ink->clear();
  ink->pixels(0)[0]=P(P::maxChannelValue,P::maxChannelValue,P::maxChannelValue,P::maxChannelValue);
  QImage styled(384,384,QImage::Format_RGB32);styled.fill(qRgb(255,0,0));
  TRenderSettings info;info.m_cameraBox=TRectD(0,0,2,1);Grid grid(info);
  require(composite<P>(raster,{},ink,styled,grid,TPointD(),1,nullptr),"Composite failed");
  require(raster->pixels(0)[0]==original,"Ink protection changed drawing");
  require(raster->pixels(0)[1].m==original.m && raster->pixels(0)[1].r==original.m,"Original alpha not preserved");
}
template<class P>void maskedFx(const QImage &reference,const QImage &content) {
  auto rgba=content.convertToFormat(QImage::Format_ARGB32);
  QImage selection(384,384,QImage::Format_RGB32),ink=selection;selection.fill(Qt::black);ink.fill(Qt::black);
  for(int y=0;y<384;++y)for(int x=0;x<384;++x) {
    const auto p=rgba.pixel(x,y);rgba.setPixel(x,y,qRgba(qRed(p),qGreen(p),qBlue(p),x%256));
    if(x<96)selection.setPixel(x,y,qRgb(255,255,255));
    if(x>=48 && x<96)ink.setPixel(x,y,qRgb(255,255,255));
  }
  NeuralStyleFx fx;auto source=new Source({rgba});configure(fx,reference,source);
  fx.getInputPort(1)->setFx(new Source({selection}));fx.getInputPort(2)->setFx(new Source({ink}));
  TRasterPT<P> original(384,384),styled(384,384);
  TTile before(original,TPointD()),after(styled,TPointD());TRenderSettings info;
  info.m_cameraBox=TRectD(0,0,384,384);info.m_bpp=P::maxChannelValue==1?128:(P::maxChannelValue==255?32:64);
  source->compute(before,0,info);fx.compute(after,0,info);
  int changed=0;
  for(int y=0;y<384;++y)for(int x=0;x<384;++x) {
    const auto a=original->pixels(y)[x],b=styled->pixels(y)[x];
    require(a.m==b.m,"End-to-end FX changed current alpha");
    if(x>=48 || a.m==0)require(a==b,"Mask or protected ink changed in end-to-end FX");
    else changed+=a!=b;
  }
  require(changed>100,"Masked neural FX produced no style effect");
}
void scenePersistence(const QImage &reference,const QDir &root) {
  ToonzScene scene;TProjectManager::instance()->initializeScene(&scene);
  TFxP card=TFxUtil::makeColorCard(TPixel32(80,110,160,170));
  auto neural=new NeuralStyleFx;TFxP owner=neural;
  configure(*neural,reference,nullptr);neural->getInputPort(0)->setFx(card.getPointer());
  neural->setFxId(L"NeuralPortable1");
  auto dag=scene.getXsheet()->getFxDag();dag->getInternalFxs()->addFx(card.getPointer());
  dag->getInternalFxs()->addFx(neural);dag->addToXsheet(neural);
  const QString originalReference=root.filePath("original-reference.png");
  require(reference.save(originalReference),"Cannot save original reference fixture");
  const TFilePath path(root.filePath("original.tnz"));scene.save(path,nullptr,false);
  TRenderSettings info;info.m_cameraBox=TRectD(0,0,384,384);
  const auto expected=render(*neural,0,info,0,0,64,64);
  require(QFile::remove(originalReference),"Cannot remove original reference fixture");
  require(QDir().mkpath(root.filePath("moved")),"Cannot create portable scene directory");
  const QString moved=root.filePath("moved/portable.tnz");require(QFile::rename(path.getQString(),moved),"Cannot move saved scene");
  ToonzScene reopened;reopened.loadTnzFile(TFilePath(moved));
  auto restored=dynamic_cast<NeuralStyleFx *>(reopened.getXsheet()->getFxDag()->getFxById(L"NeuralPortable1"));
  require(restored && restored->getInputPort(0)->isConnected(),"Saved scene lost neural FX/source graph");
  require(render(*restored,37,info,0,0,64,64)==expected,"Moved scene with missing original reference changed output");
}
void visualExamples(const QDir &directory) {
  require(QDir().mkpath(directory.absolutePath()),"Cannot create visual review directory");
  QImage reference(256,256,QImage::Format_RGB32);reference.fill(QColor(238,213,171));
  {
    QPainter painter(&reference);painter.setRenderHint(QPainter::Antialiasing);
    const QColor colors[]={QColor(40,70,110),QColor(185,95,60),QColor(220,170,65),QColor(90,135,135)};
    for(int i=0;i<48;++i) {
      painter.setPen(QPen(colors[i%4],3+i%7,Qt::SolidLine,Qt::RoundCap));
      QPainterPath path;path.moveTo(-20,i*7-30);path.cubicTo(60,i*7+25,170,i*7-60,280,i*7+5);painter.drawPath(path);
    }
  }
  require(reference.save(directory.filePath("reference.png")),"Cannot save synthetic style reference");
  QJsonArray metrics;
  for(bool background:{false,true}) {
    const QString name=background?"painted-background":"cel-character";
    std::vector<QImage> frames,inks;
    for(int f=0;f<4;++f) {
      const int shift=f==0?0:(f==3?16:8);
      QImage image(384,384,QImage::Format_ARGB32);image.fill(background?qRgb(211,225,221):QRgb(0));
      {
        QPainter p(&image);p.setRenderHint(QPainter::Antialiasing);p.translate(shift,0);
        if(background) {
          p.setPen(Qt::NoPen);p.setBrush(QColor(164,188,164));p.drawEllipse(QRectF(-80,180,340,250));
          p.setBrush(QColor(120,157,152));p.drawEllipse(QRectF(160,140,350,310));
          p.setBrush(QColor(238,207,136));p.drawEllipse(QRectF(265,35,57,57));
          for(int x=-30;x<420;x+=65) {
            p.setPen(QPen(QColor(65,85,100),3));p.setBrush(QColor(207,153+(x+30)%30,110));
            p.drawRect(QRectF(x,200+(x+30)%40,45,90));
            QPainterPath roof;roof.moveTo(x-5,200+(x+30)%40);roof.lineTo(x+22,170+(x+30)%40);roof.lineTo(x+50,200+(x+30)%40);roof.closeSubpath();
            p.setBrush(QColor(135,80,70));p.drawPath(roof);
          }
          if(f==3){p.setPen(Qt::NoPen);p.setBrush(QColor(48,70,82));p.drawRect(QRectF(155,40,42,344));}
        }else {
          p.setPen(QPen(QColor(18,22,28),5,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
          p.setBrush(QColor(65,155,157));p.drawRoundedRect(QRectF(129,174,94,120),24,24);
          p.setBrush(QColor(239,183,129));p.drawEllipse(QRectF(133,87,f==3?96:84,91));
          p.setBrush(Qt::white);p.drawEllipse(QRectF(150,116,17,21));p.drawEllipse(QRectF(184,116,17,21));
          p.setBrush(QColor(18,22,28));p.drawEllipse(QRectF(157,123,5,8));p.drawEllipse(QRectF(190,123,5,8));
          p.drawLine(QPointF(166,150),QPointF(186,f==3?154:150));
          p.setBrush(QColor(49,77,111));p.drawRoundedRect(QRectF(138,286,29,58),10,10);p.drawRoundedRect(QRectF(187,286,29,58),10,10);
          p.drawLine(QPointF(130,191),QPointF(97,f==3?160:232));p.drawLine(QPointF(221,191),QPointF(250,220));
        }
      }
      QImage ink(384,384,QImage::Format_RGB32);ink.fill(Qt::black);
      if(!background)for(int y=0;y<384;++y)for(int x=0;x<384;++x) {
        const auto p=image.pixel(x,y);
        if(qAlpha(p)>0 && qRed(p)<30 && qGreen(p)<30 && qBlue(p)<35)ink.setPixel(x,y,qRgb(255,255,255));
      }
      frames.push_back(image);inks.push_back(ink);
    }
    NeuralStyleFx independent,temporalFx;
    configure(independent,reference,new Source(frames),0);configure(temporalFx,reference,new Source(frames),70);
    independent.getInputPort(2)->setFx(new Source(inks));temporalFx.getInputPort(2)->setFx(new Source(inks));
    QImage sheet(3*384,4*384,QImage::Format_RGB32);sheet.fill(Qt::white);
    QPainter painter(&sheet);
    std::vector<QImage> plain,stable;
    TRenderSettings info;info.m_cameraBox=TRectD(0,0,384,384);
    for(int f=0;f<4;++f) {
      plain.push_back(render(independent,f,info));stable.push_back(render(temporalFx,f,info));
      for(int y=0;y<384;++y)for(int x=0;x<384;++x) {
        require(qAlpha(stable.back().pixel(x,y))==qAlpha(frames[f].pixel(x,y)),"Visual fixture alpha changed");
        if(!background && qRed(inks[f].pixel(x,y))==255) {
          const auto original=frames[f].pixel(x,y),actual=stable.back().pixel(x,y);
          const auto premult=[&](int value){return (value*qAlpha(original)+127)/255;};
          require(premult(qRed(original))==premult(qRed(actual)) && premult(qGreen(original))==premult(qGreen(actual)) &&
                  premult(qBlue(original))==premult(qBlue(actual)),"Visual fixture protected ink changed");
        }
      }
      painter.drawImage(QPoint(0,f*384),frames[f]);painter.drawImage(QPoint(384,f*384),plain.back());painter.drawImage(QPoint(768,f*384),stable.back());
      require(stable.back().save(directory.filePath(name+QString("-temporal-%1.png").arg(f))),"Cannot save temporal review frame");
    }
    painter.end();require(sheet.save(directory.filePath(name+"-comparison.png")),"Cannot save visual comparison");
    require(stable[1]==stable[2],"Visual held exposure drifted");
    double independentDifference=0,temporalDifference=0;int count=0;
    for(int y=0;y<384;++y)for(int x=0;x<376;++x) {
      const auto a=frames[0].pixel(x,y),b=frames[1].pixel(x+8,y);
      if(a!=b || qAlpha(a)!=255)continue;
      const auto delta=[](QRgb p,QRgb q){return (std::abs(qRed(p)-qRed(q))+std::abs(qGreen(p)-qGreen(q))+std::abs(qBlue(p)-qBlue(q)))/3.0;};
      independentDifference+=delta(plain[0].pixel(x,y),plain[1].pixel(x+8,y));
      temporalDifference+=delta(stable[0].pixel(x,y),stable[1].pixel(x+8,y));++count;
    }
    require(count>1000,"Visual motion metric has insufficient matched coverage");
    metrics.append(QJsonObject{{"example",name},{"independent_motion_difference",independentDifference/count},
        {"temporal_motion_difference",temporalDifference/count},{"matched_pixels",count},{"held_frame_exact",true}});
  }
  QFile file(directory.filePath("metrics.json"));require(file.open(QIODevice::WriteOnly),"Cannot save visual metrics");
  file.write(QJsonDocument(metrics).toJson());
  QFile guide(directory.filePath("README.txt"));require(guide.open(QIODevice::WriteOnly),"Cannot save review guide");
  guide.write("Original synthetic artwork. Comparison columns: source, independent neural, temporal (70%). Rows: base, eight-pixel pan, held exposure, deformation (cel) or foreground occlusion (background). Black cel ink is explicitly protected. Model texture canvas is 384 pixels; these examples do not establish full-resolution detail or unrestricted shot length. Inspect ghosting, edge integrity, style influence and motion stability. Metrics cover known opaque translated pixels only; they do not establish artist approval.\n");
  std::cout<<"Visual motion metrics: "<<QJsonDocument(metrics).toJson(QJsonDocument::Compact).constData()<<'\n';
}
}
int main(int argc,char **argv) {
  QCoreApplication app(argc,argv);
  std::cerr<<"Neural qualification: preferences initialization"<<std::endl;
  QTemporaryDir storage;require(storage.isValid(),"Test storage unavailable");const QDir root(storage.path());
  QDir().mkpath(root.filePath("stuff/profiles/layouts/rooms/Default"));QDir().mkpath(root.filePath("stuff/library"));
  TEnv::setStuffDir(TFilePath(root.filePath("stuff")));TEnv::setApplicationFullName("neuralstyle_test");
  Preferences::instance()->setValue(backupEnabled,false);Preferences::instance()->setValue(compressSceneFiles,false);
  std::cerr<<"Neural qualification: render services initialization"<<std::endl;
  QDir().mkpath(root.filePath("cache"));TImageCache::instance()->setRootDir(TFilePath(root.filePath("cache")));
  TThread::init();TRenderer::initialize();
  struct Shutdown {~Shutdown(){TThread::shutdown();}} shutdown;
  try {
    RenderContext context;
    std::cerr<<"Neural qualification: profile, motion and compositing"<<std::endl;
    profiles();temporal();preservation<TPixel32>();preservation<TPixel64>();preservation<TPixelF>();
    require(argc==2,"Expected model fixture directory");const QDir fixtures(app.arguments()[1]);
    const auto reference=fixture(fixtures.filePath("reference.rgb"),256);
    const auto content=fixture(fixtures.filePath("content.rgb"),384);
    std::cerr<<"Neural qualification: masked FX"<<std::endl;
    maskedFx<TPixel32>(reference,content);maskedFx<TPixel64>(reference,content);maskedFx<TPixelF>(reference,content);
    std::cerr<<"Neural qualification: scene persistence"<<std::endl;
    scenePersistence(reference,root);
    TRenderSettings info;info.m_cameraBox=TRectD(0,0,384,384);
    NeuralStyleFx fx;configure(fx,reference,new Source({content}));
    const auto complete=render(fx,0,info),expected=fixture(fixtures.filePath("expected.rgb"),384);
    for(int y=0;y<384;++y)for(int x=0;x<384;++x) {
      const auto a=complete.pixel(x,y),b=expected.pixel(x,y);
      require(std::abs(qRed(a)-qRed(b))<=1 && std::abs(qGreen(a)-qGreen(b))<=1 && std::abs(qBlue(a)-qBlue(b))<=1,"FX differs from model oracle");
    }
    require(render(fx,0,info,73,41,91,103)==complete.copy(73,384-41-103,91,103),"Tile partition changed neural output");
    TDoubleParamP strength=TParamP(fx.getParams()->getParam("strength"));strength->setValue(0,50.0001);
    const auto alias=fx.getAlias(0,info);strength->setValue(0,50.0002);
    require(alias!=fx.getAlias(0,info),"Fractional strength reused alias");
    strength->setValue(0,0);require(render(fx,0,info).convertToFormat(QImage::Format_RGB32)==content,"Zero strength changed source");
    auto affineInfo=info;affineInfo.m_affine=TAffine(1,.2,.25,.1,2,-.75);
    require(fx.handledAffine(affineInfo,0)==affineInfo.m_affine,"Camera grid differs from handled affine");
    strength->setValue(0,100);
    const auto hostWhole=renderHost(fx,affineInfo,40.25,39.75,48,40);
    require(renderHost(fx,affineInfo,49.25,46.75,17,19)==hostWhole.copy(9,40-7-19,17,19),"Host affine/fractional tile partition changed output");
    TRasterFP hdr(1,1);hdr->pixels(0)[0]=TPixelF(2,1,.25,.5);
    QImage red(384,384,QImage::Format_RGB32);red.fill(Qt::red);
    Grid hdrGrid(info);require(composite<TPixelF>(hdr,{},{},red,hdrGrid,TPointD(100,100),.5,nullptr),"HDR blend failed");
    require(std::abs(hdr->pixels(0)[0].r-1.25)<1e-6 && hdr->pixels(0)[0].m==.5,"Partial strength clipped untouched HDR contribution");
    NeuralStyleFx nested;auto inner=new NeuralStyleFx;
    configure(*inner,reference,new Source({content}));configure(nested,reference,nullptr);
    nested.getInputPort(0)->setFx(inner);
    require(!renderHost(nested,info,20,20,32,32).isNull(),"Nested worker dependency failed");
    auto changed=content.mirrored(true,false);
    NeuralStyleFx animation;auto source=new Source({content,changed,changed});configure(animation,reference,source,70);
    const auto frame0=render(animation,0,info),frame1=render(animation,1,info),frame2=render(animation,2,info);
    require(frame1==frame2,"Held exposure drifted");
    {std::lock_guard<std::mutex> lock(cacheMutex);cache.clear();}
    require(render(animation,2,info)==frame2 && render(animation,0,info)==frame0 && render(animation,1,info)==frame1,"Random seeking/cache eviction changed temporal result");
    const auto oldAlias=animation.getAlias(2,info);source->images[0]=changed;
    require(animation.getAlias(2,info)!=oldAlias,"Earlier source edit did not invalidate later frame");
    visualExamples(QDir(fixtures.absoluteFilePath("../artist-review")));
    std::cout<<"Neural profiles, preservation, motion and canonical FX rendering passed\n";return 0;
  }catch(const TException &error){std::cerr<<QString::fromStdWString(error.getMessage()).toStdString()<<'\n';return 1;}
  catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
