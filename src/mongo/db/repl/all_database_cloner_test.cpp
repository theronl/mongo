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

#include "mongo/db/repl/all_database_cloner.h"
#include "mongo/db/repl/cloner_test_fixture.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/repl/storage_interface_mock.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/dbtests/mock/mock_dbclient_connection.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {
namespace repl {

class AllDatabaseClonerTest : public ClonerTestFixture {
public:
    AllDatabaseClonerTest() {}

protected:
    std::unique_ptr<AllDatabaseCloner> makeAllDatabaseCloner() {
        return std::make_unique<AllDatabaseCloner>(_sharedData.get(),
                                                   _source,
                                                   _mockClient.get(),
                                                   &_storageInterface,
                                                   _dbWorkThreadPool.get());
    }

    std::vector<std::string> getDatabasesFromCloner(AllDatabaseCloner* cloner) {
        return cloner->_databases;
    }
};

TEST_F(AllDatabaseClonerTest, FailsOnListDatabases) {
    Status expectedResult{ErrorCodes::BadValue, "foo"};
    _mockServer->setCommandReply("listDatabases", expectedResult);
    auto cloner = makeAllDatabaseCloner();

    auto result = cloner->run();

    ASSERT_EQ(result, expectedResult);
}

TEST_F(AllDatabaseClonerTest, AdminIsSetToFirst) {
    _mockServer->setCommandReply(
        "listDatabases", fromjson("{ok:1, databases:[{name:'a'}, {name:'aab'}, {name:'admin'}]}"));
    auto cloner = makeAllDatabaseCloner();
    cloner->setStopAfterStage_forTest("listDatabases");

    ASSERT_OK(cloner->run());

    auto databases = getDatabasesFromCloner(cloner.get());
    ASSERT_EQUALS("admin", databases[0]);

    _mockServer->setCommandReply(
        "listDatabases", fromjson("{ok:1, databases:[{name:'admin'}, {name:'a'}, {name:'b'}]}"));
    cloner = makeAllDatabaseCloner();
    cloner->setStopAfterStage_forTest("listDatabases");

    ASSERT_OK(cloner->run());

    databases = getDatabasesFromCloner(cloner.get());
    ASSERT_EQUALS("admin", databases[0]);
}

TEST_F(AllDatabaseClonerTest, LocalIsRemoved) {
    _mockServer->setCommandReply(
        "listDatabases", fromjson("{ok:1, databases:[{name:'a'}, {name:'aab'}, {name:'local'}]}"));
    auto cloner = makeAllDatabaseCloner();
    cloner->setStopAfterStage_forTest("listDatabases");

    ASSERT_OK(cloner->run());

    auto databases = getDatabasesFromCloner(cloner.get());
    ASSERT_EQUALS(2u, databases.size());
    ASSERT_EQUALS("a", databases[0]);
    ASSERT_EQUALS("aab", databases[1]);

    _mockServer->setCommandReply(
        "listDatabases", fromjson("{ok:1, databases:[{name:'local'}, {name:'a'}, {name:'b'}]}"));
    cloner = makeAllDatabaseCloner();
    cloner->setStopAfterStage_forTest("listDatabases");

    ASSERT_OK(cloner->run());

    databases = getDatabasesFromCloner(cloner.get());
    ASSERT_EQUALS(2u, databases.size());
    ASSERT_EQUALS("a", databases[0]);
    ASSERT_EQUALS("b", databases[1]);
}

TEST_F(AllDatabaseClonerTest, DatabaseStats) {
    bool isAdminDbValidFnCalled = false;
    _storageInterface.isAdminDbValidFn = [&isAdminDbValidFnCalled](OperationContext* opCtx) {
        isAdminDbValidFnCalled = true;
        return Status::OK();
    };

    _mockServer->setCommandReply(
        "listDatabases", fromjson("{ok:1, databases:[{name:'a'}, {name:'aab'}, {name: 'admin'}]}"));

    // Make the DatabaseCloner do nothing
    _mockServer->setCommandReply("listCollections", createCursorResponse("admin.$cmd", {}));
    auto cloner = makeAllDatabaseCloner();

    // Set up the DatabaseCloner to pause so we can check stats.
    // We need to use two fail points to do this because fail points cannot have their data
    // modified atomically.
    auto dbClonerBeforeFailPoint = globalFailPointRegistry().find("hangBeforeClonerStage");
    auto dbClonerAfterFailPoint = globalFailPointRegistry().find("hangAfterClonerStage");
    auto timesEntered = dbClonerBeforeFailPoint->setMode(
        FailPoint::alwaysOn,
        0,
        fromjson("{cloner: 'DatabaseCloner', stage: 'listCollections', database: 'admin'}"));
    dbClonerAfterFailPoint->setMode(
        FailPoint::alwaysOn,
        0,
        fromjson("{cloner: 'DatabaseCloner', stage: 'listCollections', database: 'admin'}"));

    // Run the cloner in a separate thread to
    stdx::thread clonerThread([&] {
        Client::initThread("ClonerRunner");
        ASSERT_OK(cloner->run());
    });

    // Wait for the failpoint to be reached
    dbClonerBeforeFailPoint->waitForTimesEntered(timesEntered + 1);

    auto databases = getDatabasesFromCloner(cloner.get());
    ASSERT_EQUALS(3u, databases.size());
    ASSERT_EQUALS("admin", databases[0]);
    ASSERT_EQUALS("aab", databases[1]);
    ASSERT_EQUALS("a", databases[2]);

    auto stats = cloner->getStats();
    ASSERT_EQUALS(3, stats.databaseCount);
    ASSERT_EQUALS(0, stats.databasesCloned);

    // Allow the cloner to move to the next DB.
    timesEntered = dbClonerBeforeFailPoint->setMode(
        FailPoint::alwaysOn,
        0,
        fromjson("{cloner: 'DatabaseCloner', stage: 'listCollections', database: 'aab'}"));
    dbClonerAfterFailPoint->setMode(
        FailPoint::alwaysOn,
        0,
        fromjson("{cloner: 'DatabaseCloner', stage: 'listCollections', database: 'aab'}"));

    // Wait for the failpoint to be reached.
    dbClonerBeforeFailPoint->waitForTimesEntered(timesEntered + 1);

    stats = cloner->getStats();
    ASSERT_EQUALS(3, stats.databaseCount);
    ASSERT_EQUALS(1, stats.databasesCloned);
    ASSERT_EQUALS("admin", stats.databaseStats[0].dbname);
    ASSERT(isAdminDbValidFnCalled);

    // Allow the cloner to move to the last DB.
    timesEntered = dbClonerBeforeFailPoint->setMode(
        FailPoint::alwaysOn,
        0,
        fromjson("{cloner: 'DatabaseCloner', stage: 'listCollections', database: 'a'}"));
    dbClonerAfterFailPoint->setMode(
        FailPoint::alwaysOn,
        0,
        fromjson("{cloner: 'DatabaseCloner', stage: 'listCollections', database: 'a'}"));

    // Wait for the failpoint to be reached.
    dbClonerBeforeFailPoint->waitForTimesEntered(timesEntered + 1);

    stats = cloner->getStats();
    ASSERT_EQUALS(3, stats.databaseCount);
    ASSERT_EQUALS(2, stats.databasesCloned);
    ASSERT_EQUALS("admin", stats.databaseStats[0].dbname);
    ASSERT_EQUALS("aab", stats.databaseStats[1].dbname);

    // Allow the cloner to finish
    dbClonerBeforeFailPoint->setMode(FailPoint::off, 0);
    dbClonerAfterFailPoint->setMode(FailPoint::off, 0);
    clonerThread.join();

    stats = cloner->getStats();
    ASSERT_EQUALS(3, stats.databaseCount);
    ASSERT_EQUALS(3, stats.databasesCloned);
    ASSERT_EQUALS("admin", stats.databaseStats[0].dbname);
    ASSERT_EQUALS("aab", stats.databaseStats[1].dbname);
    ASSERT_EQUALS("a", stats.databaseStats[2].dbname);
}

TEST_F(AllDatabaseClonerTest, FailsOnListCollectionsOnOnlyDatabase) {
    _mockServer->setCommandReply("listDatabases", fromjson("{ok:1, databases:[{name:'a'}]}"));
    _mockServer->setCommandReply("listCollections", Status{ErrorCodes::NoSuchKey, "fake"});

    auto cloner = makeAllDatabaseCloner();
    ASSERT_NOT_OK(cloner->run());
}

}  // namespace repl
}  // namespace mongo
