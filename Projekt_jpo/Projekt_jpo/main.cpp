/******************************************************************************************
 * AQI Monitor - v1.2
 *
 * Główny plik aplikacji, który:
 * - Inicjalizuje DirectX 11 oraz ImGui.
 * - Pobiera i przetwarza dane (stacje, sensory, dane historyczne) z API.
 * - Umożliwia analizę oraz wizualizację danych na wykresach.
 *
 * Wymagania: C++17
 ******************************************************************************************/

#define _CRT_SECURE_NO_WARNINGS
#define NOMINMAX
#define _USE_MATH_DEFINES

#include <windows.h>
#include <d3d11.h>
#include <tchar.h>
#include <shellapi.h>
#include <imgui.h>
#include "ThirdParty/Imgui/imgui_impl_win32.h"
#include "ThirdParty/Imgui/imgui_impl_dx11.h"
#include <implot.h>
#include <nlohmann/json.hpp>
#include <winhttp.h>
#include <fstream>
#include <vector>
#include <string>
#include <map>
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <numeric>
#include <cmath>
#include <clocale>
#include <sstream>
#include <iomanip>
#include <time.h>
#include <wininet.h>
#include <future>
#include <mutex>
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "winhttp.lib")

using json = nlohmann::json;
using namespace std::chrono;

#ifndef M_PI
constexpr double PI = 3.14159265358979323846;
#else
constexpr double PI = M_PI;
#endif



//******************************************************************************************
// Data Structures
//******************************************************************************************



/// Struktura reprezentująca sensor
struct Sensor {
    int id = 0;
    std::string name;
};

/// Struktura reprezentująca stację monitoringu AQI
struct Station {
    int id = 0;
    std::string name, city, region;
    double lat = 0, lon = 0;
    std::vector<double> history;
    std::map<int, std::vector<double>> sensor_history;
    std::map<int, std::string> sensor_names;  // Przechowywanie nazw sensorów

    /// Zwraca ostatnią wartość pomiaru, lub 0 jeśli brak danych
    double latest() const { return history.empty() ? 0.0 : history.back(); }
};

/// Struktura analizy danych sensorycznych
struct Analysis {
    double min = 0, max = 0, avg = 0, trend = 0;
    std::string minT, maxT;
};

/// Opakowanie dla uchwytu WinHTTP, zapewniające automatyczne czyszczenie zasobów
struct WinHttpHandle {
    HINTERNET handle;
    explicit WinHttpHandle(HINTERNET h) : handle(h) {}
    ~WinHttpHandle() { if (handle) WinHttpCloseHandle(handle); }
    operator HINTERNET() const { return handle; }
};

//******************************************************************************************
// Forward Declarations
//******************************************************************************************


std::vector<Station> FetchAll();
std::vector<Sensor> FetchSensors(int sid);
json FetchData(int sensorId);
void SaveDB(const std::string& fn, const std::vector<std::string>& dates, const Station& station);

// Zmienne stanu dla wielowątkowości
std::future<std::vector<Station>> stations_future;
bool is_fetching_stations = false;
std::mutex stations_mutex;

//******************************************************************************************
// Exceptions
//******************************************************************************************

/// Wyjątek reprezentujący błędy sieciowe
class NetworkException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

//******************************************************************************************
// HTTP Helpers
//******************************************************************************************

/// Kodowanie URL – konwertuje nietypowe znaki do formatu URL
std::string UrlEncode(const std::string& s) {
    std::string out; char buf[4];
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += c;
        else {
            sprintf_s(buf, "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

/// Sprawdza dostępność połączenia internetowego
bool IsInternetAvailable() {
    DWORD flags = 0;
    return InternetGetConnectedState(&flags, 0) == TRUE;
}

/// Wysyła zapytanie HTTP GET i zwraca odpowiedź jako std::string
std::string HttpGet(const std::wstring& host, const std::wstring& path) {
    WinHttpHandle hSession(WinHttpOpen(L"AQIApp/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!hSession) throw NetworkException("WinHttpOpen failed");

    WinHttpHandle hConnect(WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0));
    if (!hConnect) throw NetworkException("WinHttpConnect failed");

    WinHttpHandle hRequest(WinHttpOpenRequest(hConnect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE));
    if (!hRequest) throw NetworkException("WinHttpOpenRequest failed");

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(hRequest, nullptr))
    {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        throw NetworkException("HTTP request failed");
    }

    std::string res;
    DWORD avail = 0;
    while (WinHttpQueryDataAvailable(hRequest, &avail) && avail) {
        std::vector<char> buf(avail + 1);
        DWORD read = 0;
        WinHttpReadData(hRequest, buf.data(), avail, &read);
        buf[read] = '\0';
        res += buf.data();
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return res;
}

/// Funkcja opakowująca HttpGet aby bezpiecznie pobierać dane
std::string SafeGet(const std::wstring& h, const std::wstring& p) {
    try {
        return HttpGet(h, p);
    }
    catch (std::exception& e) {
        throw NetworkException(e.what());
    }
}

//******************************************************************************************
// Geocode & Distance Calculations
//******************************************************************************************

/// Konwersja stopni na radiany
double Deg2Rad(double d) {
    return d * PI / 180.0;
}

/// Oblicza odległość między dwoma punktami geograficznymi przy użyciu formuły haversine
double Haversine(double lat1, double lon1, double lat2, double lon2) {
    double dlat = Deg2Rad(lat2 - lat1), dlon = Deg2Rad(lon2 - lon1);
    double a = sin(dlat / 2) * sin(dlat / 2)
        + cos(Deg2Rad(lat1)) * cos(Deg2Rad(lat2))
        * sin(dlon / 2) * sin(dlon / 2);
    return 6371.0 * 2 * atan2(sqrt(a), sqrt(1 - a));
}

/// Geokodowanie adresu przy użyciu Nominatim API (OpenStreetMap)
std::pair<double, double> Geocode(const std::string& addr) {
    std::string q = "q=" + UrlEncode(addr) + "&format=json&limit=1";
    std::wstring path = L"/search?" + std::wstring(q.begin(), q.end());

    std::string resp;
    try {
        resp = SafeGet(L"nominatim.openstreetmap.org", path);
        std::ofstream("last_geocode.json") << resp; // DEBUG
    }
    catch (const NetworkException& e) {
        throw NetworkException("Błąd geokodowania: " + std::string(e.what()));
    }

    try {
        auto arr = json::parse(resp);

        if (!arr.is_array() || arr.empty()) {
            throw NetworkException("Brak wyników geokodowania");
        }

        const auto& first_result = arr[0];
        if (!first_result.contains("lat") || !first_result["lat"].is_string() ||
            !first_result.contains("lon") || !first_result["lon"].is_string()) {
            throw NetworkException("Nieprawidłowa struktura odpowiedzi geokodowania");
        }
        double lat = std::stod(first_result["lat"].get<std::string>());
        double lon = std::stod(first_result["lon"].get<std::string>());
        return { lat, lon };
    }
    catch (const json::exception& e) {
        throw NetworkException("Błąd parsowania JSON: " + std::string(e.what()));
    }
}

//******************************************************************************************
// D3D11 + ImGui/ImPlot Setup
//******************************************************************************************

// Globalne uchwyty DirectX
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static IDXGISwapChain* g_pSwapChain = nullptr;
static ID3D11RenderTargetView* g_mainRTV = nullptr;

/// Tworzy widok bufora renderowania
void CreateRT() {
    ID3D11Texture2D* pBack = nullptr;
    HRESULT hr = g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBack));
    if (SUCCEEDED(hr) && pBack != nullptr) {
        hr = g_pd3dDevice->CreateRenderTargetView(pBack, nullptr, &g_mainRTV);
        pBack->Release();
        if (FAILED(hr)) {
            MessageBoxA(nullptr, "Błąd tworzenia widoku renderowania", "Błąd D3D11", MB_ICONERROR);
        }
    }
    else {
        MessageBoxA(nullptr, "Błąd pobierania bufora swapchain", "Błąd D3D11", MB_ICONERROR);
    }
}

/// Czyści widok bufora renderowania
void CleanupRT() {
    if (g_mainRTV) { g_mainRTV->Release(); g_mainRTV = nullptr; }
}

/// Inicjalizuje urządzenie DirectX 11 wraz ze swapchain
bool CreateDevice(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL lvls[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        lvls, 2, D3D11_SDK_VERSION, &sd,
        &g_pSwapChain, &g_pd3dDevice, &fl, &g_pd3dDeviceContext) != S_OK)
        return false;
    CreateRT();
    return true;
}

/// Czyści zasoby związane z urządzeniem DirectX
void CleanupDevice() {
    CleanupRT();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

/// Przekazuje zdarzenia okna do ImGui
extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l))
        return true;
    if (m == WM_SIZE && g_pd3dDeviceContext) {
        CleanupRT();
        g_pSwapChain->ResizeBuffers(0, LOWORD(l), HIWORD(l), DXGI_FORMAT_UNKNOWN, 0);
        CreateRT();
    }
    if (m == WM_CLOSE) {
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

//******************************************************************************************
// REST Fetch Routines
//******************************************************************************************

/// Pobiera wszystkie stacje AQI z API
std::vector<Station> FetchAll() {
    std::ofstream error_log("error_log.txt", std::ios::app); // Plik do logowania błędów

    try {
        // Pobierz odpowiedź z API
        std::string resp = SafeGet(L"api.gios.gov.pl", L"/pjp-api/rest/station/findAll");

        // Parsuj JSON
        auto arr = json::parse(resp, nullptr, false);
        if (arr.is_discarded()) {
            error_log << "Nieprawidłowa odpowiedź JSON\n";
            throw std::runtime_error("Nieprawidłowa odpowiedź JSON");
        }

        // Sprawdź, czy otrzymany obiekt to tablica
        if (!arr.is_array()) {
            error_log << "Oczekiwano tablicy stacji\n";
            throw std::runtime_error("Oczekiwano tablicy stacji");
        }

        std::vector<Station> out;
        for (auto& e : arr) {
            try {
                Station s;
                s.id = e["id"].get<int>();
                s.name = e["stationName"].get<std::string>();

                if (e.contains("city") && e["city"].is_object()) {
                    auto& city = e["city"];
                    s.city = city["name"].get<std::string>();

                    if (city.contains("commune") && city["commune"].is_object()) {
                        auto& commune = city["commune"];
                        s.region = commune["provinceName"].get<std::string>();
                    }
                }

                auto parseCoordinate = [](const json& j, const std::string& field) -> double {
                    if (!j.contains(field)) return 0.0;
                    if (j[field].is_number()) return j[field].get<double>();
                    if (j[field].is_string()) {
                        try { return std::stod(j[field].get<std::string>()); }
                        catch (...) { return 0.0; }
                    }
                    return 0.0;
                    };

                s.lat = parseCoordinate(e, "gegrLat");
                s.lon = parseCoordinate(e, "gegrLon");

                out.push_back(s);
            }
            catch (const std::exception& e) {
                error_log << "Pominięto stację: " << e.what() << "\n";
                continue;
            }
        }
        if (out.empty()) {
            error_log << "Nie znaleziono poprawnych stacji\n";
            throw std::runtime_error("Nie znaleziono żadnych poprawnych stacji");
        }
        return out;
    }
    catch (const std::exception& e) {
        error_log << "Błąd FetchAll: " << e.what() << "\n";
        throw NetworkException("Nie można pobrać stacji: " + std::string(e.what()));
    }
}

/// Pobiera stacje według nazwy miasta
std::vector<Station> FetchByCity(const std::string& city) {
    auto all = FetchAll();
    std::vector<Station> filt;
    filt.reserve(all.size());
    for (const auto& s : all)
        if (_stricmp(s.city.c_str(), city.c_str()) == 0)
            filt.push_back(s);
    return filt;
}

/// Pobiera stacje w obrębie określonego promienia od danego adresu
std::vector<Station> FetchByRadius(const std::string& addr, int km) {
    auto center = Geocode(addr);
    auto all = FetchAll();
    std::vector<Station> filt;
    filt.reserve(all.size());
    for (const auto& s : all)
        if (Haversine(center.first, center.second, s.lat, s.lon) <= km)
            filt.push_back(s);
    return filt;
}

/// Pobiera sensory danej stacji
std::vector<Sensor> FetchSensors(int sid) {
    std::string path = "/pjp-api/rest/station/sensors/" + std::to_string(sid);
    std::wstring wpath(path.begin(), path.end());
    std::string resp;
    try {
        resp = SafeGet(L"api.gios.gov.pl", wpath);
        std::ofstream("last_sensors.json") << resp; // DEBUG
    }
    catch (const NetworkException& e) {
        throw NetworkException("Błąd połączenia: " + std::string(e.what()));
    }
    try {
        auto arr = json::parse(resp);
        if (!arr.is_array()) {
            throw NetworkException("Oczekiwano tablicy w odpowiedzi");
        }
        std::vector<Sensor> out;
        for (const auto& e : arr) {
            Sensor s;
            if (!e.contains("id") || !e["id"].is_number()) {
                continue;
            }
            s.id = e["id"].get<int>();
            if (e.contains("param") && e["param"].is_object()) {
                const auto& param = e["param"];
                if (param.contains("paramName") && param["paramName"].is_string()) {
                    s.name = param["paramName"].get<std::string>();
                }
                else {
                    s.name = "Nieznany sensor";
                }
            }
            else {
                s.name = "Brak danych";
            }
            out.push_back(s);
        }
        if (out.empty()) {
            throw NetworkException("Brak dostępnych sensorów");
        }
        return out;
    }
    catch (const json::exception& e) {
        throw NetworkException("Błąd parsowania JSON: " + std::string(e.what()));
    }
}

/// Pobiera dane historyczne dla danego sensora
json FetchData(int sensorId) {
    std::string path = "/pjp-api/rest/data/getData/" + std::to_string(sensorId);
    std::wstring wpath(path.begin(), path.end());
    try {
        std::string raw_response = SafeGet(L"api.gios.gov.pl", wpath);
        std::ofstream("last_sensor_data.json") << raw_response; // DEBUG
        auto j = json::parse(raw_response);
        if (!j.contains("key") || !j["key"].is_string()) {
            throw NetworkException("Brak lub nieprawidłowy klucz 'key' w odpowiedzi");
        }
        if (!j.contains("values") || !j["values"].is_array()) {
            throw NetworkException("Brak lub nieprawidłowy klucz 'values' w odpowiedzi");
        }
        for (const auto& value : j["values"]) {
            if (!value.contains("date") || !value["date"].is_string()) {
                throw NetworkException("Brak daty w pomiarze");
            }
            if (!value.contains("value")) {
                throw NetworkException("Brak wartości w pomiarze");
            }
            if (!value["value"].is_number() && !value["value"].is_null()) {
                throw NetworkException("Nieprawidłowy typ wartości w pomiarze");
            }
        }
        return j;
    }
    catch (const NetworkException&) {
        throw;
    }
    catch (const json::exception& e) {
        throw NetworkException("Błąd parsowania JSON: " + std::string(e.what()));
    }
    catch (const std::exception& e) {
        throw NetworkException("Nieznany błąd: " + std::string(e.what()));
    }
}

//******************************************************************************************
// Obsługa zapisu/odczytu danych lokalnych (DB)
//******************************************************************************************

/// Wczytuje dane stacji z podanego pliku
bool LoadDB(const std::string& fn, std::vector<std::string>& dates, Station& station) {
    std::ifstream in("savefiles/" + fn);
    if (!in) return false;

    try {
        auto j = json::parse(in);
        station = Station();
        auto& station_data = j["station"];
        station.id = station_data["id"];
        station.name = station_data["stationName"];
        station.city = station_data["city"];
        station.region = station_data["region"];
        station.lat = station_data["lat"];
        station.lon = station_data["lon"];
        station.history = station_data["history"].get<std::vector<double>>();

        if (station_data.contains("sensors")) {
            for (const auto& [sensor_id_str, sensor_data] : station_data["sensors"].items()) {
                int sensor_id = std::stoi(sensor_id_str);
                station.sensor_history[sensor_id] = sensor_data["values"].get<std::vector<double>>();
                station.sensor_names[sensor_id] = sensor_data["name"].get<std::string>();
            }
        }
        dates = j["dates"].get<std::vector<std::string>>();
        return true;
    }
    catch (...) {
        return false;
    }
}

/// Zapisuje dane stacji do pliku lokalnego
void SaveDB(const std::string& fn, const std::vector<std::string>& dates, const Station& station) {
    try {
        CreateDirectoryA("savefiles", nullptr);
        json j;
        json station_data;
        station_data["id"] = station.id;
        station_data["stationName"] = station.name;
        station_data["city"] = station.city;
        station_data["region"] = station.region;
        station_data["lat"] = station.lat;
        station_data["lon"] = station.lon;
        station_data["history"] = station.history;

        json sensors_data;
        for (auto const& [sensor_id, sensor_name] : station.sensor_names) {
            json sensor_info;
            sensor_info["name"] = sensor_name;
            auto it = station.sensor_history.find(sensor_id);
            if (it != station.sensor_history.end() && !it->second.empty()) {
                sensor_info["values"] = it->second;
            }
            else {
                try {
                    json resp = FetchData(sensor_id);
                    std::vector<double> vals;
                    for (auto const& entry : resp["values"]) {
                        if (entry["value"].is_number())
                            vals.push_back(entry["value"].get<double>());
                    }
                    sensor_info["values"] = vals;
                }
                catch (const NetworkException&) {
                    sensor_info["values"] = json::array();
                }
            }
            sensors_data[std::to_string(sensor_id)] = sensor_info;
        }
        station_data["sensors"] = sensors_data;
        j["station"] = station_data;
        j["dates"] = dates;
        std::ofstream o("savefiles/" + fn);
        if (o) o << j.dump(2);
    }
    catch (const std::exception& e) {
        MessageBoxA(nullptr, e.what(), "Błąd zapisu", MB_ICONERROR);
    }
}

//******************************************************************************************
// Prosta analiza danych historycznych
//******************************************************************************************

/// Analizuje dane (min, max, średnia, trend) i zwraca wyniki w strukturze Analysis
Analysis Analyze(const std::vector<std::pair<system_clock::time_point, double>>& d) {
    Analysis A;
    int n = (int)d.size();
    if (n == 0) return A;
    auto mm = minmax_element(d.begin(), d.end(), [](auto& a, auto& b) { return a.second < b.second; });
    A.min = mm.first->second;
    A.max = mm.second->second;
    struct tm tm;
    auto fmt = [&](auto tp) {
        time_t t = system_clock::to_time_t(tp);
        localtime_s(&tm, &t);
        char buf[64];
        strftime(buf, 64, "%F %T", &tm);
        return std::string(buf);
        };
    A.minT = fmt(mm.first->first);
    A.maxT = fmt(mm.second->first);
    double sum = 0;
    for (auto& p : d)
        sum += p.second;
    A.avg = sum / n;
    double Sx = 0, Sy = 0, Sxx = 0, Sxy = 0;
    for (int i = 0; i < n; ++i) {
        double x = i, y = d[i].second;
        Sx += x; Sy += y; Sxx += x * x; Sxy += x * y;
    }
    A.trend = (n * Sxy - Sx * Sy) / (n * Sxx - Sx * Sx);
    return A;
}

//******************************************************************************************
// Inicjalizacja czcionek dla ImGui z obsługą polskich znaków
//******************************************************************************************

/// Inicjalizuje czcionki ImGui, próbując załadować kilka czcionek systemowych
bool InitializeFonts(ImGuiIO& io, std::string& errorMsg, bool& showErrorPopup) {
    static const ImWchar ranges[] = {
        0x0020, 0x00FF, // Basic Latin + Latin Supplement
        0x0100, 0x017F, // Extended Latin (zawiera polskie znaki)
        0x0150, 0x0151, // Őő
        0x0178, 0x017E, // Ÿźż
        0
    };

    ImFontConfig font_cfg;
    font_cfg.OversampleH = 3;
    font_cfg.OversampleV = 3;
    font_cfg.RasterizerMultiply = 1.2f;

    const char* fontPaths[] = {
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/tahoma.ttf",
        "C:/Windows/Fonts/calibri.ttf",
        "C:/Windows/Fonts/verdana.ttf",
        "C:/Windows/Fonts/times.ttf"
    };

    ImFont* font = nullptr;
    std::string loadedFontPath;

    for (const char* path : fontPaths) {
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            font = io.Fonts->AddFontFromFileTTF(path, 16.0f, &font_cfg, ranges);
            if (font) {
                loadedFontPath = path;
                break;
            }
        }
    }

    if (!font) {
        io.Fonts->AddFontDefault();
        errorMsg = "Nie udało się załadować żadnej czcionki z polskimi znakami. Używam czcionki domyślnej.";
        showErrorPopup = true;
        return false;
    }

    font_cfg.MergeMode = true;
    if (!io.Fonts->AddFontDefault(&font_cfg)) {
        errorMsg = "Ostrzeżenie: Załadowano czcionkę " + loadedFontPath + ", ale nie udało się dodać czcionki fallback.";
        showErrorPopup = true;
    }

    if (!io.Fonts->Build()) {
        errorMsg = "Krytyczny błąd: Nie udało się zbudować atlasu czcionek!";
        showErrorPopup = true;
        return false;
    }
    return true;
}

/// Przygotowuje współrzędne oraz etykiety na osi X wykresu
static void PrepareAdaptiveTicksX(int points, const std::vector<const char*>& labels)
{
    if (points < 2 || labels.empty())
        return;

    const float plot_w = ImGui::GetContentRegionAvail().x;
    const float px_per_unit = plot_w / (points - 1);

    int step = 1;
    for (; step < points; ++step)
        if (ImGui::CalcTextSize(labels[0]).x < px_per_unit * step)
            break;

    std::vector<double> ticks;
    std::vector<const char*> tick_lbl;
    for (int i = 0; i < points; i += step) {
        ticks.push_back(static_cast<double>(i));
        tick_lbl.push_back(labels[i]);
    }
    if (ticks.size() < 2) {
        ticks = { 0.0, static_cast<double>(points - 1) };
        tick_lbl = { labels.front(), labels.back() };
    }
    ImPlot::SetupAxisTicks(ImAxis_X1, ticks.data(), static_cast<int>(ticks.size()), tick_lbl.data());
}

//******************************************************************************************
// Funkcja WinMain oraz GUI aplikacji
//******************************************************************************************


int WINAPI _tWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPTSTR lpCmdLine, int nCmdShow) {

    // Ustawienia lokalne
    std::setlocale(LC_ALL, "pl_PL.UTF-8");
    std::setlocale(LC_CTYPE, "pl_PL.UTF-8");

    // Konfiguracja klasy okna
    WNDCLASSEX wc = {
        sizeof(WNDCLASSEX),
        CS_CLASSDC,
        WndProc,
        0L,
        0L,
        GetModuleHandle(nullptr),
        nullptr,
        nullptr,
        nullptr,
        nullptr,
        _T("AQIApp"),
        nullptr
    };

    if (!RegisterClassEx(&wc)) {
        MessageBoxA(nullptr, "Błąd rejestracji klasy okna!", "Błąd", MB_ICONERROR);
        return 1;
    }

    // Tworzenie okna
    HWND hwnd = CreateWindow(
        _T("AQIApp"),
        _T("AQI Monitor - v1.2"),
        WS_OVERLAPPEDWINDOW,
        100, 100, 1280, 720,
        nullptr,
        nullptr,
        wc.hInstance,
        nullptr
    );

    // Inicjalizacja DirectX 11
    if (!CreateDevice(hwnd)) {
        CleanupDevice();
        UnregisterClass(wc.lpszClassName, wc.hInstance);
        MessageBoxA(nullptr, "Błąd inicjalizacji DirectX 11!", "Błąd", MB_ICONERROR);
        return 1;
    }

    std::string errorMsg;
    bool showErrorPopup = false;



    // Konfiguracja ImGui i ImPlot
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;

    if (!InitializeFonts(io, errorMsg, showErrorPopup)) {
        io.Fonts->AddFontDefault();
        errorMsg = "Uwaga: Nie udało się załadować czcionki z polskimi znakami. Używam czcionki domyślnej.";
        showErrorPopup = true;
    }
    io.Fonts->Build();

    // Ustawienia stylu
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // Zmienne stanu aplikacji
    std::vector<std::string> dates;
    std::vector<Station> stations;
    int fetchMode = 0;
    char cityBuf[64] = {};
    char addrBuf[128] = {};
    int radiusKm = 10;
    int selStation = -1;
    int selSensor = -1;
    std::vector<Sensor> sensors;
    std::vector<std::pair<system_clock::time_point, double>> data;
    Analysis analysis;
    int days = 50;
    int plotType = 0;
    bool onlineMode = IsInternetAvailable();

    if (!IsInternetAvailable()) {
        errorMsg = "Brak połączenia z Internetem!";
        showErrorPopup = true;
    }
    else {
        onlineMode = true;
        errorMsg = "Połączenie z Internetem aktywne";
        showErrorPopup = true;
    }

    // Wyświetlenie okna
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // Główna pętla komunikatów
    MSG msg;
    while (true) {
        if (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            if (msg.message == WM_QUIT)
                break;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }


        if (is_fetching_stations && stations_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            try {
                auto result = stations_future.get();
                {
                    std::lock_guard<std::mutex> lock(stations_mutex);
                    stations = result;
                    dates.clear();
                    errorMsg = "Pobrano nowe dane!";
                    showErrorPopup = true;
                }
            }
            catch (const std::exception& e) {
                errorMsg = "Błąd sieciowy: " + std::string(e.what());
                showErrorPopup = true;
            }
            is_fetching_stations = false;
        }

        // Rozpoczęcie nowej ramki ImGui
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Obsługa okienka błędów
        if (showErrorPopup) {
            ImGui::OpenPopup("Błąd");
            showErrorPopup = false;
        }
        if (ImGui::BeginPopupModal("Błąd", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextWrapped("%s", errorMsg.c_str());
            if (ImGui::Button("OK", ImVec2(120, 0))) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // Główne okno dashboard'u
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        if (ImGui::Begin("AQI Dashboard", nullptr,
            ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings)) {

            // Panel sterowania po lewej stronie
            ImGui::BeginChild("ControlPanel", ImVec2(300, 0), true);
            if (onlineMode) {
                ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "(ONLINE)");
            }
            else {
                ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), "(OFFLINE)");
            }
            if (onlineMode) {
                ImGui::Separator();
                ImGui::RadioButton("Wszystkie stacje", &fetchMode, 0);
                ImGui::RadioButton("Wg miasta", &fetchMode, 1);
                ImGui::RadioButton("W promieniu", &fetchMode, 2);
                if (fetchMode == 1) {
                    ImGui::InputText("Miasto", cityBuf, IM_ARRAYSIZE(cityBuf));
                }
                else if (fetchMode == 2) {
                    ImGui::InputText("Adres", addrBuf, IM_ARRAYSIZE(addrBuf));
                    ImGui::SliderInt("Promień_(km)", &radiusKm, 1, 1000);
                }
                if (ImGui::Button("Pobierz dane")) {
                    if (!is_fetching_stations) {
                        is_fetching_stations = true;
                        int current_mode = fetchMode;
                        std::string current_city = cityBuf;
                        std::string current_addr = addrBuf;
                        int current_radius = radiusKm;

                        stations_future = std::async(std::launch::async,
                            [current_mode, current_city, current_addr, current_radius]() -> std::vector<Station> {  
                                try {
                                    if (current_mode == 0) return FetchAll();
                                    else if (current_mode == 1) return FetchByCity(current_city);
                                    else return FetchByRadius(current_addr, current_radius);
                                }
                                catch (...) {
                                    return {};  // Zwróć pusty vector w przypadku błędu
                                }
                            }
                        );
                    }
                }

                // Wskaźnik ładowania
                if (is_fetching_stations) {
                    ImGui::SameLine();
                    ImGui::Text(" Ładowanie stacji...");
                }
            }
            static bool showSaveDialog = false;
            static bool showLoadDialog = false;
            static char saveFilename[128] = "nowy_plik.json";
            static std::vector<std::string> availableFiles;

            ImGui::Separator();

            // Zapis danych lokalnych
            if (ImGui::Button("Zapisz lokalnie")) {
                if (selStation >= 0) {
                    std::lock_guard<std::mutex> lock(stations_mutex);
                    showSaveDialog = true;
                    auto& s = stations[selStation];
                    auto t = system_clock::now();
                    std::time_t tt = system_clock::to_time_t(t);
                    std::tm tm;
                    localtime_s(&tm, &tt);
                    char buf[64];
                    strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
                    snprintf(saveFilename, IM_ARRAYSIZE(saveFilename),
                        "station_%d_%s.json", s.id, buf);
                    ImGui::OpenPopup("Zapisz plik");
                }
            }
            if (ImGui::BeginPopupModal("Zapisz plik", &showSaveDialog, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("Nazwa pliku:");
                ImGui::InputText("##save_name", saveFilename, IM_ARRAYSIZE(saveFilename));
                if (ImGui::Button("Zapisz")) {
                    try {
                        std::string filename(saveFilename);
                        if (filename.find(".json") == std::string::npos) {
                            filename += ".json";
                        }
                        SaveDB(filename, dates, stations[selStation]);
                        errorMsg = "Zapisano dane jako: " + filename;
                        showErrorPopup = true;
                        showSaveDialog = false;
                    }
                    catch (const std::exception& e) {
                        errorMsg = "Błąd zapisu: " + std::string(e.what());
                        showErrorPopup = true;
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Anuluj")) {
                    showSaveDialog = false;
                }
                ImGui::EndPopup();
            }

            // Wczytanie danych lokalnych
            if (ImGui::Button("Wczytaj dane lokalne")) {
                availableFiles.clear();
                WIN32_FIND_DATAA findData;
                HANDLE hFind = FindFirstFileA("savefiles\\*.json", &findData);
                if (hFind != INVALID_HANDLE_VALUE) {
                    do {
                        if (!(findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                            availableFiles.push_back(findData.cFileName);
                        }
                    } while (FindNextFileA(hFind, &findData));
                    FindClose(hFind);
                }
                showLoadDialog = true;
                ImGui::OpenPopup("Wybierz plik");
            }
            if (ImGui::BeginPopupModal("Wybierz plik", &showLoadDialog, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("Dostępne pliki:");
                static int selectedFile = -1;
                if (ImGui::BeginListBox("##Files", ImVec2(-1, 200))) {
                    for (size_t i = 0; i < availableFiles.size(); ++i) {
                        if (ImGui::Selectable(availableFiles[i].c_str(), selectedFile == i)) {
                            selectedFile = i;
                        }
                    }
                    ImGui::EndListBox();
                }
                if (ImGui::Button("Wczytaj")) {
                    if (selectedFile >= 0) {
                        Station loadedStation;
                        if (LoadDB(availableFiles[selectedFile], dates, loadedStation)) {
                            auto it = std::find_if(stations.begin(), stations.end(),
                                [&](const Station& s) { return s.id == loadedStation.id; });
                            if (it != stations.end())
                                *it = loadedStation;
                            else
                                stations.push_back(loadedStation);
                        }
                    }
                }
                ImGui::SameLine();
                if (ImGui::Button("Anuluj")) {
                    showLoadDialog = false;
                }
                ImGui::EndPopup();
            }

            ImGui::Separator();
            ImGui::Text("Lista stacji:");
            std::lock_guard<std::mutex> lock(stations_mutex);
            if (ImGui::BeginListBox("##StationsList", ImVec2(-1, -1))) {
                for (int i = 0; i < static_cast<int>(stations.size()); ++i) {
                    const auto& s = stations[i];
                    char buf[128];
                    sprintf_s(buf, "%s [%s] - %.1f", s.name.c_str(), s.city.c_str(), s.latest());
                    if (ImGui::Selectable(buf, selStation == i)) {
                        selStation = i;
                        sensors.clear();
                        selSensor = -1;
                        data.clear();
                    }
                }
                ImGui::EndListBox();
            }
            ImGui::EndChild();

            // Panel szczegółów po prawej stronie
            ImGui::SameLine();
            ImGui::BeginChild("DetailsPanel", ImVec2(0, 0), true);
            if (selStation >= 0 && selStation < static_cast<int>(stations.size())) {
                auto& station = stations[selStation];
                ImGui::Text("Stacja: %s", station.name.c_str());
                ImGui::Text("Lokalizacja: %s, %s", station.city.c_str(), station.region.c_str());

                if (station.lat != 0.0 || station.lon != 0.0) {
                    if (ImGui::Button("Pokaż na mapie")) {
                        char url[256];
                        sprintf_s(url, "https://www.openstreetmap.org/?mlat=%.6f&mlon=%.6f#map=12/%.6f/%.6f",
                            station.lat, station.lon, station.lat, station.lon);
                        ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
                    }
                }

                if (sensors.empty()) {
                    if (ImGui::Button("Pobierz sensory")) {
                        if (!onlineMode) {
                            for (const auto& [sensor_id, hist] : station.sensor_history) {
                                if (station.sensor_names.count(sensor_id)) {
                                    const auto& name = station.sensor_names.at(sensor_id);
                                    if (!hist.empty()) {
                                        sensors.push_back({ sensor_id, name });
                                    }
                                }
                            }
                        }
                        else {
                            try {
                                sensors = FetchSensors(station.id);
                                for (const auto& sensor : sensors) {
                                    station.sensor_names[sensor.id] = sensor.name;
                                    if (station.sensor_history[sensor.id].empty()) {
                                        station.sensor_history[sensor.id] = {};
                                    }
                                }
                            }
                            catch (const NetworkException& e) {
                                errorMsg = "Błąd sieciowy: " + std::string(e.what());
                                showErrorPopup = true;
                                onlineMode = false;
                                if (!station.sensor_history.empty()) {
                                    sensors.clear();
                                    for (const auto& [sensor_id, values] : station.sensor_history) {
                                        if (!values.empty()) {
                                            sensors.push_back({ sensor_id, "Sensor #" + std::to_string(sensor_id) + " (" + std::to_string(values.size()) + " rekordów)" });
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                else {
                    ImGui::Separator();
                    ImGui::Text("sensory:");
                    if (ImGui::BeginListBox("##SensorsList", ImVec2(-1, 100))) {
                        for (int i = 0; i < static_cast<int>(sensors.size()); ++i) {
                            if (ImGui::Selectable(sensors[i].name.c_str(), selSensor == i)) {
                                selSensor = i;
                                data.clear();
                            }
                        }
                        ImGui::EndListBox();
                    }
                }

                // Pobieranie i analiza danych historycznych dla wybranego sensora
                if (selSensor >= 0 && selSensor < static_cast<int>(sensors.size())) {
                    const auto& sensor = sensors[selSensor];
                    if (data.empty()) {
                        if (ImGui::Button("Pobierz dane historyczne")) {
                            if (!onlineMode) {
                                if (station.sensor_history.count(sensor.id) > 0) {
                                    const auto& hist = station.sensor_history.at(sensor.id);
                                    data.clear();
                                    for (size_t i = 0; i < hist.size(); ++i) {
                                        data.emplace_back(system_clock::now() - hours(24 * i), hist.at(i));
                                    }
                                    analysis = Analyze(data);
                                }
                            }
                            else {
                                try {
                                    auto j = FetchData(sensor.id);
                                    data.clear();
                                    for (const auto& entry : j["values"]) {
                                        if (entry["value"].is_null()) {
                                            continue;
                                        }
                                        std::string date_str = entry["date"].get<std::string>();
                                        std::tm tm = {};
                                        std::istringstream ss(date_str);
                                        ss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
                                        if (ss.fail()) {
                                            continue;
                                        }
                                        tm.tm_isdst = -1;
                                        time_t time = std::mktime(&tm);
                                        if (time == -1) {
                                            continue;
                                        }
                                        auto tp = system_clock::from_time_t(time);
                                        double value = entry["value"].get<double>();
                                        data.emplace_back(tp, value);
                                    }
                                    std::sort(data.begin(), data.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
                                    station.sensor_history[sensor.id] = std::vector<double>();
                                    auto& hist = station.sensor_history[sensor.id];
                                    for (const auto& d : data) {
                                        hist.push_back(d.second);
                                    }
                                    if (hist.size() > days) {
                                        hist.erase(hist.begin(), hist.end() - days);
                                    }
                                    if (days > 0 && data.size() > days) {
                                        data.erase(data.begin(), data.end() - days);
                                    }
                                    if (!data.empty()) {
                                        time_t last_time = system_clock::to_time_t(data.back().first);
                                        std::tm last_tm;
                                        localtime_s(&last_tm, &last_time);
                                        char buf[64];
                                        strftime(buf, sizeof(buf), "%Y-%m-%d %H", &last_tm);
                                        dates.push_back(buf);
                                        double sum = 0.0;
                                        int count = 0;
                                        for (const auto& d : data) {
                                            sum += d.second;
                                            count++;
                                        }
                                        station.history.push_back(sum / count);
                                        auto& sensor_data = station.sensor_history[sensor.id];
                                        sensor_data.clear();
                                        for (const auto& d : data) {
                                            sensor_data.push_back(d.second);
                                        }
                                        if (days > 0 && sensor_data.size() > days) {
                                            sensor_data.erase(sensor_data.begin(), sensor_data.end() - days);
                                        }
                                        if (!data.empty()) {
                                            analysis = Analyze(data);
                                            days = std::min(50, static_cast<int>(data.size()));
                                        }
                                        else {
                                            errorMsg = "Brak danych do analizy";
                                            showErrorPopup = true;
                                        }
                                    }
                                    else {
                                        errorMsg = "Brak prawidłowych danych do wyświetlenia";
                                        showErrorPopup = true;
                                    }
                                }
                                catch (const NetworkException& e) {
                                    errorMsg = "Błąd pobierania: " + std::string(e.what());
                                    showErrorPopup = true;
                                    onlineMode = false;
                                }
                            }
                        }
                    }
                    else {
                        ImGui::Separator();
                        ImGui::Text("Statystyki:");
                        ImGui::Text("Min: %.2f (%s)", analysis.min, analysis.minT.c_str());
                        ImGui::Text("Max: %.2f (%s)", analysis.max, analysis.maxT.c_str());
                        ImGui::Text("Średnia: %.2f", analysis.avg);
                        ImGui::Text("Trend: %.2f jednostek/dzień", analysis.trend);
                        ImGui::Separator();
                        int maxDays = std::min(static_cast<int>(data.size()), 50);
                        ImGui::SliderInt("Okres (dni)", &days, 2, maxDays);
                        ImGui::RadioButton("Wykres liniowy", &plotType, 0);
                        ImGui::SameLine();
                        ImGui::RadioButton("Wykres słupkowy", &plotType, 1);
                        if (selSensor >= 0 && !data.empty() && days > 0) {
                            std::vector<double> x_vals;
                            std::vector<double> y_vals;
                            std::vector<std::string> labels_str;
                            std::vector<const char*> labels;
                            const int total_points = static_cast<int>(data.size());
                            const int points_to_show = std::min(days, total_points);
                            const int start_idx = total_points - points_to_show;
                            for (int i = 0; i < points_to_show; ++i) {
                                const auto& point = data[start_idx + i];
                                x_vals.push_back(i);
                                y_vals.push_back(point.second);
                                time_t t = system_clock::to_time_t(point.first);
                                struct tm tm_time;
                                localtime_s(&tm_time, &t);
                                char buf[32];
                                strftime(buf, sizeof(buf), "%d/%m %H:%M", &tm_time);
                                labels_str.push_back(buf);
                            }
                            for (const auto& str : labels_str) {
                                labels.push_back(str.c_str());
                            }
                            if (ImPlot::BeginPlot("##HistoryChart", ImVec2(-1, 300))) {
                                PrepareAdaptiveTicksX(points_to_show, labels);
                                ImPlot::SetupAxes("Data", "Wartość", ImPlotAxisFlags_AutoFit, ImPlotAxisFlags_AutoFit);
                                if (plotType == 0) {
                                    ImPlot::PlotLine("##Series", x_vals.data(), y_vals.data(), points_to_show);
                                }
                                else {
                                    ImPlot::PlotBars("##Bars", x_vals.data(), y_vals.data(), points_to_show, 0.7);
                                }
                                ImPlot::EndPlot();
                            }
                        }
                    }
                }
            }
            ImGui::EndChild();
            ImGui::End();
        }

        // Renderowanie i prezentacja
        ImGui::Render();
        const float clear_color[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRTV, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRTV, clear_color);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_pSwapChain->Present(1, 0);
    }

    // Czyszczenie zasobów i zamknięcie aplikacji
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    CleanupDevice();
    DestroyWindow(hwnd);
    UnregisterClass(wc.lpszClassName, wc.hInstance);
    return 0;
}

