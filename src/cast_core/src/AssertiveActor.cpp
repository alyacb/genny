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

#include <cast_core/actors/AssertiveActor.hpp>

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


struct AssertiveActor::PhaseConfig {
    mongocxx::database database;
    DocumentGenerator expectedExpr;
    DocumentGenerator actualExpr;
    std::optional<std::string> msg;

    PhaseConfig(PhaseContext& phaseContext, mongocxx::database&& db, ActorId id)
        : database{db},
          expectedExpr{phaseContext["Expected"].to<DocumentGenerator>(phaseContext, id)},
          actualExpr{phaseContext["Actual"].to<DocumentGenerator>(phaseContext, id)},
          msg{phaseContext["Message"].maybe<std::string>()} {}
};

auto runCommandAndGetResult(AssertiveActor::PhaseConfig* config, bsoncxx::document::value& command) {
    auto cmdView = command.view();

    BOOST_LOG_TRIVIAL(info) << "Running command " << bsoncxx::to_json(cmdView);
    auto res = config->database.run_command(std::move(command));
    auto resView = res.view();

    return res;
}

auto getBatchFromCommandResult(const bsoncxx::document::view& resView) {
    if (resView["ok"]) {
        auto cursor = resView["cursor"].get_document().view();
        auto firstBatch = cursor["firstBatch"].get_array().value;
        return firstBatch;
    }
    BOOST_THROW_EXCEPTION(MongoException("Failed to run command."));
}

double convertNumericToDouble(auto& numeric) {
    switch(numeric.type()) {
        case bsoncxx::type::k_double:
            return numeric.get_double();
        case bsoncxx::type::k_int32:
            return numeric.get_int32() * 1.0;
        case bsoncxx::type::k_int64:
            return numeric.get_int64() * 1.0;
        default:
            BOOST_THROW_EXCEPTION(MongoException("not a numeric"));
    }
}

bool isNumeric(bsoncxx::type type) {
    return (type == bsoncxx::type::k_double)
        || (type == bsoncxx::type::k_int32)
        || (type == bsoncxx::type::k_int64);
}

bool equalBSONDocs(const bsoncxx::document::view& expected, const bsoncxx::document::view& actual);
bool equalBSONArrays(const bsoncxx::array::view& expected, const bsoncxx::array::view& actual);
bool equal(auto& expectedVal, auto& actualVal) {
    auto type = expectedVal.type();
    if (isNumeric(type) && isNumeric(actualVal.type())) {
        // Rounding may result in one value being a double while another is integral. Handle this case through double comparison.
        auto expectedDouble = convertNumericToDouble(expectedVal);
        auto actualDouble = convertNumericToDouble(actualVal);
        // Accept a small difference in doubles.
        return std::abs(expectedDouble - actualDouble) < 0.001;
    } else if (type != actualVal.type()) {
        BOOST_LOG_TRIVIAL(info) << "Types differ! ";
        return false;
    } else if (type == bsoncxx::type::k_array) {
        bsoncxx::array::view expectedArr{expectedVal.get_array().value};
        bsoncxx::array::view actualArr{actualVal.get_array().value};
        return equalBSONArrays(expectedArr, actualArr);
    } else if (type == bsoncxx::type::k_document) {
        bsoncxx::document::view expectedDoc{expectedVal.get_document().value};
        bsoncxx::document::view actualDoc{actualVal.get_document().value};
        return equalBSONDocs(expectedDoc, actualDoc);
    }
    return actualVal.get_value() == expectedVal.get_value();
}

bool equalBSONDocs(const bsoncxx::document::view& expected, const bsoncxx::document::view& actual) {
    for (auto actualVal : actual) {
        auto key = actualVal.key();
        if (key == "_id") {
            continue;
        }
        if (key == "num") {
            continue;
        }
        auto expectedVal = expected[key];
        if (!equal(expectedVal, actualVal)) {
            BOOST_LOG_TRIVIAL(info) << "Documents differ in field: " << key;
            return false;
        }
    }
    for (auto expectedVal: expected) {
        auto key = expectedVal.key();
        if (key == "_id") {
            continue;
        }
        if (key == "num") {
            continue;
        }
        if (!actual[key]) {
            BOOST_LOG_TRIVIAL(info) << "Documents differ in field: " << key;
            return false;
        }
    }
    return true;
}

bool equalBSONArrays(const bsoncxx::array::view& expected, const bsoncxx::array::view& actual) {
    auto expectedArrLen = std::distance(expected.begin(), expected.end());
    auto actualArrLen = std::distance(actual.begin(), actual.end());
    if (expectedArrLen != actualArrLen) {
        BOOST_LOG_TRIVIAL(info) << "Arrays differ in length. Expected " << expectedArrLen << " but found " << actualArrLen;
        return false;
    }

    auto expectedIt = expected.begin();
    int count = 0;
    for (auto actualVal : actual) {
        auto expectedVal = *expectedIt;
        if (!equal(expectedVal, actualVal)) {
            BOOST_LOG_TRIVIAL(info) << "Arrays differ in idx " << count;
            return false;
        }
        ++expectedIt;
        ++count;
    }

    return true;
}

void AssertiveActor::run() {
    for (auto&& config : _loop) {
        for (const auto&& _ : config) {
            auto expected = config->expectedExpr();
            auto actual = config->actualExpr();

            auto assertOp = _assert.start();

            try {
                auto expectedRes = runCommandAndGetResult(config, expected);
                auto actualRes = runCommandAndGetResult(config, actual);
                auto msg = (config->msg ? "\"" + *(config->msg) + "\" ": "");
                if (equalBSONArrays(getBatchFromCommandResult(expectedRes.view()), getBatchFromCommandResult(actualRes.view()))) {
                    BOOST_LOG_TRIVIAL(info) << "Assert " << msg << "passed; results are equal.";
                    assertOp.success();
                } else {
                    auto errStr = "assert " + msg + "failed; results were unequal.";
                    BOOST_LOG_TRIVIAL(info) << "Assert " << msg << "failed; results are unequal.";
                    BOOST_THROW_EXCEPTION(MongoException(errStr));
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

AssertiveActor::AssertiveActor(genny::ActorContext& context)
    : Actor{context},
      _assert{context.operation("Assert", AssertiveActor::id())},
      _client{context.client()},
      _loop{context, (*_client)[context["Database"].to<std::string>()], AssertiveActor::id()} {}

namespace {
auto registerAssertiveActor = Cast::registerDefault<AssertiveActor>();
}  // namespace
}  // namespace genny::actor
