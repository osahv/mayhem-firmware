/*
 * Copyright (C) 2026 Alexey Verhogladov
 *
 * This file is part of PortaPack.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "mesh_nodedb.hpp"
#include "ch.h"
#include "file.hpp"
#include <cstring>
#include <algorithm>
#include <new>

namespace meshtastic {

void NodeDB::clear() {
    for (auto& n : nodes_) n = {};
    count_ = 0;
}

NodeEntry* NodeDB::find(uint32_t node_id) {
    for (size_t i = 0; i < count_; i++) {
        if (nodes_[i].active && nodes_[i].node_id == node_id)
            return &nodes_[i];
    }
    return nullptr;
}

const NodeEntry* NodeDB::find(uint32_t node_id) const {
    for (size_t i = 0; i < count_; i++) {
        if (nodes_[i].active && nodes_[i].node_id == node_id)
            return &nodes_[i];
    }
    return nullptr;
}

// Colour derived from the id: stable for a node across restarts, and different for
// neighbouring ids. Assigned the moment the entry is created, so even the very first
// message from a node already carries its mark in the chat.
static uint8_t colour_for(uint32_t node_id) {
    uint32_t h = node_id * 2654435761u;
    h ^= h >> 13;
    return static_cast<uint8_t>(1 + (h % 7));
}

NodeEntry* NodeDB::insert(uint32_t node_id) {
    if (count_ < MAX_NODES) {
        auto& e = nodes_[count_++];
        e = {};
        e.node_id = node_id;
        e.active = true;
        e.colour = colour_for(node_id);
        return &e;
    }
    // Evict oldest entry (smallest last_seen_uptime)
    NodeEntry* oldest = &nodes_[0];
    for (size_t i = 1; i < MAX_NODES; i++) {
        if (nodes_[i].last_seen_uptime < oldest->last_seen_uptime)
            oldest = &nodes_[i];
    }
    if (oldest->active && oldest->node_id) queue_evicted(*oldest);
    *oldest = {};
    oldest->node_id = node_id;
    oldest->active = true;
    oldest->colour = colour_for(node_id);
    return oldest;
}

NodeEntry* NodeDB::update_node(uint32_t node_id,
                               const char* long_name,
                               const char* short_name) {
    auto* e = find(node_id);
    if (!e) e = insert(node_id);
    if (!e) return nullptr;
    if (long_name && long_name[0]) strncpy(e->long_name, long_name, 40);
    if (short_name && short_name[0]) strncpy(e->short_name, short_name, 4);
    return e;
}

void NodeDB::update_position(uint32_t node_id, const PositionData& pos) {
    auto* e = update_node(node_id);
    if (e) {
        e->position = pos;
        e->has_position = pos.valid;
    }
}

void NodeDB::update_telemetry(uint32_t node_id, const TelemetryData& tel) {
    auto* e = update_node(node_id);
    if (!e) return;
    // Keep the packet in full for the detail pages; the per-node fields below are the
    // summary the list and the map work from.
    last_telemetry_ = tel;
    last_telemetry_node_ = node_id;
    if (tel.has_device) {
        e->battery_level = static_cast<float>(tel.battery_level);
        e->voltage = tel.voltage;
    }
    if (tel.has_env) {
        e->temperature = tel.temperature;
        e->humidity = tel.relative_humidity;
        e->pressure = tel.barometric_pressure;
    }
    // Remember what the node is able to report, so the list can show one icon per
    // capability instead of columns that stay blank for most nodes.
    if (tel.has_device) {
        e->has_telemetry = true;
        e->uptime_seconds = tel.uptime_seconds;
    }
    if (tel.has_env) e->has_env_data = true;
    if (tel.has_air) {
        e->has_air_data = true;
        e->pm25 = tel.pm25;
        e->pm10 = tel.pm10;
        e->co2 = tel.co2;
    }
    if (tel.channel_utilization >= 0) e->channel_utilization = tel.channel_utilization;
    if (tel.air_util_tx >= 0) e->air_util_tx = tel.air_util_tx;
    if (tel.gas_resistance >= 0) e->gas_resistance = tel.gas_resistance;
    if (tel.lux >= 0) e->lux = tel.lux;
    if (tel.iaq) e->iaq = tel.iaq;
    // LocalStats, the reply to a "request stats" - a snapshot of the node's radio.
    if (tel.has_stats) {
        e->has_stats = true;
        last_stats_.node_id = node_id;
        last_stats_.uptime = tel.stat_uptime;
        last_stats_.packets_tx = tel.packets_tx;
        last_stats_.packets_rx = tel.packets_rx;
        last_stats_.packets_rx_bad = tel.packets_rx_bad;
        last_stats_.nodes_online = tel.nodes_online;
        last_stats_.nodes_total = tel.nodes_total;
        last_stats_.rx_dupe = tel.rx_dupe;
        last_stats_.tx_relay = tel.tx_relay;
        last_stats_.heap_free = tel.heap_free;
        last_stats_.heap_total = tel.heap_total;
    }
}

void NodeDB::update_signal(uint32_t node_id, int8_t rssi, float snr, uint8_t hops, uint32_t uptime) {
    auto* e = find(node_id);
    if (!e) e = insert(node_id);
    if (!e) return;
    e->last_rssi = rssi;
    e->last_snr = snr;
    e->hop_count = hops;
    if (e->first_seen_uptime == 0) e->first_seen_uptime = uptime;  // discovery time
    e->last_seen_uptime = uptime;
}

void NodeDB::set_identity(uint32_t node_id, uint8_t hw_model, uint8_t role) {
    auto* e = find(node_id);
    if (!e) e = insert(node_id);
    if (!e) return;
    if (hw_model) e->hw_model = hw_model;
    if (role) e->role = role;
}

// Newest first. A repeat trace of the same node replaces its previous entry instead
// of pushing it down: the journal is for watching a route change, and six copies of
// the same path would crowd out every other node.
void NodeDB::add_route(uint32_t node_id, uint32_t when, const uint16_t* hops, uint8_t count) {
    if (count > 3) count = 3;
    bool replacing = false;
    size_t drop = route_count_ < MAX_ROUTES ? route_count_ : MAX_ROUTES - 1;
    for (size_t i = 0; i < route_count_; i++) {
        if (routes_[i].node_id == node_id) {
            drop = i;
            replacing = true;
            break;
        }
    }
    for (size_t i = drop; i > 0; i--) routes_[i] = routes_[i - 1];
    RouteLog& r = routes_[0];
    r.node_id = node_id;
    r.when = when;
    r.hop_count = count;
    for (size_t i = 0; i < 3; i++) r.hops[i] = (i < count && hops) ? hops[i] : 0;
    // Only a genuinely new node consumes a slot. Counting a replacement as well left
    // the log claiming more entries than it held, and the last one read as a duplicate
    // of whatever had been shifted over it.
    if (!replacing && route_count_ < MAX_ROUTES) route_count_++;
}

void NodeDB::set_pubkey(uint32_t node_id, const uint8_t pub[32]) {
    auto* e = find(node_id);
    if (!e) e = insert(node_id);
    if (!e) return;
    memcpy(e->public_key, pub, 32);
    e->has_pubkey = true;
}

void NodeDB::mark_dm(uint32_t node_id) {
    auto* e = find(node_id);
    if (!e) e = insert(node_id);
    if (e) e->dm_thread = true;
}

size_t NodeDB::count() const {
    return count_;
}

NodeEntry* NodeDB::at(size_t idx) {
    return (idx < count_) ? &nodes_[idx] : nullptr;
}

const NodeEntry* NodeDB::at(size_t idx) const {
    return (idx < count_) ? &nodes_[idx] : nullptr;
}

size_t NodeDB::active_count() const {
    size_t n = 0;
    for (size_t i = 0; i < count_; i++) {
        if (nodes_[i].active) n++;
    }
    return n;
}

SeenRecord NodeDB::make_record(const NodeEntry& e, uint32_t unix_now, uint32_t ticks_now) {
    SeenRecord r{};
    r.node_id = e.node_id;
    if (unix_now && e.last_seen_uptime && ticks_now >= e.last_seen_uptime) {
        const uint32_t age_s = (ticks_now - e.last_seen_uptime) / 60;  // ~60 Hz ticks
        r.last_seen_unix = (unix_now > age_s) ? unix_now - age_s : 0;
    }
    r.rssi = e.last_rssi;
    r.snr_q4 = e.last_snr.raw;
    r.hw_model = e.hw_model;
    r.role = e.role;
    r.hops = e.hop_count;
    r.flags = e.has_pubkey ? 1 : 0;
    memcpy(r.short_name, e.short_name, sizeof(r.short_name));
    memcpy(r.long_name, e.long_name, sizeof(r.long_name));
    return r;
}

void NodeDB::queue_evicted(const NodeEntry& e) {
    if (pending_n_ >= 2) {  // the owner is behind: drop the older of the two parked entries
        pending_[0] = pending_[1];
        pending_n_ = 1;
    }
    pending_[pending_n_++] = make_record(e, clock_unix_, clock_ticks_);
}

bool NodeDB::take_evicted(SeenRecord& out) {
    if (!pending_n_) return false;
    out = pending_[0];
    pending_[0] = pending_[1];
    pending_n_--;
    return true;
}

// ---- the card side -------------------------------------------------------------------------
static const char16_t* const SEEN_PATH = u"/LOGS/mesh_seen.bin";

// A File without operator new (see core_control.cpp): null when the heap has no room, and the caller gives up.
static File* seen_open(bool write) {
    void* const mem = chHeapAlloc(0x0, sizeof(File));
    if (!mem) return nullptr;
    File* const f = new (mem) File();
    // Optional<Error> is truthy when it failed; read/write and create when writing.
    if (f->open(std::filesystem::path{SEEN_PATH}, !write, write)) {
        f->~File();
        chHeapFree(mem);
        return nullptr;
    }
    return f;
}

static void seen_close(File* f) {
    f->~File();
    chHeapFree(f);
}

size_t seen_count() {
    File* const f = seen_open(false);
    if (!f) return 0;
    size_t n = f->size() / sizeof(SeenRecord);
    seen_close(f);
    return n > SEEN_MAX ? SEEN_MAX : n;
}

bool seen_read(size_t index, SeenRecord& out) {
    File* const f = seen_open(false);
    if (!f) return false;
    bool ok = false;
    if (index < f->size() / sizeof(SeenRecord) && f->seek(index * sizeof(SeenRecord))) {
        const auto r = f->read(&out, sizeof(SeenRecord));
        ok = r && *r == sizeof(SeenRecord);
    }
    seen_close(f);
    return ok;
}

void seen_for_each_newest_first(SeenVisit fn, void* ctx) {
    File* const f = seen_open(false);
    if (!f) return;
    const size_t n = f->size() / sizeof(SeenRecord);
    SeenRecord cur;
    for (size_t i = (n > SEEN_MAX ? SEEN_MAX : n); i-- > 0;) {
        if (!f->seek(i * sizeof(SeenRecord))) break;
        const auto r = f->read(&cur, sizeof(cur));
        if (!r || *r != sizeof(cur)) break;
        if (!fn(cur, ctx)) break;
    }
    seen_close(f);
}

bool seen_put(const SeenRecord& rec) {
    File* const f = seen_open(true);
    if (!f) return false;
    const size_t n = f->size() / sizeof(SeenRecord);
    size_t target = n, oldest = 0;
    uint32_t oldest_t = 0xFFFFFFFFu;
    SeenRecord cur;
    for (size_t i = 0; i < n && i < SEEN_MAX; i++) {
        const auto r = f->read(&cur, sizeof(cur));
        if (!r || *r != sizeof(cur)) break;
        if (cur.node_id == rec.node_id) {
            target = i;
            break;
        }
        if (cur.last_seen_unix < oldest_t) {
            oldest_t = cur.last_seen_unix;
            oldest = i;
        }
    }
    if (target >= SEEN_MAX) target = oldest;  // full: the record heard longest ago makes room
    bool ok = false;
    if (f->seek(target * sizeof(SeenRecord))) {
        const auto w = f->write(&rec, sizeof(rec));
        ok = w && *w == sizeof(rec);
    }
    seen_close(f);
    return ok;
}

}  // namespace meshtastic
