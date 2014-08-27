#ifndef HOLMES_SERVER_MEMDAL_H_
#define HOLMES_SERVER_MEMDAL_H_

#include "dal.h"

#include <vector>
#include <set>
#include <atomic>
#include <mutex>

#include <kj/common.h>
#include <capnp/message.h>

#include "holmes.capnp.h"
#include "fact_util.h"

namespace holmes {

class MemDAL : public DAL {
  public:
    MemDAL(){}
    ~MemDAL() {
      for (auto b : mm) {
        delete b;
      }
    }
    bool setFact(Holmes::Fact::Reader);
    DAL::FactResults getFacts(
      Holmes::FactTemplate::Reader,
      Context ctx = Context());
    bool addType(std::string name,
                 capnp::List<Holmes::HType>::Reader argTypes);
  private:
    std::mutex mutex;
    std::set<Holmes::Fact::Reader, FactCompare> facts;
    std::vector<capnp::MessageBuilder*> mm;
    std::map<std::string, std::vector<Holmes::HType>> types;
    KJ_DISALLOW_COPY(MemDAL);
};

}

#endif
