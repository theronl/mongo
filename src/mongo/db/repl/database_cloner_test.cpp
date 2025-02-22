/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/clientcursor.h"
#include "mongo/db/repl/cloner_test_fixture.h"
#include "mongo/db/repl/database_cloner.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {
namespace repl {

struct CollectionCloneInfo {
    std::shared_ptr<CollectionMockStats> stats = std::make_shared<CollectionMockStats>();
    CollectionBulkLoaderMock* loader = nullptr;
};

class DatabaseClonerTest : public ClonerTestFixture {
public:
    DatabaseClonerTest() {}

protected:
    void setUp() override {
        ClonerTestFixture::setUp();
        _storageInterface.createCollectionForBulkFn =
            [this](const NamespaceString& nss,
                   const CollectionOptions& options,
                   const BSONObj& idIndexSpec,
                   const std::vector<BSONObj>& secondaryIndexSpecs)
            -> StatusWith<std::unique_ptr<CollectionBulkLoaderMock>> {
            const auto collInfo = &_collections[nss];

            auto localLoader = std::make_unique<CollectionBulkLoaderMock>(collInfo->stats);
            auto status = localLoader->init(secondaryIndexSpecs);
            if (!status.isOK())
                return status;
            collInfo->loader = localLoader.get();

            return std::move(localLoader);
        };
    }
    std::unique_ptr<DatabaseCloner> makeDatabaseCloner() {
        return std::make_unique<DatabaseCloner>(_dbName,
                                                _sharedData.get(),
                                                _source,
                                                _mockClient.get(),
                                                &_storageInterface,
                                                _dbWorkThreadPool.get());
    }

    BSONObj createListCollectionsResponse(const std::vector<BSONObj>& collections) {
        auto ns = _dbName + "$cmd.listCollections";
        BSONObjBuilder bob;
        {
            BSONObjBuilder cursorBob(bob.subobjStart("cursor"));
            cursorBob.append("id", CursorId(0));
            cursorBob.append("ns", ns);
            {
                BSONArrayBuilder batchBob(cursorBob.subarrayStart("firstBatch"));
                for (const auto& coll : collections) {
                    batchBob.append(coll);
                }
            }
        }
        bob.append("ok", 1);
        return bob.obj();
    }

    std::vector<std::pair<NamespaceString, CollectionOptions>> getCollectionsFromCloner(
        DatabaseCloner* cloner) {
        return cloner->_collections;
    }

    std::map<NamespaceString, CollectionCloneInfo> _collections;

    static std::string _dbName;
};

/* static */
std::string DatabaseClonerTest::_dbName = "testDb";

// A database may have no collections. Nothing to do for the database cloner.
TEST_F(DatabaseClonerTest, ListCollectionsReturnedNoCollections) {
    _mockServer->setCommandReply("listCollections", createListCollectionsResponse({}));
    auto cloner = makeDatabaseCloner();

    ASSERT_OK(cloner->run());
    ASSERT_OK(_sharedData->getInitialSyncStatus(WithLock::withoutLock()));
    ASSERT(getCollectionsFromCloner(cloner.get()).empty());
}

TEST_F(DatabaseClonerTest, ListCollections) {
    auto cloner = makeDatabaseCloner();
    cloner->setStopAfterStage_forTest("listCollections");
    auto uuid1 = UUID::gen();
    auto uuid2 = UUID::gen();
    const std::vector<BSONObj> sourceInfos = {BSON("name"
                                                   << "a"
                                                   << "type"
                                                   << "collection"
                                                   << "options" << BSONObj() << "info"
                                                   << BSON("readOnly" << false << "uuid" << uuid1)),
                                              BSON(
                                                  "name"
                                                  << "b"
                                                  << "type"
                                                  << "collection"
                                                  << "options" << BSONObj() << "info"
                                                  << BSON("readOnly" << false << "uuid" << uuid2))};
    _mockServer->setCommandReply("listCollections",
                                 createListCollectionsResponse({sourceInfos[0], sourceInfos[1]}));
    ASSERT_OK(cloner->run());
    ASSERT_OK(_sharedData->getInitialSyncStatus(WithLock::withoutLock()));
    auto collections = getCollectionsFromCloner(cloner.get());

    ASSERT_EQUALS(2U, collections.size());
    ASSERT_EQ(NamespaceString(_dbName, "a"), collections[0].first);
    ASSERT_BSONOBJ_EQ(BSON("uuid" << uuid1), collections[0].second.toBSON());
    ASSERT_EQ(NamespaceString(_dbName, "b"), collections[1].first);
    ASSERT_BSONOBJ_EQ(BSON("uuid" << uuid2), collections[1].second.toBSON());
}

// The listCollections command may return new fields in later versions; we do not want that
// to cause upgrade/downgrade issues.
TEST_F(DatabaseClonerTest, ListCollectionsAllowsExtraneousFields) {
    auto cloner = makeDatabaseCloner();
    cloner->setStopAfterStage_forTest("listCollections");
    auto uuid1 = UUID::gen();
    auto uuid2 = UUID::gen();
    const std::vector<BSONObj> sourceInfos = {BSON("name"
                                                   << "a"
                                                   << "type"
                                                   << "collection"
                                                   // The "flavor" field is not really found in
                                                   // listCollections.
                                                   << "flavor"
                                                   << "raspberry"
                                                   << "options" << BSONObj() << "info"
                                                   << BSON("readOnly" << false << "uuid" << uuid1)),
                                              BSON("name"
                                                   << "b"
                                                   << "type"
                                                   << "collection"
                                                   << "options" << BSONObj()
                                                   << "info"
                                                   // The "comet" field is not really found in
                                                   // listCollections.
                                                   << BSON("readOnly" << false << "uuid" << uuid2
                                                                      << "comet"
                                                                      << "2l_Borisov"))};
    _mockServer->setCommandReply("listCollections",
                                 createListCollectionsResponse({sourceInfos[0], sourceInfos[1]}));
    ASSERT_OK(cloner->run());
    ASSERT_OK(_sharedData->getInitialSyncStatus(WithLock::withoutLock()));
    auto collections = getCollectionsFromCloner(cloner.get());

    ASSERT_EQUALS(2U, collections.size());
    ASSERT_EQ(NamespaceString(_dbName, "a"), collections[0].first);
    ASSERT_BSONOBJ_EQ(BSON("uuid" << uuid1), collections[0].second.toBSON());
    ASSERT_EQ(NamespaceString(_dbName, "b"), collections[1].first);
    ASSERT_BSONOBJ_EQ(BSON("uuid" << uuid2), collections[1].second.toBSON());
}

TEST_F(DatabaseClonerTest, ListCollectionsFailsOnDuplicateNames) {
    auto cloner = makeDatabaseCloner();
    cloner->setStopAfterStage_forTest("listCollections");
    auto uuid1 = UUID::gen();
    auto uuid2 = UUID::gen();
    const std::vector<BSONObj> sourceInfos = {BSON("name"
                                                   << "a"
                                                   << "type"
                                                   << "collection"
                                                   << "options" << BSONObj() << "info"
                                                   << BSON("readOnly" << false << "uuid" << uuid1)),
                                              BSON(
                                                  "name"
                                                  << "a"
                                                  << "type"
                                                  << "collection"
                                                  << "options" << BSONObj() << "info"
                                                  << BSON("readOnly" << false << "uuid" << uuid2))};
    _mockServer->setCommandReply("listCollections",
                                 createListCollectionsResponse({sourceInfos[0], sourceInfos[1]}));
    auto status = cloner->run();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(51005, status.code());
}

TEST_F(DatabaseClonerTest, ListCollectionsFailsOnMissingNameField) {
    auto cloner = makeDatabaseCloner();
    cloner->setStopAfterStage_forTest("listCollections");
    auto uuid1 = UUID::gen();
    auto uuid2 = UUID::gen();
    const std::vector<BSONObj> sourceInfos = {BSON("type"
                                                   << "collection"
                                                   << "options" << BSONObj() << "info"
                                                   << BSON("readOnly" << false << "uuid" << uuid1)),
                                              BSON(
                                                  "name"
                                                  << "b"
                                                  << "type"
                                                  << "collection"
                                                  << "options" << BSONObj() << "info"
                                                  << BSON("readOnly" << false << "uuid" << uuid2))};
    _mockServer->setCommandReply("listCollections",
                                 createListCollectionsResponse({sourceInfos[0], sourceInfos[1]}));
    auto status = cloner->run();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
}

TEST_F(DatabaseClonerTest, ListCollectionsFailsOnMissingOptions) {
    auto cloner = makeDatabaseCloner();
    cloner->setStopAfterStage_forTest("listCollections");
    auto uuid1 = UUID::gen();
    const std::vector<BSONObj> sourceInfos = {BSON("name"
                                                   << "a"
                                                   << "type"
                                                   << "collection"),
                                              BSON(
                                                  "name"
                                                  << "a"
                                                  << "type"
                                                  << "collection"
                                                  << "options" << BSONObj() << "info"
                                                  << BSON("readOnly" << false << "uuid" << uuid1))};
    _mockServer->setCommandReply("listCollections",
                                 createListCollectionsResponse({sourceInfos[0], sourceInfos[1]}));
    auto status = cloner->run();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
}

TEST_F(DatabaseClonerTest, ListCollectionsFailsOnMissingUUID) {
    auto cloner = makeDatabaseCloner();
    cloner->setStopAfterStage_forTest("listCollections");
    auto uuid1 = UUID::gen();
    const std::vector<BSONObj> sourceInfos = {BSON("name"
                                                   << "a"
                                                   << "type"
                                                   << "collection"
                                                   << "options" << BSONObj() << "info"
                                                   << BSON("readOnly" << false)),
                                              BSON(
                                                  "name"
                                                  << "b"
                                                  << "type"
                                                  << "collection"
                                                  << "options" << BSONObj() << "info"
                                                  << BSON("readOnly" << false << "uuid" << uuid1))};
    _mockServer->setCommandReply("listCollections",
                                 createListCollectionsResponse({sourceInfos[0], sourceInfos[1]}));
    auto status = cloner->run();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
}

TEST_F(DatabaseClonerTest, ListCollectionsFailsOnInvalidCollectionOptions) {
    auto cloner = makeDatabaseCloner();
    cloner->setStopAfterStage_forTest("listCollections");
    auto uuid1 = UUID::gen();
    auto uuid2 = UUID::gen();
    const std::vector<BSONObj>
        sourceInfos = {BSON("name"
                            << "a"
                            << "type"
                            << "collection"
                            << "options" << BSONObj() << "info"
                            << BSON("readOnly" << false << "uuid" << uuid1)),
                       BSON("name"
                            << "b"
                            << "type"
                            << "collection"
                            // "storageEngine" is not an integer collection option.
                            << "options" << BSON("storageEngine" << 1) << "info"
                            << BSON("readOnly" << false << "uuid" << uuid2))};
    _mockServer->setCommandReply("listCollections",
                                 createListCollectionsResponse({sourceInfos[0], sourceInfos[1]}));
    auto status = cloner->run();
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::FailedToParse, status.code());
}

TEST_F(DatabaseClonerTest, FirstCollectionListIndexesFailed) {
    auto uuid1 = UUID::gen();
    auto uuid2 = UUID::gen();
    const BSONObj idIndexSpec = BSON("v" << 1 << "key" << BSON("_id" << 1) << "name"
                                         << "_id_");
    const std::vector<BSONObj> sourceInfos = {BSON("name"
                                                   << "a"
                                                   << "type"
                                                   << "collection"
                                                   << "options" << BSONObj() << "info"
                                                   << BSON("readOnly" << false << "uuid" << uuid1)),
                                              BSON(
                                                  "name"
                                                  << "b"
                                                  << "type"
                                                  << "collection"
                                                  << "options" << BSONObj() << "info"
                                                  << BSON("readOnly" << false << "uuid" << uuid2))};
    _mockServer->setCommandReply("listCollections",
                                 createListCollectionsResponse({sourceInfos[0], sourceInfos[1]}));
    _mockServer->setCommandReply("count", {createCountResponse(0), createCountResponse(0)});
    _mockServer->setCommandReply("listIndexes",
                                 {BSON("ok" << 0 << "errmsg"
                                            << "fake message"
                                            << "code" << ErrorCodes::CursorNotFound),
                                  createCursorResponse(_dbName + ".b", BSON_ARRAY(idIndexSpec))});
    auto cloner = makeDatabaseCloner();
    auto status = cloner->run();
    ASSERT_NOT_OK(status);

    ASSERT_EQ(status.code(), ErrorCodes::InitialSyncFailure);
    ASSERT_EQUALS(0u, _collections.size());
}

TEST_F(DatabaseClonerTest, CreateCollections) {
    auto uuid1 = UUID::gen();
    auto uuid2 = UUID::gen();
    const BSONObj idIndexSpec = BSON("v" << 1 << "key" << BSON("_id" << 1) << "name"
                                         << "_id_");
    const std::vector<BSONObj> sourceInfos = {BSON("name"
                                                   << "a"
                                                   << "type"
                                                   << "collection"
                                                   << "options" << BSONObj() << "info"
                                                   << BSON("readOnly" << false << "uuid" << uuid1)),
                                              BSON(
                                                  "name"
                                                  << "b"
                                                  << "type"
                                                  << "collection"
                                                  << "options" << BSONObj() << "info"
                                                  << BSON("readOnly" << false << "uuid" << uuid2))};
    _mockServer->setCommandReply("listCollections",
                                 createListCollectionsResponse({sourceInfos[0], sourceInfos[1]}));
    _mockServer->setCommandReply("count", {createCountResponse(0), createCountResponse(0)});
    _mockServer->setCommandReply("listIndexes",
                                 {createCursorResponse(_dbName + ".a", BSON_ARRAY(idIndexSpec)),
                                  createCursorResponse(_dbName + ".b", BSON_ARRAY(idIndexSpec))});
    auto cloner = makeDatabaseCloner();
    auto status = cloner->run();
    ASSERT_OK(status);

    ASSERT_EQUALS(2U, _collections.size());

    auto collInfo = _collections[NamespaceString{_dbName, "a"}];
    auto stats = *collInfo.stats;
    ASSERT_EQUALS(0, stats.insertCount);
    ASSERT(stats.commitCalled);

    collInfo = _collections[NamespaceString{_dbName, "b"}];
    stats = *collInfo.stats;
    ASSERT_EQUALS(0, stats.insertCount);
    ASSERT(stats.commitCalled);
}

}  // namespace repl
}  // namespace mongo
