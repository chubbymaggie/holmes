#include "pgDal.h"

#include <assert.h>

#include <capnp/message.h>
#include <iostream>

#include "fact_util.h"

namespace holmes {

void PgDAL::initDB() {
  pqxx::work work(conn);
  work.exec("create schema if not exists facts");
  auto res = work.exec("select table_name, udt_name from information_schema.columns where table_schema = 'facts' ORDER BY table_name, ordinal_position");
  work.commit();
  for (auto line : res) {
    std::string name = line[0].c_str();
    if (types.find(name) == types.end()) {
      std::vector<Holmes::HType> sig;
      types[name] = sig;
    }
    Holmes::HType typ;
    std::string type_string = line[1].c_str();
    if (type_string == "int8") {
      typ = Holmes::HType::ADDR;
    } else if (type_string == "varchar") {
      typ = Holmes::HType::STRING;
    } else if (type_string == "bytea") {
      typ = Holmes::HType::BLOB;
    } else {
      std::cerr << "Type parse failure: " << type_string << std::endl;
      exit(1);
    }
    types[name].push_back(typ);
  }
  for (auto type : types) {
    registerPrepared(type.first, type.second.size());
  }
}

void PgDAL::registerPrepared(std::string name, size_t n) {
  std::string argVals = "(";
  for (size_t i = 1; i <= n; i++) {
    argVals += "$" + std::to_string(i);
    if (i != n) {
      argVals += ", ";
    } else {
      argVals += ")";
    }
  }
  conn.prepare(name + ".insert", "INSERT INTO facts." + name + " VALUES " + argVals);
}

bool PgDAL::setFact(Holmes::Fact::Reader fact) {
  std::lock_guard<std::mutex> lock(mutex);
  assert(typecheck(types, fact));
  pqxx::work work(conn);
  std::string name = fact.getFactName();
  auto query = work.prepared(name + ".insert");
  for (auto arg : fact.getArgs()) {
    switch (arg.which()) {
      case Holmes::Val::STRING_VAL:
        query(std::string(arg.getStringVal()));
        break;
      case Holmes::Val::ADDR_VAL:
      //PgSQL is bad, and only has a signed int type
        query((int64_t)arg.getAddrVal());
        break;
      case Holmes::Val::BLOB_VAL:
        capnp::Data::Reader data = arg.getBlobVal();
        pqxx::binarystring blob(data.begin(), data.size());
        query(blob);
        break;
    }
  }
  auto res = query.exec();
  work.commit();
  return (res.affected_rows() != 0);
}

size_t PgDAL::setFacts(capnp::List<Holmes::Fact>::Reader facts) {
  std::lock_guard<std::mutex> lock(mutex);
  pqxx::work work(conn);
  std::vector<pqxx::result> res;
  for (auto fact : facts) {
    assert(typecheck(types, fact));
    std::string name = fact.getFactName();
    auto query = work.prepared(name + ".insert");
    for (auto arg : fact.getArgs()) {
      switch (arg.which()) {
        case Holmes::Val::STRING_VAL:
          query(std::string(arg.getStringVal()));
          break;
        case Holmes::Val::ADDR_VAL:
        //PgSQL is bad, and only has a signed int type
          query((int64_t)arg.getAddrVal());
          break;
        case Holmes::Val::BLOB_VAL:
          capnp::Data::Reader data = arg.getBlobVal();
          pqxx::binarystring blob(data.begin(), data.size());
          query(blob);
          break;
      }
    }
    res.push_back(query.exec());
  }
  work.commit();
  size_t affected = 0;
  for (auto r : res) {
    affected += r.affected_rows();
  }
  return affected;
}


std::string htype_to_sqltype(Holmes::HType hType) {
  switch (hType) {
    case Holmes::HType::STRING:
      return "varchar";
    case Holmes::HType::ADDR:
      return "bigint";
    case Holmes::HType::BLOB:
      return "bytea";
  }
  return "unknown";
}

bool valid_name(std::string s) {
  for (auto c : s) {
    if (c == '-') {
      continue;
    }
    if ((c >= 'a') && (c <= 'z')) {
      continue;
    }
    if ((c >= '0') && (c <= '9')) {
      continue;
    }
    return false;
  }
  return true;
}

bool PgDAL::addType(std::string name, capnp::List<Holmes::HType>::Reader argTypes) {
  std::lock_guard<std::mutex> lock(mutex);
  //We're using this for a table name, so we have restrictions
  if (!valid_name(name)) {
    return false;
  }
  auto itt = types.find(name);
  if (itt != types.end()) {
    if (argTypes.size() != itt->second.size()) {
      return false;
    }
    for (size_t i = 0; i < argTypes.size(); i++) {
      if (argTypes[i] != itt->second[i]) {
        return false;
      }
    }
    return true;
  } else {
    std::vector<Holmes::HType> sig;
    std::string tableSpec = "(";
    for (size_t i = 0; i < argTypes.size(); i++) {
      tableSpec += "arg" + std::to_string(i) + " " + htype_to_sqltype(argTypes[i]);
      sig.push_back(argTypes[i]);
      if (i == argTypes.size() - 1) {
        tableSpec += ")";
      } else {
        tableSpec += ", ";
      }
    }
    pqxx::work work(conn);
    
    work.exec("CREATE TABLE facts." + name + " " + tableSpec);
    work.commit();
    types[name] = sig;
    registerPrepared(name, sig.size());
    return true;
  }
}

std::string quoteVal(pqxx::work& w, Holmes::Val::Reader v) {
  switch (v.which()) {
    case Holmes::Val::STRING_VAL:
      return w.quote(std::string(v.getStringVal()));
    case Holmes::Val::BLOB_VAL:
      //You probably don't want to do this... but for completeness sake
      return w.quote_raw(v.getBlobVal().begin(), v.getBlobVal().size());
    case Holmes::Val::ADDR_VAL:
      //Postgres doesn't support uint64_t
      return w.quote((int64_t)v.getAddrVal());
  }
  throw "Failed to quote value";
}

DAL::FactResults PgDAL::getFacts(
  Holmes::FactTemplate::Reader query,
  Context ctx) {
  std::lock_guard<std::mutex> lock(mutex);
  pqxx::work work(conn);
  DAL::FactResults results;
  auto args = query.getArgs();
  std::vector<std::string> whereClause;
  for (size_t i = 0; i < args.size(); i++) {
    Holmes::Val::Reader val;
    switch (args[i].which()) {
      case Holmes::TemplateVal::EXACT_VAL:
        whereClause.push_back("arg" + std::to_string(i) + "=" + quoteVal(work, args[i].getExactVal()));
        break;
      case Holmes::TemplateVal::BOUND:
        {
          auto itv = ctx.find(std::string(args[i].getBound()));
          if (itv != ctx.end()) {
            whereClause.push_back("arg" + std::to_string(i) + "=" + quoteVal(work, itv->second));
          }
          break;
        }
      case Holmes::TemplateVal::UNBOUND:
        break;
    }
  }
  std::string whereStr;
  if (whereClause.size() != 0) {
    whereStr += " WHERE ";
  }
  for (size_t i = 0; i < whereClause.size(); i++) {
    whereStr += whereClause[i];
    if (i != whereClause.size() - 1) {
      whereStr += " AND ";
    }
  }
  auto q = "SELECT * FROM facts." + std::string(query.getFactName()) + whereStr;
  auto facts = work.exec(q);
  work.commit();
  std::map<Context, std::vector<Holmes::Fact::Reader>, ContextCompare> fam;
  std::vector<Holmes::Fact::Reader> hFacts;
  auto type = types[query.getFactName()];
  for (auto f : facts) {
    capnp::MallocMessageBuilder *mb = new capnp::MallocMessageBuilder;
    auto builder = mb->initRoot<Holmes::Fact>();
    builder.setFactName(query.getFactName());
    auto flb = builder.initArgs(f.size());
    for (size_t i = 0; i < f.size(); i++) {
      pqxx::field arg = f[(int)i];
      switch (type[i]) {
        case Holmes::HType::ADDR:
          //Postgres has no uint64_t storage
          flb[i].setAddrVal(static_cast<uint64_t>(arg.as<int64_t>()));
          break;
        case Holmes::HType::STRING:
          flb[i].setStringVal(arg.as<std::string>());
          break;
        case Holmes::HType::BLOB:
          {
            pqxx::binarystring bs(arg);
            auto bb = flb[i].initBlobVal(bs.size());
            for (size_t i = 0; i < bs.size(); i++) {
              bb[i] = bs[i];
            }
          }
          break;
      }
    }
    hFacts.push_back(builder.asReader());
    results.mbs.push_back(mb);
  }
  for (auto f : hFacts) {
    Context newCtx = ctx;
    auto fa = f.getArgs();
    auto itf = fa.begin();
    auto itq = args.begin();
    bool matched = true;
    for (; matched && (itf != fa.end()) && (itq != args.end()); ++itf, ++itq) {
      switch (itq->which()) {
        case Holmes::TemplateVal::EXACT_VAL:
        case Holmes::TemplateVal::UNBOUND:
          break;
        case Holmes::TemplateVal::BOUND:
          {
            auto var = itq->getBound();
            if (ctx.find(var) != ctx.end()) {
               //We already did this in the select
               continue;
            }
            auto itv = newCtx.find(var);
            if (itv != newCtx.end()) {
              //We bound this during this fact, it needs checking
              ValCompare compare;
              matched &= ~ (compare(itv->second, *itf)
                         || compare(*itf, itv->second));
            } else {
              newCtx[var] = *itf;
            }
         }
      }
    }
    if (matched) {
      auto itc = fam.find(newCtx);
      if (itc != fam.end()) {
        itc->second.push_back(f);
      } else {
        fam[newCtx] = {f};
      }
      //XXX: Merge in facts
    }
  }
  for (auto fa : fam) {
    results.results.push_back(FactAssignment(fa.first, fa.second));
  }
  return std::move(results);
}

}
