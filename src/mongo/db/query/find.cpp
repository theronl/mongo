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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "mongo/db/query/find.h"

#include <memory>

#include "mongo/base/error_codes.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/curop_failpoint_helpers.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/filter.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/s/chunk_version.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

namespace mongo {

using std::unique_ptr;

// Failpoint for checking whether we've received a getmore.
MONGO_FAIL_POINT_DEFINE(failReceivedGetmore);

// Failpoint to keep a cursor pinned.
MONGO_FAIL_POINT_DEFINE(legacyGetMoreWaitWithCursor)

bool shouldSaveCursor(OperationContext* opCtx,
                      const Collection* collection,
                      PlanExecutor::ExecState finalState,
                      PlanExecutor* exec) {
    if (PlanExecutor::FAILURE == finalState) {
        return false;
    }

    const QueryRequest& qr = exec->getCanonicalQuery()->getQueryRequest();
    if (!qr.wantMore()) {
        return false;
    }

    // We keep a tailable cursor around unless the collection we're tailing has no
    // records.
    //
    // SERVER-13955: we should be able to create a tailable cursor that waits on
    // an empty collection. Right now we do not keep a cursor if the collection
    // has zero records.
    if (qr.isTailable()) {
        return collection && collection->numRecords(opCtx) != 0U;
    }

    return !exec->isEOF();
}

bool shouldSaveCursorGetMore(PlanExecutor::ExecState finalState,
                             PlanExecutor* exec,
                             bool isTailable) {
    if (PlanExecutor::FAILURE == finalState) {
        return false;
    }

    if (isTailable) {
        return true;
    }

    return !exec->isEOF();
}

void beginQueryOp(OperationContext* opCtx,
                  const NamespaceString& nss,
                  const BSONObj& queryObj,
                  long long ntoreturn,
                  long long ntoskip) {
    auto curOp = CurOp::get(opCtx);
    curOp->debug().ntoreturn = ntoreturn;
    curOp->debug().ntoskip = ntoskip;
    stdx::lock_guard<Client> lk(*opCtx->getClient());
    curOp->setOpDescription_inlock(queryObj);
    curOp->setNS_inlock(nss.ns());
}

void endQueryOp(OperationContext* opCtx,
                Collection* collection,
                const PlanExecutor& exec,
                long long numResults,
                CursorId cursorId) {
    auto curOp = CurOp::get(opCtx);

    // Fill out basic CurOp query exec properties.
    curOp->debug().nreturned = numResults;
    curOp->debug().cursorid = (0 == cursorId ? -1 : cursorId);
    curOp->debug().cursorExhausted = (0 == cursorId);

    // Fill out CurOp based on explain summary statistics.
    PlanSummaryStats summaryStats;
    Explain::getSummaryStats(exec, &summaryStats);
    curOp->debug().setPlanSummaryMetrics(summaryStats);

    if (collection) {
        CollectionQueryInfo::get(collection).notifyOfQuery(opCtx, summaryStats);
    }

    if (curOp->shouldDBProfile()) {
        BSONObjBuilder statsBob;
        Explain::getWinningPlanStats(&exec, &statsBob);
        curOp->debug().execStats = statsBob.obj();
    }
}

namespace {

/**
 * Uses 'cursor' to fill out 'bb' with the batch of result documents to
 * be returned by this getMore.
 *
 * Returns the number of documents in the batch in 'numResults', which must be initialized to
 * zero by the caller. Returns the final ExecState returned by the cursor in *state. Returns
 * whether or not to save the ClientCursor in 'shouldSaveCursor'.
 *
 * Returns an OK status if the batch was successfully generated, and a non-OK status if the
 * PlanExecutor encounters a failure.
 */
void generateBatch(int ntoreturn,
                   ClientCursor* cursor,
                   BufBuilder* bb,
                   std::uint64_t* numResults,
                   PlanExecutor::ExecState* state) {
    PlanExecutor* exec = cursor->getExecutor();

    Document doc;
    while (!FindCommon::enoughForGetMore(ntoreturn, *numResults) &&
           PlanExecutor::ADVANCED == (*state = exec->getNext(&doc, nullptr))) {
        BSONObj obj = doc.toBson();

        // If we can't fit this result inside the current batch, then we stash it for later.
        if (!FindCommon::haveSpaceForNext(obj, *numResults, bb->len())) {
            exec->enqueue(obj);
            break;
        }

        // Add result to output buffer.
        bb->appendBuf((void*)obj.objdata(), obj.objsize());

        // Count the result.
        (*numResults)++;
    }

    // Propagate any errors to the caller.
    switch (*state) {
        // Log an error message and then perform the cleanup.
        case PlanExecutor::FAILURE: {
            error() << "getMore executor error, stats: "
                    << redact(Explain::getWinningPlanStats(exec));
            // We should always have a valid status object by this point.
            auto status = WorkingSetCommon::getMemberObjectStatus(doc);
            invariant(!status.isOK());
            uassertStatusOK(status);
        }
        default:
            return;
    }

    MONGO_UNREACHABLE;
}

Message makeCursorNotFoundResponse() {
    const int initialBufSize = 512 + sizeof(QueryResult::Value);
    BufBuilder bb(initialBufSize);
    bb.skip(sizeof(QueryResult::Value));
    QueryResult::View qr = bb.buf();
    qr.msgdata().setLen(bb.len());
    qr.msgdata().setOperation(opReply);
    qr.setResultFlags(ResultFlag_CursorNotFound);
    qr.setCursorId(0);
    qr.setStartingFrom(0);
    qr.setNReturned(0);
    return Message(bb.release());
}

}  // namespace

/**
 * Called by db/instance.cpp.  This is the getMore entry point.
 */
Message getMore(OperationContext* opCtx,
                const char* ns,
                int ntoreturn,
                long long cursorid,
                bool* exhaust,
                bool* isCursorAuthorized) {
    invariant(ntoreturn >= 0);

    LOG(5) << "Running getMore, cursorid: " << cursorid;

    CurOp& curOp = *CurOp::get(opCtx);
    curOp.ensureStarted();

    // For testing, we may want to fail if we receive a getmore.
    if (MONGO_unlikely(failReceivedGetmore.shouldFail())) {
        MONGO_UNREACHABLE;
    }

    *exhaust = false;

    const NamespaceString nss(ns);

    // Cursors come in one of two flavors:
    //
    // - Cursors which read from a single collection, such as those generated via the find command.
    //   For these cursors, we hold the appropriate collection lock for the duration of the getMore
    //   using AutoGetCollectionForRead. These cursors have the 'kLockExternally' lock policy.
    //
    // - Cursors which may read from many collections, e.g. those generated via the aggregate
    //   command, or which do not read from a collection at all, e.g. those generated by the
    //   listIndexes command. We don't need to acquire locks to use these cursors, since they either
    //   manage locking themselves or don't access data protected by collection locks. These cursors
    //   have the 'kLocksInternally' lock policy.
    //
    // While we only need to acquire locks for 'kLockExternally' cursors, we need to create an
    // AutoStatsTracker in either case. This is responsible for updating statistics in CurOp and
    // Top. We avoid using AutoGetCollectionForReadCommand because we may need to drop and reacquire
    // locks when the cursor is awaitData, but we don't want to update the stats twice.
    UninterruptibleLockGuard noInterrupt(opCtx->lockState());
    boost::optional<AutoGetCollectionForRead> readLock;
    boost::optional<AutoStatsTracker> statsTracker;

    // These are set in the QueryResult msg we return.
    int resultFlags = ResultFlag_AwaitCapable;

    auto cursorManager = CursorManager::get(opCtx);
    auto statusWithCursorPin = cursorManager->pinCursor(opCtx, cursorid);
    if (statusWithCursorPin == ErrorCodes::CursorNotFound) {
        return makeCursorNotFoundResponse();
    }
    uassertStatusOK(statusWithCursorPin.getStatus());
    auto cursorPin = std::move(statusWithCursorPin.getValue());

    if (cursorPin->lockPolicy() == ClientCursorParams::LockPolicy::kLocksInternally) {
        if (!nss.isCollectionlessCursorNamespace()) {
            AutoGetDb autoDb(opCtx, nss.db(), MODE_IS);
            const auto profilingLevel = autoDb.getDb()
                ? boost::optional<int>{autoDb.getDb()->getProfilingLevel()}
                : boost::none;
            statsTracker.emplace(opCtx,
                                 nss,
                                 Top::LockType::NotLocked,
                                 AutoStatsTracker::LogMode::kUpdateTopAndCurop,
                                 profilingLevel);
            auto view = autoDb.getDb() ? ViewCatalog::get(autoDb.getDb())->lookup(opCtx, nss.ns())
                                       : nullptr;
            uassert(
                ErrorCodes::CommandNotSupportedOnView,
                str::stream() << "Namespace " << nss.ns()
                              << " is a view. OP_GET_MORE operations are not supported on views. "
                              << "Only clients which support the getMore command can be used to "
                                 "query views.",
                !view);
        }
    } else {
        readLock.emplace(opCtx, nss);
        const int doNotChangeProfilingLevel = 0;
        statsTracker.emplace(opCtx,
                             nss,
                             Top::LockType::ReadLocked,
                             AutoStatsTracker::LogMode::kUpdateTopAndCurop,
                             readLock->getDb() ? readLock->getDb()->getProfilingLevel()
                                               : doNotChangeProfilingLevel);

        // This checks to make sure the operation is allowed on a replicated node.  Since we are not
        // passing in a query object (necessary to check SlaveOK query option), we allow reads
        // whether we are PRIMARY or SECONDARY.
        uassertStatusOK(
            repl::ReplicationCoordinator::get(opCtx)->checkCanServeReadsFor(opCtx, nss, true));
    }

    std::uint64_t numResults = 0;
    int startingResult = 0;

    const int initialBufSize =
        512 + sizeof(QueryResult::Value) + FindCommon::kMaxBytesToReturnToClientAtOnce;

    BufBuilder bb(initialBufSize);
    bb.skip(sizeof(QueryResult::Value));

    // Check for spoofing of the ns such that it does not match the one originally there for the
    // cursor.
    uassert(ErrorCodes::Unauthorized,
            str::stream() << "Requested getMore on namespace " << ns << ", but cursor " << cursorid
                          << " belongs to namespace " << cursorPin->nss().ns(),
            nss == cursorPin->nss());

    // A user can only call getMore on their own cursor. If there were multiple users authenticated
    // when the cursor was created, then at least one of them must be authenticated in order to run
    // getMore on the cursor.
    uassert(ErrorCodes::Unauthorized,
            str::stream() << "cursor id " << cursorid
                          << " was not created by the authenticated user",
            AuthorizationSession::get(opCtx->getClient())
                ->isCoauthorizedWith(cursorPin->getAuthenticatedUsers()));

    *isCursorAuthorized = true;

    // Only used by the failpoints.
    std::function<void()> dropAndReaquireReadLock = [&] {
        // Make sure an interrupted operation does not prevent us from reacquiring the lock.
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());

        readLock.reset();
        readLock.emplace(opCtx, nss);
    };

    // On early return, get rid of the cursor.
    auto cursorFreer = makeGuard([&] { cursorPin.deleteUnderlying(); });

    // If the 'waitAfterPinningCursorBeforeGetMoreBatch' fail point is enabled, set the
    // 'msg' field of this operation's CurOp to signal that we've hit this point and then
    // repeatedly release and re-acquire the collection readLock at regular intervals until
    // the failpoint is released. This is done in order to avoid deadlocks caused by the
    // pinned-cursor failpoints in this file (see SERVER-21997).
    waitAfterPinningCursorBeforeGetMoreBatch.execute([&](const BSONObj& data) {
        if (data["shouldNotdropLock"].booleanSafe()) {
            dropAndReaquireReadLock = []() {};
        }

        CurOpFailpointHelpers::waitWhileFailPointEnabled(&waitAfterPinningCursorBeforeGetMoreBatch,
                                                         opCtx,
                                                         "waitAfterPinningCursorBeforeGetMoreBatch",
                                                         dropAndReaquireReadLock,
                                                         false,
                                                         nss);
    });


    const auto replicationMode = repl::ReplicationCoordinator::get(opCtx)->getReplicationMode();

    if (replicationMode == repl::ReplicationCoordinator::modeReplSet &&
        cursorPin->getReadConcernArgs().getLevel() ==
            repl::ReadConcernLevel::kMajorityReadConcern) {
        opCtx->recoveryUnit()->setTimestampReadSource(RecoveryUnit::ReadSource::kMajorityCommitted);
        uassertStatusOK(opCtx->recoveryUnit()->obtainMajorityCommittedSnapshot());
    }

    uassert(40548,
            "OP_GET_MORE operations are not supported on tailable aggregations. Only clients "
            "which support the getMore command can be used on tailable aggregations.",
            readLock || !cursorPin->isAwaitData());
    uassert(
        31124,
        str::stream()
            << "OP_GET_MORE does not support cursors with a write concern other than the default."
               " Use the getMore command instead. Write concern was: "
            << cursorPin->getWriteConcernOptions().toBSON(),
        cursorPin->getWriteConcernOptions().usedDefault);

    // If the operation that spawned this cursor had a time limit set, apply leftover time to this
    // getmore.
    if (cursorPin->getLeftoverMaxTimeMicros() < Microseconds::max()) {
        uassert(40136,
                "Illegal attempt to set operation deadline within DBDirectClient",
                !opCtx->getClient()->isInDirectClient());
        opCtx->setDeadlineAfterNowBy(cursorPin->getLeftoverMaxTimeMicros(),
                                     ErrorCodes::MaxTimeMSExpired);
    }
    opCtx->checkForInterrupt();  // May trigger maxTimeAlwaysTimeOut fail point.

    // What number result are we starting at?  Used to fill out the reply.
    startingResult = cursorPin->nReturnedSoFar();

    uint64_t notifierVersion = 0;
    std::shared_ptr<CappedInsertNotifier> notifier;
    if (cursorPin->isAwaitData()) {
        invariant(readLock->getCollection()->isCapped());
        // Retrieve the notifier which we will wait on until new data arrives. We make sure to do
        // this in the lock because once we drop the lock it is possible for the collection to
        // become invalid. The notifier itself will outlive the collection if the collection is
        // dropped, as we keep a shared_ptr to it.
        notifier = readLock->getCollection()->getCappedInsertNotifier();

        // Must get the version before we call generateBatch in case a write comes in after that
        // call and before we call wait on the notifier.
        notifierVersion = notifier->getVersion();
    }

    PlanExecutor* exec = cursorPin->getExecutor();
    exec->reattachToOperationContext(opCtx);
    exec->restoreState();

    auto planSummary = Explain::getPlanSummary(exec);
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        curOp.setPlanSummary_inlock(planSummary);

        // Ensure that the original query object is available in the slow query log, profiler and
        // currentOp. Upconvert _query to resemble a getMore command, and set the original command
        // or upconverted legacy query in the originatingCommand field.
        curOp.setOpDescription_inlock(upconvertGetMoreEntry(nss, cursorid, ntoreturn));
        curOp.setOriginatingCommand_inlock(cursorPin->getOriginatingCommandObj());
        // Update the generic cursor in curOp.
        curOp.setGenericCursor_inlock(cursorPin->toGenericCursor());
    }

    PlanExecutor::ExecState state;

    // We report keysExamined and docsExamined to OpDebug for a given getMore operation. To obtain
    // these values we need to take a diff of the pre-execution and post-execution metrics, as they
    // accumulate over the course of a cursor's lifetime.
    PlanSummaryStats preExecutionStats;
    Explain::getSummaryStats(*exec, &preExecutionStats);
    if (MONGO_unlikely(waitWithPinnedCursorDuringGetMoreBatch.shouldFail())) {
        CurOpFailpointHelpers::waitWhileFailPointEnabled(&waitWithPinnedCursorDuringGetMoreBatch,
                                                         opCtx,
                                                         "waitWithPinnedCursorDuringGetMoreBatch",
                                                         nullptr);
    }

    generateBatch(ntoreturn, cursorPin.getCursor(), &bb, &numResults, &state);

    // If this is an await data cursor, and we hit EOF without generating any results, then we block
    // waiting for new data to arrive.
    if (cursorPin->isAwaitData() && state == PlanExecutor::IS_EOF && numResults == 0) {
        // Save the PlanExecutor and drop our locks.
        exec->saveState();
        readLock.reset();

        // Block waiting for data for up to 1 second. Time spent blocking is not counted towards the
        // total operation latency.
        curOp.pauseTimer();
        Seconds timeout(1);
        notifier->waitUntil(notifierVersion,
                            opCtx->getServiceContext()->getPreciseClockSource()->now() + timeout);
        notifier.reset();
        curOp.resumeTimer();

        // Reacquiring locks.
        readLock.emplace(opCtx, nss);
        exec->restoreState();

        // We woke up because either the timed_wait expired, or there was more data. Either way,
        // attempt to generate another batch of results.
        generateBatch(ntoreturn, cursorPin.getCursor(), &bb, &numResults, &state);
    }

    PlanSummaryStats postExecutionStats;
    Explain::getSummaryStats(*exec, &postExecutionStats);
    postExecutionStats.totalKeysExamined -= preExecutionStats.totalKeysExamined;
    postExecutionStats.totalDocsExamined -= preExecutionStats.totalDocsExamined;
    curOp.debug().setPlanSummaryMetrics(postExecutionStats);

    // We do not report 'execStats' for aggregation or other cursors with the 'kLocksInternally'
    // policy, both in the original request and subsequent getMore. It would be useful to have this
    // info for an aggregation, but the source PlanExecutor could be destroyed before we know if we
    // need 'execStats' and we do not want to generate the stats eagerly for all operations due to
    // cost.
    if (cursorPin->lockPolicy() != ClientCursorParams::LockPolicy::kLocksInternally &&
        curOp.shouldDBProfile()) {
        BSONObjBuilder execStatsBob;
        Explain::getWinningPlanStats(exec, &execStatsBob);
        curOp.debug().execStats = execStatsBob.obj();
    }

    // Our two possible ClientCursorPin cleanup paths are:
    // 1) If the cursor is not going to be saved, we call deleteUnderlying() on the pin.
    // 2) If the cursor is going to be saved, we simply let the pin go out of scope. In this case,
    // the pin's destructor will be invoked, which will call release() on the pin.  Because our
    // ClientCursorPin is declared after our lock is declared, this will happen under the lock if
    // any locking was necessary.
    if (!shouldSaveCursorGetMore(state, exec, cursorPin->isTailable())) {
        // cc is now invalid, as is the executor
        cursorid = 0;
        curOp.debug().cursorExhausted = true;

        LOG(5) << "getMore NOT saving client cursor, ended with state "
               << PlanExecutor::statestr(state);
    } else {
        cursorFreer.dismiss();
        // Continue caching the ClientCursor.
        cursorPin->incNReturnedSoFar(numResults);
        cursorPin->incNBatches();
        exec->saveState();
        exec->detachFromOperationContext();
        LOG(5) << "getMore saving client cursor ended with state " << PlanExecutor::statestr(state);

        *exhaust = cursorPin->queryOptions() & QueryOption_Exhaust;

        // We assume that cursors created through a DBDirectClient are always used from their
        // original OperationContext, so we do not need to move time to and from the cursor.
        if (!opCtx->getClient()->isInDirectClient()) {
            // If the getmore had a time limit, remaining time is "rolled over" back to the cursor
            // (for use by future getmore ops).
            cursorPin->setLeftoverMaxTimeMicros(opCtx->getRemainingMaxTimeMicros());
        }
    }

    // We're about to unpin or delete the cursor as the ClientCursorPin goes out of scope.
    // If the 'waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch' failpoint is active, we
    // set the 'msg' field of this operation's CurOp to signal that we've hit this point and
    // then spin until the failpoint is released.
    if (MONGO_unlikely(waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch.shouldFail())) {
        CurOpFailpointHelpers::waitWhileFailPointEnabled(
            &waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch,
            opCtx,
            "waitBeforeUnpinningOrDeletingCursorAfterGetMoreBatch",
            dropAndReaquireReadLock);
    }

    QueryResult::View qr = bb.buf();
    qr.msgdata().setLen(bb.len());
    qr.msgdata().setOperation(opReply);
    qr.setResultFlags(resultFlags);
    qr.setCursorId(cursorid);
    qr.setStartingFrom(startingResult);
    qr.setNReturned(numResults);
    LOG(5) << "getMore returned " << numResults << " results\n";
    return Message(bb.release());
}

std::string runQuery(OperationContext* opCtx,
                     QueryMessage& q,
                     const NamespaceString& nss,
                     Message& result) {
    CurOp& curOp = *CurOp::get(opCtx);
    curOp.ensureStarted();

    uassert(ErrorCodes::InvalidNamespace,
            str::stream() << "Invalid ns [" << nss.ns() << "]",
            nss.isValid());
    invariant(!nss.isCommand());

    // Set CurOp information.
    const auto upconvertedQuery = upconvertQueryEntry(q.query, nss, q.ntoreturn, q.ntoskip);

    // Extract the 'comment' parameter from the upconverted query, if it exists.
    if (auto commentField = upconvertedQuery["comment"]) {
        opCtx->setComment(commentField.wrap());
    }

    beginQueryOp(opCtx, nss, upconvertedQuery, q.ntoreturn, q.ntoskip);

    // Parse the qm into a CanonicalQuery.
    const boost::intrusive_ptr<ExpressionContext> expCtx;
    auto cq = uassertStatusOKWithContext(
        CanonicalQuery::canonicalize(opCtx,
                                     q,
                                     expCtx,
                                     ExtensionsCallbackReal(opCtx, &nss),
                                     MatchExpressionParser::kAllowAllSpecialFeatures),
        "Can't canonicalize query");
    invariant(cq.get());

    LOG(5) << "Running query:\n" << redact(cq->toString());
    LOG(2) << "Running query: " << redact(cq->toStringShort());

    // Parse, canonicalize, plan, transcribe, and get a plan executor.
    AutoGetCollectionForReadCommand ctx(opCtx, nss, AutoGetCollection::ViewMode::kViewsForbidden);
    Collection* const collection = ctx.getCollection();

    {
        const QueryRequest& qr = cq->getQueryRequest();

        // Allow the query to run on secondaries if the read preference permits it. If no read
        // preference was specified, allow the query to run iff slaveOk has been set.
        const bool slaveOK = qr.hasReadPref()
            ? uassertStatusOK(ReadPreferenceSetting::fromContainingBSON(q.query))
                  .canRunOnSecondary()
            : qr.isSlaveOk();
        uassertStatusOK(
            repl::ReplicationCoordinator::get(opCtx)->checkCanServeReadsFor(opCtx, nss, slaveOK));
    }

    // Get the execution plan for the query.
    auto exec = uassertStatusOK(getExecutorLegacyFind(opCtx, collection, std::move(cq)));

    const QueryRequest& qr = exec->getCanonicalQuery()->getQueryRequest();

    // If it's actually an explain, do the explain and return rather than falling through
    // to the normal query execution loop.
    if (qr.isExplain()) {
        BufBuilder bb;
        bb.skip(sizeof(QueryResult::Value));

        BSONObjBuilder explainBob;
        Explain::explainStages(exec.get(),
                               collection,
                               ExplainOptions::Verbosity::kExecAllPlans,
                               BSONObj(),
                               &explainBob);

        // Add the resulting object to the return buffer.
        BSONObj explainObj = explainBob.obj();
        bb.appendBuf((void*)explainObj.objdata(), explainObj.objsize());

        // Set query result fields.
        QueryResult::View qr = bb.buf();
        qr.setResultFlagsToOk();
        qr.msgdata().setLen(bb.len());
        curOp.debug().responseLength = bb.len();
        qr.msgdata().setOperation(opReply);
        qr.setCursorId(0);
        qr.setStartingFrom(0);
        qr.setNReturned(1);
        result.setData(bb.release());
        return "";
    }

    // Handle query option $maxTimeMS (not used with commands).
    if (qr.getMaxTimeMS() > 0) {
        uassert(40116,
                "Illegal attempt to set operation deadline within DBDirectClient",
                !opCtx->getClient()->isInDirectClient());
        opCtx->setDeadlineAfterNowBy(Milliseconds{qr.getMaxTimeMS()}, ErrorCodes::MaxTimeMSExpired);
    }
    opCtx->checkForInterrupt();  // May trigger maxTimeAlwaysTimeOut fail point.

    FindCommon::waitInFindBeforeMakingBatch(opCtx, *exec->getCanonicalQuery());

    // Run the query.
    // bb is used to hold query results
    // this buffer should contain either requested documents per query or
    // explain information, but not both
    BufBuilder bb(FindCommon::kInitReplyBufferSize);
    bb.skip(sizeof(QueryResult::Value));

    // How many results have we obtained from the executor?
    int numResults = 0;

    BSONObj obj;
    PlanExecutor::ExecState state;

    // Get summary info about which plan the executor is using.
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        curOp.setPlanSummary_inlock(Explain::getPlanSummary(exec.get()));
    }

    Document doc;
    while (PlanExecutor::ADVANCED == (state = exec->getNext(&doc, nullptr))) {
        obj = doc.toBson();

        // If we can't fit this result inside the current batch, then we stash it for later.
        if (!FindCommon::haveSpaceForNext(obj, numResults, bb.len())) {
            exec->enqueue(obj);
            break;
        }

        // Add result to output buffer.
        bb.appendBuf((void*)obj.objdata(), obj.objsize());

        // Count the result.
        ++numResults;

        if (FindCommon::enoughForFirstBatch(qr, numResults)) {
            LOG(5) << "Enough for first batch, wantMore=" << qr.wantMore()
                   << " ntoreturn=" << qr.getNToReturn().value_or(0)
                   << " numResults=" << numResults;
            break;
        }
    }

    // Caller expects exceptions thrown in certain cases.
    if (PlanExecutor::FAILURE == state) {
        error() << "Plan executor error during find: " << PlanExecutor::statestr(state)
                << ", stats: " << redact(Explain::getWinningPlanStats(exec.get()));
        uassertStatusOKWithContext(WorkingSetCommon::getMemberObjectStatus(doc),
                                   "Executor error during OP_QUERY find");
        MONGO_UNREACHABLE;
    }

    // Fill out CurOp based on query results. If we have a cursorid, we will fill out CurOp with
    // this cursorid later.
    long long ccId = 0;

    if (shouldSaveCursor(opCtx, collection, state, exec.get())) {
        // We won't use the executor until it's getMore'd.
        exec->saveState();
        exec->detachFromOperationContext();

        const auto& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
        // Allocate a new ClientCursor and register it with the cursor manager.
        ClientCursorPin pinnedCursor = CursorManager::get(opCtx)->registerCursor(
            opCtx,
            {
                std::move(exec),
                nss,
                AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserNames(),
                opCtx->getWriteConcern(),
                readConcernArgs,
                upconvertedQuery,
                ClientCursorParams::LockPolicy::kLockExternally,
                {Privilege(ResourcePattern::forExactNamespace(nss), ActionType::find)},
                false  // needsMerge always 'false' for find().
            });
        ccId = pinnedCursor.getCursor()->cursorid();

        LOG(5) << "caching executor with cursorid " << ccId << " after returning " << numResults
               << " results";

        // TODO document
        if (qr.isExhaust()) {
            curOp.debug().exhaust = true;
        }

        pinnedCursor.getCursor()->setNReturnedSoFar(numResults);
        pinnedCursor.getCursor()->incNBatches();

        // We assume that cursors created through a DBDirectClient are always used from their
        // original OperationContext, so we do not need to move time to and from the cursor.
        if (!opCtx->getClient()->isInDirectClient()) {
            // If the query had a time limit, remaining time is "rolled over" to the cursor (for
            // use by future getmore ops).
            pinnedCursor.getCursor()->setLeftoverMaxTimeMicros(opCtx->getRemainingMaxTimeMicros());
        }

        endQueryOp(opCtx, collection, *pinnedCursor.getCursor()->getExecutor(), numResults, ccId);
    } else {
        LOG(5) << "Not caching executor but returning " << numResults << " results.";
        endQueryOp(opCtx, collection, *exec, numResults, ccId);
    }

    // Fill out the output buffer's header.
    QueryResult::View queryResultView = bb.buf();
    queryResultView.setCursorId(ccId);
    queryResultView.setResultFlagsToOk();
    queryResultView.msgdata().setLen(bb.len());
    queryResultView.msgdata().setOperation(opReply);
    queryResultView.setStartingFrom(0);
    queryResultView.setNReturned(numResults);

    // Add the results from the query into the output buffer.
    result.setData(bb.release());

    // curOp.debug().exhaust is set above.
    return curOp.debug().exhaust ? nss.ns() : "";
}

}  // namespace mongo
