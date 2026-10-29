#include "../referencecolormatchfx.cpp"
#include "toonz/palettecmd.h"
#include "toonz/tpalettehandle.h"
#include "toonz/lut3d.h"
#include "tparamcontainer.h"
#include "tstream.h"
#include "tfilepath.h"
#include "tundo.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <future>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool ok,const char *message) {if(!ok)throw std::runtime_error(message);}
void close(double actual,double expected,double tolerance=1e-5) {
  require(std::abs(actual-expected)<=tolerance,"Color mismatch");
}
ColorMatch::Profile fixture() {
  QImage source(4,1,QImage::Format_ARGB32),reference(4,1,QImage::Format_ARGB32);
  source.fill(qRgb(100,100,100));reference.fill(qRgb(180,60,40));
  ColorMatch::Profile p;p.preserveLightness=false;QString error;
  require(ColorMatch::analyze(source,{},QRectF(0,0,1,1),p.source,error),"Source analysis");
  require(ColorMatch::analyze(reference,{},QRectF(0,0,1,1),p.reference,error),"Reference analysis");return p;
}
void mathAndProfiles() {
  auto p=fixture();auto c=ColorMatch::transform({{100/255.0,100/255.0,100/255.0}},p);
  close(c[0],180/255.0);close(c[1],60/255.0);close(c[2],40/255.0);
  QString error;ColorMatch::Profile restored;
  require(ColorMatch::parse(ColorMatch::serialize(p),restored,error),"Roundtrip");
  require(ColorMatch::serialize(restored)==ColorMatch::serialize(p),"Stable serialization");
  const auto original=ColorMatch::serialize(restored);
  for(const auto &bad:{QString("{}"),QString(4097,'x'),QString(original).replace("srgb-oklab","unknown")})
    require(!ColorMatch::parse(bad,restored,error),"Invalid profile accepted");
  p.strength=0;ColorMatch::Color identity{{.1,.3,.7}};
  require(ColorMatch::transform(identity,p)==identity,"Zero strength must be exact");
}
void sampling() {
  QImage image(2,1,QImage::Format_ARGB32);image.setPixel(0,0,qRgb(255,0,0));image.setPixel(1,0,qRgb(0,0,255));
  QImage mask(2,1,QImage::Format_ARGB32);mask.setPixel(0,0,qRgb(255,255,255));mask.setPixel(1,0,qRgb(0,0,0));
  ColorMatch::Statistics a,b;QString error;
  require(ColorMatch::analyze(image,mask,QRectF(0,0,1,1),a,error),"Masked sample");
  require(ColorMatch::analyze(image,{},QRectF(0,0,.5,1),b,error),"Cropped sample");
  require(a.mean==b.mean && a.deviation==b.deviation,"Crop and mask disagree");
  mask.fill(0);require(!ColorMatch::analyze(image,mask,QRectF(0,0,1,1),a,error),"Empty mask accepted");
  image.setPixel(1,0,qRgba(0,0,255,0));
  require(ColorMatch::analyze(image,{},QRectF(0,0,1,1),a,error),"Transparent sample");
  require(a.mean==b.mean,"Transparent RGB contaminated statistics");
  std::atomic_bool cancelled{true};
  require(!ColorMatch::analyze(image,{},QRectF(0,0,1,1),a,error,&cancelled),"Analysis cancellation");
  require(ColorMatch::preview(image,fixture(),&cancelled).isNull(),"Preview cancellation");
}
class Source final:public TStandardRasterFx {
  TRasterP raster;
public:
  Source(TRasterP r):raster(r){}
  const TPersistDeclaration *getDeclaration() const override {return nullptr;}
  bool canHandle(const TRenderSettings &,double) override {return true;}
  bool doGetBBox(double,TRectD &box,const TRenderSettings &) override {box=TRectD(0,0,raster->getLx(),raster->getLy());return true;}
  std::string getAlias(double,const TRenderSettings &) const override {return "reference-color-test-source";}
  void compute(TTile &tile,double,const TRenderSettings &) override {tile.getRaster()->copy(raster);}
  void doCompute(TTile &tile,double f,const TRenderSettings &r) override {compute(tile,f,r);}
};
void setProfile(ReferenceColorMatchFx &fx,const ColorMatch::Profile &p) {
  TStringParamP param=TParamP(fx.getParams()->getParam("profile"));
  param->setValue(ColorMatch::serialize(p).toStdWString());
}
template<class P> void rendering() {
  TRasterPT<P> source(2,1),output(2,1),mask(2,1),ink(2,1);
  const double max=P::maxChannelValue;
  auto &pixel=source->pixels(0)[0];
  pixel=P(typename P::Channel(max*.2),typename P::Channel(max*.2),typename P::Channel(max*.2),typename P::Channel(max*.5));
  source->pixels(0)[1]=P(0,0,0,0);
  ReferenceColorMatchFx fx;setProfile(fx,fixture());fx.getInputPort(0)->setFx(new Source(source));
  TTile tile(output,TPointD());TRenderSettings settings;
  fx.doCompute(tile,0,settings);
  require(output->pixels(0)[0].m==pixel.m && output->pixels(0)[1].m==0,"Alpha changed");
  auto first=output->pixels(0)[0];fx.doCompute(tile,80,settings);
  require(first==output->pixels(0)[0],"Shot profile changed with frame");
  mask->clear();fx.getInputPort(1)->setFx(new Source(mask));fx.doCompute(tile,0,settings);
  require(output->pixels(0)[0]==pixel,"Black mask changed source");
  mask->fill(P(P::maxChannelValue,P::maxChannelValue,P::maxChannelValue,P::maxChannelValue));
  ink->fill(P(P::maxChannelValue,P::maxChannelValue,P::maxChannelValue,P::maxChannelValue));
  fx.getInputPort(2)->setFx(new Source(ink));fx.doCompute(tile,0,settings);
  require(output->pixels(0)[0]==pixel,"Protected ink changed");
  auto p=fixture();p.strength=0;setProfile(fx,p);fx.doCompute(tile,0,settings);
  require(output->pixels(0)[0]==pixel,"Zero strength changed raster");
}
void paletteUndo() {
  TPaletteP palette=new TPalette();TPaletteHandle handle;handle.setPalette(palette.getPointer());
  int id=palette->addStyle(TPixel32(100,100,100,128));
  auto before=palette->getStyle(id)->getMainColor();
  require(!PaletteCmd::applyReferenceColors(&handle,{{id,TPixel32::Red},{0,TPixel32::Red}}),"Invalid batch accepted");
  require(palette->getStyle(id)->getMainColor()==before,"Partial palette update");
  const int count=palette->getStyleCount();
  require(PaletteCmd::applyReferenceColors(&handle,{{id,TPixel32::Red}}),"Palette map rejected");
  require(palette->getStyle(id)->getMainColor().m==128 && palette->getStyleCount()==count,"Palette identity/alpha changed");
  TUndoManager::manager()->undo();require(palette->getStyle(id)->getMainColor()==before,"Palette undo failed");
  TUndoManager::manager()->redo();require(palette->getStyle(id)->getMainColor().r==255,"Palette redo failed");
  palette->setIsLocked(true);
  require(!PaletteCmd::applyReferenceColors(&handle,{{id,before}}),"Locked palette changed");
  TUndoManager::manager()->reset();
}
}
int main(int argc,char **argv) {
  QCoreApplication application(argc,argv);
  try {
    mathAndProfiles();sampling();rendering<TPixel32>();rendering<TPixel64>();rendering<TPixelF>();paletteUndo();
    QTemporaryDir dir;require(dir.isValid(),"Temporary directory");QString error;
    require(ColorMatch::exportCube(dir.filePath("look.cube"),fixture(),error),"LUT export");
    Lut3D lut;require(lut.load(dir.filePath("look.cube"),&error),"Existing LUT parser rejected export");
    ReferenceColorMatchFx original;setProfile(original,fixture());
    TFilePath path(dir.filePath("profile.xml").toStdWString());
    {TOStream stream(path);stream << &original;}
    TPersist *loaded=nullptr;{TIStream stream(path);stream >> loaded;}
    std::unique_ptr<TPersist> owner(loaded);
    auto restored=dynamic_cast<ReferenceColorMatchFx *>(loaded);
    require(restored!=nullptr,"FX type persistence failed");
    TStringParamP p=TParamP(restored->getParams()->getParam("profile"));
    require(p->getValue()==ColorMatch::serialize(fixture()).toStdWString(),"Scene persistence failed");
    std::cout<<"Reference color matching checks passed\n";
  }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
  return 0;
}
