#include "souffle/problog/QueryManager.h"

QueryManager::QueryManager(std::vector<Query> queries) {
    for (auto& q : queries) {
        addQuery(std::move(q));
    }
}

void QueryManager::addQuery(Query query) {
    std::size_t id = nextQueryId++;
    queries.emplace(id, std::move(query));
}

std::vector<const Query*> QueryManager::getAllQuery() const {
    std::vector<const Query*> result;
    result.reserve(queries.size());
    for (const auto& [_, query] : queries) {
        result.push_back(&query);
    }
    return result;
}
