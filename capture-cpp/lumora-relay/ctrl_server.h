// Control-API (Standard-Port 9997, nur 127.0.0.1), bewusst mediamtx-API-kompatibel,
// damit die Shell (bcMtxReaders/Kick/Selftest) unveraendert weiterlaeuft:
//   GET  /v3/webrtcsessions/list        -> {items:[{id,path,state,remoteCandidate,userAgent,query,bytesSent}]}
//   POST /v3/webrtcsessions/kick/<id>   -> 200/404
//   GET  /v3/paths/get/live             -> {name,ready,tracks,bytesReceived} (E2E-Selftest, Abschluss-Statistik)
// Neu (ersetzt mediamtx-YAML-Hot-Reload):
//   POST /v3/config/ice  {additionalHosts:[..], iceServers:[{url,user,pass}]} - wirkt auf NEUE Sessions
#pragma once
#include "http_mini.h"
class RelayCore;

class CtrlServer {
public:
    bool start(uint16_t port, RelayCore* core);
    void stop() { http_.stop(); }
private:
    void handle(const HttpReq& req, HttpResp& resp);
    HttpMini http_;
    RelayCore* core_ = nullptr;
};
