#include "../../stdfx/tests/neuraltemporal_cases.h"
#include <QCoreApplication>
#include <iostream>
int main(int argc,char **argv) {
  QCoreApplication app(argc,argv);
  try {NeuralTemporalCases::run();std::cout<<"Temporal motion/cut/occlusion checks passed\n";return 0;}
  catch(const std::exception &error){std::cerr<<"::error title=Temporal qualification::"<<error.what()<<'\n';return 1;}
}
