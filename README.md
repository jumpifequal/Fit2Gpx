# FIT → GPX Converter

Command-line converter from Garmin/FIT activity files to GPX.

## Dependencies

The Visual Studio project uses the bundled Garmin FIT SDK sources plus Windows
WinHTTP from the Windows SDK for elevation API requests.

## Usage

```text
Fit2Gpx.exe <input.fit> [output.gpx] [options]
```

If `[output.gpx]` is omitted, it is generated automatically as:

```text
<input>.gpx
```

## Options

| Option | Description |
|---|---|
| `--do-not-fetch-elevation` | Do not call OpenTopoData for missing elevations. |
| `--elevation-dataset <name\|csv>` | Dataset ID or comma-separated list. Default: `srtm90m`. |
| `-h`, `--help` | Show help. |
| `-v`, `--version` | Show the version and exit. |

## Examples

```powershell
Fit2Gpx.exe activity.fit
Fit2Gpx.exe activity.fit activity.gpx
Fit2Gpx.exe activity.fit --elevation-dataset aster30m
Fit2Gpx.exe activity.fit --elevation-dataset srtm30m,eudem25m
```

## Allowed datasets: public API

| Dataset | Description | Coverage |
|---|---|---|
| `srtm90m` | Global baseline ~90 m / 3 arc-sec SRTM. Good default when detail is not critical. | Global land roughly between 60°N and 56°S |
| `srtm30m` | Higher-detail SRTM ~30 m / SRTMGL1 v3. Fewer voids than older SRTM90. | Near-global land, within SRTM latitude limits |
| `aster30m` | ASTER GDEM ~30 m. Good detail in mountains, but may contain artefacts in forests or snow. | Global land |
| `eudem25m` | EU-DEM ~25 m. Balanced and clean DEM for Europe. | Europe / EU / EEA |
| `ned10m` | US NED/3DEP ~10 m. Higher detail, suitable for U.S. hiking and biking routes. | United States; CONUS, AK/HI varies |
| `nzdem8m` | New Zealand 8 m DEM. Very detailed for local routes. | New Zealand |
| `etopo1` | Coarse 1 arc-min ~1.8 km topography and bathymetry. Stable global fallback. | Global land and ocean |
| `gebco2020` | Global bathymetry/topography ~15 arc-sec / ~500 m. Useful near coasts and ocean. | Global land and ocean |
| `emod2018` | European marine bathymetry composite. Better sea-floor detail than global datasets. | European seas |
| `mapzen` | Composite terrain tiles. Reasonable global fallback where other datasets are sparse. | Global composite |
| `bkg200` | Germany DEM at ~200 m. Lightweight background dataset. | Germany |

Multiple datasets can be specified as a comma-separated list:

```powershell
Fit2Gpx.exe activity.fit --elevation-dataset srtm30m,eudem25m
```
