#ifndef QUERYMANAGER_H
#define QUERYMANAGER_H

#include "souffle/problog/Query.h"
#include <cstddef>
#include <unordered_map>
#include <vector>

class QueryManager {
public:
    QueryManager(std::vector<Query> queries);
    void addQuery(Query query);
    std::vector<const Query*> getAllQuery() const;
private:
    std::unordered_map<std::size_t, Query> queries;
    std::size_t nextQueryId = 0;
};

#endif // QUERYMANAGER_H
