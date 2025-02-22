/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/pipeline/parsed_exclusion_projection.h"

#include <iostream>
#include <iterator>
#include <string>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/pipeline/dependencies.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace parsed_aggregation_projection {
namespace {

using std::vector;

// Helper to simplify the creation of a ParsedExclusionProjection with default policies.
ParsedExclusionProjection makeExclusionProjectionWithDefaultPolicies() {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ProjectionPolicies defaultPolicies;
    return {expCtx, defaultPolicies};
}

// Helper to simplify the creation of a ParsedExclusionProjection which excludes _id by default.
ParsedExclusionProjection makeExclusionProjectionWithDefaultIdExclusion() {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ProjectionPolicies defaultExcludeId{ProjectionPolicies::DefaultIdPolicy::kExcludeId,
                                        ProjectionPolicies::kArrayRecursionPolicyDefault,
                                        ProjectionPolicies::kComputedFieldsPolicyDefault};
    return {expCtx, defaultExcludeId};
}

// Helper to simplify the creation of a ParsedExclusionProjection which does not recurse arrays.
ParsedExclusionProjection makeExclusionProjectionWithNoArrayRecursion() {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ProjectionPolicies noArrayRecursion{
        ProjectionPolicies::kDefaultIdPolicyDefault,
        ProjectionPolicies::ArrayRecursionPolicy::kDoNotRecurseNestedArrays,
        ProjectionPolicies::kComputedFieldsPolicyDefault};

    return {expCtx, noArrayRecursion};
}

//
// Errors.
//

DEATH_TEST(ExclusionProjectionExecutionTest,
           ShouldFailWhenGivenIncludedNonIdField,
           "Invariant failure !elem.trueValue() || elem.fieldNameStringData() == \"_id\"_sd") {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies();
    exclusion.parse(BSON("a" << true));
}

DEATH_TEST(ExclusionProjectionExecutionTest,
           ShouldFailWhenGivenIncludedIdSubfield,
           "Invariant failure !elem.trueValue() || elem.fieldNameStringData() == \"_id\"_sd") {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies();
    exclusion.parse(BSON("_id.id1" << true));
}

TEST(ExclusionProjectionExecutionTest, ShouldAllowExplicitIdInclusionInExclusionSpec) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies();
    exclusion.parse(BSON("_id" << true << "a" << false));
}

TEST(ExclusionProjectionExecutionTest, ShouldSerializeToEquivalentProjection) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies();
    exclusion.parse(
        fromjson("{a: 0, b: {c: NumberLong(0), d: 0.0}, 'x.y': false, _id: NumberInt(0)}"));

    // Converts numbers to bools, converts dotted paths to nested documents. Note order of excluded
    // fields is subject to change.
    auto serialization = exclusion.serializeTransformation(boost::none);
    ASSERT_EQ(serialization.size(), 4UL);
    ASSERT_VALUE_EQ(serialization["a"], Value(false));
    ASSERT_VALUE_EQ(serialization["_id"], Value(false));

    ASSERT_EQ(serialization["b"].getType(), BSONType::Object);
    ASSERT_EQ(serialization["b"].getDocument().size(), 2UL);
    ASSERT_VALUE_EQ(serialization["b"].getDocument()["c"], Value(false));
    ASSERT_VALUE_EQ(serialization["b"].getDocument()["d"], Value(false));

    ASSERT_EQ(serialization["x"].getType(), BSONType::Object);
    ASSERT_EQ(serialization["x"].getDocument().size(), 1UL);
    ASSERT_VALUE_EQ(serialization["x"].getDocument()["y"], Value(false));
}

TEST(ExclusionProjectionExecutionTest, ShouldNotAddAnyDependencies) {
    // An exclusion projection will cause the stage to return DepsTracker::State::SEE_NEXT, meaning
    // it doesn't strictly require any fields.
    //
    // For example, if our projection was {a: 0}, and a later stage requires the field "a", then "a"
    // will be added to the dependencies correctly. If a later stage doesn't need "a", then we don't
    // need to include the "a" in the dependencies of this projection, since it will just be ignored
    // later. If there are no later stages, then we will finish the dependency computation
    // cycle without full knowledge of which fields are needed, and thus include all the fields.
    auto exclusion = makeExclusionProjectionWithDefaultPolicies();
    exclusion.parse(BSON("_id" << false << "a" << false << "b.c" << false << "x.y.z" << false));

    DepsTracker deps;
    exclusion.addDependencies(&deps);

    ASSERT_EQ(deps.fields.size(), 0UL);
    ASSERT_FALSE(deps.needWholeDocument);
    ASSERT_FALSE(deps.getNeedsMetadata(DocumentMetadataFields::kTextScore));
}

TEST(ExclusionProjectionExecutionTest, ShouldReportExcludedFieldsAsModified) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies();
    exclusion.parse(BSON("_id" << false << "a" << false << "b.c" << false));

    auto modifiedPaths = exclusion.getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kFiniteSet);
    ASSERT_EQ(modifiedPaths.paths.count("_id"), 1UL);
    ASSERT_EQ(modifiedPaths.paths.count("a"), 1UL);
    ASSERT_EQ(modifiedPaths.paths.count("b.c"), 1UL);
    ASSERT_EQ(modifiedPaths.paths.size(), 3UL);
}

TEST(ExclusionProjectionExecutionTest,
     ShouldReportExcludedFieldsAsModifiedWhenSpecifiedAsNestedObj) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies();
    exclusion.parse(BSON("a" << BSON("b" << false << "c" << BSON("d" << false))));

    auto modifiedPaths = exclusion.getModifiedPaths();
    ASSERT(modifiedPaths.type == DocumentSource::GetModPathsReturn::Type::kFiniteSet);
    ASSERT_EQ(modifiedPaths.paths.count("a.b"), 1UL);
    ASSERT_EQ(modifiedPaths.paths.count("a.c.d"), 1UL);
    ASSERT_EQ(modifiedPaths.paths.size(), 2UL);
}

//
// Tests of execution of exclusions at the top level.
//

TEST(ExclusionProjectionExecutionTest, ShouldExcludeTopLevelField) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies();
    exclusion.parse(BSON("a" << false));

    // More than one field in document.
    auto result = exclusion.applyProjection(Document{{"a", 1}, {"b", 2}});
    auto expectedResult = Document{{"b", 2}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is the only field in the document.
    result = exclusion.applyProjection(Document{{"a", 1}});
    expectedResult = Document{};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is not present in the document.
    result = exclusion.applyProjection(Document{{"c", 1}});
    expectedResult = Document{{"c", 1}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // There are no fields in the document.
    result = exclusion.applyProjection(Document{});
    expectedResult = Document{};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldCoerceNumericsToBools) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies();
    exclusion.parse(BSON("a" << Value(0) << "b" << Value(0LL) << "c" << Value(0.0) << "d"
                             << Value(Decimal128(0))));

    auto result =
        exclusion.applyProjection(Document{{"_id", "ID"_sd}, {"a", 1}, {"b", 2}, {"c", 3}});
    auto expectedResult = Document{{"_id", "ID"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldPreserveOrderOfExistingFields) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies();
    exclusion.parse(BSON("second" << false));
    auto result = exclusion.applyProjection(Document{{"first", 0}, {"second", 1}, {"third", 2}});
    auto expectedResult = Document{{"first", 0}, {"third", 2}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldImplicitlyIncludeId) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies();
    exclusion.parse(BSON("a" << false));
    auto result = exclusion.applyProjection(Document{{"a", 1}, {"b", 2}, {"_id", "ID"_sd}});
    auto expectedResult = Document{{"b", 2}, {"_id", "ID"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldExcludeIdIfExplicitlyExcluded) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies();
    exclusion.parse(BSON("a" << false << "_id" << false));
    auto result = exclusion.applyProjection(Document{{"a", 1}, {"b", 2}, {"_id", "ID"_sd}});
    auto expectedResult = Document{{"b", 2}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldExcludeIdAndKeepAllOtherFields) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies();
    exclusion.parse(BSON("_id" << false));
    auto result = exclusion.applyProjection(Document{{"a", 1}, {"b", 2}, {"_id", "ID"_sd}});
    auto expectedResult = Document{{"a", 1}, {"b", 2}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

//
// Tests of execution of nested exclusions.
//

TEST(ExclusionProjectionExecutionTest, ShouldExcludeSubFieldsOfId) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies();
    exclusion.parse(BSON("_id.x" << false << "_id" << BSON("y" << false)));
    auto result = exclusion.applyProjection(
        Document{{"_id", Document{{"x", 1}, {"y", 2}, {"z", 3}}}, {"a", 1}});
    auto expectedResult = Document{{"_id", Document{{"z", 3}}}, {"a", 1}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldExcludeSimpleDottedFieldFromSubDoc) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies();
    exclusion.parse(BSON("a.b" << false));

    // More than one field in sub document.
    auto result = exclusion.applyProjection(Document{{"a", Document{{"b", 1}, {"c", 2}}}});
    auto expectedResult = Document{{"a", Document{{"c", 2}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is the only field in the sub document.
    result = exclusion.applyProjection(Document{{"a", Document{{"b", 1}}}});
    expectedResult = Document{{"a", Document{}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Specified field is not present in the sub document.
    result = exclusion.applyProjection(Document{{"a", Document{{"c", 1}}}});
    expectedResult = Document{{"a", Document{{"c", 1}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // There are no fields in sub document.
    result = exclusion.applyProjection(Document{{"a", Document{}}});
    expectedResult = Document{{"a", Document{}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldNotCreateSubDocIfDottedExcludedFieldDoesNotExist) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies();
    exclusion.parse(BSON("sub.target" << false));

    // Should not add the path if it doesn't exist.
    auto result = exclusion.applyProjection(Document{});
    auto expectedResult = Document{};
    ASSERT_DOCUMENT_EQ(result, expectedResult);

    // Should not replace non-documents with documents.
    result = exclusion.applyProjection(Document{{"sub", "notADocument"_sd}});
    expectedResult = Document{{"sub", "notADocument"_sd}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldApplyDottedExclusionToEachElementInArray) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies();
    exclusion.parse(BSON("a.b" << false));

    std::vector<Value> nestedValues = {
        Value(1),
        Value(Document{}),
        Value(Document{{"b", 1}}),
        Value(Document{{"b", 1}, {"c", 2}}),
        Value(vector<Value>{}),
        Value(vector<Value>{Value(1), Value(Document{{"c", 1}, {"b", 1}})})};
    std::vector<Value> expectedNestedValues = {
        Value(1),
        Value(Document{}),
        Value(Document{}),
        Value(Document{{"c", 2}}),
        Value(vector<Value>{}),
        Value(vector<Value>{Value(1), Value(Document{{"c", 1}})})};
    auto result = exclusion.applyProjection(Document{{"a", nestedValues}});
    auto expectedResult = Document{{"a", expectedNestedValues}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldAllowMixedNestedAndDottedFields) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies();
    // Exclude all of "a.b", "a.c", "a.d", and "a.e".
    exclusion.parse(
        BSON("a.b" << false << "a.c" << false << "a" << BSON("d" << false << "e" << false)));
    auto result = exclusion.applyProjection(
        Document{{"a", Document{{"b", 1}, {"c", 2}, {"d", 3}, {"e", 4}, {"f", 5}}}});
    auto expectedResult = Document{{"a", Document{{"f", 5}}}};
    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldAlwaysKeepMetadataFromOriginalDoc) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies();
    exclusion.parse(BSON("a" << false));

    MutableDocument inputDocBuilder(Document{{"_id", "ID"_sd}, {"a", 1}});
    inputDocBuilder.metadata().setRandVal(1.0);
    inputDocBuilder.metadata().setTextScore(10.0);
    Document inputDoc = inputDocBuilder.freeze();

    auto result = exclusion.applyProjection(inputDoc);

    MutableDocument expectedDoc(Document{{"_id", "ID"_sd}});
    expectedDoc.copyMetaDataFrom(inputDoc);
    ASSERT_DOCUMENT_EQ(result, expectedDoc.freeze());
}

//
// _id exclusion policy.
//

TEST(ExclusionProjectionExecutionTest, ShouldIncludeIdByDefault) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies();
    exclusion.parse(BSON("a" << false));

    auto result = exclusion.applyProjection(Document{{"_id", 2}, {"a", 3}});
    auto expectedResult = Document{{"_id", 2}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldExcludeIdWithExplicitPolicy) {
    auto exclusion = makeExclusionProjectionWithDefaultIdExclusion();
    exclusion.parse(BSON("a" << false));

    auto result = exclusion.applyProjection(Document{{"_id", 2}, {"a", 3}});
    auto expectedResult = Document{};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldOverrideIncludePolicyWithExplicitExcludeIdSpec) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies();
    exclusion.parse(BSON("_id" << false << "a" << false));

    auto result = exclusion.applyProjection(Document{{"_id", 2}, {"a", 3}});
    auto expectedResult = Document{};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldOverrideExcludePolicyWithExplicitIncludeIdSpec) {
    auto exclusion = makeExclusionProjectionWithDefaultIdExclusion();
    exclusion.parse(BSON("_id" << true << "a" << false));

    auto result = exclusion.applyProjection(Document{{"_id", 2}, {"a", 3}, {"b", 4}});
    auto expectedResult = Document{{"_id", 2}, {"b", 4}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldAllowExclusionOfIdSubfieldWithDefaultIncludePolicy) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies();
    exclusion.parse(BSON("_id.id1" << false << "a" << false));

    auto result = exclusion.applyProjection(
        Document{{"_id", Document{{"id1", 1}, {"id2", 2}}}, {"a", 3}, {"b", 4}});
    auto expectedResult = Document{{"_id", Document{{"id2", 2}}}, {"b", 4}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldAllowExclusionOfIdSubfieldWithDefaultExcludePolicy) {
    auto exclusion = makeExclusionProjectionWithDefaultIdExclusion();
    exclusion.parse(BSON("_id.id1" << false << "a" << false));

    auto result = exclusion.applyProjection(
        Document{{"_id", Document{{"id1", 1}, {"id2", 2}}}, {"a", 3}, {"b", 4}});
    auto expectedResult = Document{{"_id", Document{{"id2", 2}}}, {"b", 4}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldAllowLimitedDollarPrefixedFields) {
    auto exclusion = makeExclusionProjectionWithDefaultIdExclusion();
    exclusion.parse(
        BSON("$id" << false << "$db" << false << "$ref" << false << "$sortKey" << false));

    auto result = exclusion.applyProjection(
        Document{{"$id", 5}, {"$db", 3}, {"$ref", 4}, {"$sortKey", 5}, {"someField", 6}});
    auto expectedResult = Document{{"someField", 6}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

//
// Nested array recursion.
//

TEST(ExclusionProjectionExecutionTest, ShouldRecurseNestedArraysByDefault) {
    auto exclusion = makeExclusionProjectionWithDefaultPolicies();
    exclusion.parse(BSON("a.b" << false));

    // {a: [1, {b: 2, c: 3}, [{b: 4, c: 5}], {d: 6}]} => {a: [1, {c: 3}, [{c: 5}], {d: 6}]}
    auto result = exclusion.applyProjection(
        Document{{"a",
                  vector<Value>{Value(1),
                                Value(Document{{"b", 2}, {"c", 3}}),
                                Value(vector<Value>{Value(Document{{"b", 4}, {"c", 5}})}),
                                Value(Document{{"d", 6}})}}});

    auto expectedResult = Document{{"a",
                                    vector<Value>{Value(1),
                                                  Value(Document{{"c", 3}}),
                                                  Value(vector<Value>{Value(Document{{"c", 5}})}),
                                                  Value(Document{{"d", 6}})}}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldNotRecurseNestedArraysForNoRecursePolicy) {
    auto exclusion = makeExclusionProjectionWithNoArrayRecursion();
    exclusion.parse(BSON("a.b" << false));

    // {a: [1, {b: 2, c: 3}, [{b: 4, c: 5}], {d: 6}]} => {a: [1, {c: 3}, [{b: 4, c: 5}], {d:
    // 6}]}
    auto result = exclusion.applyProjection(
        Document{{"a",
                  vector<Value>{Value(1),
                                Value(Document{{"b", 2}, {"c", 3}}),
                                Value(vector<Value>{Value(Document{{"b", 4}, {"c", 5}})}),
                                Value(Document{{"d", 6}})}}});

    auto expectedResult =
        Document{{"a",
                  vector<Value>{Value(1),
                                Value(Document{{"c", 3}}),
                                Value(vector<Value>{Value(Document{{"b", 4}, {"c", 5}})}),
                                Value(Document{{"d", 6}})}}};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

TEST(ExclusionProjectionExecutionTest, ShouldNotRetainNestedArraysIfNoRecursionNeeded) {
    auto exclusion = makeExclusionProjectionWithNoArrayRecursion();
    exclusion.parse(BSON("a" << false));

    // {a: [1, {b: 2, c: 3}, [{b: 4, c: 5}], {d: 6}]} => {}
    const auto inputDoc =
        Document{{"a",
                  vector<Value>{Value(1),
                                Value(Document{{"b", 2}, {"c", 3}}),
                                Value(vector<Value>{Value(Document{{"b", 4}, {"c", 5}})}),
                                Value(Document{{"d", 6}})}}};

    auto result = exclusion.applyProjection(inputDoc);
    const auto expectedResult = Document{};

    ASSERT_DOCUMENT_EQ(result, expectedResult);
}

}  // namespace
}  // namespace parsed_aggregation_projection
}  // namespace mongo
