// Copyright 2022-present MongoDB Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cast_core/actors/AssertAggregationResult.hpp>

#include <memory>

#include <yaml-cpp/yaml.h>

#include <bsoncxx/builder/stream/document.hpp>
#include <bsoncxx/json.hpp>

#include <mongocxx/client.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/database.hpp>

#include <boost/log/trivial.hpp>
#include <boost/throw_exception.hpp>

#include <gennylib/Cast.hpp>
#include <gennylib/MongoException.hpp>
#include <gennylib/context.hpp>

#include <value_generators/DocumentGenerator.hpp>

namespace genny::actor {


struct AssertAggregationResult::PhaseConfig {
    mongocxx::database database;
    DocumentGenerator expectedExpr;
    DocumentGenerator actualExpr;
    std::string compareCollection;

    PhaseConfig(PhaseContext& phaseContext, mongocxx::database&& db, ActorId id)
        : database{db},
          expectedExpr{phaseContext["Expected"].to<DocumentGenerator>(phaseContext, id)},
          actualExpr{phaseContext["Actual"].to<DocumentGenerator>(phaseContext, id)} {}
};

bool resultMatchesExpected(bsoncxx::array::view &firstBatch) {
    return firstBatch.empty();
}

auto runCommandAndGetBatch(AssertAggregationResult::PhaseConfig* config, bsoncxx::document::value command) {
    auto cmdView = command.view();

    BOOST_LOG_TRIVIAL(info) << " running command " << bsoncxx::to_json(cmdView);
    auto res = config->database.run_command(std::move(command));
    auto resView = res.view();

    BOOST_LOG_TRIVIAL(info) << " command result: " << bsoncxx::to_json(resView);

    if (resView["ok"]) {
        BOOST_LOG_TRIVIAL(info) << " command result ok.";
        auto cursor = resView["cursor"].get_document().view();
        return bsoncxx::array::view {cursor["firstBatch"].get_array().value};
    }

    BOOST_THROW_EXCEPTION(MongoException("Failed to run command."));
}

bool equalBSONDocs(bsoncxx::document::view expected, bsoncxx::document::view actual) {
    if (expected != actual) {
        for (auto actualVal : actual) {
            auto key = actualVal.key();
            auto expectedVal = expected[key];
            auto type = actualVal.type();
            // We don't handle nested documents/arrays since they don't appear in the TPC-H benchmark results.
            if (type != expectedVal.type() || actualVal.get_value() != expected[key].get_value()) {
                BOOST_LOG_TRIVIAL(info) << "Documents differ in field: " << key;
                return false;;
            }
        }
    }

    return true;
}

bool equalBSONArrays(bsoncxx::array::view expected, bsoncxx::array::view actual) {
    if (expected == actual) {
        return true;
    }

    if (expected.length() != actual.length()) {
        return false;
    }

    auto expectedIt = expected.begin();
    for (auto actualVal : actual) {
        auto expectedVal = *expectedIt;
        auto type = actualVal.type();
        if (type != expectedVal.type()) {
            return false;
        } else if (type == bsoncxx::type::k_array) {
            bsoncxx::array::view expectedArr{expectedVal.get_array().value};
            bsoncxx::array::view actualArr{actualVal.get_array().value};
            if (!equalBSONArrays(std::move(expectedArr), std::move(actualArr))) {
                return false;
            }
        } else if (type == bsoncxx::type::k_document) {
            bsoncxx::document::view expectedDoc{expectedVal.get_document().value};
            bsoncxx::document::view actualDoc{expectedVal.get_document().value};
            if (!equalBSONDocs(std::move(expectedDoc), std::move(actualDoc))) {
                return false;
            }
        } else if (actualVal.get_value() != expectedVal.get_value()) {
            return false;
        }
        ++expectedIt;
    }

    return true;
}

void AssertAggregationResult::run() {
    for (auto&& config : _loop) {
        for (const auto&& _ : config) {
            auto expected = config->expectedExpr();
            auto actual = config->actualExpr();

            auto assertOp = _assert.start();

            try {
                auto expectedRes = runCommandAndGetBatch(config, std::move(expected));
                auto actualRes = runCommandAndGetBatch(config, std::move(actual));
                if (equalBSONArrays(std::move(expectedRes), std::move(actualRes))) {
                    BOOST_LOG_TRIVIAL(info) << " assert passed; results are equal.";
                    assertOp.success();
                } else {
                    BOOST_THROW_EXCEPTION(MongoException("assert failed; results were unequal."));
                }
            } catch (mongocxx::operation_exception& e) {
                BOOST_LOG_TRIVIAL(info)
                            << "Caught error: " << boost::diagnostic_information(e);
                assertOp.failure();
                throw;
            } catch (boost::exception& e) {
                BOOST_LOG_TRIVIAL(info)
                            << "Caught error: " << boost::diagnostic_information(e);
                assertOp.failure();
                throw;
            }
        }
    }
}

AssertAggregationResult::AssertAggregationResult(genny::ActorContext& context)
    : Actor{context},
      _assert{context.operation("Assert", AssertAggregationResult::id())},
      _client{context.client()},
      _loop{context, (*_client)[context["Database"].to<std::string>()], AssertAggregationResult::id()} {}

namespace {
auto registerAssertAggregationResult = Cast::registerDefault<AssertAggregationResult>();
}  // namespace
}  // namespace genny::actor
