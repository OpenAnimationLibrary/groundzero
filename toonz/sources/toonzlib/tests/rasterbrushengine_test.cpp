#include "toonz/rasterbrushengine.h"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace RasterBrush;
namespace {
void require(bool v,const char *s){if(!v)throw std::runtime_error(s);}
struct Canvas:Surface {
  int w=128,h=128;bool refuse=false;std::vector<Pixel> pixels;
  Canvas():pixels(size_t(w)*h){}
  int width()const override{return w;}int height()const override{return h;}
  bool prepare(int x0,int y0,int x1,int y1)override {
    require(x0>=0&&y0>=0&&x1<=w&&y1<=h&&x0<x1&&y0<y1,"unclipped transaction");return !refuse;
  }
  Pixel read(int x,int y)const override{return pixels.at(size_t(y)*w+x);}
  void write(int x,int y,const Pixel &p)override{pixels.at(size_t(y)*w+x)=p;}
  double coverage(int x,int)const override{return x<100?1:0;}
};
bool same(const Canvas &a,const Canvas &b){
  for(size_t i=0;i<a.pixels.size();++i){const auto &x=a.pixels[i],&y=b.pixels[i];
    if(x.r!=y.r||x.g!=y.g||x.b!=y.b||x.a!=y.a)return false;}return true;
}
Sample sample(double x,double y,double t,double pressure=1){Sample s;s.x=x;s.y=y;s.time=t;s.pressure=pressure;s.hasPressure=true;return s;}
void line(Canvas &c,Preset p){Stroke s(c,p);for(int i=0;i<=120;++i)require(s.add(sample(i,64,i/240.0)),s.error().c_str());s.finish();}
void testComposition(){
  Pixel a{.2,.1,0,.5},b{.1,.3,.2,.5};
  Pixel n=composite(a,b,Blend::Normal);
  require(std::abs(n.a-.75)<1e-12&&std::abs(n.r-.2)<1e-12,"premultiplied source-over");
  require(composite(a,b,Blend::Erase).a==.25,"erase alpha");
  require(composite(a,b,Blend::Screen,true).a==a.a,"lock alpha");
  Pixel z=composite({}, {},Blend::Overlay);require(z.a==0&&z.r==0,"transparent color");
}
void testDeterminismAndMasks(){
  Preset p;p.size.value=12;p.scatter.value=.2;p.count=3;p.color={.8,.2,.1,1};
  Canvas a,b,c;line(a,p);line(b,p);require(same(a,b),"random stroke not deterministic");
  ++p.seed;line(c,p);require(!same(a,c),"different seed ignored");
  for(int y=0;y<c.h;++y)for(int x=100;x<c.w;++x)require(c.read(x,y).a==0,"selection escaped");
  p.scatter.value=0;p.count=1;p.secondary.enabled=true;p.secondary.mode=MaskMode::Subtract;
  p.secondary.size.value=1;p.secondary.tip.hardness=1;
  Canvas d;line(d,p);for(const auto &v:d.pixels)require(v.a==0,"subtract mask ignored");
}
void testWashAndInput(){
  Preset p;p.wash=true;p.opacity.value=.3;p.size.value=20;p.color={1,0,0,1};
  Canvas c;line(c,p);double peak=0;for(auto v:c.pixels)peak=std::max(peak,v.a);
  require(std::abs(peak-.3)<1e-10,"wash opacity accumulated per dab");
  p.wash=false;Canvas d;line(d,p);require(d.read(64,64).a>.3,"build-up did not accumulate");
  p.flow.mappings.push_back({Sensor::Pressure,{{0,0},{1,1}},1});
  Canvas e;Stroke s(e,p);require(s.add(sample(20,20,0,0)),"zero pressure sample rejected");
  for(auto v:e.pixels)require(v.a==0,"zero pressure painted");
  Sample invalid=sample(20,20,-1);require(!s.add(invalid),"backward timestamp accepted");
  Canvas f;f.refuse=true;Stroke denied(f,p);require(!denied.add(sample(20,20,0)),"transaction denial ignored");
}
void testResourcesAndWet(){
  Preset p;p.wet=Wet::Dulling;p.color={1,0,0,1};
  Canvas c;for(auto &v:c.pixels)v={0,0,1,1};
  line(c,p);Pixel middle=c.read(64,64);require(middle.r>0&&middle.b>0,"wet brush did not mix pickup and paint");
  p.wet=Wet::Smear;Canvas a,b;for(auto &v:a.pixels)v={0,0,1,1};b.pixels=a.pixels;
  line(a,p);line(b,p);require(same(a,b),"smear not deterministic");
  p.wet=Wet::Off;p.wash=true;Canvas low;Stroke limited(low,p,1);
  require(!limited.add(sample(20,20,0)),"stroke memory budget ignored");
  p.profile="krita-unverified";require(!validate(p).empty(),"unknown profile silently approximated");
  p.profile="groundzero-native-1";p.tip.shape=Shape::Bitmap;
  require(!validate(p).empty(),"missing bitmap accepted");
  p.tip.shape=Shape::Round;p.size.value=std::numeric_limits<double>::infinity();
  require(!validate(p).empty(),"non-finite setting accepted");
}
void testStationaryAndBatching(){
  Preset p;p.airbrushRate=10;p.size.value=4;Canvas a,b;Stroke sa(a,p),sb(b,p);
  std::vector<Sample> events;for(int i=0;i<=240;++i)events.push_back(sample(32,32,i/240.0));
  for(auto e:events)require(sa.add(e),"stationary event failed");
  for(size_t batch=0;batch<events.size();batch+=16)
    for(size_t i=batch;i<std::min(batch+16,events.size());++i)require(sb.add(events[i]),"batched event failed");
  require(same(a,b)&&sa.dabCount()==sb.dabCount(),"delivery batching changed stroke");
  require(sa.dabCount()>=10&&sa.dabCount()<=11,"airbrush timer did not schedule stationary dabs");
}
}
int main(){try{testComposition();testDeterminismAndMasks();testWashAndInput();testResourcesAndWet();testStationaryAndBatching();}
catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}std::cout<<"Native raster engine regression passed\n";}
