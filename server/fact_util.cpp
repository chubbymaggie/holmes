#include "fact_util.h"
#include <glog/logging.h>

namespace holmes {

bool typecheck(const std::map<std::string, std::vector<Holmes::HType>> &types,
               Holmes::Fact::Reader fact) {
  auto itt = types.find(fact.getFactName());
  if (itt == types.end()) {
    LOG(ERROR) << "Fact not found: " << std::string(fact.getFactName());
    return false;
  }
  auto fa = fact.getArgs();
  auto ts = itt->second;
  if (fa.size() != ts.size()) {
    LOG(ERROR) << "Arity mismatch for fact " << std::string(fact.getFactName()) << ", expected " << ts.size() << " got " << fa.size();
    return false;
  }
  for (size_t i = 0; i < fa.size(); i++) {
    switch (fa[i].which()) {
      case Holmes::Val::JSON_VAL:
        if (ts[i] != Holmes::HType::JSON) {
          LOG(ERROR) << "Non-json value at position " << i << " in fact " << std::string(fact.getFactName());
          return false;
        }
        break;
      case Holmes::Val::STRING_VAL:
        if (ts[i] != Holmes::HType::STRING) {
          LOG(ERROR) << "Non-string value at position " << i << " in fact " << std::string(fact.getFactName());
          return false;
        }
        break;
      case Holmes::Val::ADDR_VAL:
        if (ts[i] != Holmes::HType::ADDR) {
          LOG(ERROR) << "Non-addr value at position " << i << " in fact " << std::string(fact.getFactName());
          return false;
        }
        break;
      case Holmes::Val::BLOB_VAL:
        if (ts[i] != Holmes::HType::BLOB) {
          LOG(ERROR) << "Non-blob value at position " << i << " in fact " << std::string(fact.getFactName());
          return false;
        }
        break;
    }
  }
  return true;
}

}