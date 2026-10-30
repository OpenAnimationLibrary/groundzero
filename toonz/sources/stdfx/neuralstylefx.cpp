#include "stdfx.h"
#include "globalcontrollablefx.h"
#include "tnotanimatableparam.h"
#include "tfxparam.h"
#include "celfxsourcegrid.h"
#include "neuraltemporal.h"
#include "toonz/neuralstyle.h"
#include <QCache>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <vector>

namespace {
struct Frame { QImage source, coverage, styled; };
std::mutex cacheMutex;
QCache<QByteArray,Frame> cache(32*1024); // KiB, complete immutable checkpoints only.
QString componentDirectory() {
  return QDir(QCoreApplication::applicationDirPath()).filePath("neural-style");
}
QByteArray componentIdentity() {
  QFile file(QDir(componentDirectory()).filePath("package.json"));
  if(!file.open(QIODevice::ReadOnly) || file.size()>16384) return "missing-component";
  return QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256);
}
void hashImage(QCryptographicHash &hash,const QImage &image) {
  for(int y=0;y<image.height();++y)
    hash.addData(reinterpret_cast<const char *>(image.constScanLine(y)),image.width()*4);
}
struct Grid {
  TRectD domain;
  TRectD sample;
  TDimension size;
  TRenderSettings settings;
  double sx,sy;
  int left,bottom;
  explicit Grid(const TRenderSettings &info) : domain(info.m_cameraBox),settings(info) {
    if(!CelFxSourceGrid::finiteBox(domain) || domain.getLx()>1000000 || domain.getLy()>1000000)
      throw TException("Neural Style: a finite camera view is required.");
    const double scale=384/std::max(domain.getLx(),domain.getLy());
    size=TDimension(std::max(1,int(std::round(domain.getLx()*scale))),
                    std::max(1,int(std::round(domain.getLy()*scale))));
    left=(384-size.lx)/2;bottom=(384-size.ly)/2;
    sx=size.lx/domain.getLx();sy=size.ly/domain.getLy();
    const TAffine mapping(sx,0,left-domain.x0*sx,0,sy,bottom-domain.y0*sy);
    settings.m_affine=mapping*info.m_affine;
    settings.m_cameraBox=mapping*domain;
    settings.m_bpp=32;settings.m_linearColorSpace=false;
    sample=TRectD(left,bottom,left+size.lx,bottom+size.ly);
  }
};
double mask(const TRaster32P &raster,int x,int y) {
  if(!raster)return 1;
  const auto p=raster->pixels(y)[x];
  return (.2126*p.r+.7152*p.g+.0722*p.b)/255;
}
template<class P> double mask(const TRasterPT<P> &raster,int x,int y) {
  if(!raster)return 1;
  const auto p=raster->pixels(y)[x];
  const double value=(.2126*p.r+.7152*p.g+.0722*p.b)/P::maxChannelValue;
  return std::isfinite(value)?std::clamp(value,0.0,1.0):0;
}
std::array<double,3> sample(const QImage &image,double x,double y) {
  x=std::clamp(x,0.0,383.0);y=std::clamp(y,0.0,383.0);
  const int x0=int(x),y0=int(y),x1=std::min(x0+1,383),y1=std::min(y0+1,383);
  const double dx=x-x0,dy=y-y0;
  std::array<double,3> result{{0,0,0}};
  const auto add=[&](int xx,int yy,double w){
    const auto p=image.pixel(xx,yy);result[0]+=qRed(p)*w/255;result[1]+=qGreen(p)*w/255;result[2]+=qBlue(p)*w/255;
  };
  add(x0,y0,(1-dx)*(1-dy));add(x1,y0,dx*(1-dy));add(x0,y1,(1-dx)*dy);add(x1,y1,dx*dy);
  return result;
}
template<class P> bool composite(TRasterPT<P> output,TRasterPT<P> selection,TRasterPT<P> ink,
                                 const QImage &styled,const Grid &grid,const TPointD &position,
                                 double strength,const int *cancel) {
  output->lock();if(selection)selection->lock();if(ink)ink->lock();
  bool complete=true;
  for(int y=0;y<output->getLy();++y) {
    if(cancel && *cancel){complete=false;break;}
    auto row=output->pixels(y);
    for(int x=0;x<output->getLx();++x) {
      auto &p=row[x];
      const double xx=position.x+x+.5,yy=position.y+y+.5;
      if(xx<grid.domain.x0 || xx>=grid.domain.x1 || yy<grid.domain.y0 || yy>=grid.domain.y1 ||
         p.m<=0 || !std::isfinite(double(p.m)))continue;
      const double weight=strength*mask<P>(selection,x,y)*(ink?1-mask<P>(ink,x,y):1);
      if(weight<=0)continue;
      const auto color=sample(styled,grid.left+(xx-grid.domain.x0)*grid.sx-.5,
                              383-(grid.bottom+(yy-grid.domain.y0)*grid.sy-.5));
      auto blend=[&](typename P::Channel original,double value) {
        if(!std::isfinite(double(original)))return original;
        const double mixed=original*(1-weight)+value*p.m*weight;
        if(P::maxChannelValue==1)return typename P::Channel(mixed);
        return typename P::Channel(std::clamp(mixed,0.0,double(p.m))+.5);
      };
      p.r=blend(p.r,color[0]);p.g=blend(p.g,color[1]);p.b=blend(p.b,color[2]);
    }
  }
  if(ink)ink->unlock();if(selection)selection->unlock();output->unlock();return complete;
}
}

class NeuralStyleFx final : public GlobalControllableFx {
  FX_PLUGIN_DECLARATION(NeuralStyleFx)
  TRasterFxPort m_source,m_mask,m_ink;
  TStringParamP m_profile;
  TDoubleParamP m_strength;
  TIntParamP m_history,m_start,m_end;
  std::vector<double> frames(double frame) const {
    const int start=m_start->getValue()-1,end=m_end->getValue()-1;
    if(end<start || end-start>=120)throw TException("Neural Style: split shots into ranges of at most 120 frames.");
    if(frame<start || frame>end)return {};
    if(m_history->getValue()==0 || frame!=std::floor(frame))return {frame};
    std::vector<double> result;
    for(int f=start;f<=int(frame);++f)result.push_back(f);
    return result;
  }
  Frame input(double frame,const Grid &grid) {
    TRaster32P pixels(grid.size.lx,grid.size.ly);
    TTile tile(pixels,TPointD(grid.left,grid.bottom)),selection,ink;
    m_source->compute(tile,frame,grid.settings);
    if(m_mask.isConnected())m_mask->allocateAndCompute(selection,tile.m_pos,grid.size,pixels,frame,grid.settings);
    if(m_ink.isConnected())m_ink->allocateAndCompute(ink,tile.m_pos,grid.size,pixels,frame,grid.settings);
    Frame result;
    result.source=QImage(384,384,QImage::Format_RGB32);result.source.fill(Qt::white);
    result.coverage=QImage(384,384,QImage::Format_RGB32);result.coverage.fill(Qt::black);
    const TRaster32P selected=selection.getRaster(),protectedInk=ink.getRaster();
    pixels->lock();if(selected)selected->lock();if(protectedInk)protectedInk->lock();
    for(int y=0;y<grid.size.ly;++y) {
      const auto row=pixels->pixels(y);
      auto colors=reinterpret_cast<QRgb *>(result.source.scanLine(383-grid.bottom-y));
      auto coverage=reinterpret_cast<QRgb *>(result.coverage.scanLine(383-grid.bottom-y));
      for(int x=0;x<grid.size.lx;++x) {
        const auto p=row[x];
        colors[grid.left+x]=qRgb(std::min(255,int(p.r)+255-p.m),std::min(255,int(p.g)+255-p.m),std::min(255,int(p.b)+255-p.m));
        const int a=int(p.m*mask(selected,x,y)*(protectedInk?1-mask(protectedInk,x,y):1)+.5);
        coverage[grid.left+x]=qRgb(a,a,a);
      }
    }
    if(protectedInk)protectedInk->unlock();if(selected)selected->unlock();pixels->unlock();
    return result;
  }
public:
  NeuralStyleFx():m_profile(L""),m_strength(100),m_history(70),m_start(1),m_end(120) {
    addInputPort("Source",m_source);addInputPort("Mask",m_mask);addInputPort("ProtectInk",m_ink);
    bindParam(this,"profile",m_profile);bindParam(this,"strength",m_strength);
    bindParam(this,"temporal",m_history);bindParam(this,"shotStart",m_start);bindParam(this,"shotEnd",m_end);
    m_strength->setValueRange(0,100);m_history->setValueRange(0,85);
    m_start->setValueRange(1,1000000);m_end->setValueRange(1,1000000);
    enableComputeInFloat(true);
  }
  bool canHandle(const TRenderSettings &,double) override{return true;}
  bool requiresFullPrecisionSerialization() const override{return true;}
  TAffine handledAffine(const TRenderSettings &info,double) override{return info.m_affine;}
  bool toBeComputedInLinearColorSpace(bool,bool) const override{return false;}
  void compute(TTile &tile,double frame,const TRenderSettings &info) override {
    CelFxSourceGrid::computeInCamera(this,tile,frame,info);
  }
  void dryCompute(TRectD &rect,double frame,const TRenderSettings &info) override {
    CelFxSourceGrid::dryComputeInCamera(this,rect,frame,info);
  }
  bool doGetBBox(double frame,TRectD &box,const TRenderSettings &info) override {
    if(!m_source.isConnected()){box=TRectD();return false;}
    return m_source->doGetBBox(frame,box,info);
  }
  int getMemoryRequirement(const TRectD &rect,double,const TRenderSettings &info) override {
    // 512 MiB worker, <=203 MiB canonical input/result history, 32 MiB cache,
    // flow/intermediate buffers and atomic output/mask tiles. Model input is
    // always 384 square regardless of the camera's output resolution.
    const long double width=std::max(0.0,rect.getLx()+1),height=std::max(0.0,rect.getLy()+1);
    const long double estimate=800+width*height*info.m_bpp/(2*1024*1024);
    return !std::isfinite(estimate) || estimate>std::numeric_limits<int>::max() ?
        std::numeric_limits<int>::max():int(std::ceil(estimate));
  }
  std::string getAlias(double frame,const TRenderSettings &info) const override {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(QByteArray::fromStdString(TRasterFx::getAlias(frame,info)));
    hash.addData(componentIdentity());
    std::ostringstream camera;camera.imbue(std::locale::classic());camera<<std::setprecision(17)
      <<info.m_cameraBox.x0<<','<<info.m_cameraBox.y0<<','<<info.m_cameraBox.x1<<','<<info.m_cameraBox.y1
      <<':'<<m_strength->getValue(frame);
    hash.addData(QByteArray::fromStdString(camera.str()));
    if(!m_profile->getValue().empty() && m_strength->getValue(frame)>0 && m_source.isConnected())
      for(const auto f:frames(frame))for(const auto port:{&m_source,&m_mask,&m_ink})
        if(port->isConnected())hash.addData(QByteArray::fromStdString(port->getFx()->getAlias(f,info)));
    return "neural-style-v1:"+hash.result().toHex().toStdString();
  }
  void doDryCompute(TRectD &rect,double frame,const TRenderSettings &info) override {
    if(!m_source.isConnected())return;
    auto sourceRect=rect;m_source->dryCompute(sourceRect,frame,info);
    if(m_profile->getValue().empty() || m_strength->getValue(frame)<=0)return;
    const auto times=frames(frame);if(times.empty())return;
    for(auto port:{&m_mask,&m_ink})if(port->isConnected()) {
      auto area=rect;port->operator->()->dryCompute(area,frame,info);
    }
    const Grid grid(info);
    for(const auto f:times)for(auto port:{&m_source,&m_mask,&m_ink})if(port->isConnected()) {
      auto area=grid.sample;port->operator->()->dryCompute(area,f,grid.settings);
    }
  }
  void doCompute(TTile &tile,double frame,const TRenderSettings &info) override {
    if(!m_source.isConnected()){tile.getRaster()->clear();return;}
    if(!m_profile->getValue().empty() && m_strength->getValue(frame)>0 &&
       qint64(tile.getRaster()->getLx())*tile.getRaster()->getLy()>16777216)
      throw TException("Neural Style: output tile exceeds the 16 megapixel limit.");
    m_source->compute(tile,frame,info);
    if(m_profile->getValue().empty() || m_strength->getValue(frame)<=0 || (info.m_isCanceled && *info.m_isCanceled))return;
    const auto times=frames(frame);if(times.empty())return;
    const auto raster=tile.getRaster();
    if(qint64(raster->getLx())*raster->getLy()>16777216)throw TException("Neural Style: output tile exceeds the 16 megapixel limit.");
    NeuralStyle::Profile profile;QString error;
    const QString serialized=QString::fromStdWString(m_profile->getValue());
    if(!NeuralStyle::parse(serialized,profile,error))throw TException(error.toStdWString());
    const Grid grid(info);
    // Complete all upstream rendering before reserving the one inference worker.
    // This permits nested neural FX without a process-lease dependency cycle.
    std::vector<Frame> inputs;inputs.reserve(times.size());
    for(const auto f:times) {
      if(info.m_isCanceled && *info.m_isCanceled)return;
      inputs.push_back(input(f,grid));
    }
    NeuralStyle::Session session(componentDirectory());
    QByteArray chain=QCryptographicHash::hash(serialized.toUtf8()+componentIdentity()+
        QByteArray::number(m_history->getValue()),QCryptographicHash::Sha256);
    std::vector<QByteArray> keys;
    std::vector<QImage> pending, fresh(inputs.size());
    std::vector<size_t> pendingIndices;
    for(size_t i=0;i<inputs.size();++i) {
      auto &current=inputs[i];
      QCryptographicHash hash(QCryptographicHash::Sha256);hash.addData(chain);
      hashImage(hash,current.source);hashImage(hash,current.coverage);chain=hash.result();
      keys.push_back(chain);
      {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if(const auto found=cache.object(chain))current.styled=found->styled;
      }
      const bool held=i && inputs[i-1].source==current.source && inputs[i-1].coverage==current.coverage;
      if(current.styled.isNull() && !held) {
        pending.push_back(current.source);pendingIndices.push_back(i);
      }
    }
    if(!pending.empty()) {
      std::vector<QImage> inferred;
      if(!session.styleBatch(profile.reference,pending,inferred,error,{nullptr,info.m_isCanceled})) {
        if(info.m_isCanceled && *info.m_isCanceled)return;
        throw TException(error.toStdWString());
      }
      for(size_t i=0;i<pendingIndices.size();++i)fresh[pendingIndices[i]]=inferred[i];
    }
    Frame previous;
    for(size_t i=0;i<inputs.size();++i) {
      if(info.m_isCanceled && *info.m_isCanceled)return;
      auto &current=inputs[i];
      if(current.styled.isNull()) {
        if(!previous.styled.isNull() && previous.source==current.source && previous.coverage==current.coverage)
          current.styled=previous.styled;
        else if(previous.styled.isNull())current.styled=fresh[i];
        else if(!NeuralTemporal::stabilize(previous.source,current.source,previous.coverage,current.coverage,
                     previous.styled,fresh[i],m_history->getValue()/100.0,current.styled,info.m_isCanceled))return;
        std::lock_guard<std::mutex> lock(cacheMutex);
        cache.insert(keys[i],new Frame(current),3*384*384*4/1024);
      }
      previous=current;
    }
    session.stop();
    TTile selection,ink;
    if(m_mask.isConnected())m_mask->allocateAndCompute(selection,tile.m_pos,raster->getSize(),raster,frame,info);
    if(m_ink.isConnected())m_ink->allocateAndCompute(ink,tile.m_pos,raster->getSize(),raster,frame,info);
    TRasterP output=raster->clone();bool complete=false;
    const double strength=m_strength->getValue(frame)/100;
    if(TRaster32P r=output)complete=composite<TPixel32>(r,selection.getRaster(),ink.getRaster(),previous.styled,grid,tile.m_pos,strength,info.m_isCanceled);
    else if(TRaster64P r=output)complete=composite<TPixel64>(r,selection.getRaster(),ink.getRaster(),previous.styled,grid,tile.m_pos,strength,info.m_isCanceled);
    else if(TRasterFP r=output)complete=composite<TPixelF>(r,selection.getRaster(),ink.getRaster(),previous.styled,grid,tile.m_pos,strength,info.m_isCanceled);
    else throw TException("Neural Style: unsupported raster type.");
    if(complete)raster->copy(output);
  }
};
FX_PLUGIN_IDENTIFIER(NeuralStyleFx,"neuralStyleFx")
