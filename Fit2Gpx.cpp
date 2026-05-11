#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifdef _MSC_VER
// Required for static libcurl on Windows (schannel/sspi build)
#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Normaliz.lib")
#pragma comment(lib, "Wldap32.lib")
#pragma comment(lib, "Secur32.lib")
#pragma comment(lib, "Bcrypt.lib")
#pragma comment(lib, "Iphlpapi.lib")
#endif

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <utility>
#include <stdexcept>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <chrono>
#include <iomanip>
#include <set>
#include <map>

#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <tinyxml2.h>

// --- Garmin FIT SDK headers (in ./garmin_sdk) ---
#include "garmin_sdk/fit_decode.hpp"
#include "garmin_sdk/fit_mesg_broadcaster.hpp"
#include "garmin_sdk/fit_record_mesg.hpp"
#include "garmin_sdk/fit_mesg_listener.hpp"
#include "garmin_sdk/fit_runtime_exception.hpp"

using json = nlohmann::json;

// ===================== Utilities =====================

static inline double SemiCirclesToDegrees(int32_t sc) {
    return static_cast<double>(sc) * (180.0 / (double)(1ULL << 31));
}

// FIT epoch is 1989-12-31 00:00:00 UTC
static time_t FitEpochUtc() {
    std::tm tm_epoch{};
    tm_epoch.tm_year = 1989 - 1900;
    tm_epoch.tm_mon = 12 - 1;
    tm_epoch.tm_mday = 31;
    tm_epoch.tm_hour = 0;
    tm_epoch.tm_min = 0;
    tm_epoch.tm_sec = 0;
    return _mkgmtime(&tm_epoch); // UTC
}

static inline time_t FitToTimeT(uint32_t fitSeconds) {
    return FitEpochUtc() + static_cast<time_t>(fitSeconds);
}

static std::string ToIso8601Utc(time_t tt) {
    std::tm tm{};
    gmtime_s(&tm, &tt);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
        tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

static std::string DeriveGpxPath(const std::string& inPath) {
    const std::string::size_type sep = inPath.find_last_of("/\\");
    const std::string::size_type dot = inPath.rfind('.');
    bool hasExt = (dot != std::string::npos) && (sep == std::string::npos || dot > sep);
    if (hasExt) return inPath.substr(0, dot) + ".gpx";
    return inPath + ".gpx";
}

static std::vector<std::string> Split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == delim) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        }
        else cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// ===================== HTTP via libcurl =====================

static size_t CurlWriteToString(void* contents, size_t sz, size_t nmemb, void* userp) {
    size_t total = sz * nmemb;
    std::string* s = static_cast<std::string*>(userp);
    s->append(static_cast<const char*>(contents), total);
    return total;
}

static bool HttpPostJson(const std::string& url,
    const std::string& json_body,
    std::string& out_response,
    long timeout_ms = 10000) {
    CURL* curl = curl_easy_init();
    if (!curl) return false;

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)json_body.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &out_response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    return (res == CURLE_OK) && (http_code >= 200 && http_code < 300);
}

// ===================== Data structures =====================

struct TrackPoint {
    double latDeg = 0.0;
    double lonDeg = 0.0;

    double ele = 0.0; bool has_ele = false; // C++14: flags instead of std::optional
    unsigned hr = 0;  bool has_hr = false;
    unsigned cad = 0; bool has_cad = false;

    time_t t = 0;     bool has_time = false;
};

// ===================== Elevation (OpenTopoData) =====================

static const std::string OPENTOPODATA_BASE = "https://api.opentopodata.org/v1/";
static const size_t API_BATCH_SIZE = 100;

// Describe datasets: what they’re best for and where they cover (brief)
struct DatasetInfo {
    const char* id;
    const char* best_for;  // short description / strengths
    const char* where;     // coverage / region
};

static const DatasetInfo kDatasetInfos[] = {
    { "srtm90m",  "Global baseline ~90 m (3 arc-sec) SRTM; good default when detail not critical.",
                   "Global land roughly between 60°N and 56°S" },
    { "srtm30m",  "Higher-detail SRTM (~30 m, SRTMGL1 v3); fewer voids than older SRTM90.",
                   "Near-global land (same SRTM latitude limits)" },
    { "aster30m", "ASTER GDEM (~30 m); good detail in mountains but can have artifacts in forests/snow.",
                   "Global land" },
    { "eudem25m", "EU-DEM (~25 m); balanced & clean DEM for European Union.",
                   "Europe (EU/EEA)" },
    { "ned10m",   "US NED/3DEP (~10 m); higher detail—great for U.S. hiking/biking.",
                   "United States (CONUS, AK/HI varies)" },
    { "nzdem8m",  "NZ 8 m DEM; very detailed for local routes.",
                   "New Zealand" },
    { "etopo1",   "Coarse 1 arc-min (~1.8 km) topo+bathymetry; stable global fallback.",
                   "Global land+ocean" },
    { "gebco2020","Global bathymetry/topography (~15 arc-sec ~500 m); good near coasts/ocean.",
                   "Global land+ocean" },
    { "emod2018", "European marine bathymetry composite; better sea-floor detail than global sets.",
                   "European seas" },
    { "mapzen",   "Composite terrain tiles; reasonable global fallback where others are sparse.",
                   "Global (composite)" },
    { "bkg200",   "Germany DEM at ~200 m; light-weight background.",
                   "Germany" }
};
static const size_t kDatasetInfosCount = sizeof(kDatasetInfos) / sizeof(kDatasetInfos[0]);

static const std::set<std::string>& AllowedDatasets() {
    static std::set<std::string> s;
    if (s.empty()) {
        for (size_t i = 0; i < kDatasetInfosCount; ++i) s.insert(kDatasetInfos[i].id);
    }
    return s;
}

static bool ValidateDatasetsCSV(const std::string& csv, std::string& badToken) {
    auto parts = Split(csv, ',');
    if (parts.empty()) return false;
    const auto& allowed = AllowedDatasets();
    for (auto& p : parts) {
        if (allowed.find(p) == allowed.end()) {
            badToken = p;
            return false;
        }
    }
    return true;
}

static void PrintAllowedDatasetsDetailed() {
    std::cout << "Allowed datasets (public API):\n";
    for (size_t i = 0; i < kDatasetInfosCount; ++i) {
        std::cout << "  - " << kDatasetInfos[i].id
            << " -> " << kDatasetInfos[i].best_for
            << " [" << kDatasetInfos[i].where << "]\n";
    }
    std::cout << "You may specify multiple datasets separated by commas (e.g. srtm30m,eudem25m).\n";
}

static void FetchElevationBatch(
    const std::vector<std::pair<size_t, std::pair<double, double>>>& batch,
    const std::string& dataset_csv,
    std::vector<TrackPoint>& points)
{
    if (batch.empty()) return;

    json payload;
    payload["locations"] = json::array();
    for (auto& it : batch) {
        payload["locations"].push_back({ {"lat", it.second.first}, {"lng", it.second.second} });
    }

    // OpenTopoData supports comma-separated datasets in the path: /v1/<d1,d2,...>
    std::string url = OPENTOPODATA_BASE + dataset_csv;
    std::string resp;

    if (!HttpPostJson(url, payload.dump(), resp)) {
        std::cerr << "[elev] HTTP error contacting " << url << "\n";
        return;
    }

    try {
        auto j = json::parse(resp);
        if (j.contains("status") && j["status"] == "OK" && j.contains("results")) {
            const auto& results = j["results"];
            for (size_t i = 0; i < results.size() && i < batch.size(); ++i) {
                auto idx = batch[i].first;
                if (results[i].contains("elevation") && !results[i]["elevation"].is_null()) {
                    points[idx].ele = results[i]["elevation"].get<double>();
                    points[idx].has_ele = true;
                }
            }
        }
        else {
            std::string st = j.value("status", "UNKNOWN");
            std::string em = j.value("error_message", "");
            std::cerr << "[elev] API status=" << st << " error=" << em << "\n";
        }
    }
    catch (const std::exception& e) {
        std::cerr << "[elev] JSON parse error: " << e.what() << "\n";
    }
}

// ===================== GPX writer (TinyXML2 fully-qualified) =====================

static bool WriteGpx(const std::string& outPath, const std::vector<TrackPoint>& pts) {
    tinyxml2::XMLDocument doc;
    auto decl = doc.NewDeclaration(R"(xml version="1.0" encoding="UTF-8")");
    doc.InsertFirstChild(decl);

    tinyxml2::XMLElement* gpx = doc.NewElement("gpx");
    gpx->SetAttribute("version", "1.1");
    gpx->SetAttribute("creator", "FIT to GPX Converter (C++/libcurl)");
    gpx->SetAttribute("xmlns", "http://www.topografix.com/GPX/1/1");
    gpx->SetAttribute("xmlns:gpxtpx", "http://www.garmin.com/xmlschemas/TrackPointExtension/v1");
    doc.InsertEndChild(gpx);

    tinyxml2::XMLElement* trk = doc.NewElement("trk");
    gpx->InsertEndChild(trk);
    tinyxml2::XMLElement* seg = doc.NewElement("trkseg");
    trk->InsertEndChild(seg);

    for (const auto& p : pts) {
        tinyxml2::XMLElement* trkpt = doc.NewElement("trkpt");
        trkpt->SetAttribute("lat", p.latDeg);
        trkpt->SetAttribute("lon", p.lonDeg);

        if (p.has_ele) {
            tinyxml2::XMLElement* ele = doc.NewElement("ele");
            ele->SetText(p.ele);
            trkpt->InsertEndChild(ele);
        }
        if (p.has_time) {
            tinyxml2::XMLElement* time = doc.NewElement("time");
            std::string s = ToIso8601Utc(p.t);
            time->SetText(s.c_str());
            trkpt->InsertEndChild(time);
        }

        if (p.has_hr || p.has_cad) {
            tinyxml2::XMLElement* ext = doc.NewElement("extensions");
            tinyxml2::XMLElement* tpx = doc.NewElement("gpxtpx:TrackPointExtension");
            if (p.has_hr) {
                tinyxml2::XMLElement* hr = doc.NewElement("gpxtpx:hr");
                hr->SetText((unsigned)p.hr);
                tpx->InsertEndChild(hr);
            }
            if (p.has_cad) {
                tinyxml2::XMLElement* cad = doc.NewElement("gpxtpx:cad");
                cad->SetText((unsigned)p.cad);
                tpx->InsertEndChild(cad);
            }
            ext->InsertEndChild(tpx);
            trkpt->InsertEndChild(ext);
        }

        seg->InsertEndChild(trkpt);
    }

    tinyxml2::XMLError rc = doc.SaveFile(outPath.c_str());
    if (rc != tinyxml2::XML_SUCCESS) {
        std::cerr << "[gpx] SaveFile error code=" << rc << "\n";
        return false;
    }
    return true;
}

// ===================== FIT listener =====================

class RecordCollector : public fit::RecordMesgListener {
public:
    std::vector<TrackPoint> points;

    void OnMesg(fit::RecordMesg& mesg) override {
        FIT_SINT32 latSC = mesg.GetPositionLat();
        FIT_SINT32 lonSC = mesg.GetPositionLong();
        FIT_DATE_TIME ts = mesg.GetTimestamp();
        FIT_FLOAT32 altitude = mesg.GetAltitude();
        FIT_FLOAT32 enhanced_alt = mesg.GetEnhancedAltitude();
        FIT_UINT8 hr = mesg.GetHeartRate();
        FIT_UINT8 cad = mesg.GetCadence();

        if (latSC == FIT_SINT32_INVALID || lonSC == FIT_SINT32_INVALID || ts == FIT_DATE_TIME_INVALID) {
            return;
        }

        TrackPoint p;
        p.latDeg = SemiCirclesToDegrees(latSC);
        p.lonDeg = SemiCirclesToDegrees(lonSC);
        p.t = FitToTimeT(ts);
        p.has_time = true;

        if (enhanced_alt != FIT_FLOAT32_INVALID) {
            p.ele = (double)enhanced_alt; p.has_ele = true;
        }
        else if (altitude != FIT_FLOAT32_INVALID) {
            p.ele = (double)altitude;     p.has_ele = true;
        }

        if (hr != FIT_UINT8_INVALID) { p.hr = hr;   p.has_hr = true; }
        if (cad != FIT_UINT8_INVALID) { p.cad = cad; p.has_cad = true; }

        points.emplace_back(p);
    }
};

// ===================== Args & Help =====================

struct Args {
    std::string fitFile;
    std::string gpxFile;           // may be auto-derived
    bool fetchElevation = true;
    std::string dataset_csv = "srtm90m"; // default on public API
    bool showHelp = false;
};

static void ShowHelp(const char* exe) {
    std::cout <<
        R"(FIT -> GPX converter

Usage:
  )" << exe << R"( <input.fit> [output.gpx] [options]

If [output.gpx] is omitted, it will be set automatically to <input>.gpx

Options:
  --do-not-fetch-elevation          Do not call OpenTopoData for missing elevations
  --elevation-dataset <name|csv>    Dataset id or comma-separated list (default: srtm90m)
  -h, --help                        Show this help
  -v, --version                     Show the version and exit

Examples:
  )" << exe << R"( activity.fit
  )" << exe << R"( activity.fit activity.gpx
  )" << exe << R"( activity.fit --elevation-dataset aster30m
  )" << exe << R"( activity.fit --elevation-dataset srtm30m,eudem25m

)";
    // Detailed dataset list with descriptions
    PrintAllowedDatasetsDetailed();
}

static bool ParseArgs(int argc, char** argv, Args& a) {
    if (argc < 2) { a.showHelp = true; return false; }

    // Local helpers (no external dependencies)
    auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
            [](unsigned char c) { return (char)std::tolower(c); });
        return s;
        };
    auto normPath = [&](std::string p) {
        p = toLower(p);
        std::replace(p.begin(), p.end(), '\\', '/');
        return p;
        };

    // Single pass: options can appear anywhere; first positional = input, second = output
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];

        // --version: print and exit with code 0 (keeps changes scoped to this function)
        if (s == "--version" || s=="-v") {
            std::cout << "fit2gpx 1.0.0\n";
            std::exit(0);
        }

        // Help can appear anywhere
        if (s == "-h" || s == "--help") {
            a.showHelp = true;
            return false;
        }

        // Known options
        if (s == "--do-not-fetch-elevation") {
            a.fetchElevation = false;
            continue;
        }
        if (s == "--elevation-dataset") {
            if ((i + 1) >= argc) {
                std::cerr << "[args] Missing value for --elevation-dataset\n\n";
                a.showHelp = true;
                return false;
            }
            a.dataset_csv = argv[++i];
            if (a.dataset_csv.empty()) {
                std::cerr << "[args] Empty value for --elevation-dataset\n\n";
                a.showHelp = true;
                return false;
            }
            continue;
        }

        // Positionals (do not start with '-')
        if (!s.empty() && s[0] != '-') {
            if (a.fitFile.empty()) {
                a.fitFile = s;
            }
            else if (a.gpxFile.empty()) {
                a.gpxFile = s;
            }
            else {
                std::cerr << "[args] Unexpected extra positional argument: " << s << "\n\n";
                a.showHelp = true;
                return false;
            }
            continue;
        }

        // Unknown argument
        std::cerr << "[args] Unknown argument: " << s << "\n\n";
        a.showHelp = true;
        return false;
    }

    // Require input .fit
    if (a.fitFile.empty()) {
        a.showHelp = true;
        return false;
    }

    // Default output if missing
    if (a.gpxFile.empty()) {
        a.gpxFile = DeriveGpxPath(a.fitFile);
    }

    // Guardrail: never overwrite input
    if (normPath(a.gpxFile) == normPath(a.fitFile)) {
        a.gpxFile = a.fitFile + ".converted.gpx";
    }

    return true;
}

// ===================== main =====================

int main(int argc, char** argv) {
    const auto t0 = std::chrono::steady_clock::now();
    curl_global_init(CURL_GLOBAL_DEFAULT);

    Args args;
    if (!ParseArgs(argc, argv, args)) {
        ShowHelp(argv[0]);
        curl_global_cleanup();
        return (args.showHelp ? 0 : 1);
    }

    // --- Log selected options (confirmation of dataset used) ---
    std::cout << "[fit] Reading: " << args.fitFile << "\n";
    std::cout << "[opts] Elevation fetch: " << (args.fetchElevation ? "enabled" : "disabled") << "\n";
    if (args.fetchElevation) {
        std::cout << "[opts] Elevation dataset(s): " << args.dataset_csv << "\n";
    }

    std::ifstream fitStream(args.fitFile, std::ios::binary);
    if (!fitStream) { std::cerr << "[fit] Cannot open file\n"; curl_global_cleanup(); return 1; }

    RecordCollector collector;

    try {
        fit::Decode decode;

        if (!decode.CheckIntegrity(fitStream)) {
            std::cerr << "[fit] Integrity check failed (continuo comunque)\n";
        }

        fitStream.clear();
        fitStream.seekg(0, std::ios::beg);

        fit::MesgBroadcaster broadcaster;
        broadcaster.AddListener((fit::RecordMesgListener&)collector);

        try {
            if (!decode.Read(fitStream,
                (fit::MesgListener&)broadcaster,
                (fit::MesgDefinitionListener&)broadcaster)) {
                std::cerr << "[fit] Decode returned false\n";
            }
        }
        catch (const fit::RuntimeException& e) {
            std::cerr << "[fit] Decode error: " << e.what() << "\n";
        }
    }
    catch (const std::exception& e) {
        std::cerr << "[fit] Exception: " << e.what() << "\n";
        curl_global_cleanup();
        return 1;
    }

    if (collector.points.empty()) {
        std::cerr << "[fit] No track points found; writing empty GPX\n";
        if (!WriteGpx(args.gpxFile, collector.points)) { curl_global_cleanup(); return 1; }
        const auto t1 = std::chrono::steady_clock::now();
        double secs = std::chrono::duration<double>(t1 - t0).count();
        std::cout << "[gpx] Saved: " << args.gpxFile << "\n";
        std::cout << std::fixed << std::setprecision(3)
            << "[time] Conversion took " << secs << " s\n";
        curl_global_cleanup();
        return 0;
    }

    // Fetch elevation for points without ele
    if (args.fetchElevation) {
        std::vector<std::pair<size_t, std::pair<double, double>>> missing;
        missing.reserve(collector.points.size());
        for (size_t i = 0; i < collector.points.size(); ++i) {
            if (!collector.points[i].has_ele) {
                missing.push_back({ i, {collector.points[i].latDeg, collector.points[i].lonDeg} });
            }
        }

        if (!missing.empty()) {
            std::cout << "[elev] Fetching " << missing.size()
                << " elevations via dataset(s) '" << args.dataset_csv << "'\n";
            for (size_t i = 0; i < missing.size(); i += API_BATCH_SIZE) {
                size_t end = i + API_BATCH_SIZE;
                if (end > missing.size()) end = missing.size();
                auto first = missing.begin() + i;
                auto last = missing.begin() + end;

                std::vector<std::pair<size_t, std::pair<double, double>>> batch;
                batch.insert(batch.end(), first, last);

                FetchElevationBatch(batch, args.dataset_csv, collector.points);
                std::cout << "[elev] Batch " << (i / API_BATCH_SIZE + 1)
                    << "/" << ((missing.size() + API_BATCH_SIZE - 1) / API_BATCH_SIZE) << " done\n";
            }
        }
    }

    if (!WriteGpx(args.gpxFile, collector.points)) { curl_global_cleanup(); return 1; }

    const auto t1 = std::chrono::steady_clock::now();
    double secs = std::chrono::duration<double>(t1 - t0).count();
    std::cout << "[gpx] Saved: " << args.gpxFile << "\n";
    std::cout << std::fixed << std::setprecision(3)
        << "[time] Conversion took " << secs << " s\n";

    curl_global_cleanup();
    return 0;
}