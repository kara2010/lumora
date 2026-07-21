// WHEP-Endpunkt (Standard-Port 8889, nur 127.0.0.1 - die Shell proxied 8787 -> hier).
// Pfade wie mediamtx, damit Shell-Proxy und Location-Rewrite unveraendert funktionieren:
//   POST   /live/whep            Offer (application/sdp) -> 201 + Answer + Location: /live/whep/<id>
//   DELETE /live/whep/<id>       Session beenden
//   PATCH  /live/whep/<id>       (Trickle-ICE) -> 405, Kandidaten stehen komplett im Answer
#pragma once
#include "http_mini.h"
class RelayCore;

class WhepServer {
public:
    bool start(uint16_t port, RelayCore* core);
    void stop() { http_.stop(); }
private:
    void handle(const HttpReq& req, HttpResp& resp);
    HttpMini http_;
    RelayCore* core_ = nullptr;
};
