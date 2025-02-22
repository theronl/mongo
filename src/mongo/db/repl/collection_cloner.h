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

#pragma once

#include <memory>
#include <vector>

#include "mongo/db/repl/base_cloner.h"
#include "mongo/db/repl/task_runner.h"
#include "mongo/util/progress_meter.h"

namespace mongo {
namespace repl {

namespace {
const int kProgressMeterSecondsBetween = 60;
const int kProgressMeterCheckInterval = 128;
}  // namespace

class CollectionCloner final : public BaseCloner {
public:
    struct Stats {
        static constexpr StringData kDocumentsToCopyFieldName = "documentsToCopy"_sd;
        static constexpr StringData kDocumentsCopiedFieldName = "documentsCopied"_sd;

        std::string ns;
        Date_t start;
        Date_t end;
        size_t documentToCopy{0};
        size_t documentsCopied{0};
        size_t indexes{0};
        size_t fetchedBatches{0};  // This is actually inserted batches.
        size_t receivedBatches{0};

        std::string toString() const;
        BSONObj toBSON() const;
        void append(BSONObjBuilder* builder) const;
    };

    /**
     * Type of function to schedule storage interface tasks with the executor.
     *
     * Used for testing only.
     */
    using ScheduleDbWorkFn = unique_function<StatusWith<executor::TaskExecutor::CallbackHandle>(
        executor::TaskExecutor::CallbackFn)>;

    CollectionCloner(const NamespaceString& ns,
                     const CollectionOptions& collectionOptions,
                     InitialSyncSharedData* sharedData,
                     const HostAndPort& source,
                     DBClientConnection* client,
                     StorageInterface* storageInterface,
                     ThreadPool* dbPool,
                     ClockSource* clock = SystemClockSource::get());

    virtual ~CollectionCloner() = default;

    Stats getStats() const;

    std::string toString() const;

    NamespaceString getSourceNss() const {
        return _sourceNss;
    }
    UUID getSourceUuid() const {
        return *_sourceDbAndUuid.uuid();
    }

    /**
     * Set the cloner batch size.
     *
     * Used for testing only.  Set by server parameter 'collectionClonerBatchSize' in normal
     * operation.
     */
    void setBatchSize_forTest(int batchSize) {
        _collectionClonerBatchSize = batchSize;
    }

protected:
    ClonerStages getStages() final;

    bool isMyFailPoint(const BSONObj& data) const final;

private:
    friend class CollectionClonerTest;

    class CollectionClonerStage : public ClonerStage<CollectionCloner> {
    public:
        CollectionClonerStage(std::string name, CollectionCloner* cloner, ClonerRunFn stageFunc)
            : ClonerStage<CollectionCloner>(name, cloner, stageFunc) {}
        AfterStageBehavior run() override;
    };

    std::string describeForFuzzer(BaseClonerStage* stage) const final {
        return _sourceNss.db() + " db: { " + stage->getName() + ": UUID(\"" +
            _sourceDbAndUuid.uuid()->toString() + "\") coll: " + _sourceNss.coll() + " }";
    }

    /**
     * Stage function that counts the number of documents in the collection on the source in order
     * to generate progress information.
     */
    AfterStageBehavior countStage();

    /**
     * Stage function that gets the index information of the collection on the source to re-create
     * it.
     */
    AfterStageBehavior listIndexesStage();

    /**
     * Stage function that creates the collection using the storageInterface.  This stage does not
     * actually contact the sync source.
     */
    AfterStageBehavior createCollectionStage();

    /**
     * Stage function that executes a query to retrieve all documents in the collection.  For each
     * batch returned by the upstream node, handleNextBatch will be called with the data.  This
     * stage will finish when the entire query is finished or failed.
     */
    AfterStageBehavior queryStage();

    /**
     * Put all results from a query batch into a buffer to be inserted, and schedule
     * it to be inserted.
     */
    void handleNextBatch(DBClientCursorBatchIterator& iter);

    /**
     * Called whenever there is a new batch of documents ready from the DBClientConnection.
     *
     * Each document returned will be inserted via the storage interfaceRequest storage
     * interface.
     */
    void insertDocumentsCallback(const executor::TaskExecutor::CallbackArgs& cbd);

    // All member variables are labeled with one of the following codes indicating the
    // synchronization rules for accessing them.
    //
    // (R)  Read-only in concurrent operation; no synchronization required.
    // (S)  Self-synchronizing; access according to class's own rules.
    // (M)  Reads and writes guarded by _mutex (defined in base class).
    // (X)  Access only allowed from the main flow of control called from run() or constructor.
    const NamespaceString _sourceNss;            // (R)
    const CollectionOptions _collectionOptions;  // (R)
    // Despite the type name, this member must always contain a UUID.
    NamespaceStringOrUUID _sourceDbAndUuid;  // (R)
    // The size of the batches of documents returned in collection cloning.
    int _collectionClonerBatchSize;  // (R)

    CollectionClonerStage _countStage;             // (R)
    CollectionClonerStage _listIndexesStage;       // (R)
    CollectionClonerStage _createCollectionStage;  // (R)
    CollectionClonerStage _queryStage;             // (R)

    ProgressMeter _progressMeter;                       // (X) progress meter for this instance.
    std::vector<BSONObj> _indexSpecs;                   // (X) Except for _id_
    BSONObj _idIndexSpec;                               // (X)
    std::unique_ptr<CollectionBulkLoader> _collLoader;  // (X)
    TaskRunner _dbWorkTaskRunner;                       // (R)
    //  Function for scheduling database work using the executor.
    ScheduleDbWorkFn _scheduleDbWorkFn;  // (R)
    // Documents read from source to insert.
    std::vector<BSONObj> _documentsToInsert;  // (M)
    Stats _stats;                             // (M)
};

}  // namespace repl
}  // namespace mongo
