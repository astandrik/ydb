#include "aggregator_impl.h"

#include <ydb/core/statistics/database/database.h>
#include <ydb/core/statistics/service/service.h>

#include <ydb/core/engine/minikql/flat_local_tx_factory.h>
#include <ydb/core/protos/feature_flags.pb.h>
#include <ydb/core/tx/datashard/datashard.h>
#include <ydb/core/tx/scheme_cache/scheme_cache.h>

#include <library/cpp/monlib/service/pages/templates.h>

namespace NKikimr::NStat {

TStatisticsAggregator::TStatisticsAggregator(const NActors::TActorId& tablet, TTabletStorageInfo* info, bool forTests)
    : TActor(&TThis::StateInit)
    , TTabletExecutedFlat(info, tablet, new NMiniKQL::TMiniKQLFactory)
{
    PropagateInterval = forTests ? TDuration::Seconds(5) : TDuration::Minutes(3);
    PropagateTimeout = forTests ? TDuration::Seconds(3) : TDuration::Minutes(2);

    auto seed = std::random_device{}();
    RandomGenerator.seed(seed);
}

void TStatisticsAggregator::OnDetach(const TActorContext& ctx) {
    Die(ctx);
}

void TStatisticsAggregator::OnTabletDead(TEvTablet::TEvTabletDead::TPtr&, const TActorContext& ctx) {
    Die(ctx);
}

void TStatisticsAggregator::OnActivateExecutor(const TActorContext& ctx) {
    SA_LOG_I("[" << TabletID() << "] OnActivateExecutor");

    Execute(CreateTxInitSchema(), ctx);
}

void TStatisticsAggregator::DefaultSignalTabletActive(const TActorContext& ctx) {
    Y_UNUSED(ctx);
}

void TStatisticsAggregator::SubscribeForConfigChanges(const TActorContext& ctx) {
    ui32 configKind = (ui32)NKikimrConsole::TConfigItem::FeatureFlagsItem;
    ctx.Send(NConsole::MakeConfigsDispatcherID(ctx.SelfID.NodeId()),
        new NConsole::TEvConfigsDispatcher::TEvSetConfigSubscriptionRequest({configKind}));
}

void TStatisticsAggregator::HandleConfig(NConsole::TEvConfigsDispatcher::TEvSetConfigSubscriptionResponse::TPtr&) {
    SA_LOG_I("[" << TabletID() << "] Subscribed for config changes");
}

void TStatisticsAggregator::HandleConfig(NConsole::TEvConsole::TEvConfigNotificationRequest::TPtr& ev) {
    const auto& record = ev->Get()->Record;
    const auto& config = record.GetConfig();
    if (config.HasFeatureFlags()) {
        const auto& featureFlags = config.GetFeatureFlags();
        EnableStatistics = featureFlags.GetEnableStatistics();

        bool enableColumnStatisticsOld = EnableColumnStatistics;
        EnableColumnStatistics = featureFlags.GetEnableColumnStatistics();
        if (!enableColumnStatisticsOld && EnableColumnStatistics) {
            InitializeStatisticsTable();
        }
    }
    auto response = std::make_unique<NConsole::TEvConsole::TEvConfigNotificationResponse>(record);
    Send(ev->Sender, response.release(), 0, ev->Cookie);
}

void TStatisticsAggregator::Handle(TEvTabletPipe::TEvServerConnected::TPtr &ev) {
    auto pipeServerId = ev->Get()->ServerId;

    SA_LOG_D("[" << TabletID() << "] EvServerConnected"
        << ", pipe server id = " << pipeServerId);
}

void TStatisticsAggregator::Handle(TEvTabletPipe::TEvServerDisconnected::TPtr &ev) {
    auto pipeServerId = ev->Get()->ServerId;

    SA_LOG_D("[" << TabletID() << "] EvServerDisconnected"
        << ", pipe server id = " << pipeServerId);

    auto itNodeServer = NodePipes.find(pipeServerId);
    if (itNodeServer != NodePipes.end()) {
        auto nodeId = itNodeServer->second;
        auto itNode = Nodes.find(nodeId);
        if (itNode != Nodes.end()) {
            --itNode->second;
            if (itNode->second == 0) {
                Nodes.erase(itNode);
            }
        }
        NodePipes.erase(itNodeServer);
        return;
    }

    auto itShardServer = SchemeShardPipes.find(pipeServerId);
    if (itShardServer != SchemeShardPipes.end()) {
        auto ssId = itShardServer->second;
        auto itShard = SchemeShards.find(ssId);
        if (itShard != SchemeShards.end()) {
            --itShard->second;
            if (itShard->second == 0) {
                SchemeShards.erase(itShard);
            }
        }
        SchemeShardPipes.erase(itShardServer);
        return;
    }
}

void TStatisticsAggregator::Handle(TEvStatistics::TEvConnectNode::TPtr& ev) {
    const auto& record = ev->Get()->Record;
    const TNodeId nodeId = record.GetNodeId();
    auto pipeServerId = ev->Recipient;

    SA_LOG_D("[" << TabletID() << "] EvConnectNode"
        << ", pipe server id = " << pipeServerId
        << ", node id = " << nodeId
        << ", have schemeshards count = " << record.HaveSchemeShardsSize()
        << ", need schemeshards count = " << record.NeedSchemeShardsSize());

    if (NodePipes.find(pipeServerId) == NodePipes.end()) {
        NodePipes[pipeServerId] = nodeId;
        ++Nodes[nodeId];
    }

    for (const auto& ssEntry : record.GetHaveSchemeShards()) {
        RequestedSchemeShards.insert(ssEntry.GetSchemeShardId());
    }

    if (!EnableStatistics) {
        auto disabled = std::make_unique<TEvStatistics::TEvStatisticsIsDisabled>();
        Send(NStat::MakeStatServiceID(nodeId), disabled.release());
        return;
    }

    if (!record.NeedSchemeShardsSize()) {
        return;
    }

    std::vector<TSSId> ssIds;
    ssIds.reserve(record.NeedSchemeShardsSize());
    for (const auto& ssId : record.GetNeedSchemeShards()) {
        ssIds.push_back(ssId);
        RequestedSchemeShards.insert(ssId);
    }

    ProcessRequests(nodeId, ssIds);
}

void TStatisticsAggregator::Handle(TEvStatistics::TEvRequestStats::TPtr& ev) {
    const auto& record = ev->Get()->Record;
    const auto nodeId = record.GetNodeId();

    SA_LOG_D("[" << TabletID() << "] EvRequestStats"
        << ", node id = " << nodeId
        << ", schemeshard count = " << record.NeedSchemeShardsSize()
        << ", urgent = " << record.GetUrgent());

    if (!EnableStatistics) {
        auto disabled = std::make_unique<TEvStatistics::TEvStatisticsIsDisabled>();
        Send(NStat::MakeStatServiceID(nodeId), disabled.release());
        return;
    }

    for (const auto& ssId : record.GetNeedSchemeShards()) {
        RequestedSchemeShards.insert(ssId);
    }

    if (record.GetUrgent()) {
        PendingRequests.push(std::move(ev));
        if (!ProcessUrgentInFlight) {
            Send(SelfId(), new TEvPrivate::TEvProcessUrgent());
            ProcessUrgentInFlight = true;
        }
        return;
    }

    std::vector<TSSId> ssIds;
    ssIds.reserve(record.NeedSchemeShardsSize());
    for (const auto& ssId : record.GetNeedSchemeShards()) {
        ssIds.push_back(ssId);
    }

    ProcessRequests(nodeId, ssIds);
}

void TStatisticsAggregator::Handle(TEvStatistics::TEvConnectSchemeShard::TPtr& ev) {
    const auto& record = ev->Get()->Record;
    const TSSId schemeShardId = record.GetSchemeShardId();
    auto pipeServerId = ev->Recipient;

    if (SchemeShardPipes.find(pipeServerId) == SchemeShardPipes.end()) {
        SchemeShardPipes[pipeServerId] = schemeShardId;
        ++SchemeShards[schemeShardId];
    }

    SA_LOG_D("[" << TabletID() << "] EvConnectSchemeShard"
        << ", pipe server id = " << pipeServerId
        << ", schemeshard id = " << schemeShardId);
}

void TStatisticsAggregator::Handle(TEvPrivate::TEvFastPropagateCheck::TPtr&) {
    SA_LOG_D("[" << TabletID() << "] EvFastPropagateCheck");

    if (!EnableStatistics) {
        return;
    }

    PropagateFastStatistics();

    FastCheckInFlight = false;
    FastCounter = StatsOptimizeFirstNodesCount;
    FastNodes.clear();
    FastSchemeShards.clear();
}

void TStatisticsAggregator::Handle(TEvPrivate::TEvPropagate::TPtr&) {
    SA_LOG_D("[" << TabletID() << "] EvPropagate");

    if (EnableStatistics) {
        PropagateStatistics();
    }

    Schedule(PropagateInterval, new TEvPrivate::TEvPropagate());
}

void TStatisticsAggregator::Handle(TEvStatistics::TEvPropagateStatisticsResponse::TPtr&) {
    if (!PropagationInFlight) {
        return;
    }
    if (LastSSIndex < PropagationSchemeShards.size()) {
        LastSSIndex = PropagatePart(PropagationNodes, PropagationSchemeShards, LastSSIndex, true);
    } else {
        PropagationInFlight = false;
        PropagationNodes.clear();
        PropagationSchemeShards.clear();
        LastSSIndex = 0;
    }
}

void TStatisticsAggregator::Handle(TEvPrivate::TEvProcessUrgent::TPtr&) {
    SA_LOG_D("[" << TabletID() << "] EvProcessUrgent");

    ProcessUrgentInFlight = false;

    if (PendingRequests.empty()) {
        return;
    }

    TEvStatistics::TEvRequestStats::TPtr ev = std::move(PendingRequests.front());
    PendingRequests.pop();

    if (!PendingRequests.empty()) {
        Send(SelfId(), new TEvPrivate::TEvProcessUrgent());
        ProcessUrgentInFlight = true;
    }

    auto record = ev->Get()->Record;
    const auto nodeId = record.GetNodeId();

    std::vector<TSSId> ssIds;
    ssIds.reserve(record.NeedSchemeShardsSize());
    for (const auto& ssId : record.GetNeedSchemeShards()) {
        ssIds.push_back(ssId);
    }

    SendStatisticsToNode(nodeId, ssIds);
}

void TStatisticsAggregator::Handle(TEvPrivate::TEvPropagateTimeout::TPtr&) {
    SA_LOG_D("[" << TabletID() << "] EvPropagateTimeout");

    if (PropagationInFlight) {
        PropagationInFlight = false;
        PropagationNodes.clear();
        PropagationSchemeShards.clear();
        LastSSIndex = 0;
    }
}

void TStatisticsAggregator::ProcessRequests(TNodeId nodeId, const std::vector<TSSId>& ssIds) {
    if (FastCounter > 0) {
        --FastCounter;
        SendStatisticsToNode(nodeId, ssIds);
    } else {
        FastNodes.insert(nodeId);
        for (const auto& ssId : ssIds) {
            FastSchemeShards.insert(ssId);
        }
    }
    if (!FastCheckInFlight) {
        Schedule(FastCheckInterval, new TEvPrivate::TEvFastPropagateCheck());
        FastCheckInFlight = true;
    }
}

void TStatisticsAggregator::SendStatisticsToNode(TNodeId nodeId, const std::vector<TSSId>& ssIds) {
    SA_LOG_D("[" << TabletID() << "] SendStatisticsToNode()"
        << ", node id = " << nodeId
        << ", schemeshard count = " << ssIds.size());

    std::vector<TNodeId> nodeIds;
    nodeIds.push_back(nodeId);

    PropagatePart(nodeIds, ssIds, 0, false);
}

void TStatisticsAggregator::PropagateStatistics() {
    SA_LOG_D("[" << TabletID() << "] PropagateStatistics()"
        << ", node count = " << Nodes.size()
        << ", schemeshard count = " << RequestedSchemeShards.size());

    if (Nodes.empty() || RequestedSchemeShards.empty()) {
        return;
    }

    std::vector<TNodeId> nodeIds;
    nodeIds.reserve(Nodes.size());
    for (const auto& [nodeId, _] : Nodes) {
        nodeIds.push_back(nodeId);
    }
    std::shuffle(std::begin(nodeIds), std::end(nodeIds), RandomGenerator);

    std::vector<TSSId> ssIds;
    ssIds.reserve(RequestedSchemeShards.size());
    for (const auto& ssId : RequestedSchemeShards) {
        ssIds.push_back(ssId);
    }

    Schedule(PropagateTimeout, new TEvPrivate::TEvPropagateTimeout);

    PropagationInFlight = true;
    PropagationNodes = std::move(nodeIds);
    PropagationSchemeShards = std::move(ssIds);

    LastSSIndex = PropagatePart(PropagationNodes, PropagationSchemeShards, 0, true);
}

void TStatisticsAggregator::PropagateFastStatistics() {
    SA_LOG_D("[" << TabletID() << "] PropagateFastStatistics()"
        << ", node count = " << FastNodes.size()
        << ", schemeshard count = " << FastSchemeShards.size());

    if (FastNodes.empty() || FastSchemeShards.empty()) {
        return;
    }

    std::vector<TNodeId> nodeIds;
    nodeIds.reserve(FastNodes.size());
    for (const auto& nodeId : FastNodes) {
        nodeIds.push_back(nodeId);
    }
    std::shuffle(std::begin(nodeIds), std::end(nodeIds), RandomGenerator);

    std::vector<TSSId> ssIds;
    ssIds.reserve(FastSchemeShards.size());
    for (const auto& ssId : FastSchemeShards) {
        ssIds.push_back(ssId);
    }

    PropagatePart(nodeIds, ssIds, 0, false);
}

size_t TStatisticsAggregator::PropagatePart(const std::vector<TNodeId>& nodeIds, const std::vector<TSSId>& ssIds,
    size_t lastSSIndex, bool useSizeLimit)
{
    auto propagate = std::make_unique<TEvStatistics::TEvPropagateStatistics>();
    auto* record = propagate->MutableRecord();

    TNodeId leadingNodeId = nodeIds[0];
    record->MutableNodeIds()->Reserve(nodeIds.size() - 1);
    for (size_t i = 1; i < nodeIds.size(); ++i) {
        record->AddNodeIds(nodeIds[i]);
    }

    size_t sizeLimit = useSizeLimit ? StatsSizeLimitBytes : std::numeric_limits<size_t>::max();
    size_t index = lastSSIndex;
    for (size_t size = 0; index < ssIds.size() && size < sizeLimit; ++index) {
        auto ssId = ssIds[index];
        auto* entry = record->AddEntries();
        entry->SetSchemeShardId(ssId);
        auto itStats = BaseStats.find(ssId);
        if (itStats != BaseStats.end()) {
            entry->SetStats(itStats->second);
            size += itStats->second.size();
        } else {
            entry->SetStats(TString()); // stats are not sent from SS yet
        }
    }

    Send(NStat::MakeStatServiceID(leadingNodeId), propagate.release());

    return index;
}

void TStatisticsAggregator::Handle(TEvPipeCache::TEvDeliveryProblem::TPtr& ev) {
    if (!ScanTableId.PathId) {
        return;
    }
    auto tabletId = ev->Get()->TabletId;
    if (IsColumnTable) {
        if (tabletId == HiveId) {
            Schedule(HiveRetryInterval, new TEvPrivate::TEvRequestDistribution);
        } else {
            SA_LOG_CRIT("[" << TabletID() << "] TEvDeliveryProblem with unexpected tablet " << tabletId);
        }
    } else {
        if (ShardRanges.empty()) {
            return;
        }
        auto& range = ShardRanges.front();
        if (tabletId != range.DataShardId) {
            return;
        }
        Resolve();
    }
}

void TStatisticsAggregator::Handle(TEvStatistics::TEvStatTableCreationResponse::TPtr&) {
    IsStatisticsTableCreated = true;
    if (PendingSaveStatistics) {
        PendingSaveStatistics = false;
        SaveStatisticsToTable();
    }
    if (PendingDeleteStatistics) {
        PendingDeleteStatistics = false;
        DeleteStatisticsFromTable();
    }
}

void TStatisticsAggregator::Handle(TEvStatistics::TEvAnalyzeStatus::TPtr& ev) {
    auto& inRecord = ev->Get()->Record;
    auto pathId = PathIdFromPathId(inRecord.GetPathId());

    auto response = std::make_unique<TEvStatistics::TEvAnalyzeStatusResponse>();
    auto& outRecord = response->Record;

    if (ScanTableId.PathId == pathId) {
        outRecord.SetStatus(NKikimrStat::TEvAnalyzeStatusResponse::STATUS_IN_PROGRESS);
    } else {
        auto it = ScanOperationsByPathId.find(pathId);
        if (it != ScanOperationsByPathId.end()) {
            outRecord.SetStatus(NKikimrStat::TEvAnalyzeStatusResponse::STATUS_ENQUEUED);
        } else {
            outRecord.SetStatus(NKikimrStat::TEvAnalyzeStatusResponse::STATUS_NO_OPERATION);
        }
    }
    Send(ev->Sender, response.release(), 0, ev->Cookie);
}

void TStatisticsAggregator::Handle(TEvPrivate::TEvResolve::TPtr&) {
    Resolve();
}

void TStatisticsAggregator::Handle(TEvPrivate::TEvRequestDistribution::TPtr&) {
    ++HiveRequestRound;

    auto reqDistribution = std::make_unique<TEvHive::TEvRequestTabletDistribution>();
    reqDistribution->Record.MutableTabletIds()->Reserve(TabletsForReqDistribution.size());
    for (auto& tablet : TabletsForReqDistribution) {
        reqDistribution->Record.AddTabletIds(tablet);
    }

    Send(MakePipePerNodeCacheID(false),
        new TEvPipeCache::TEvForward(reqDistribution.release(), HiveId, true));
}

void TStatisticsAggregator::Handle(TEvStatistics::TEvAggregateKeepAlive::TPtr& ev) {
    auto ack = std::make_unique<TEvStatistics::TEvAggregateKeepAliveAck>();
    ack->Record.SetRound(ev->Get()->Record.GetRound());
    Send(ev->Sender, ack.release());
    Schedule(KeepAliveTimeout, new TEvPrivate::TEvAckTimeout(++KeepAliveSeqNo));
}

void TStatisticsAggregator::InitializeStatisticsTable() {
    if (!EnableColumnStatistics) {
        return;
    }
    Register(CreateStatisticsTableCreator(std::make_unique<TEvStatistics::TEvStatTableCreationResponse>()));
}

void TStatisticsAggregator::Navigate() {
    using TNavigate = NSchemeCache::TSchemeCacheNavigate;
    TNavigate::TEntry entry;
    entry.TableId = ScanTableId;
    entry.RequestType = TNavigate::TEntry::ERequestType::ByTableId;
    entry.Operation = TNavigate::OpTable;

    auto request = std::make_unique<TNavigate>();
    request->ResultSet.emplace_back(entry);

    Send(MakeSchemeCacheID(), new TEvTxProxySchemeCache::TEvNavigateKeySet(request.release()));
}

void TStatisticsAggregator::Resolve() {
    ++ResolveRound;

    TVector<TCell> plusInf;
    TTableRange range(StartKey.GetCells(), true, plusInf, true, false);
    auto keyDesc = MakeHolder<TKeyDesc>(
        ScanTableId, range, TKeyDesc::ERowOperation::Read, KeyColumnTypes, Columns);

    auto request = std::make_unique<NSchemeCache::TSchemeCacheRequest>();
    request->ResultSet.emplace_back(std::move(keyDesc));

    Send(MakeSchemeCacheID(), new TEvTxProxySchemeCache::TEvResolveKeySet(request.release()));
}

void TStatisticsAggregator::NextRange() {
    if (ShardRanges.empty()) {
        SaveStatisticsToTable();
        return;
    }

    auto& range = ShardRanges.front();
    auto request = std::make_unique<NStat::TEvStatistics::TEvStatisticsRequest>();
    auto& record = request->Record;
    auto* path = record.MutableTable()->MutablePathId();
    path->SetOwnerId(ScanTableId.PathId.OwnerId);
    path->SetLocalId(ScanTableId.PathId.LocalPathId);
    record.SetStartKey(StartKey.GetBuffer());

    Send(MakePipePerNodeCacheID(false),
        new TEvPipeCache::TEvForward(request.release(), range.DataShardId, true),
        IEventHandle::FlagTrackDelivery);
}

void TStatisticsAggregator::SaveStatisticsToTable() {
    if (!IsStatisticsTableCreated) {
        PendingSaveStatistics = true;
        return;
    }

    PendingSaveStatistics = false;

    std::vector<ui32> columnTags;
    std::vector<TString> data;
    auto count = CountMinSketches.size();
    if (count == 0) {
        return;
    }
    columnTags.reserve(count);
    data.reserve(count);

    for (auto& [tag, sketch] : CountMinSketches) {
        auto itColumnName = ColumnNames.find(tag);
        if (itColumnName == ColumnNames.end()) {
            continue;
        }
        columnTags.push_back(tag);
        TString strSketch(sketch->AsStringBuf());
        data.push_back(strSketch);
    }

    Register(CreateSaveStatisticsQuery(ScanTableId.PathId, EStatType::COUNT_MIN_SKETCH,
        std::move(columnTags), std::move(data)));
}

void TStatisticsAggregator::DeleteStatisticsFromTable() {
    if (!IsStatisticsTableCreated) {
        PendingDeleteStatistics = true;
        return;
    }

    PendingDeleteStatistics = false;

    Register(CreateDeleteStatisticsQuery(ScanTableId.PathId));
}

void TStatisticsAggregator::ScheduleNextScan(NIceDb::TNiceDb& db) {
    if (!ScanOperations.Empty()) {
        auto* operation = ScanOperations.Front();
        ReplyToActorIds.swap(operation->ReplyToActorIds);

        bool doStartScan = true;
        bool isColumnTable = false;
        auto pathId = operation->PathId;
        auto itPath = ScanTables.find(pathId);
        if (itPath != ScanTables.end()) {
            isColumnTable = itPath->second.IsColumnTable;
        } else {
            doStartScan = false;
        }
        if (doStartScan) {
            StartScan(db, pathId, isColumnTable);
        }
        db.Table<Schema::ScanOperations>().Key(operation->OperationId).Delete();
        ScanOperations.PopFront();
        ScanOperationsByPathId.erase(pathId);
        return;
    }
    if (ScanTablesByTime.Empty()) {
        return;
    }
    auto* topTable = ScanTablesByTime.Top();
    if (TInstant::Now() < topTable->LastUpdateTime + ScanIntervalTime) {
        return;
    }
    bool isColumnTable = false;
    auto itPath = ScanTables.find(topTable->PathId);
    if (itPath != ScanTables.end()) {
        isColumnTable = itPath->second.IsColumnTable;
    } else {
        return;
    }
    StartScan(db, topTable->PathId, isColumnTable);
}

void TStatisticsAggregator::StartScan(NIceDb::TNiceDb& db, TPathId pathId, bool isColumnTable) {
    ScanTableId.PathId = pathId;
    ScanStartTime = TInstant::Now();
    IsColumnTable = isColumnTable;
    PersistCurrentScan(db);

    StartKey = TSerializedCellVec();
    PersistStartKey(db);

    Navigate();
}

void TStatisticsAggregator::FinishScan(NIceDb::TNiceDb& db) {
    auto pathId = ScanTableId.PathId;

    auto pathIt = ScanTables.find(pathId);
    if (pathIt != ScanTables.end()) {
        auto& scanTable = pathIt->second;
        scanTable.LastUpdateTime = ScanStartTime;
        db.Table<Schema::ScanTables>().Key(pathId.OwnerId, pathId.LocalPathId).Update(
            NIceDb::TUpdate<Schema::ScanTables::LastUpdateTime>(ScanStartTime.MicroSeconds()));

        if (ScanTablesByTime.Has(&scanTable)) {
            ScanTablesByTime.Update(&scanTable);
        }
    }

    ResetScanState(db);
}

void TStatisticsAggregator::PersistSysParam(NIceDb::TNiceDb& db, ui64 id, const TString& value) {
    db.Table<Schema::SysParams>().Key(id).Update(
        NIceDb::TUpdate<Schema::SysParams::Value>(value));
}

void TStatisticsAggregator::PersistCurrentScan(NIceDb::TNiceDb& db) {
    PersistSysParam(db, Schema::SysParam_ScanTableOwnerId, ToString(ScanTableId.PathId.OwnerId));
    PersistSysParam(db, Schema::SysParam_ScanTableLocalPathId, ToString(ScanTableId.PathId.LocalPathId));
    PersistSysParam(db, Schema::SysParam_ScanStartTime, ToString(ScanStartTime.MicroSeconds()));
    PersistSysParam(db, Schema::SysParam_IsColumnTable, ToString(IsColumnTable));
}

void TStatisticsAggregator::PersistStartKey(NIceDb::TNiceDb& db) {
    PersistSysParam(db, Schema::SysParam_StartKey, StartKey.GetBuffer());
}

void TStatisticsAggregator::PersistLastScanOperationId(NIceDb::TNiceDb& db) {
    PersistSysParam(db, Schema::SysParam_LastScanOperationId, ToString(LastScanOperationId));
}

void TStatisticsAggregator::PersistGlobalTraversalRound(NIceDb::TNiceDb& db) {
    PersistSysParam(db, Schema::SysParam_GlobalTraversalRound, ToString(GlobalTraversalRound));
}

void TStatisticsAggregator::ResetScanState(NIceDb::TNiceDb& db) {
    ScanTableId.PathId = TPathId();
    ScanStartTime = TInstant::MicroSeconds(0);
    PersistCurrentScan(db);

    StartKey = TSerializedCellVec();
    PersistStartKey(db);

    ReplyToActorIds.clear();

    for (auto& [tag, _] : CountMinSketches) {
        db.Table<Schema::Statistics>().Key(tag).Delete();
    }
    CountMinSketches.clear();

    ShardRanges.clear();

    KeyColumnTypes.clear();
    Columns.clear();
    ColumnNames.clear();

    TabletsForReqDistribution.clear();

    ResolveRound = 0;
    HiveRequestRound = 0;
    TraversalRound = 0;
}

template <typename T, typename S>
void PrintContainerStart(const T& container, size_t count, TStringStream& str,
    std::function<S(const typename T::value_type&)> extractor)
{
    if (container.empty()) {
        return;
    }
    str << "    ";
    size_t i = 0;
    for (const auto& entry : container) {
        if (i) {
            str << ", ";
        }
        str << extractor(entry);
        if (++i == count) {
            break;
        }
    }
    if (container.size() > count) {
        str << " ...";
    }
    str << Endl;
}

bool TStatisticsAggregator::OnRenderAppHtmlPage(NMon::TEvRemoteHttpInfo::TPtr ev,
    const TActorContext& ctx)
{
    if (!ev) {
        return true;
    }

    TStringStream str;
    HTML(str) {
        PRE() {
            str << "---- StatisticsAggregator ----" << Endl << Endl;
            str << "Database: " << Database << Endl;
            str << "BaseStats: " << BaseStats.size() << Endl;
            str << "SchemeShards: " << SchemeShards.size() << Endl;
            {
                std::function<TSSId(const std::pair<const TSSId, size_t>&)> extr =
                    [](const auto& x) { return x.first; };
                PrintContainerStart(SchemeShards, 4, str, extr);
            }
            str << "Nodes: " << Nodes.size() << Endl;
            {
                std::function<TNodeId(const std::pair<const TNodeId, size_t>&)> extr =
                    [](const auto& x) { return x.first; };
                PrintContainerStart(Nodes, 8, str, extr);
            }
            str << "RequestedSchemeShards: " << RequestedSchemeShards.size() << Endl;
            {
                std::function<TSSId(const TSSId&)> extr = [](const auto& x) { return x; };
                PrintContainerStart(RequestedSchemeShards, 4, str, extr);
            }
            str << "FastCounter: " << FastCounter << Endl;
            str << "FastCheckInFlight: " << FastCheckInFlight << Endl;
            str << "FastSchemeShards: " << FastSchemeShards.size() << Endl;
            {
                std::function<TSSId(const TSSId&)> extr = [](const auto& x) { return x; };
                PrintContainerStart(FastSchemeShards, 4, str, extr);
            }
            str << "FastNodes: " << FastNodes.size() << Endl;
            {
                std::function<TNodeId(const TNodeId&)> extr = [](const auto& x) { return x; };
                PrintContainerStart(FastNodes, 8, str, extr);
            }
            str << "PropagationInFlight: " << PropagationInFlight << Endl;
            str << "PropagationSchemeShards: " << PropagationSchemeShards.size() << Endl;
            {
                std::function<TSSId(const TSSId&)> extr = [](const auto& x) { return x; };
                PrintContainerStart(PropagationSchemeShards, 4, str, extr);
            }
            str << "PropagationNodes: " << PropagationNodes.size() << Endl;
            {
                std::function<TNodeId(const TNodeId&)> extr = [](const auto& x) { return x; };
                PrintContainerStart(FastNodes, 8, str, extr);
            }
            str << "LastSSIndex: " << LastSSIndex << Endl;
            str << "PendingRequests: " << PendingRequests.size() << Endl;
            str << "ProcessUrgentInFlight: " << ProcessUrgentInFlight << Endl << Endl;

            str << "ScanTableId: " << ScanTableId << Endl;
            str << "Columns: " << Columns.size() << Endl;
            str << "ShardRanges: " << ShardRanges.size() << Endl;
            str << "CountMinSketches: " << CountMinSketches.size() << Endl << Endl;

            str << "ScanTablesByTime: " << ScanTablesByTime.Size() << Endl;
            if (!ScanTablesByTime.Empty()) {
                auto* scanTable = ScanTablesByTime.Top();
                str << "    top: " << scanTable->PathId
                    << ", last update time: " << scanTable->LastUpdateTime << Endl;
            }
            str << "ScanTablesBySchemeShard: " << ScanTablesBySchemeShard.size() << Endl;
            if (!ScanTablesBySchemeShard.empty()) {
                str << "    " << ScanTablesBySchemeShard.begin()->first << Endl;
                std::function<TPathId(const TPathId&)> extr = [](const auto& x) { return x; };
                PrintContainerStart(ScanTablesBySchemeShard.begin()->second, 2, str, extr);
            }
            str << "ScanStartTime: " << ScanStartTime << Endl;

        }
    }

    ctx.Send(ev->Sender, new NMon::TEvRemoteHttpInfoRes(str.Str()));
    return true;
}

} // NKikimr::NStat
