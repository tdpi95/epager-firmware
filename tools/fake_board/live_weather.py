"""Live weather for the fake board: the same requests the firmware makes (components/weather, PLAN D62),
fetched with Python; the forecast document then goes through the board's own parser (fb_weather_json).
Needs the network (the environment's proxy is honoured)."""
from __future__ import annotations

import json
import urllib.parse
import urllib.request

TIMEOUT_S = 10


class LiveWeatherError(Exception):
    """The place was not found, or a service could not be reached."""
FORECAST = ("https://api.open-meteo.com/v1/forecast?latitude={lat:.4f}&longitude={lon:.4f}"
            "&current=temperature_2m,weather_code,is_day"
            "&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max"
            "&timezone=auto&forecast_days=1")


def _get(url: str) -> bytes:
    try:
        with urllib.request.urlopen(url, timeout=TIMEOUT_S) as r:  # noqa: S310 (fixed https URLs)
            return r.read()
    except OSError as e:
        raise LiveWeatherError(f"{url.split('?')[0]}: {e}") from e


def locate(city: str | None, lang: str = "en") -> tuple[float, float, str]:
    """(latitude, longitude, place name): by name through Open-Meteo's geocoder, else by IP (ipinfo.io)."""
    if city:
        q = urllib.parse.urlencode({"name": city, "count": 5, "language": lang, "format": "json"},
                                   quote_via=urllib.parse.quote)
        doc = json.loads(_get(f"https://geocoding-api.open-meteo.com/v1/search?{q}"))
        if not doc.get("results"):
            raise LiveWeatherError(f"place not found: {city!r}")
        # as the board: the most populous answer, else the first
        r = max(doc["results"], key=lambda p: p.get("population", -1))
        return float(r["latitude"]), float(r["longitude"]), r.get("name", city)
    doc = json.loads(_get("https://ipinfo.io/json"))
    if "loc" not in doc:
        raise LiveWeatherError("no location for this network's IP address")
    lat, lon = (float(v) for v in doc["loc"].split(","))
    return lat, lon, doc.get("city", "")


def forecast(lat: float, lon: float) -> bytes:
    return _get(FORECAST.format(lat=lat, lon=lon))
