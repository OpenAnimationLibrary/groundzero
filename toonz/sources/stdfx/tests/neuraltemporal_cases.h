#pragma once
#include "../neuraltemporal.h"
#include <QPoint>
#include <stdexcept>
namespace NeuralTemporalCases {
inline void run() {
  const auto require=[](bool value,const char *message){if(!value)throw std::runtime_error(message);};
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
  // A proven integer pan must not interpolate a texture that is already stable.
  // Exercise both signs, both axes, diagonals, reveal strips and the motion bound.
  quint32 seed=9127;
  for(int y=0;y<384;++y)for(int x=0;x<384;++x) {
    seed=seed*1664525u+1013904223u;
    source.setPixel(x,y,qRgb((seed>>16)&255,(seed>>8)&255,seed&255));
    history.setPixel(x,y,qRgb((x*7+y*3)%256,(x*3+y*11)%256,(x*13+y)%256));
  }
  for(const QPoint delta:{QPoint(8,0),QPoint(-8,0),QPoint(0,8),QPoint(0,-8),QPoint(8,-8),QPoint(64,0)}) {
    changed.fill(Qt::white);fresh.fill(Qt::blue);
    for(int y=0;y<384;++y)for(int x=0;x<384;++x) {
      const int px=x-delta.x(),py=y-delta.y();if(px<0 || py<0 || px>=384 || py>=384)continue;
      changed.setPixel(x,y,source.pixel(px,py));fresh.setPixel(x,y,history.pixel(px,py));
    }
    for(double amount:{0.,.01,.7,.85}) {
      require(NeuralTemporal::stabilize(source,changed,alpha,alpha,history,fresh,amount,output),"Exact pan processing failed");
      require(output==fresh,"Temporal processing moved an already stable translated texture");
    }
  }
  // A broad opaque occluder disappears while alpha stays unchanged. Fresh pixels
  // only inside its rectangle would leave a palette seam against retained history.
  for(int y=0;y<384;++y)for(int x=0;x<384;++x)
    changed.setPixel(x,y,qRgb(160+x/4,170+y/5,140));
  source=changed;
  for(int y=30;y<354;++y)for(int x=150;x<215;++x)source.setPixel(x,y,qRgb(10,20,30));
  history.fill(Qt::red);fresh.fill(Qt::blue);
  require(NeuralTemporal::stabilize(source,changed,alpha,alpha,history,fresh,.7,output),"Large reveal processing failed");
  require(output==fresh,"Large disocclusion left a history/fresh tone seam");
}
}
