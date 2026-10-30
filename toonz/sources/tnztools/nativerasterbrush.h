#pragma once
#include <algorithm>
#include <cmath>
#include "toonz/rasterbrushengine.h"
#include "mypainttoonzbrush.h"

class NativeRasterBrushSurface final : public RasterBrush::Surface {
  TRaster32P raster;
  RasterController &controller;
public:
  NativeRasterBrushSurface(const TRaster32P &r, RasterController &c):raster(r),controller(c){}
  int width()const override{return raster->getLx();}
  int height()const override{return raster->getLy();}
  bool prepare(int x0,int y0,int x1,int y1)override {
    return controller.askWrite(TRect(x0,y0,x1-1,y1-1));
  }
  RasterBrush::Pixel read(int x,int y)const override{
    const TPixel32 &p=raster->pixels(y)[x];return {p.r/255.0,p.g/255.0,p.b/255.0,p.m/255.0};
  }
  void write(int x,int y,const RasterBrush::Pixel &p)override{
    auto q=[](double v){return (TPixel32::Channel)std::max(0,std::min(255,int(std::floor(v*255+.5))));};
    raster->pixels(y)[x]=TPixel32(q(p.r),q(p.g),q(p.b),q(p.a));
  }
};
