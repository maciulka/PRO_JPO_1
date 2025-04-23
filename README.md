# AQI Monitor - v1.2

Aplikacja do monitorowania jakości powietrza (AQI) z wykorzystaniem danych z API GIOS.  
Wizualizuje dane na wykresach i umożliwia analizę historyczną.

## Funkcje
- Pobieranie danych stacji i sensorów w czasie rzeczywistym.
- Filtrowanie stacji według lokalizacji lub promienia od adresu.
- Zapis i odczyt danych lokalnych w formacie JSON.
- Wykresy liniowe/słupkowe z analizą statystyczną (min/max/średnia/trend).
- Integracja z OpenStreetMap do wyświetlania lokalizacji stacji.

## Wymagania
- System operacyjny: Windows 10/11.
- Kompilator obsługujący C++17 (np. MSVC w Visual Studio 2022+).
- Biblioteki:
  - DirectX 11 SDK (część Windows SDK).
  - ImGui i ImPlot (załóżczone w katalogu `ThirdParty/`).
  - nlohmann/json (dołączony w projekcie lub via vcpkg).
- CMake (opcjonalnie, dla alternatywnej kompilacji).

## Instalacja zależności
1. **ImGui i ImPlot**:
   - Sklonuj repozytoria do katalogu `ThirdParty/`:
     ```
     git clone https://github.com/ocornut/imgui.git ThirdParty/Imgui
     git clone https://github.com/epezent/implot.git ThirdParty/Implot
     ```
2. **nlohmann/json**:
   - Sklonuj do projektu lub zainstaluj przez vcpkg:
     ```
     vcpkg install nlohmann-json
     ```

## Kompilacja
1. Otwórz projekt w **Visual Studio**.
2. Skonfiguruj ścieżki do bibliotek w ustawieniach projektu:
   - Dodaj katalogi `ThirdParty/Imgui`, `ThirdParty/Implot` oraz `nlohmann/json` do ścieżek include.
   - Dodaj biblioteki `d3d11.lib`, `dxgi.lib`, `winhttp.lib` w ustawieniach linkera.
3. Skompiluj w trybie **Release** lub **Debug** (C++17).

## Użycie
1. Uruchom plik wykonywalny `AQI_Monitor.exe`.
2. Wybierz tryb pobierania danych (wszystkie stacje/wg miasta/w promieniu).
3. Kliknij **Pobierz dane**, aby załadować stacje.
4. Wybierz stację z listy, a następnie sensor do analizy.
5. Użyj przycisków **Zapisz lokalnie**/**Wczytaj dane lokalne**, aby zarządzać danymi.

## Licencja
Kod źródłowy dostępny na licencji MIT.  
Dane pochodzą z API GIOS (https://api.gios.gov.pl/).
