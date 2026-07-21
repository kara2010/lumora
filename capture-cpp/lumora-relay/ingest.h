// UDP-Ingest: lauscht auf dem MPEG-TS-Port (Standard 8558) und fuettert den Demuxer.
#pragma once
#include <atomic>
#include <cstdint>
#include <thread>

struct TsDemux;

class Ingest {
public:
    bool start(uint16_t port, TsDemux* demux);
    void stop();
    ~Ingest() { stop(); }
    uint64_t datagrams = 0;
private:
    void loop();
    uintptr_t sock_ = ~(uintptr_t)0;
    TsDemux* demux_ = nullptr;
    std::thread thread_;
    std::atomic<bool> run_{ false };
};
