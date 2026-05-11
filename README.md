FIT -> GPX converter

Usage:
  Fit2Gpx.exe <input.fit> [output.gpx] [options]

If [output.gpx] is omitted, it will be set automatically to <input>.gpx

Options:
  --do-not-fetch-elevation          Do not call OpenTopoData for missing elevations
  --elevation-dataset <name|csv>    Dataset id or comma-separated list (default: srtm90m)
  -h, --help                        Show this help
  -v, --version                     Show the version and exit

Examples:
  Fit2Gpx.exe activity.fit
  Fit2Gpx.exe activity.fit activity.gpx
  Fit2Gpx.exe activity.fit --elevation-dataset aster30m
  Fit2Gpx.exe activity.fit --elevation-dataset srtm30m,eudem25m

Allowed datasets (public API):
  - srtm90m -> Global baseline ~90 m (3 arc-sec) SRTM; good default when detail not critical. [Global land roughly between 60°N and 56°S]
  - srtm30m -> Higher-detail SRTM (~30 m, SRTMGL1 v3); fewer voids than older SRTM90. [Near-global land (same SRTM latitude limits)]
  - aster30m -> ASTER GDEM (~30 m); good detail in mountains but can have artifacts in forests/snow. [Global land]
  - eudem25m -> EU-DEM (~25 m); balanced & clean DEM for European Union. [Europe (EU/EEA)]
  - ned10m -> US NED/3DEP (~10 m); higher detail—great for U.S. hiking/biking. [United States (CONUS, AK/HI varies)]
  - nzdem8m -> NZ 8 m DEM; very detailed for local routes. [New Zealand]
  - etopo1 -> Coarse 1 arc-min (~1.8 km) topo+bathymetry; stable global fallback. [Global land+ocean]
  - gebco2020 -> Global bathymetry/topography (~15 arc-sec ~500 m); good near coasts/ocean. [Global land+ocean]
  - emod2018 -> European marine bathymetry composite; better sea-floor detail than global sets. [European seas]
  - mapzen -> Composite terrain tiles; reasonable global fallback where others are sparse. [Global (composite)]
  - bkg200 -> Germany DEM at ~200 m; light-weight background. [Germany]
You may specify multiple datasets separated by commas (e.g. srtm30m,eudem25m).
