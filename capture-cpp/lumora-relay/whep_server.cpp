#include "whep_server.h"
#include "relay_core.h"

bool WhepServer::start(uint16_t port, RelayCore* core) {
    core_ = core;
    return http_.start(port, [this](const HttpReq& q, HttpResp& r) { handle(q, r); });
}

void WhepServer::handle(const HttpReq& req, HttpResp& resp) {
    // Pfad normalisieren: /live/whep[...] und /whep[...] gleichwertig
    std::string p = req.path;
    if (p.rfind("/live/whep", 0) != 0 && p.rfind("/whep", 0) != 0) { resp.status = 404; return; }
    std::string rest = p.substr(p.find("whep") + 4);   // "" oder "/<id>"

    if (req.method == "POST" && (rest.empty() || rest == "/")) {
        std::string ua;
        auto it = req.headers.find("user-agent");
        if (it != req.headers.end()) ua = it->second;
        std::string sid;
        std::string answer = core_->createSession(req.body, ua, req.query, sid);
        if (answer.empty()) { resp.status = 400; resp.contentType = "text/plain"; resp.body = "invalid offer"; return; }
        resp.status = 201;
        resp.contentType = "application/sdp";
        resp.headers["Location"] = "/live/whep/" + sid;
        resp.body = answer;
        return;
    }
    if (req.method == "DELETE" && rest.size() > 1) {
        resp.contentType = "text/plain";
        if (core_->deleteSession(rest.substr(1))) { resp.status = 200; resp.body = "ok"; }
        else resp.status = 404;
        return;
    }
    if (req.method == "PATCH") { resp.status = 405; return; }   // kein Trickle-ICE noetig
    if (req.method == "OPTIONS") { resp.status = 204; resp.body.clear(); return; }
    resp.status = 405;
}
