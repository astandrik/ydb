#pragma once
#include <ydb/library/actors/core/actor_bootstrapped.h>
#include <ydb/library/actors/core/mon.h>
#include <ydb/core/base/tablet_pipe.h>
#include <ydb/library/services/services.pb.h>
#include <ydb/core/tx/schemeshard/schemeshard.h>
#include <ydb/core/tx/tx_proxy/proxy.h>
#include "viewer.h"
#include "json_pipe_req.h"

namespace NKikimr {
namespace NViewer {

using namespace NActors;
using NSchemeShard::TEvSchemeShard;

class TJsonACL : public TViewerPipeClient {
    using TThis = TJsonACL;
    using TBase = TViewerPipeClient;
    IViewer* Viewer;
    NMon::TEvHttpInfo::TPtr Event;
    TAutoPtr<TEvTxProxySchemeCache::TEvNavigateKeySetResult> CacheResult;
    TJsonSettings JsonSettings;
    bool MergeRules = false;
    ui32 Timeout = 0;

public:
    TJsonACL(IViewer* viewer, NMon::TEvHttpInfo::TPtr &ev)
        : Viewer(viewer)
        , Event(ev)
    {}

    void Bootstrap() override {
        const auto& params(Event->Get()->Request.GetParams());
        Timeout = FromStringWithDefault<ui32>(params.Get("timeout"), 10000);
        TString database;
        if (params.Has("database")) {
            database = params.Get("database");
        }
        if (database && database != AppData()->TenantName) {
            BLOG_TRACE("Requesting StateStorageEndpointsLookup for " << database);
            RequestStateStorageEndpointsLookup(database); // to find some dynamic node and redirect query there
        } else {
            if (params.Has("path")) {
                RequestSchemeCacheNavigate(params.Get("path"));
            } else {
                return ReplyAndPassAway(Viewer->GetHTTPBADREQUEST(Event->Get(), "text/plain", "field 'path' is required"));
            }
            MergeRules = FromStringWithDefault<bool>(params.Get("merge_rules"), MergeRules);
        }

        Become(&TThis::StateRequestedDescribe, TDuration::MilliSeconds(Timeout), new TEvents::TEvWakeup());
    }

    void Handle(TEvStateStorage::TEvBoardInfo::TPtr& ev) {
        BLOG_TRACE("Received TEvBoardInfo");
        ReplyAndPassAway(Viewer->MakeForward(Event->Get(), GetNodesFromBoardReply(ev)));
    }

    STATEFN(StateRequestedDescribe) {
        switch (ev->GetTypeRewrite()) {
            hFunc(TEvStateStorage::TEvBoardInfo, Handle);
            hFunc(TEvTxProxySchemeCache::TEvNavigateKeySetResult, Handle);
            cFunc(TEvents::TSystem::Wakeup, HandleTimeout);
        }
    }

    void Handle(TEvTxProxySchemeCache::TEvNavigateKeySetResult::TPtr& ev) {
        CacheResult = ev->Release();
        RequestDone();
    }

    static bool Has(ui32 accessRights, ui32 mask) {
        return (accessRights & mask) == mask;
    }

    void FillACE(const NACLibProto::TACE& ace, NKikimrViewer::TMetaCommonInfo::TACE& pbAce) {
        if (static_cast<NACLib::EAccessType>(ace.GetAccessType()) == NACLib::EAccessType::Deny) {
            pbAce.SetAccessType("Deny");
        }
        if (static_cast<NACLib::EAccessType>(ace.GetAccessType()) == NACLib::EAccessType::Allow) {
            pbAce.SetAccessType("Allow");
        }

        auto ar = ace.GetAccessRight();

        static std::pair<ui32, TString> accessRules[] = {
            {NACLib::EAccessRights::GenericFull, "Full"},
            {NACLib::EAccessRights::GenericFullLegacy, "FullLegacy"},
            {NACLib::EAccessRights::GenericManage, "Manage"},
            {NACLib::EAccessRights::GenericUse, "Use"},
            {NACLib::EAccessRights::GenericUseLegacy, "UseLegacy"},
            {NACLib::EAccessRights::GenericWrite, "Write"},
            {NACLib::EAccessRights::GenericRead, "Read"},
            {NACLib::EAccessRights::GenericList, "List"},
        };
        if (MergeRules) {
            for (const auto& [rule, name] : accessRules) {
                if (Has(ar, rule)) {
                    pbAce.AddAccessRules(name);
                    ar &= ~rule;
                }
            }
        }

        static std::pair<ui32, TString> accessRights[] = {
            {NACLib::EAccessRights::SelectRow, "SelectRow"},
            {NACLib::EAccessRights::UpdateRow, "UpdateRow"},
            {NACLib::EAccessRights::EraseRow, "EraseRow"},
            {NACLib::EAccessRights::ReadAttributes, "ReadAttributes"},
            {NACLib::EAccessRights::WriteAttributes, "WriteAttributes"},
            {NACLib::EAccessRights::CreateDirectory, "CreateDirectory"},
            {NACLib::EAccessRights::CreateTable, "CreateTable"},
            {NACLib::EAccessRights::CreateQueue, "CreateQueue"},
            {NACLib::EAccessRights::RemoveSchema, "RemoveSchema"},
            {NACLib::EAccessRights::DescribeSchema, "DescribeSchema"},
            {NACLib::EAccessRights::AlterSchema, "AlterSchema"},
            {NACLib::EAccessRights::CreateDatabase, "CreateDatabase"},
            {NACLib::EAccessRights::DropDatabase, "DropDatabase"},
            {NACLib::EAccessRights::GrantAccessRights, "GrantAccessRights"},
            {NACLib::EAccessRights::WriteUserAttributes, "WriteUserAttributes"},
            {NACLib::EAccessRights::ConnectDatabase, "ConnectDatabase"},
            {NACLib::EAccessRights::ReadStream, "ReadStream"},
            {NACLib::EAccessRights::WriteStream, "WriteStream"},
            {NACLib::EAccessRights::ReadTopic, "ReadTopic"},
            {NACLib::EAccessRights::WriteTopic, "WriteTopic"}
        };
        for (const auto& [right, name] : accessRights) {
            if (Has(ar, right)) {
                pbAce.AddAccessRights(name);
                ar &= ~right;
            }
        }

        if (ar != 0) {
            pbAce.AddAccessRights(NACLib::AccessRightsToString(ar));
        }

        pbAce.SetSubject(ace.GetSID());

        auto inht = ace.GetInheritanceType();
        if ((inht & NACLib::EInheritanceType::InheritObject) != 0) {
            pbAce.AddInheritanceType("Object");
        }
        if ((inht & NACLib::EInheritanceType::InheritContainer) != 0) {
            pbAce.AddInheritanceType("Container");
        }
        if ((inht & NACLib::EInheritanceType::InheritOnly) != 0) {
            pbAce.AddInheritanceType("Only");
        }
    }

    void ReplyAndPassAway(TString data) {
        Send(Event->Sender, new NMon::TEvHttpInfoRes(data, 0, NMon::IEvHttpInfoRes::EContentType::Custom));
        PassAway();
    }

    void ReplyAndPassAway() override {
        if (CacheResult == nullptr) {
            return ReplyAndPassAway(Viewer->GetHTTPINTERNALERROR(Event->Get(), "text/plain", "no SchemeCache response"));
        }
        if (CacheResult->Request == nullptr) {
            return ReplyAndPassAway(Viewer->GetHTTPINTERNALERROR(Event->Get(), "text/plain", "wrong SchemeCache response"));
        }
        if (CacheResult->Request.Get()->ResultSet.empty()) {
            return ReplyAndPassAway(Viewer->GetHTTPINTERNALERROR(Event->Get(), "text/plain", "SchemeCache response is empty"));
        }
        if (CacheResult->Request.Get()->ErrorCount != 0) {
            return ReplyAndPassAway(Viewer->GetHTTPBADREQUEST(Event->Get(), "text/plain", TStringBuilder() << "SchemeCache response error " << static_cast<int>(CacheResult->Request.Get()->ResultSet.front().Status)));
        }
        const auto& entry = CacheResult->Request.Get()->ResultSet.front();
        NKikimrViewer::TMetaInfo metaInfo;
        NKikimrViewer::TMetaCommonInfo& pbCommon = *metaInfo.MutableCommon();
        pbCommon.SetPath(CanonizePath(entry.Path));
        pbCommon.SetOwner(entry.Self->Info.GetOwner());
        if (entry.Self->Info.HasACL()) {
            NACLib::TACL acl(entry.Self->Info.GetACL());
            for (const NACLibProto::TACE& ace : acl.GetACE()) {
                auto& pbAce = *pbCommon.AddACL();
                FillACE(ace, pbAce);
            }
        }
        if (entry.Self->Info.HasEffectiveACL()) {
            NACLib::TACL acl(entry.Self->Info.GetEffectiveACL());
            for (const NACLibProto::TACE& ace : acl.GetACE()) {
                auto& pbAce = *pbCommon.AddEffectiveACL();
                FillACE(ace, pbAce);
            }
        }

        TStringStream json;
        TProtoToJson::ProtoToJson(json, metaInfo, JsonSettings);

        ReplyAndPassAway(Viewer->GetHTTPOKJSON(Event->Get(), json.Str()));
    }

    void HandleTimeout() {
        ReplyAndPassAway(Viewer->GetHTTPGATEWAYTIMEOUT(Event->Get()));
    }
};

template <>
YAML::Node TJsonRequestSwagger<TJsonACL>::GetSwagger() {
    YAML::Node node = YAML::Load(R"___(
        get:
          tags:
          - viewer
          summary: ACL information
          description: Returns information about ACL of an object
          parameters:
          - name: database
            in: query
            description: database name
            type: string
            required: false
          - name: path
            in: query
            description: schema path
            required: true
            type: string
          - name: merge_rules
            in: query
            description: merge access rights into access rules
            type: boolean
          - name: timeout
            in: query
            description: timeout in ms
            required: false
            type: integer
          responses:
            200:
              description: OK
              content:
                application/json:
                  schema:
                    type: object
                    properties:
                      Common:
                        type: object
                        properties:
                          Path:
                            type: string
                          Owner:
                            type: string
                          ACL:
                            type: array
                            items:
                              type: object
                              properties:
                                AccessType:
                                  type: string
                                Subject:
                                  type: string
                                AccessRules:
                                  type: array
                                  items:
                                    type: string
                                AccessRights:
                                  type: array
                                  items:
                                    type: string
                                InheritanceType:
                                  type: array
                                  items:
                                    type: string
                          EffectiveACL:
                            type: array
                            items:
                              type: object
                              properties:
                                AccessType:
                                  type: string
                                Subject:
                                  type: string
                                AccessRules:
                                  type: array
                                  items:
                                    type: string
                                AccessRights:
                                  type: array
                                  items:
                                    type: string
                                InheritanceType:
                                  type: array
                                  items:
                                    type: string
            400:
              description: Bad Request
            403:
              description: Forbidden
            504:
              description: Gateway Timeout

            )___");

    return node;
}

}
}
