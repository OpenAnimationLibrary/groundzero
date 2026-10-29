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
  p.sourceCrop=QRectF(.1,.2,.5,.6);
  p.referenceCrop=QRectF(.2,.1,.7,.8);
  require(ColorMatch::parse(ColorMatch::serialize(p),restored,error),"Roundtrip");
  require(ColorMatch::serialize(restored)==ColorMatch::serialize(p),"Stable serialization");
  require(restored.sourceCrop==p.sourceCrop && restored.referenceCrop==p.referenceCrop,"Crop persistence");
  QImage sample(2,1,QImage::Format_ARGB32);sample.fill(qRgb(1,2,3));
  p.sampleNames[0]="source.png";p.sampleDigests[0]=ColorMatch::sampleDigest(sample);
  require(p.sampleDigests[0].size()==64,"Missing sample fingerprint");
  require(ColorMatch::sampleDigest(sample.copy())==p.sampleDigests[0],"Unstable fingerprint");
  sample.setPixel(1,0,qRgb(2,3,4));
  require(ColorMatch::sampleDigest(sample)!=p.sampleDigests[0],"Changed sample has old fingerprint");
  require(ColorMatch::parse(ColorMatch::serialize(p),restored,error),"Sample metadata roundtrip");
  require(restored.sampleNames==p.sampleNames && restored.sampleDigests==p.sampleDigests,"Sample metadata lost");
  const auto original=ColorMatch::serialize(restored);
  require(!ColorMatch::parse(QString(original).replace("source.png","folder/source.png"),restored,error),"Sample paths accepted");
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
  void compute(TTile &tile,double,const TRenderSettings &) override {
    const int x=int(tile.m_pos.x),y=int(tile.m_pos.y);
    tile.getRaster()->copy(raster->extract(x,y,x+tile.getRaster()->getLx()-1,y+tile.getRaster()->getLy()-1));
  }
  void doCompute(TTile &tile,double f,const TRenderSettings &r) override {compute(tile,f,r);}
};
void setProfile(ReferenceColorMatchFx &fx,const ColorMatch::Profile &p) {
  TStringParamP param=TParamP(fx.getParams()->getParam("profile"));
  param->setValue(ColorMatch::serialize(p).toStdWString());
}
template<class P> void rendering() {
  TRasterPT<P> source(2,1),output(2,1),mask(2,1),ink(2,1);
  const double max=P::maxChannelValue;
  const auto channel=[max](double value) {return typename P::Channel(value*max/255.0);};
  auto &pixel=source->pixels(0)[0];
  pixel=P(channel(40),channel(40),channel(40),channel(102));
  source->pixels(0)[1]=P(0,0,0,0);
  ReferenceColorMatchFx fx;setProfile(fx,fixture());fx.getInputPort(0)->setFx(new Source(source));
  TTile tile(output,TPointD());TRenderSettings settings;
  fx.doCompute(tile,0,settings);
  const double tolerance=max==1?1e-5:1.0;
  close(output->pixels(0)[0].r,72*max/255,tolerance);
  close(output->pixels(0)[0].g,24*max/255,tolerance);
  close(output->pixels(0)[0].b,16*max/255,tolerance);
  require(output->pixels(0)[0].m==pixel.m && output->pixels(0)[1]==source->pixels(0)[1],"Alpha changed");
  auto first=output->pixels(0)[0];
  for(double frame:{80.0,2.0,41.0,0.0}) {
    fx.doCompute(tile,frame,settings);
    require(first==output->pixels(0)[0],"Shot profile changed with frame order");
  }
  TRasterPT<P> partition(1,1);TTile part(partition,TPointD(1,0));
  fx.doCompute(part,0,settings);
  require(partition->pixels(0)[0]==output->pixels(0)[1],"Offset tile mismatch");
  part.m_pos=TPointD(0,0);fx.doCompute(part,0,settings);
  require(partition->pixels(0)[0]==first,"Partitioned tile mismatch");
  mask->clear();fx.getInputPort(1)->setFx(new Source(mask));fx.doCompute(tile,0,settings);
  require(output->pixels(0)[0]==pixel,"Black mask changed source");
  mask->fill(P(channel(51),channel(51),channel(51),channel(255)));
  fx.doCompute(tile,0,settings);
  close(output->pixels(0)[0].r,46.4*max/255,tolerance);
  close(output->pixels(0)[0].g,36.8*max/255,tolerance);
  close(output->pixels(0)[0].b,35.2*max/255,tolerance);
  mask->fill(P(P::maxChannelValue,P::maxChannelValue,P::maxChannelValue,P::maxChannelValue));
  ink->fill(P(channel(204),channel(204),channel(204),channel(255)));
  fx.getInputPort(2)->setFx(new Source(ink));fx.doCompute(tile,0,settings);
  close(output->pixels(0)[0].r,46.4*max/255,tolerance);
  close(output->pixels(0)[0].g,36.8*max/255,tolerance);
  close(output->pixels(0)[0].b,35.2*max/255,tolerance);
  ink->fill(P(P::maxChannelValue,P::maxChannelValue,P::maxChannelValue,P::maxChannelValue));
  fx.doCompute(tile,0,settings);
  require(output->pixels(0)[0]==pixel,"Protected ink changed");
  fx.getInputPort(1)->setFx(nullptr);fx.getInputPort(2)->setFx(nullptr);
  const auto alias=fx.getAlias(0,settings);
  auto p=fixture();p.strength=0;setProfile(fx,p);
  require(fx.getAlias(0,settings)!=alias,"Profile did not invalidate render identity");
  fx.doCompute(tile,0,settings);
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
    QImage small(32,16,QImage::Format_ARGB32);small.fill(qRgb(100,100,100));
    require(small.save(dir.filePath("small.png")),"Sample fixture save");
    require(ColorMatch::loadSample(dir.filePath("small.png"),error).size()==small.size(),
            "Small references/masks must not be upsampled");
    require(ColorMatch::exportCube(dir.filePath("look.cube"),fixture(),error),"LUT export");
    Lut3D lut;require(lut.load(dir.filePath("look.cube"),&error),"Existing LUT parser rejected export");
    for(const auto &input:{ColorMatch::Color{{0,0,0}},ColorMatch::Color{{1,1,1}},
                           ColorMatch::Color{{.25,.5,.75}},ColorMatch::Color{{.75,.25,.5}}}) {
      float r=float(input[0]),g=float(input[1]),b=float(input[2]);lut.convert(r,g,b);
      const auto expected=ColorMatch::transform(input,fixture());
      close(r,expected[0]);close(g,expected[1]);close(b,expected[2]);
    }
    ReferenceColorMatchFx original;setProfile(original,fixture());
    TFilePath path(dir.filePath("profile.xml").toStdWString());
    {TOStream stream(path);stream << &original;}
    TPersist *loaded=nullptr;{TIStream stream(path);stream >> loaded;}
    std::unique_ptr<TPersist> owner(loaded);
    auto restored=dynamic_cast<ReferenceColorMatchFx *>(loaded);
    require(restored!=nullptr,"FX type persistence failed");
    TStringParamP p=TParamP(restored->getParams()->getParam("profile"));
    require(p->getValue()==ColorMatch::serialize(fixture()).toStdWString(),"Scene persistence failed");
    TRaster32P source(1,1),direct(1,1),reopened(1,1);source->fill(TPixel32(40,40,40,102));
    original.getInputPort(0)->setFx(new Source(source));restored->getInputPort(0)->setFx(new Source(source));
    TTile a(direct,TPointD()),b(reopened,TPointD());TRenderSettings settings;
    original.doCompute(a,0,settings);restored->doCompute(b,37,settings);
    require(direct->pixels(0)[0]==reopened->pixels(0)[0],"Reopened scene rendered differently");
    std::cout<<"Reference color matching checks passed\n";
  }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
  return 0;
}
