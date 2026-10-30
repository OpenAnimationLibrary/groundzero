#include "toonz/rasterbrushpreset.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QFile>
#include <iostream>
#include <stdexcept>
using namespace RasterBrush;
namespace {
void require(bool value,const char *message){if(!value)throw std::runtime_error(message);}
QByteArray altered(const QByteArray &bytes,const QString &key,const QJsonValue &value){
  auto root=QJsonDocument::fromJson(bytes).object();root[key]=value;return QJsonDocument(root).toJson();
}
void testPackage(){
  Record r;r.preset.name="Original brush";r.provenance={{"license","CC0-1.0"},{"origin","original test fixture"}};
  auto image=std::make_shared<Image>();image->width=2;image->height=2;image->color=true;
  image->pixels={{1,0,0,1},{0,1,0,1},{0,0,1,1},{0,0,0,0}};
  r.preset.tip.shape=Shape::Bitmap;r.preset.tip.images.push_back(image);
  r.preset.texture.image=image;r.preset.texture.depth=.5;
  r.preset.size.mappings.push_back({Sensor::Rotation,{{0,.2},{1,1}},1});
  r.preset.seed=UINT64_C(18446744073709551615);
  QString error;auto bytes=encode(r,error);require(!bytes.isEmpty(),qPrintable(error));
  auto restored=decode(bytes,error);require(error.isEmpty()&&restored.semanticallySupported,qPrintable(error));
  require(!restored.referenceVerified,"unearned reference verification");
  require(restored.preset.seed==r.preset.seed,"seed precision lost");
  require(restored.preset.tip.images[0]->pixels[0].r==1,"image lost");
  require(restored.preset.tip.images[0]==restored.preset.texture.image,"resource not deduplicated");
  require(encode(restored,error)==bytes,"package roundtrip changed");
  decode(altered(bytes,"version",2),error);require(!error.isEmpty(),"future version accepted");
  auto root=QJsonDocument::fromJson(bytes).object();auto settings=root["settings"].toObject();
  settings["unrecognizedDynamics"]=true;root["settings"]=settings;
  decode(QJsonDocument(root).toJson(),error);require(!error.isEmpty(),"unknown behavior ignored");
  root=QJsonDocument::fromJson(bytes).object();root["assets"]=QJsonObject();
  decode(QJsonDocument(root).toJson(),error);require(!error.isEmpty(),"missing resource accepted");
  root=QJsonDocument::fromJson(bytes).object();auto assets=root["assets"].toObject();auto a=assets.begin().value().toObject();a["color"]=false;assets[assets.begin().key()]=a;root["assets"]=assets;
  decode(QJsonDocument(root).toJson(),error);require(!error.isEmpty(),"hash mismatch accepted");
  QTemporaryDir dir;require(dir.isValid(),"temporary directory unavailable");
  auto path=install(dir.path(),r,error);require(!path.isEmpty(),qPrintable(error));
  require(install(dir.path(),r,error)==path,"duplicate install changed identity");
  auto saved=load(path,error);require(saved.semanticallySupported,qPrintable(error));
  QFile f(path);require(f.open(QIODevice::WriteOnly),"cannot damage fixture");f.write("damaged");f.close();
  require(install(dir.path(),r,error).isEmpty(),"conflicting install overwritten");
  r.preset.tip.shape=Shape(99);require(encode(r,error).isEmpty(),"invalid enum serialized");
  r.preset.tip.shape=Shape::Bitmap;r.preset.color={1,0,0,.5};require(encode(r,error).isEmpty(),"invalid premultiplication accepted");
}
}
int main(int argc,char **argv){QCoreApplication app(argc,argv);try{testPackage();std::cout<<"Native brush package tests passed\n";return 0;}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
