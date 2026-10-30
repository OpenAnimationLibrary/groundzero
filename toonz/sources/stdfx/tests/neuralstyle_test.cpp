#include "../neuralstylefx.cpp"
#include "../neuraltemporal.cpp"
#include "tparamcontainer.h"
#include <QTemporaryDir>
#include <future>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool value,const char *message){if(!value)throw std::runtime_error(message);}
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
void temporal() {
  QImage source(384,384,QImage::Format_RGB32),alpha(384,384,QImage::Format_RGB32);
  source.fill(qRgb(80,80,80));alpha.fill(Qt::white);
  QImage history(384,384,QImage::Format_RGB32),fresh(384,384,QImage::Format_RGB32),output;
  history.fill(Qt::red);fresh.fill(Qt::blue);
  require(NeuralTemporal::stabilize(source,source,alpha,alpha,history,fresh,.7,output),"Held frame processing");
  require(output==history,"Held drawing accumulated temporal drift");
  auto changed=source;changed.fill(Qt::white);
  require(NeuralTemporal::stabilize(source,changed,alpha,alpha,history,fresh,.7,output) && output==fresh,"Cut retained old texture");
  int cancel=1;output=history;
  require(!NeuralTemporal::stabilize(source,changed,alpha,alpha,history,fresh,.7,output,&cancel) && output==history,"Cancelled temporal result published");
  // Textured translation with a newly exposed stripe: known image motion must
  // carry some history while out-of-bounds/disoccluded pixels stay fresh.
  for(int y=0;y<384;++y)for(int x=0;x<384;++x) {
    const int v=((x/9+y/11)%2)?190:50;source.setPixel(x,y,qRgb(v,v,v));
  }
  changed.fill(Qt::white);QImage movedAlpha=alpha; movedAlpha.fill(Qt::black);
  for(int y=0;y<384;++y)for(int x=6;x<384;++x) {
    changed.setPixel(x,y,source.pixel(x-6,y));movedAlpha.setPixel(x,y,qRgb(255,255,255));
  }
  require(NeuralTemporal::stabilize(source,changed,alpha,movedAlpha,history,fresh,.7,output),"Motion stabilization failed");
  int carried=0;
  for(int y=40;y<344;++y)for(int x=40;x<344;++x)carried+=qRed(output.pixel(x,y))>20;
  require(carried>10000,"No reliable translated history accepted");
  require(output.pixel(0,192)==fresh.pixel(0,192),"Newly exposed pixels inherited old texture");
  QImage previousAlpha=alpha;
  for(int y=0;y<384;++y)for(int x=0;x<24;++x)previousAlpha.setPixel(x,y,qRgb(0,0,0));
  changed=source;changed.setPixel(383,383,qRgb(0,0,0));
  require(NeuralTemporal::stabilize(source,changed,previousAlpha,alpha,history,fresh,.7,output),"Opaque reveal failed");
  require(output.pixel(10,192)==fresh.pixel(10,192),"Newly opaque artwork inherited occluded texture");
}
class Source final:public TStandardRasterFx {
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
    TRaster32P raster=tile.getRaster();require(bool(raster),"Fixture requires 8-bit raster");
    const auto &image=images[std::min(size_t(std::max(0,int(frame))),images.size()-1)];
    const auto inverse=info.m_affine.inv();
    for(int y=0;y<raster->getLy();++y)for(int x=0;x<raster->getLx();++x) {
      const auto p=inverse*TPointD(tile.m_pos.x+x+.5,tile.m_pos.y+y+.5);
      const int xx=int(std::floor(p.x)),yy=383-int(std::floor(p.y));
      const auto color=xx>=0 && xx<384 && yy>=0 && yy<384?image.pixel(xx,yy):QRgb(0);
      raster->pixels(y)[x]=TPixel32(qRed(color),qGreen(color),qBlue(color),qAlpha(color));
    }
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
  QImage result(width,height,QImage::Format_RGB32);
  for(int yy=0;yy<height;++yy)for(int xx=0;xx<width;++xx) {
    const auto p=raster->pixels(yy)[xx];result.setPixel(xx,height-1-yy,qRgba(p.r,p.g,p.b,p.m));
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
}
int main(int argc,char **argv) {
  QCoreApplication app(argc,argv);
  try {
    profiles();temporal();preservation<TPixel32>();preservation<TPixel64>();preservation<TPixelF>();
    require(argc==2,"Expected model fixture directory");const QDir fixtures(app.arguments()[1]);
    const auto reference=fixture(fixtures.filePath("reference.rgb"),256);
    const auto content=fixture(fixtures.filePath("content.rgb"),384);
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
    strength->setValue(0,0);require(render(fx,0,info)==content,"Zero strength changed source");
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
    std::cout<<"Neural profiles, preservation, motion and canonical FX rendering passed\n";return 0;
  }catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}
