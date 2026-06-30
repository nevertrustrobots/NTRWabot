#include "NTR_Plugin.hpp"
#include "lib/httplib.h"
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>
#include <chrono>
#include <memory>
#include <algorithm>
#include <unordered_map>
#include <cmath>
#include <cstring>
#include <sys/resource.h>
#include <patch.hpp>

// ── JSON helpers ──────────────────────────────────────────────────────────────
static std::string jStr(const std::string& s) {
    std::string r = "\"";
    for (unsigned char c : s) {
        if      (c == '"')  r += "\\\"";
        else if (c == '\\') r += "\\\\";
        else if (c == '\n') r += "\\n";
        else if (c == '\r') r += "\\r";
        else if (c == '\t') r += "\\t";
        else if (c < 0x20)  { char buf[8]; snprintf(buf, sizeof(buf), "\\u%04x", c); r += buf; }
        else r += (char)c;
    }
    return r + "\"";
}

static std::string jNum(float f) {
    if (std::isnan(f) || std::isinf(f)) return "0";
    return std::to_string(f);
}

static float parseFloatField(const std::string& body, const std::string& key) {
    auto p = body.find("\"" + key + "\"");
    if (p == std::string::npos) return 0.f;
    p = body.find(":", p);
    if (p == std::string::npos) return 0.f;
    try { return std::stof(body.substr(p + 1)); } catch (...) { return 0.f; }
}

static int64_t parseInt64Field(const std::string& body, const std::string& key) {
    auto p = body.find("\"" + key + "\"");
    if (p == std::string::npos) return -1;
    p = body.find(":", p);
    if (p == std::string::npos) return -1;
    try { return std::stoll(body.substr(p + 1)); } catch (...) { return -1; }
}

static std::string parseStringField(const std::string& body, const std::string& key) {
    auto p = body.find("\"" + key + "\"");
    if (p == std::string::npos) return "";
    p = body.find(":", p);
    if (p == std::string::npos) return "";
    auto q1 = body.find("\"", p);
    if (q1 == std::string::npos) return "";
    std::string out;
    for (size_t i = q1 + 1; i < body.size(); i++) {
        char c = body[i];
        if (c == '\\' && i + 1 < body.size()) { out += body[i + 1]; i++; continue; }
        if (c == '"') break;
        out += c;
    }
    return out;
}

static bool parseBoolField(const std::string& body, const std::string& key) {
    auto p = body.find("\"" + key + "\"");
    if (p == std::string::npos) return false;
    p = body.find(":", p);
    if (p == std::string::npos) return false;
    while (p < body.size() && (body[p] == ':' || body[p] == ' ')) p++;
    return p + 4 <= body.size() && body.substr(p, 4) == "true";
}

// ── Note name from frequency ──────────────────────────────────────────────────
static std::string freqToNote(float freq) {
    if (freq <= 0.f) return "?";
    static const char* names[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    float midi = 12.0f * log2f(freq / 440.0f) + 69.0f;
    int m = (int)roundf(midi);
    int oct = m / 12 - 1;
    int n   = ((m % 12) + 12) % 12;
    return std::string(names[n]) + std::to_string(oct);
}

// ── Audio ring buffer (written by audio thread, snapshotted by HTTP thread) ───
static const int AUDIO_BUF = 4096;  // power of 2, ~93ms @ 44100 Hz

struct AudioRingBuf {
    float            data[AUDIO_BUF] = {};
    std::atomic<int> head{0};         // total samples written; pos = head & (AUDIO_BUF-1)
};

// ── Oscilloscope voltage tracking ─────────────────────────────────────────────
static const int HISTORY_LEN       = 16;
static const int VOLT_SAMPLE_EVERY = 6;

struct PortVolts {
    float peak    = 0.f;
    float history[HISTORY_LEN] = {};
    int   histIdx = 0;
};
struct ModuleVolts {
    std::vector<PortVolts> inputs;
    std::vector<PortVolts> outputs;
};

// ── Job types ─────────────────────────────────────────────────────────────────
struct SetParamJob {
    int64_t moduleId; int paramId; float value;
    std::atomic<bool> done{false}; bool success = false;
};
struct CableJob {
    enum Type { ADD, REMOVE, REMOVE_OUTPUT };
    Type type;
    int64_t outModId=-1, outPortId=-1, inModId=-1, inPortId=-1;
    std::atomic<bool> done{false}; bool success=false; std::string error;
};
struct AddModuleJob {
    std::string pluginSlug, modelSlug;
    std::atomic<bool> done{false}; bool success=false; int64_t newId=-1; std::string error;
};
struct DeleteModuleJob {
    int64_t moduleId;
    std::atomic<bool> done{false}; bool success=false; std::string error;
};
struct MoveModuleJob {
    int64_t moduleId; float hp;
    std::atomic<bool> done{false}; bool success=false; std::string error;
};
struct BypassJob {
    int64_t moduleId; bool bypassed;
    std::atomic<bool> done{false}; bool success=false; std::string error;
};
struct ModuleStateJob {
    enum Type { GET, SET };
    Type type; int64_t moduleId;
    std::string stateJson; // output for GET, input for SET
    std::atomic<bool> done{false}; bool success=false; std::string error;
};
struct PatchSaveJob {
    std::string savedPath;
    std::atomic<bool> done{false}; bool success=false; std::string error;
};
struct ParamEntry {
    float value=0.f, minV=0.f, maxV=1.f, defaultV=0.f;
    std::string name, unit;
};
struct ModuleEntry {
    int64_t id; std::string plugin, model;
    std::vector<ParamEntry> params;
    float posX=0.f, posY=0.f;
};

// ── Nevertrustrobots Wabot Module ─────────────────────────────────────────────
struct NTRWabot : Module {
    enum ParamId  { START_PARAM, PARAMS_LEN };
    enum InputId  { AUDIO_IN_L, AUDIO_IN_R, INPUTS_LEN };
    enum OutputId { OUTPUTS_LEN };
    enum LightId  { STATUS_R_LIGHT, STATUS_G_LIGHT, STATUS_B_LIGHT, LIGHTS_LEN };

    std::atomic<bool> serverRunning{false};
    httplib::Server   svr;
    std::thread       serverThread;

    // Audio analysis — written in audio thread, read by HTTP thread
    AudioRingBuf audioRing[2];    // 0 = L input, 1 = R input
    float        sampleRate = 44100.f;  // set in process(), read by HTTP

    // Patch cache — UI thread writes, HTTP thread reads
    std::vector<ModuleEntry> cache;
    std::string              moduleListJson    = "[]";
    std::string              moduleSummaryJson = "[]";
    std::string              cableListJson  = "[]";
    std::string              graphJson      = "[]";
    std::string              scopeJson      = "{}";
    std::string              patchInfoJson  = "{\"path\":\"\",\"hasSavePath\":false}";
    std::string              voltagesJson   = "[]";
    std::mutex               cacheMtx;

    // Job queues
    std::queue<std::shared_ptr<SetParamJob>>  setQueue;   std::mutex setQueueMtx;
    std::queue<std::shared_ptr<CableJob>>     cableQueue; std::mutex cableQueueMtx;
    std::queue<std::shared_ptr<AddModuleJob>> addQueue;   std::mutex addQueueMtx;

    // Voltage scope — UI thread only
    std::unordered_map<int64_t, ModuleVolts> voltTrack;
    int voltSampleTimer = 0;

    // Cached perf data — written by updateCache() (UI thread), read via cacheMtx
    double cachedBlockDurationMs = 0.0;
    int    cachedBlockFrames = 0;
    int    cachedModuleCount = 0;
    int    cachedCableCount  = 0;

    // Delete / Move job queues
    std::queue<std::shared_ptr<DeleteModuleJob>> deleteQueue; std::mutex deleteQueueMtx;
    std::queue<std::shared_ptr<MoveModuleJob>>   moveQueue;   std::mutex moveQueueMtx;
    std::queue<std::shared_ptr<BypassJob>>      bypassQueue;    std::mutex bypassQueueMtx;
    std::queue<std::shared_ptr<ModuleStateJob>> stateQueue;     std::mutex stateQueueMtx;
    std::queue<std::shared_ptr<PatchSaveJob>>   patchSaveQueue; std::mutex patchSaveQueueMtx;

    dsp::BooleanTrigger startTrig;

    NTRWabot() {
        config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
        configButton(START_PARAM, "Start Wabot Server");
        configInput(AUDIO_IN_L, "Audio Analyze L");
        configInput(AUDIO_IN_R, "Audio Analyze R");
        setupRoutes();
    }
    ~NTRWabot() { stopServer(); }

    // ── Audio thread: sample inputs into ring buffers ─────────────────────────
    void process(const ProcessArgs& args) override {
        if (startTrig.process(params[START_PARAM].getValue() > 0.f)) startServer();

        sampleRate = args.sampleRate;

        // Sample audio inputs into ring buffers (lock-free: aligned float writes are atomic on x86/ARM)
        for (int j = 0; j < 2; j++) {
            float v = inputs[j].getVoltage();
            int h = audioRing[j].head.load(std::memory_order_relaxed);
            audioRing[j].data[h & (AUDIO_BUF - 1)] = v;
            audioRing[j].head.store(h + 1, std::memory_order_relaxed);
        }

        bool on = serverRunning;
        lights[STATUS_R_LIGHT].setBrightness(on ? 0.f  : 0.8f);
        lights[STATUS_G_LIGHT].setBrightness(on ? 0.8f : 0.f);
        lights[STATUS_B_LIGHT].setBrightness(0.f);
    }

    // ── UI-thread: sample voltages ─────────────────────────────────────────────
    void updateVoltages() {
        bool doHistory = (++voltSampleTimer >= VOLT_SAMPLE_EVERY);
        if (doHistory) voltSampleTimer = 0;
        for (int64_t id : APP->engine->getModuleIds()) {
            engine::Module* m = APP->engine->getModule(id);
            if (!m) continue;
            ModuleVolts& mv = voltTrack[id];
            if (mv.inputs.size()  != m->inputs.size())  mv.inputs.resize(m->inputs.size());
            if (mv.outputs.size() != m->outputs.size()) mv.outputs.resize(m->outputs.size());
            for (size_t j = 0; j < m->inputs.size(); j++) {
                float v = m->inputs[j].getVoltage(), av = std::fabs(v);
                if (av > mv.inputs[j].peak) mv.inputs[j].peak = av;
                if (doHistory) { mv.inputs[j].history[mv.inputs[j].histIdx % HISTORY_LEN] = v; mv.inputs[j].histIdx++; }
            }
            for (size_t j = 0; j < m->outputs.size(); j++) {
                float v = m->outputs[j].getVoltage(), av = std::fabs(v);
                if (av > mv.outputs[j].peak) mv.outputs[j].peak = av;
                if (doHistory) { mv.outputs[j].history[mv.outputs[j].histIdx % HISTORY_LEN] = v; mv.outputs[j].histIdx++; }
            }
        }
    }

    // ── UI-thread: build cache JSON ────────────────────────────────────────────
    void updateCache() {
        std::vector<ModuleEntry> newCache;
        std::string json = "[\n";
        std::string summary = "[\n";
        std::string vjson = "[\n";
        std::vector<int64_t> ids = APP->engine->getModuleIds();
        cachedModuleCount = (int)ids.size();
        cachedBlockFrames = APP->engine->getBlockFrames();
        cachedBlockDurationMs = APP->engine->getBlockDuration() * 1000.0;

        for (size_t i = 0; i < ids.size(); i++) {
            engine::Module* m = APP->engine->getModule(ids[i]);
            if (!m) continue;
            ModuleEntry e;
            e.id = ids[i];
            e.plugin = (m->model && m->model->plugin) ? m->model->plugin->slug : "unknown";
            e.model  = m->model ? m->model->slug : "unknown";
            ModuleWidget* mw_pos = APP->scene->rack->getModule(ids[i]);
            if (mw_pos) { e.posX = mw_pos->box.pos.x / RACK_GRID_WIDTH; e.posY = mw_pos->box.pos.y / RACK_GRID_WIDTH; }
            for (size_t pi = 0; pi < m->params.size(); pi++) {
                ParamEntry pe;
                pe.value = m->params[pi].getValue();
                if (pi < m->paramQuantities.size() && m->paramQuantities[pi]) {
                    ParamQuantity* pq = m->paramQuantities[pi];
                    pe.name = pq->name; pe.unit = pq->unit;
                    pe.minV = pq->getMinValue(); pe.maxV = pq->getMaxValue();
                    pe.defaultV = pq->getDefaultValue();
                }
                e.params.push_back(pe);
            }
            newCache.push_back(e);

            ModuleVolts* mv = nullptr;
            auto it = voltTrack.find(ids[i]);
            if (it != voltTrack.end()) mv = &it->second;

            json += "  {";
            json += jStr("id")     + ": " + std::to_string(ids[i]) + ", ";
            json += jStr("plugin") + ": " + jStr(e.plugin) + ", ";
            json += jStr("model")  + ": " + jStr(e.model) + ", ";
            json += jStr("bypassed") + ": " + (m->isBypassed() ? "true" : "false") + ", ";
            json += jStr("posX") + ": " + std::to_string(e.posX) + ", ";
            json += jStr("posY") + ": " + std::to_string(e.posY) + ", ";

            json += jStr("params") + ": [";
            for (size_t j = 0; j < m->params.size(); j++) {
                std::string nm, unit; float minV=0.f, maxV=1.f, defV=0.f;
                if (j < m->paramQuantities.size() && m->paramQuantities[j]) {
                    ParamQuantity* pq = m->paramQuantities[j];
                    nm=pq->name; unit=pq->unit;
                    minV=pq->getMinValue(); maxV=pq->getMaxValue(); defV=pq->getDefaultValue();
                }
                json += "{" + jStr("id") + ": " + std::to_string(j) + ", "
                            + jStr("name") + ": " + jStr(nm) + ", "
                            + jStr("value") + ": " + jNum(m->params[j].getValue()) + ", "
                            + jStr("min") + ": " + jNum(minV) + ", "
                            + jStr("max") + ": " + jNum(maxV) + ", "
                            + jStr("default") + ": " + jNum(defV) + ", "
                            + jStr("unit") + ": " + jStr(unit) + "}";
                if (j + 1 < m->params.size()) json += ", ";
            }
            json += "], ";

            json += jStr("inputs") + ": [";
            for (size_t j = 0; j < m->inputs.size(); j++) {
                std::string nm;
                if (j < m->inputInfos.size() && m->inputInfos[j]) nm = m->inputInfos[j]->name;
                float peak = (mv && j < mv->inputs.size()) ? mv->inputs[j].peak : 0.f;
                int ichs = m->inputs[j].getChannels();
                std::string ivArr = "[";
                for (int ch = 0; ch < ichs; ch++) { if (ch>0) ivArr+=","; ivArr+=jNum(m->inputs[j].getVoltage(ch)); }
                ivArr += "]";
                json += "{" + jStr("id") + ": " + std::to_string(j) + ", "
                            + jStr("name") + ": " + jStr(nm) + ", "
                            + jStr("channels") + ": " + std::to_string(ichs) + ", "
                            + jStr("voltage") + ": " + jNum(m->inputs[j].getVoltage()) + ", "
                            + jStr("voltages") + ": " + ivArr + ", "
                            + jStr("peak") + ": " + jNum(peak) + ", "
                            + jStr("connected") + ": " + (m->inputs[j].isConnected() ? "true" : "false") + "}";
                if (j + 1 < m->inputs.size()) json += ", ";
            }
            json += "], ";

            json += jStr("outputs") + ": [";
            for (size_t j = 0; j < m->outputs.size(); j++) {
                std::string nm;
                if (j < m->outputInfos.size() && m->outputInfos[j]) nm = m->outputInfos[j]->name;
                float peak = (mv && j < mv->outputs.size()) ? mv->outputs[j].peak : 0.f;
                int ochs = m->outputs[j].getChannels();
                std::string ovArr = "[";
                for (int ch = 0; ch < ochs; ch++) { if (ch>0) ovArr+=","; ovArr+=jNum(m->outputs[j].getVoltage(ch)); }
                ovArr += "]";
                json += "{" + jStr("id") + ": " + std::to_string(j) + ", "
                            + jStr("name") + ": " + jStr(nm) + ", "
                            + jStr("channels") + ": " + std::to_string(ochs) + ", "
                            + jStr("voltage") + ": " + jNum(m->outputs[j].getVoltage()) + ", "
                            + jStr("voltages") + ": " + ovArr + ", "
                            + jStr("peak") + ": " + jNum(peak) + ", "
                            + jStr("connected") + ": " + (m->outputs[j].isConnected() ? "true" : "false") + "}";
                if (j + 1 < m->outputs.size()) json += ", ";
            }
            json += "]";
            json += "}";
            if (i + 1 < ids.size()) json += ",";
            json += "\n";

            // max output polyphony for summary
            int maxOutCh = 0;
            for (size_t j = 0; j < m->outputs.size(); j++) {
                int ch = m->outputs[j].getChannels();
                if (ch > maxOutCh) maxOutCh = ch;
            }
            summary += "  {" + jStr("id") + ": " + std::to_string(ids[i]) + ", "
                     + jStr("plugin") + ": " + jStr(e.plugin) + ", "
                     + jStr("model") + ": " + jStr(e.model) + ", "
                     + jStr("bypassed") + ": " + (m->isBypassed() ? "true" : "false") + ", "
                     + jStr("posX") + ": " + std::to_string(e.posX) + ", "
                     + jStr("posY") + ": " + std::to_string(e.posY) + ", "
                     + jStr("polyOut") + ": " + std::to_string(maxOutCh) + ", "
                     + jStr("paramCount") + ": " + std::to_string(m->params.size()) + ", "
                     + jStr("inputCount") + ": " + std::to_string(m->inputs.size()) + ", "
                     + jStr("outputCount") + ": " + std::to_string(m->outputs.size()) + "}";
            if (i + 1 < ids.size()) summary += ",";
            summary += "\n";

            // compact voltage snapshot
            vjson += "  {" + jStr("id") + ": " + std::to_string(ids[i]) + ", "
                   + jStr("model") + ": " + jStr(e.model) + ", "
                   + jStr("outputs") + ": [";
            for (size_t j = 0; j < m->outputs.size(); j++) {
                std::string nm;
                if (j < m->outputInfos.size() && m->outputInfos[j]) nm = m->outputInfos[j]->name;
                float peak = (mv && j < mv->outputs.size()) ? mv->outputs[j].peak : 0.f;
                int ochs = m->outputs[j].getChannels();
                vjson += "{" + jStr("id") + ": " + std::to_string(j) + ", "
                       + jStr("name") + ": " + jStr(nm) + ", "
                       + jStr("ch") + ": " + std::to_string(ochs) + ", "
                       + jStr("v") + ": " + jNum(m->outputs[j].getVoltage()) + ", "
                       + jStr("peak") + ": " + jNum(peak) + ", "
                       + jStr("connected") + ": " + (m->outputs[j].isConnected() ? "true" : "false") + "}";
                if (j + 1 < m->outputs.size()) vjson += ", ";
            }
            vjson += "]}";
            if (i + 1 < ids.size()) vjson += ",";
            vjson += "\n";
        }
        json += "]";
        summary += "]";
        vjson += "]";

        // Scope JSON
        std::string sjson = "{";
        bool sfirst = true;
        for (int64_t id : ids) {
            engine::Module* m = APP->engine->getModule(id);
            if (!m) continue;
            auto it = voltTrack.find(id);
            if (it == voltTrack.end()) continue;
            ModuleVolts& mv = it->second;
            if (!sfirst) sjson += ", ";
            sfirst = false;
            sjson += jStr(std::to_string(id)) + ": {";
            sjson += jStr("model") + ": " + jStr(m->model ? m->model->slug : "?") + ", ";

            sjson += jStr("inputs") + ": [";
            for (size_t j = 0; j < mv.inputs.size() && j < m->inputs.size(); j++) {
                if (j > 0) sjson += ", ";
                std::string nm = (j < m->inputInfos.size() && m->inputInfos[j]) ? m->inputInfos[j]->name : "";
                sjson += "{" + jStr("id") + ": " + std::to_string(j) + ", " + jStr("name") + ": " + jStr(nm) + ", ";
                sjson += jStr("peak") + ": " + jNum(mv.inputs[j].peak) + ", ";
                sjson += jStr("history") + ": [";
                int cnt = std::min(HISTORY_LEN, mv.inputs[j].histIdx);
                for (int k = 0; k < cnt; k++) { if (k>0) sjson+=", "; sjson += jNum(mv.inputs[j].history[(mv.inputs[j].histIdx-cnt+k)%HISTORY_LEN]); }
                sjson += "]}";
            }
            sjson += "], ";

            sjson += jStr("outputs") + ": [";
            for (size_t j = 0; j < mv.outputs.size() && j < m->outputs.size(); j++) {
                if (j > 0) sjson += ", ";
                std::string nm = (j < m->outputInfos.size() && m->outputInfos[j]) ? m->outputInfos[j]->name : "";
                sjson += "{" + jStr("id") + ": " + std::to_string(j) + ", " + jStr("name") + ": " + jStr(nm) + ", ";
                sjson += jStr("peak") + ": " + jNum(mv.outputs[j].peak) + ", ";
                sjson += jStr("history") + ": [";
                int cnt = std::min(HISTORY_LEN, mv.outputs[j].histIdx);
                for (int k = 0; k < cnt; k++) { if (k>0) sjson+=", "; sjson += jNum(mv.outputs[j].history[(mv.outputs[j].histIdx-cnt+k)%HISTORY_LEN]); }
                sjson += "]}";
            }
            sjson += "]}";
        }
        sjson += "}";

        // Reset peaks
        for (auto& kv : voltTrack) {
            for (auto& pv : kv.second.inputs)  pv.peak = 0.f;
            for (auto& pv : kv.second.outputs) pv.peak = 0.f;
        }

        // Cable + graph JSON
        auto modSlug    = [&](int64_t mid) { engine::Module* mm=APP->engine->getModule(mid); return (mm&&mm->model)?mm->model->slug:std::string("?"); };
        auto outPortNm  = [&](int64_t mid, int pid) -> std::string { engine::Module* mm=APP->engine->getModule(mid); if(!mm||pid<0||pid>=(int)mm->outputInfos.size()) return ""; return mm->outputInfos[pid]?mm->outputInfos[pid]->name:""; };
        auto inPortNm   = [&](int64_t mid, int pid) -> std::string { engine::Module* mm=APP->engine->getModule(mid); if(!mm||pid<0||pid>=(int)mm->inputInfos.size()) return ""; return mm->inputInfos[pid]?mm->inputInfos[pid]->name:""; };

        std::string cjson="[", gjson="[";
        bool cfirst=true, gfirst=true;
        auto allCableIds = APP->engine->getCableIds();
        cachedCableCount = (int)allCableIds.size();
        for (int64_t cid : allCableIds) {
            engine::Cable* c = APP->engine->getCable(cid);
            if (!c) continue;
            int64_t omid = c->outputModule ? c->outputModule->id : -1;
            int64_t imid = c->inputModule  ? c->inputModule->id  : -1;
            std::string fromMod=modSlug(omid), fromPort=outPortNm(omid,c->outputId);
            std::string toMod=modSlug(imid),   toPort=inPortNm(imid,c->inputId);
            std::string label = fromMod+":"+fromPort+" → "+toMod+":"+toPort;
            if (!cfirst) cjson+=", "; cfirst=false;
            cjson += "{" + jStr("id")+": "+std::to_string(cid)+", "
                         + jStr("outputModuleId")+": "+std::to_string(omid)+", "
                         + jStr("outputModule")+": "+jStr(fromMod)+", "
                         + jStr("outputPortId")+": "+std::to_string(c->outputId)+", "
                         + jStr("outputPortName")+": "+jStr(fromPort)+", "
                         + jStr("inputModuleId")+": "+std::to_string(imid)+", "
                         + jStr("inputModule")+": "+jStr(toMod)+", "
                         + jStr("inputPortId")+": "+std::to_string(c->inputId)+", "
                         + jStr("inputPortName")+": "+jStr(toPort)+", "
                         + jStr("label")+": "+jStr(label)+"}";
            if (!gfirst) gjson+=", "; gfirst=false;
            gjson += jStr(label);
        }
        cjson+="]"; gjson+="]";

        std::unique_lock<std::mutex> lock(cacheMtx);
        cache=std::move(newCache); moduleListJson=std::move(json); moduleSummaryJson=std::move(summary);
        cableListJson=std::move(cjson); graphJson=std::move(gjson); scopeJson=std::move(sjson);
        voltagesJson=std::move(vjson);
        // update patch info cache (safe: called from UI thread)
        std::string ppath = APP->patch->path;
        bool hsp = !ppath.empty();
        patchInfoJson = "{" + jStr("path") + ": " + jStr(ppath) + ", "
                      + jStr("hasSavePath") + ": " + (hsp ? "true" : "false") + "}";
    }

    // ── UI-thread: job processors ─────────────────────────────────────────────
    void processSetQueue() {
        std::shared_ptr<SetParamJob> job;
        { std::unique_lock<std::mutex> lk(setQueueMtx); if (!setQueue.empty()) { job=setQueue.front(); setQueue.pop(); } }
        if (!job) return;
        engine::Module* m = APP->engine->getModule(job->moduleId);
        if (m && job->paramId>=0 && job->paramId<(int)m->params.size()) {
            float v=job->value;
            ParamQuantity* pq=m->getParamQuantity(job->paramId);
            if (pq) v=rack::math::clamp(v,pq->getMinValue(),pq->getMaxValue());
            APP->engine->setParamValue(m,job->paramId,v);
            job->success=true;
        }
        job->done=true;
    }

    void processCableQueue() {
        std::shared_ptr<CableJob> job;
        { std::unique_lock<std::mutex> lk(cableQueueMtx); if (!cableQueue.empty()) { job=cableQueue.front(); cableQueue.pop(); } }
        if (!job) return;
        if (job->type==CableJob::ADD) {
            ModuleWidget* outW=APP->scene->rack->getModule(job->outModId);
            ModuleWidget* inW =APP->scene->rack->getModule(job->inModId);
            engine::Module* src=APP->engine->getModule(job->outModId);
            engine::Module* dst=APP->engine->getModule(job->inModId);
            if (outW&&inW&&src&&dst) {
                engine::Cable* c=new engine::Cable;
                c->outputModule=src; c->outputId=(int)job->outPortId;
                c->inputModule=dst;  c->inputId=(int)job->inPortId;
                APP->engine->addCable(c);
                CableWidget* cw=new CableWidget;
                cw->setCable(c);
                if (!cw->isComplete()) {
                    APP->engine->removeCable(cw->releaseCable()); delete cw;
                    job->error="cable incomplete: port not found (outPort="+std::to_string(job->outPortId)+" inPort="+std::to_string(job->inPortId)+")";
                } else { APP->scene->rack->addCable(cw); job->success=true; }
            } else { job->error="module not found"; }
        } else if (job->type==CableJob::REMOVE) {
            ModuleWidget* inW=APP->scene->rack->getModule(job->inModId);
            if (inW) {
                PortWidget* inPort=inW->getInput((int)job->inPortId);
                if (inPort) { APP->scene->rack->clearCablesOnPort(inPort); job->success=true; }
                else { job->error="port not found"; }
            } else { job->error="module not found"; }
        } else { // REMOVE_OUTPUT — disconnect all cables from one output port
            // Collect input locations first, then remove (avoid modifying list while iterating)
            std::vector<std::pair<int64_t,int>> targets;
            for (int64_t cid : APP->engine->getCableIds()) {
                engine::Cable* c = APP->engine->getCable(cid);
                if (c && c->outputModule && c->outputModule->id==job->outModId
                    && c->outputId==(int)job->outPortId && c->inputModule)
                    targets.push_back({c->inputModule->id, c->inputId});
            }
            for (auto& t : targets) {
                ModuleWidget* inW = APP->scene->rack->getModule(t.first);
                if (!inW) continue;
                PortWidget* inPort = inW->getInput(t.second);
                if (inPort) APP->scene->rack->clearCablesOnPort(inPort);
            }
            job->success=true;
        }
        job->done=true;
    }

    void processAddQueue() {
        std::shared_ptr<AddModuleJob> job;
        { std::unique_lock<std::mutex> lk(addQueueMtx); if (!addQueue.empty()) { job=addQueue.front(); addQueue.pop(); } }
        if (!job) return;
        plugin::Model* model=plugin::getModel(job->pluginSlug,job->modelSlug);
        if (!model) { job->error="model not found: "+job->pluginSlug+"/"+job->modelSlug; }
        else {
            try {
                engine::Module* m=model->createModule(); APP->engine->addModule(m);
                ModuleWidget* mw=model->createModuleWidget(m); APP->scene->rack->addModule(mw);
                float sumX=0.f,sumY=0.f; int cnt=0;
                for (int64_t oid : APP->engine->getModuleIds()) {
                    if (oid==m->id) continue;
                    ModuleWidget* omw=APP->scene->rack->getModule(oid);
                    if (omw) { sumX+=omw->box.pos.x+omw->box.size.x*0.5f; sumY+=omw->box.pos.y+omw->box.size.y*0.5f; cnt++; }
                }
                math::Vec target=cnt>0?math::Vec(sumX/cnt,sumY/cnt):math::Vec(0.f,0.f);
                APP->scene->rack->setModulePosNearest(mw,target);
                job->newId=m->id; job->success=true;
            } catch (std::exception& e) { job->error=e.what(); }
        }
        job->done=true;
    }

    void processDeleteQueue() {
        std::shared_ptr<DeleteModuleJob> job;
        { std::unique_lock<std::mutex> lk(deleteQueueMtx); if (!deleteQueue.empty()) { job=deleteQueue.front(); deleteQueue.pop(); } }
        if (!job) return;
        ModuleWidget* mw = APP->scene->rack->getModule(job->moduleId);
        if (!mw) { job->error="module not found"; job->done=true; return; }
        engine::Module* m = mw->module;
        // removeModule: disconnects cables + removes mw from scene (no engine removal)
        APP->scene->rack->removeModule(mw);
        // Remove module from engine
        APP->engine->removeModule(m);
        // Memory intentionally NOT freed here (leak test — to isolate crash source)
        job->success=true; job->done=true;
    }

    void processMoveQueue() {
        std::shared_ptr<MoveModuleJob> job;
        { std::unique_lock<std::mutex> lk(moveQueueMtx); if (!moveQueue.empty()) { job=moveQueue.front(); moveQueue.pop(); } }
        if (!job) return;
        ModuleWidget* mw = APP->scene->rack->getModule(job->moduleId);
        if (!mw) { job->error="module not found"; job->done=true; return; }
        // RACK_GRID_WIDTH = 15px per HP
        APP->scene->rack->setModulePosNearest(mw, math::Vec(job->hp * RACK_GRID_WIDTH, 0.f));
        job->success=true; job->done=true;
    }

    void processBypassQueue() {
        std::shared_ptr<BypassJob> job;
        { std::unique_lock<std::mutex> lk(bypassQueueMtx); if (!bypassQueue.empty()) { job=bypassQueue.front(); bypassQueue.pop(); } }
        if (!job) return;
        engine::Module* m = APP->engine->getModule(job->moduleId);
        if (!m) { job->error="module not found"; job->done=true; return; }
        APP->engine->bypassModule(m, job->bypassed);
        job->success=true; job->done=true;
    }

    void processStateQueue() {
        std::shared_ptr<ModuleStateJob> job;
        { std::unique_lock<std::mutex> lk(stateQueueMtx); if (!stateQueue.empty()) { job=stateQueue.front(); stateQueue.pop(); } }
        if (!job) return;
        engine::Module* m = APP->engine->getModule(job->moduleId);
        if (!m) { job->error="module not found"; job->done=true; return; }
        if (job->type == ModuleStateJob::GET) {
            json_t* rootJ = m->toJson();
            if (!rootJ) { job->error="toJson() returned null"; job->done=true; return; }
            char* str = json_dumps(rootJ, JSON_COMPACT);
            job->stateJson = str ? std::string(str) : "{}";
            if (str) free(str);
            json_decref(rootJ);
        } else {
            json_error_t jerr;
            json_t* rootJ = json_loads(job->stateJson.c_str(), 0, &jerr);
            if (!rootJ) { job->error = std::string("JSON parse: ") + jerr.text; job->done=true; return; }
            m->fromJson(rootJ);
            json_decref(rootJ);
        }
        job->success=true; job->done=true;
    }

    void processPatchSaveQueue() {
        std::shared_ptr<PatchSaveJob> job;
        { std::unique_lock<std::mutex> lk(patchSaveQueueMtx); if (!patchSaveQueue.empty()) { job=patchSaveQueue.front(); patchSaveQueue.pop(); } }
        if (!job) return;
        if (APP->patch->path.empty()) {
            job->error = "patch has no file path (File > Save As in VCV Rack first)";
        } else {
            job->savedPath = APP->patch->path;
            APP->patch->save(APP->patch->path);
            job->success = true;
        }
        job->done = true;
    }

    template<typename T>
    static bool waitDone(std::shared_ptr<T>& job, int ms=500) {
        auto dl=std::chrono::steady_clock::now()+std::chrono::milliseconds(ms);
        while (!job->done.load() && std::chrono::steady_clock::now()<dl)
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        return job->done.load();
    }

    // ── HTTP routes ───────────────────────────────────────────────────────────
    void setupRoutes() {
        svr.Get("/status", [this](const httplib::Request&, httplib::Response& res) {
            std::string b = "{"+jStr("running")+": "+(serverRunning?"true":"false")+", "+jStr("port")+": 7777, "+jStr("version")+": "+jStr("2.9.0")+"}";
            res.set_content(b,"application/json");
        });

        svr.Get("/modules",  [this](const httplib::Request&, httplib::Response& res){ std::unique_lock<std::mutex> lk(cacheMtx); res.set_content(moduleListJson,"application/json"); });
        svr.Get("/modules/summary", [this](const httplib::Request&, httplib::Response& res){ std::unique_lock<std::mutex> lk(cacheMtx); res.set_content(moduleSummaryJson,"application/json"); });
        svr.Get(R"(/modules/(\d+)$)", [this](const httplib::Request& r, httplib::Response& res){
            int64_t modId = std::stoll(r.matches[1].str());
            std::unique_lock<std::mutex> lk(cacheMtx);
            std::string needle = jStr("id") + ": " + std::to_string(modId) + ",";
            auto pos = moduleListJson.find(needle);
            if (pos == std::string::npos) { res.set_content("{\"error\":\"module not found\"}","application/json"); return; }
            auto start = moduleListJson.rfind('{', pos);
            if (start == std::string::npos) { res.set_content("{\"error\":\"parse error\"}","application/json"); return; }
            int depth = 0; size_t end = start;
            for (; end < moduleListJson.size(); end++) {
                if (moduleListJson[end] == '{') depth++;
                else if (moduleListJson[end] == '}') { depth--; if (!depth) break; }
            }
            res.set_content(moduleListJson.substr(start, end - start + 1), "application/json");
        });
        svr.Get("/cables",   [this](const httplib::Request&, httplib::Response& res){ std::unique_lock<std::mutex> lk(cacheMtx); res.set_content(cableListJson,"application/json"); });
        svr.Get("/graph",    [this](const httplib::Request&, httplib::Response& res){ std::unique_lock<std::mutex> lk(cacheMtx); res.set_content(graphJson,"application/json"); });
        svr.Get("/scope",    [this](const httplib::Request&, httplib::Response& res){ std::unique_lock<std::mutex> lk(cacheMtx); res.set_content(scopeJson,"application/json"); });

        svr.Get(R"(/scope/(\d+))", [this](const httplib::Request& r, httplib::Response& res){
            int64_t modId=std::stoll(r.matches[1].str());
            std::unique_lock<std::mutex> lk(cacheMtx);
            std::string key="\""+std::to_string(modId)+"\"";
            auto pos=scopeJson.find(key);
            if (pos==std::string::npos){ res.set_content("{\"error\":\"module not found\"}","application/json"); return; }
            auto start=scopeJson.find("{",pos+key.size());
            if (start==std::string::npos){ res.set_content("{\"error\":\"parse error\"}","application/json"); return; }
            int depth=0; size_t end=start;
            for (;end<scopeJson.size();end++){ if(scopeJson[end]=='{') depth++; else if(scopeJson[end]=='}'){ depth--; if(!depth) break; } }
            res.set_content(scopeJson.substr(start,end-start+1),"application/json");
        });

        // ── GET /audio/{port} — audio-rate analysis ───────────────────────────
        // port 0 = AUDIO_IN_L, port 1 = AUDIO_IN_R
        // Connect a cable from any module's output into the Bridge's ANALYZE input
        // to get real-time frequency analysis.
        svr.Get(R"(/audio/(\d+))", [this](const httplib::Request& r, httplib::Response& res){
            int port = std::stoi(r.matches[1].str());
            if (port < 0 || port >= 2) {
                res.set_content("{\"error\": \"port must be 0 (L) or 1 (R)\"}", "application/json");
                return;
            }

            // Snapshot ring buffer
            float buf[AUDIO_BUF];
            {
                int h = audioRing[port].head.load(std::memory_order_relaxed);
                for (int i = 0; i < AUDIO_BUF; i++)
                    buf[i] = audioRing[port].data[(h - AUDIO_BUF + i + AUDIO_BUF) & (AUDIO_BUF - 1)];
            }
            float sr = sampleRate;

            // RMS + peak
            float sumSq = 0.f, pk = 0.f;
            for (int i = 0; i < AUDIO_BUF; i++) {
                sumSq += buf[i] * buf[i];
                float av = std::fabs(buf[i]);
                if (av > pk) pk = av;
            }
            float rms = sqrtf(sumSq / AUDIO_BUF);

            // Spectrum: DFT at 20–2000 Hz in 20 Hz steps, stride-8 sampling
            // Stride-8 → effective sr = 44100/8 = 5512 Hz (Nyquist = 2756 Hz, covers our range)
            const int STRIDE = 8;
            const int N      = AUDIO_BUF / STRIDE;  // 512 samples per DFT
            float strSr      = sr / STRIDE;

            float bestMag  = 0.f;
            float bestFreq = 0.f;
            std::string spec = "[";
            bool sfirst = true;

            for (int fi = 1; fi <= 100; fi++) {           // 20, 40, 60 … 2000 Hz
                float freq = fi * 20.f;
                if (freq > strSr * 0.5f) break;            // above Nyquist → skip
                float re = 0.f, im = 0.f;
                float k  = 2.0f * float(M_PI) * freq / strSr;
                for (int i = 0; i < N; i++) {
                    float v = buf[i * STRIDE];
                    re += v * cosf(k * i);
                    im += v * sinf(k * i);
                }
                float mag = sqrtf(re*re + im*im) / N;
                if (mag > bestMag) { bestMag = mag; bestFreq = freq; }
                if (!sfirst) spec += ", ";
                sfirst = false;
                spec += "{" + jStr("hz") + ": " + std::to_string((int)freq)
                           + ", " + jStr("mag") + ": " + std::to_string(mag) + "}";
            }
            spec += "]";

            std::string note = freqToNote(bestFreq);

            std::string body = "{";
            body += jStr("port")         + ": " + std::to_string(port) + ", ";
            body += jStr("rms")          + ": " + std::to_string(rms) + ", ";
            body += jStr("peak")         + ": " + std::to_string(pk) + ", ";
            body += jStr("dominantHz")   + ": " + std::to_string(bestFreq) + ", ";
            body += jStr("dominantNote") + ": " + jStr(note) + ", ";
            body += jStr("spectrum")     + ": " + spec;
            body += "}";
            res.set_content(body, "application/json");
        });

        svr.Get(R"(/modules/(\d+)/params/(\d+))",
            [this](const httplib::Request& r, httplib::Response& res){
                int64_t modId=std::stoll(r.matches[1].str()); int pid=std::stoi(r.matches[2].str());
                std::unique_lock<std::mutex> lk(cacheMtx);
                for (auto& e:cache) {
                    if (e.id==modId && pid<(int)e.params.size()) {
                        const ParamEntry& pe = e.params[pid];
                        std::string b = "{";
                        b += jStr("module_id")+": "+std::to_string(modId)+", ";
                        b += jStr("param_id")+": "+std::to_string(pid)+", ";
                        b += jStr("name")+": "+jStr(pe.name)+", ";
                        b += jStr("value")+": "+jNum(pe.value)+", ";
                        b += jStr("min")+": "+jNum(pe.minV)+", ";
                        b += jStr("max")+": "+jNum(pe.maxV)+", ";
                        b += jStr("default")+": "+jNum(pe.defaultV)+", ";
                        b += jStr("unit")+": "+jStr(pe.unit);
                        b += "}";
                        res.set_content(b,"application/json"); return;
                    }
                }
                res.set_content("{\"error\":\"not found\"}","application/json");
            });

        svr.Post("/modules", [this](const httplib::Request& r, httplib::Response& res){
            auto job=std::make_shared<AddModuleJob>();
            job->pluginSlug=parseStringField(r.body,"plugin"); job->modelSlug=parseStringField(r.body,"model");
            { std::unique_lock<std::mutex> lk(addQueueMtx); addQueue.push(job); }
            if (!waitDone(job,2000)) res.set_content("{\"error\":\"timeout\"}","application/json");
            else if (job->success) res.set_content("{\"ok\":true,\"id\":"+std::to_string(job->newId)+"}","application/json");
            else res.set_content("{\"error\":"+jStr(job->error)+"}","application/json");
        });

        svr.Post(R"(/modules/(\d+)/params/(\d+))",
            [this](const httplib::Request& r, httplib::Response& res){
                auto job=std::make_shared<SetParamJob>();
                job->moduleId=std::stoll(r.matches[1].str()); job->paramId=std::stoi(r.matches[2].str()); job->value=parseFloatField(r.body,"value");
                { std::unique_lock<std::mutex> lk(setQueueMtx); setQueue.push(job); }
                if (!waitDone(job)) res.set_content("{\"error\":\"timeout\"}","application/json");
                else if (job->success) res.set_content("{\"ok\":true,\"value\":"+std::to_string(job->value)+"}","application/json");
                else res.set_content("{\"error\":\"param not found\"}","application/json");
            });

        svr.Post("/cables", [this](const httplib::Request& r, httplib::Response& res){
            auto job=std::make_shared<CableJob>(); job->type=CableJob::ADD;
            job->outModId=parseInt64Field(r.body,"outputModuleId"); job->outPortId=parseInt64Field(r.body,"outputPortId");
            job->inModId=parseInt64Field(r.body,"inputModuleId");   job->inPortId=parseInt64Field(r.body,"inputPortId");
            { std::unique_lock<std::mutex> lk(cableQueueMtx); cableQueue.push(job); }
            if (!waitDone(job)) res.set_content("{\"error\":\"timeout\"}","application/json");
            else if (job->success) res.set_content("{\"ok\":true}","application/json");
            else res.set_content("{\"error\":\""+job->error+"\"}","application/json");
        });

        svr.Post("/cables/disconnect", [this](const httplib::Request& r, httplib::Response& res){
            auto job=std::make_shared<CableJob>(); job->type=CableJob::REMOVE;
            job->inModId=parseInt64Field(r.body,"inputModuleId"); job->inPortId=parseInt64Field(r.body,"inputPortId");
            { std::unique_lock<std::mutex> lk(cableQueueMtx); cableQueue.push(job); }
            if (!waitDone(job)) res.set_content("{\"error\":\"timeout\"}","application/json");
            else if (job->success) res.set_content("{\"ok\":true}","application/json");
            else res.set_content("{\"error\":\""+job->error+"\"}","application/json");
        });

        svr.Post("/cables/disconnect-output", [this](const httplib::Request& r, httplib::Response& res){
            auto job=std::make_shared<CableJob>(); job->type=CableJob::REMOVE_OUTPUT;
            job->outModId=parseInt64Field(r.body,"outputModuleId"); job->outPortId=parseInt64Field(r.body,"outputPortId");
            { std::unique_lock<std::mutex> lk(cableQueueMtx); cableQueue.push(job); }
            if (!waitDone(job)) res.set_content("{\"error\":\"timeout\"}","application/json");
            else if (job->success) res.set_content("{\"ok\":true}","application/json");
            else res.set_content("{\"error\":\""+job->error+"\"}","application/json");
        });

        svr.Delete(R"(/modules/(\d+))", [this](const httplib::Request& r, httplib::Response& res){
            auto job=std::make_shared<DeleteModuleJob>();
            job->moduleId=std::stoll(r.matches[1].str());
            { std::unique_lock<std::mutex> lk(deleteQueueMtx); deleteQueue.push(job); }
            if (!waitDone(job)) res.set_content("{\"error\":\"timeout\"}","application/json");
            else if (job->success) res.set_content("{\"ok\":true}","application/json");
            else res.set_content("{\"error\":"+jStr(job->error)+"}","application/json");
        });

        svr.Post(R"(/modules/(\d+)/position)", [this](const httplib::Request& r, httplib::Response& res){
            auto job=std::make_shared<MoveModuleJob>();
            job->moduleId=std::stoll(r.matches[1].str());
            job->hp=parseFloatField(r.body,"hp");
            { std::unique_lock<std::mutex> lk(moveQueueMtx); moveQueue.push(job); }
            if (!waitDone(job)) res.set_content("{\"error\":\"timeout\"}","application/json");
            else if (job->success) res.set_content("{\"ok\":true}","application/json");
            else res.set_content("{\"error\":"+jStr(job->error)+"}","application/json");
        });

        svr.Get("/perf", [this](const httplib::Request&, httplib::Response& res){
            double blockDur; int blockFr, modCnt, cableCnt;
            { std::unique_lock<std::mutex> lk(cacheMtx);
              blockDur=cachedBlockDurationMs; blockFr=cachedBlockFrames;
              modCnt=cachedModuleCount; cableCnt=cachedCableCount; }
            struct rusage ru; getrusage(RUSAGE_SELF, &ru);
            double userSec = ru.ru_utime.tv_sec + ru.ru_utime.tv_usec/1e6;
            double sysSec  = ru.ru_stime.tv_sec + ru.ru_stime.tv_usec/1e6;
            std::string b = "{";
            b += jStr("sampleRate")+": "+std::to_string(sampleRate)+", ";
            b += jStr("blockFrames")+": "+std::to_string(blockFr)+", ";
            b += jStr("blockDurationMs")+": "+std::to_string(blockDur)+", ";
            b += jStr("moduleCount")+": "+std::to_string(modCnt)+", ";
            b += jStr("cableCount")+": "+std::to_string(cableCnt)+", ";
            b += jStr("processCpuUserSec")+": "+std::to_string(userSec)+", ";
            b += jStr("processCpuSysSec")+": "+std::to_string(sysSec);
            b += "}";
            res.set_content(b,"application/json");
        });
        svr.Post(R"(/modules/(\d+)/bypass)", [this](const httplib::Request& r, httplib::Response& res){
            auto job=std::make_shared<BypassJob>();
            job->moduleId=std::stoll(r.matches[1].str());
            job->bypassed=parseBoolField(r.body,"bypassed");
            { std::unique_lock<std::mutex> lk(bypassQueueMtx); bypassQueue.push(job); }
            if (!waitDone(job)) res.set_content("{\"error\":\"timeout\"}","application/json");
            else if (job->success) res.set_content("{\"ok\":true,\"bypassed\":"+std::string(job->bypassed?"true":"false")+"}","application/json");
            else res.set_content("{\"error\":"+jStr(job->error)+"}","application/json");
        });

        svr.Get(R"(/modules/(\d+)/state)", [this](const httplib::Request& r, httplib::Response& res){
            auto job=std::make_shared<ModuleStateJob>(); job->type=ModuleStateJob::GET;
            job->moduleId=std::stoll(r.matches[1].str());
            { std::unique_lock<std::mutex> lk(stateQueueMtx); stateQueue.push(job); }
            if (!waitDone(job,2000)) res.set_content("{\"error\":\"timeout\"}","application/json");
            else if (job->success) res.set_content(job->stateJson,"application/json");
            else res.set_content("{\"error\":"+jStr(job->error)+"}","application/json");
        });

        svr.Post(R"(/modules/(\d+)/state)", [this](const httplib::Request& r, httplib::Response& res){
            auto job=std::make_shared<ModuleStateJob>(); job->type=ModuleStateJob::SET;
            job->moduleId=std::stoll(r.matches[1].str());
            job->stateJson=r.body;
            { std::unique_lock<std::mutex> lk(stateQueueMtx); stateQueue.push(job); }
            if (!waitDone(job,2000)) res.set_content("{\"error\":\"timeout\"}","application/json");
            else if (job->success) res.set_content("{\"ok\":true}","application/json");
            else res.set_content("{\"error\":"+jStr(job->error)+"}","application/json");
        });

        svr.Post("/patch/save", [this](const httplib::Request&, httplib::Response& res){
            auto job=std::make_shared<PatchSaveJob>();
            { std::unique_lock<std::mutex> lk(patchSaveQueueMtx); patchSaveQueue.push(job); }
            if (!waitDone(job,5000)) res.set_content("{\"error\":\"timeout\"}","application/json");
            else if (job->success) res.set_content("{\"ok\":true,\"path\":"+jStr(job->savedPath)+"}","application/json");
            else res.set_content("{\"error\":"+jStr(job->error)+"}","application/json");
        });

        svr.Get("/patch", [this](const httplib::Request&, httplib::Response& res){
            std::unique_lock<std::mutex> lk(cacheMtx);
            res.set_content(patchInfoJson, "application/json");
        });

        svr.Get("/modules/voltages", [this](const httplib::Request&, httplib::Response& res){
            std::unique_lock<std::mutex> lk(cacheMtx);
            res.set_content(voltagesJson, "application/json");
        });

        svr.Get("/library", [](const httplib::Request& req, httplib::Response& res){
            std::string qFilter = req.has_param("q") ? req.get_param_value("q") : "";
            std::string pFilter = req.has_param("plugin") ? req.get_param_value("plugin") : "";
            // lowercase helper
            auto toLower = [](std::string s){ for(auto& c:s) c=tolower(c); return s; };
            std::string qLow = toLower(qFilter);

            std::string json = "[";
            bool pfirst = true;
            for (plugin::Plugin* plug : plugin::plugins) {
                if (!pFilter.empty() && plug->slug != pFilter) continue;
                // collect matching models
                std::string mJson = "[";
                bool mfirst = true;
                for (plugin::Model* mod : plug->models) {
                    if (!qLow.empty()) {
                        std::string slugL=toLower(mod->slug), nameL=toLower(mod->name);
                        if (slugL.find(qLow)==std::string::npos && nameL.find(qLow)==std::string::npos) continue;
                    }
                    if (!mfirst) mJson += ",";
                    mfirst = false;
                    mJson += "{" + jStr("slug") + ": " + jStr(mod->slug) + ", "
                               + jStr("name") + ": " + jStr(mod->name) + "}";
                }
                mJson += "]";
                if (mfirst && !qLow.empty()) continue; // no models matched
                if (!pfirst) json += ",";
                pfirst = false;
                json += "{" + jStr("slug") + ": " + jStr(plug->slug) + ", "
                           + jStr("name") + ": " + jStr(plug->name) + ", "
                           + jStr("models") + ": " + mJson + "}";
            }
            json += "]";
            res.set_content(json, "application/json");
        });
    }

    void startServer() {
        if (serverRunning) return;
        serverRunning=true;
        serverThread=std::thread([this](){ svr.listen("127.0.0.1",7777); serverRunning=false; });
        serverThread.detach();
    }
    void stopServer() { if (!serverRunning) return; svr.stop(); serverRunning=false; }
};

// ── Colors ────────────────────────────────────────────────────────────────────
static const NVGcolor WHITE = nvgRGB(255,255,255);
static const NVGcolor DIM   = nvgRGB(80,80,80);
static const NVGcolor GOLD  = nvgRGB(160,120,0);

// ── Sphere (status orb) — layered NanoVG radial gradients ────────────────────
static void drawSphere(NVGcontext* vg, float cx, float cy, float r, bool on) {
    // Layer 1: black base
    nvgBeginPath(vg); nvgCircle(vg, cx, cy, r);
    nvgFillColor(vg, nvgRGB(0,0,0)); nvgFill(vg);

    // Layer 2: body light — offset center, bright color → transparent at rim
    float hx = cx - r*0.20f, hy = cy - r*0.24f;
    NVGpaint body = nvgRadialGradient(vg, hx, hy, r*0.05f, r*1.35f,
        on ? nvgRGBA(68,255,136,255) : nvgRGBA(255,68,34,255),
        nvgRGBA(0,0,0,0));
    nvgBeginPath(vg); nvgCircle(vg, cx, cy, r);
    nvgFillPaint(vg, body); nvgFill(vg);

    // Layer 3: rim shadow — transparent center → black at edge
    NVGpaint rim = nvgRadialGradient(vg, cx, cy, r*0.45f, r,
        nvgRGBA(0,0,0,0), nvgRGBA(0,0,0,210));
    nvgBeginPath(vg); nvgCircle(vg, cx, cy, r);
    nvgFillPaint(vg, rim); nvgFill(vg);

    // Layer 4: specular highlight — top-left
    float sx = cx - r*0.28f, sy = cy - r*0.32f;
    NVGpaint spec = nvgRadialGradient(vg, sx, sy, 0.f, r*0.50f,
        on ? nvgRGBA(180,255,200,100) : nvgRGBA(255,180,150,100),
        nvgRGBA(0,0,0,0));
    nvgBeginPath(vg); nvgCircle(vg, cx, cy, r);
    nvgFillPaint(vg, spec); nvgFill(vg);
}

// ── Widget ────────────────────────────────────────────────────────────────────
struct NTRWabotWidget : ModuleWidget {
    int uiTimer = 0;

    void step() override {
        ModuleWidget::step();
        if (!module) return;
        NTRWabot* m = static_cast<NTRWabot*>(module);
        m->updateVoltages();
        if (++uiTimer >= 60) { uiTimer=0; m->updateCache(); }
        m->processSetQueue();
        m->processCableQueue();
        m->processAddQueue();
        m->processDeleteQueue();
        m->processMoveQueue();
        m->processBypassQueue();
        m->processStateQueue();
        m->processPatchSaveQueue();
    }

    NTRWabotWidget(NTRWabot* module) {
        setModule(module);
        setPanel(createPanel(asset::plugin(pluginInstance,"res/NTR_Wabot.svg")));
        addChild(createWidget<ScrewBlack>(Vec(0,0)));
        addChild(createWidget<ScrewBlack>(Vec(0,RACK_GRID_HEIGHT-RACK_GRID_WIDTH)));

        // Sphere replaces STATUS LED — drawn in draw()
        addParam(createParamCentered<TL1105>(mm2px(Vec(23.f,77.f)), module, NTRWabot::START_PARAM));

        // Audio analysis inputs
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(8.f,110.f)),  module, NTRWabot::AUDIO_IN_L));
        addInput(createInputCentered<PJ301MPort>(mm2px(Vec(22.f,110.f)), module, NTRWabot::AUDIO_IN_R));
    }

    void draw(const DrawArgs& args) override {
        ModuleWidget::draw(args);

        // Read server state (false in preview / no module)
        bool on = module ? static_cast<NTRWabot*>(module)->serverRunning.load() : false;

        float cx = mm2px(Vec(15.24f,0.f)).x;
        float lx = mm2px(Vec(4.5f,0.f)).x;

        // ── Zone 1: Identity ──────────────────────────────────────────────────
        // Sphere center y=17mm, r=7mm (bottom edge ≈ 24mm)
        drawSphere(args.vg, cx, mm2px(Vec(0.f,17.f)).y, mm2px(Vec(0.f,7.f)).y, on);

        std::shared_ptr<Font> font = APP->window->loadFont(
            asset::plugin(pluginInstance,"res/fonts/NTR_Switzer-Semibold.ttf"));
        if (!font) return;
        nvgFontFaceId(args.vg, font->handle);
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER|NVG_ALIGN_MIDDLE);

        // Text group centered between sphere bottom (24mm) and Port label (63mm) → midpoint 43.5mm
        nvgFontSize(args.vg,18.f); nvgFillColor(args.vg,WHITE);
        nvgText(args.vg, cx, mm2px(Vec(0.f,40.f)).y, "Wabot", NULL);

        nvgFontSize(args.vg,7.8f); nvgFillColor(args.vg,WHITE);
        nvgText(args.vg, cx, mm2px(Vec(0.f,49.f)).y, "nevertrustrobots", NULL);

        // ── Zone 2: Control (y 60–80mm) ───────────────────────────────────────
        nvgFontSize(args.vg,8.f);
        nvgTextAlign(args.vg, NVG_ALIGN_LEFT|NVG_ALIGN_MIDDLE);
        nvgFillColor(args.vg,DIM);
        nvgText(args.vg, lx, mm2px(Vec(0.f,63.f)).y, "Port", NULL);
        nvgTextAlign(args.vg, NVG_ALIGN_RIGHT|NVG_ALIGN_MIDDLE);
        nvgFillColor(args.vg,WHITE);
        nvgText(args.vg, mm2px(Vec(26.f,0.f)).x, mm2px(Vec(0.f,63.f)).y, "7777", NULL);

        nvgTextAlign(args.vg, NVG_ALIGN_LEFT|NVG_ALIGN_MIDDLE);
        nvgFillColor(args.vg,WHITE);
        nvgText(args.vg, lx, mm2px(Vec(0.f,77.f)).y, "Start", NULL);

        // ── Zone 3: Input (y 97–123mm) ────────────────────────────────────────
        nvgFontSize(args.vg,7.f); nvgFillColor(args.vg,GOLD);
        nvgTextAlign(args.vg, NVG_ALIGN_CENTER|NVG_ALIGN_MIDDLE);
        nvgText(args.vg, mm2px(Vec(8.f,0.f)).x,  mm2px(Vec(0.f,100.f)).y, "L", NULL);
        nvgText(args.vg, mm2px(Vec(22.f,0.f)).x, mm2px(Vec(0.f,100.f)).y, "R", NULL);
        nvgFontSize(args.vg,5.5f); nvgFillColor(args.vg,nvgRGBA(160,120,0,128));
        nvgText(args.vg, cx, mm2px(Vec(0.f,120.5f)).y, "Analyze", NULL);
    }
};

Model* modelWabot = createModel<NTRWabot, NTRWabotWidget>("Wabot");
