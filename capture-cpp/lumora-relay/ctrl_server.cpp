#include "ctrl_server.h"
#include "relay_core.h"
#include "json.hpp"

using json = nlohmann::json;

bool CtrlServer::start(uint16_t port, RelayCore* core) {
    core_ = core;
    return http_.start(port, [this](const HttpReq& q, HttpResp& r) { handle(q, r); });
}

void CtrlServer::handle(const HttpReq& req, HttpResp& resp) {
    if (req.method == "GET" && req.path == "/v3/webrtcsessions/list") {
        json items = json::array();
        for (const auto& s : core_->listSessions()) {
            items.push_back({
                {"id", s.id}, {"path", "live"}, {"state", "read"},
                {"remoteCandidate", s.remoteCandidate}, {"userAgent", s.userAgent},
                {"query", s.query}, {"bytesSent", (long long)s.bytesSent},
            });
        }
        resp.body = json{ {"itemCount", items.size()}, {"pageCount", 1}, {"items", items} }.dump();
        return;
    }
    if (req.method == "POST" && req.path.rfind("/v3/webrtcsessions/kick/", 0) == 0) {
        std::string id = req.path.substr(strlen("/v3/webrtcsessions/kick/"));
        if (core_->deleteSession(id)) { resp.body = "{}"; }
        else { resp.status = 404; resp.body = "{\"error\":\"not found\"}"; }
        return;
    }
    if (req.method == "GET" && req.path == "/v3/paths/get/live") {
        // "ready" = Ingest liefert gerade Daten (mediamtx-Semantik: Publisher verbunden)
        resp.body = json{
            {"name", "live"}, {"ready", core_->ingestAlive()},
            {"tracks", json::array({"H264", "Opus"})},
        }.dump();
        return;
    }
    if (req.method == "POST" && req.path == "/v3/config/ice") {
        json j = json::parse(req.body, nullptr, false);
        if (!j.is_object()) { resp.status = 400; resp.body = "{}"; return; }
        std::vector<std::string> hosts;
        for (auto& h : j.value("additionalHosts", json::array()))
            if (h.is_string()) hosts.push_back(h.get<std::string>());
        std::vector<RelayIceServer> servers;
        for (auto& s : j.value("iceServers", json::array())) {
            if (!s.is_object()) continue;
            RelayIceServer is;
            is.url = s.value("url", ""); is.user = s.value("user", ""); is.pass = s.value("pass", "");
            if (!is.url.empty()) servers.push_back(std::move(is));
        }
        core_->setIceConfig(std::move(hosts), std::move(servers));
        resp.body = "{}";
        return;
    }
    resp.status = 404; resp.body = "{}";
}
