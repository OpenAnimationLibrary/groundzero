#include "neuraltemporal.h"
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/video/tracking.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace NeuralTemporal {
namespace {
cv::Mat gray(const QImage &image) {
  const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
  const cv::Mat view(rgba.height(),rgba.width(),CV_8UC4,
                     const_cast<uchar *>(rgba.constBits()),rgba.bytesPerLine());
  cv::Mat result; cv::cvtColor(view,result,cv::COLOR_RGBA2GRAY); return result;
}
cv::Vec3f rgb(const QImage &image, double x, double y) {
  const int x0=int(x), y0=int(y), x1=std::min(x0+1,image.width()-1), y1=std::min(y0+1,image.height()-1);
  const double dx=x-x0,dy=y-y0;
  const auto value=[](QRgb p){return cv::Vec3f(float(qRed(p)),float(qGreen(p)),float(qBlue(p)));};
  return value(image.pixel(x0,y0))*float((1-dx)*(1-dy)) + value(image.pixel(x1,y0))*float(dx*(1-dy)) +
         value(image.pixel(x0,y1))*float((1-dx)*dy) + value(image.pixel(x1,y1))*float(dx*dy);
}
cv::Vec2f flow(const cv::Mat &image, double x, double y) {
  const int x0=int(x),y0=int(y),x1=std::min(x0+1,image.cols-1),y1=std::min(y0+1,image.rows-1);
  const double dx=x-x0,dy=y-y0;
  return image.at<cv::Vec2f>(y0,x0)*float((1-dx)*(1-dy)) + image.at<cv::Vec2f>(y0,x1)*float(dx*(1-dy)) +
         image.at<cv::Vec2f>(y1,x0)*float((1-dx)*dy) + image.at<cv::Vec2f>(y1,x1)*float(dx*dy);
}
// Phase correlation proposes a displacement; it never authorizes history by
// itself. Exact source/coverage comparison proves a translated drawing before
// replacing approximate flow with an integer sample (which cannot blur texture).
bool exactTranslation(const QImage &previous,const QImage &current,
                      const QImage &previousAlpha,const QImage &currentAlpha,
                      const cv::Mat &a,const cv::Mat &b,cv::Point &offset,
                      const int *cancel) {
  cv::Mat af,bf;a.convertTo(af,CV_32F);b.convertTo(bf,CV_32F);
  double response=0;const auto proposed=cv::phaseCorrelate(af,bf,cv::noArray(),&response);
  if(!std::isfinite(proposed.x) || !std::isfinite(proposed.y) || response<.5 ||
     std::abs(proposed.x)>64.5 || std::abs(proposed.y)>64.5)return false;
  const int dx=int(std::round(proposed.x)),dy=int(std::round(proposed.y));
  if(std::abs(dx)>64 || std::abs(dy)>64 || (dx==0 && dy==0) ||
     (384-std::abs(dx))*(384-std::abs(dy))<384*384*3/4)return false;
  int covered=0,visible=0,lowest=255,highest=0;
  for(int y=0;y<384;++y) {
    if(cancel && *cancel)return false;
    for(int x=0;x<384;++x) {
      const int alpha=qGray(currentAlpha.pixel(x,y));visible+=alpha>0;
      const int px=x-dx,py=y-dy;if(px<0 || py<0 || px>=384 || py>=384)continue;
      if(previous.pixel(px,py)!=current.pixel(x,y) || previousAlpha.pixel(px,py)!=currentAlpha.pixel(x,y))return false;
      if(alpha>0) {++covered;const int v=qGray(current.pixel(x,y));lowest=std::min(lowest,v);highest=std::max(highest,v);}
    }
  }
  if(covered<1024 || covered*4<visible*3 || highest-lowest<16)return false;
  offset=cv::Point(dx,dy);return true;
}
}
bool stabilize(const QImage &previous, const QImage &current,
               const QImage &previousAlpha, const QImage &currentAlpha,
               const QImage &previousStyled, const QImage &fresh,
               double amount, QImage &result, const int *cancel) {
  for(const auto &image:{previous,current,previousAlpha,currentAlpha,previousStyled,fresh})
    if(image.size()!=QSize(384,384)) throw std::invalid_argument("Temporal inputs require the fixed model canvas");
  if(!std::isfinite(amount) || amount<0 || amount>.85)
    throw std::invalid_argument("Temporal history strength must be between zero and 0.85");
  if(cancel && *cancel) return false;
  if(amount==0) {result=fresh;return true;}
  // Held drawings are exact, including their placement and alpha. Do not apply
  // the recurrence repeatedly to the same drawing, which would cause drift.
  if(previous==current && previousAlpha==currentAlpha) {result=previousStyled;return true;}
  const cv::Mat a=gray(previous),b=gray(current);
  cv::Point translation;
  if(exactTranslation(previous,current,previousAlpha,currentAlpha,a,b,translation,cancel)) {
    QImage complete=fresh.convertToFormat(QImage::Format_RGB32);
    for(int y=0;y<384;++y) {
      if(cancel && *cancel)return false;
      auto row=reinterpret_cast<QRgb *>(complete.scanLine(y));
      for(int x=0;x<384;++x) {
        const int px=x-translation.x,py=y-translation.y;
        if(px<0 || py<0 || px>=384 || py>=384)continue;
        const double weight=amount*qGray(currentAlpha.pixel(x,y))/255.0;
        if(weight==0)continue;
        const auto old=previousStyled.pixel(px,py),now=fresh.pixel(x,y);
        const auto blend=[weight](int oldValue,int value){return int(oldValue*weight+value*(1-weight)+.5);};
        row[x]=qRgb(blend(qRed(old),qRed(now)),blend(qGreen(old),qGreen(now)),blend(qBlue(old),qBlue(now)));
      }
    }
    result=complete;return true;
  }
  if(cancel && *cancel)return false;
  // A pan can produce a large pixelwise difference without being a cut. Use
  // displacement-invariant tonal distributions for the early guard; motion
  // consistency/residual/coverage checks below reject unrelated spatial content.
  std::array<int,32> histogramA{},histogramB{};
  for(int y=0;y<384;++y)for(int x=0;x<384;++x) {
    ++histogramA[a.at<uchar>(y,x)/8];++histogramB[b.at<uchar>(y,x)/8];
  }
  double histogramDistance=0;
  for(int i=0;i<32;++i)histogramDistance+=std::abs(histogramA[i]-histogramB[i]);
  if(histogramDistance/(2*384*384)>.8) {result=fresh;return true;}
  cv::Mat forward,backward;
  cv::calcOpticalFlowFarneback(a,b,forward,.5,3,21,3,5,1.2,0);
  if(cancel && *cancel) return false;
  cv::calcOpticalFlowFarneback(b,a,backward,.5,3,21,3,5,1.2,0);
  if(cancel && *cancel) return false;
  QImage complete=fresh.convertToFormat(QImage::Format_RGB32);
  int visible=0,accepted=0;
  for(int y=0;y<384;++y) {
    if(cancel && *cancel) return false;
    auto output=reinterpret_cast<QRgb *>(complete.scanLine(y));
    for(int x=0;x<384;++x) {
      const double coverage=qGray(currentAlpha.pixel(x,y))/255.0;
      if(coverage<=0) continue;
      ++visible;
      const auto motion=backward.at<cv::Vec2f>(y,x);
      const double px=x+motion[0],py=y+motion[1];
      if(!std::isfinite(px) || !std::isfinite(py) || px<0 || py<0 || px>383 || py>383) continue;
      const auto inverse=flow(forward,px,py);
      const double disagreement=cv::norm(motion+inverse);
      const double limit=1+.05*cv::norm(motion);
      if(disagreement>=limit) continue;
      const double alpha=rgb(previousAlpha,px,py)[0]/255.0;
      if(std::abs(alpha-coverage)>.08) continue;
      const auto before=rgb(previous,px,py),now=rgb(current,x,y);
      const double residual=(std::abs(before[0]-now[0])+std::abs(before[1]-now[1])+std::abs(before[2]-now[2]))/(3*255.0);
      if(residual>=.12) continue;
      const double confidence=(1-disagreement/limit)*(1-residual/.12)*std::min(alpha,coverage);
      const double weight=amount*confidence;
      const auto history=rgb(previousStyled,px,py),freshPixel=rgb(fresh,x,y);
      const auto blended=history*float(weight)+freshPixel*float(1-weight);
      output[x]=qRgb(int(std::clamp(blended[0],0.f,255.f)+.5f),
                     int(std::clamp(blended[1],0.f,255.f)+.5f),
                     int(std::clamp(blended[2],0.f,255.f)+.5f));
      ++accepted;
    }
  }
  // Too little reliable overlap is another cut/discontinuity signal.
  result=(visible && accepted*10<visible)?fresh:complete;
  return true;
}
}
