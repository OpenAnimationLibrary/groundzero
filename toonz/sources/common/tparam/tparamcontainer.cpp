

#include "tparamcontainer.h"
//#include "tdoubleparam.h"
#include "tparamset.h"

#include "tparam.h"
#include "texception.h"

void TParamVar::setParamObserver(TParamObserver *obs) {
  if (m_paramObserver == obs) return;
  TParam *param = getParam();
  if (param) {
    if (obs) param->addObserver(obs);
    if (m_paramObserver) param->removeObserver(m_paramObserver);
  }
  m_paramObserver = obs;
}

class TParamContainer::Imp {
public:
  std::map<std::string, TParamVar *> m_nameTable;
  std::vector<TParamVar *> m_vars;
  TParamObserver *m_paramObserver;
  Imp() : m_paramObserver(0) {}
  ~Imp() { clearPointerContainer(m_vars); }
};

TParamContainer::TParamContainer() : m_imp(new Imp()) {}

TParamContainer::~TParamContainer() {}

void TParamContainer::setParamObserver(TParamObserver *observer) {
  m_imp->m_paramObserver = observer;
}

TParamObserver *TParamContainer::getParamObserver() const {
  return m_imp->m_paramObserver;
}

void TParamContainer::add(TParamVar *var) {
  m_imp->m_vars.push_back(var);
  m_imp->m_nameTable[var->getName()] = var;
  var->setParamObserver(m_imp->m_paramObserver);
  var->getParam()->setName(var->getName());
}

void TParamContainer::reorder(const std::vector<std::string> &names) {
  if (names.size() != m_imp->m_vars.size())
    throw TException("Parameter order must contain every existing parameter");
  std::vector<TParamVar *> ordered;
  std::set<std::string> seen;
  ordered.reserve(names.size());
  for (const auto &name : names) {
    const auto entry = m_imp->m_nameTable.find(name);
    if (entry == m_imp->m_nameTable.end() || !seen.insert(name).second)
      throw TException("Parameter order is not an exact permutation");
    ordered.push_back(entry->second);
  }
  m_imp->m_vars.swap(ordered);
}

int TParamContainer::getParamCount() const { return m_imp->m_vars.size(); }

TParam *TParamContainer::getParam(int index) const {
  assert(0 <= index && index < getParamCount());
  return m_imp->m_vars[index]->getParam();
}

bool TParamContainer::isParamHidden(int index) const {
  assert(0 <= index && index < getParamCount());
  return m_imp->m_vars[index]->isHidden();
}

std::string TParamContainer::getParamName(int index) const {
  assert(0 <= index && index < getParamCount());
  return m_imp->m_vars[index]->getName();
}

const TParamVar *TParamContainer::getParamVar(int index) const {
  assert(0 <= index && index < getParamCount());
  return m_imp->m_vars[index];
}

TParam *TParamContainer::getParam(std::string name) const {
  TParamVar *var = getParamVar(name);
  return (var) ? var->getParam() : 0;
}

TParamVar *TParamContainer::getParamVar(std::string name) const {
  std::map<std::string, TParamVar *>::const_iterator it;
  it = m_imp->m_nameTable.find(name);
  if (it == m_imp->m_nameTable.end())
    return 0;
  else
    return it->second;
}

void TParamContainer::unlink() {
  for (int i = 0; i < getParamCount(); i++) {
    // TRangeParam *p0;//,*p1;
    TParamVar *var = m_imp->m_vars[i];
    TParam *param  = var->getParam();
    // p0 = dynamic_cast<TRangeParam *>(param);
    TParamP replacement = param->clone();
    var->setParamObserver(nullptr);
    var->setParam(replacement.getPointer());
    var->setParamObserver(m_imp->m_paramObserver);
    /*p1 = dynamic_cast<TRangeParam *>(var->getParam());
if(p0 && p1)
{
string name = p0->getName();
name = p1->getName();
}*/
  }
}

void TParamContainer::link(const TParamContainer *src) {
  assert(getParamCount() == src->getParamCount());
  for (int i = 0; i < getParamCount(); i++) {
    assert(getParamName(i) == src->getParamName(i));
    assert(m_imp->m_vars[i]->getName() == getParamName(i));
    m_imp->m_vars[i]->setParamObserver(nullptr);
    m_imp->m_vars[i]->setParam(src->getParam(i));
    m_imp->m_vars[i]->setParamObserver(m_imp->m_paramObserver);
  }
}

void TParamContainer::copy(const TParamContainer *src) {
  assert(getParamCount() == src->getParamCount());
  for (int i = 0; i < getParamCount(); i++) {
    assert(getParamName(i) == src->getParamName(i));
    assert(m_imp->m_vars[i]->getName() == getParamName(i));
    getParam(i)->copy(src->getParam(i));
  }
}
