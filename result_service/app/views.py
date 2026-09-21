from __future__ import annotations

import json
import hashlib
import os
import re
import threading
from pathlib import Path
from typing import Any
from uuid import uuid4


SCHEMA_VERSION = 1
_PLUGIN_NAME = re.compile(r"^[A-Za-z_][A-Za-z0-9_-]*$")
_VALUE_PATH = re.compile(
    r"^(run|analysis|result_completeness|outcomes)(\.[A-Za-z_][A-Za-z0-9_]*)+$"
)
_FORMATS = {"text", "number", "integer", "percent", "datetime", "duration"}
_TONES = {"neutral", "success", "warning", "danger", "accent"}
_EMPTY_STATES = {"pending", "error", "not_configured", "not_selected"}
_COMMON_WIDGET_KEYS = {"id", "type", "title", "description"}


class PluginViewError(ValueError):
    """A plugin view definition does not satisfy the declarative contract."""

    def __init__(self, message: str, status: int = 400, *, current_revision: str | None = None):
        super().__init__(message)
        self.status = status
        self.current_revision = current_revision


def view_revision(definition: dict[str, Any]) -> str:
    canonical = json.dumps(definition, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(canonical.encode("utf-8")).hexdigest()


class PluginViewStore:
    """Load and persist validated, data-only plugin result views."""

    # Shared across store instances: HTTP requests and plugin bundles can hold
    # different stores referring to the same files in this service process.
    lock = threading.RLock()

    def __init__(self, directory: str | Path | None = None):
        self.directory = Path(directory).resolve() if directory else None
        self.plugin_source_root = Path(__file__).resolve().parents[2] / "plugins"
        self.schema_path = Path(__file__).resolve().parents[1] / "schema" / "plugin-view.schema.json"

    def schema(self) -> dict[str, Any]:
        return self._read_json(self.schema_path)

    def plugin_names(self) -> list[str]:
        if self.directory:
            candidates = (path.stem for path in self.directory.glob("*.json"))
        else:
            candidates = (
                path.parent.name for path in self.plugin_source_root.glob("*/result_view.json")
            )
        names = sorted({name for name in candidates if _PLUGIN_NAME.fullmatch(name)})
        for name in names:
            self.load(name)
        return names

    def load(self, plugin: str) -> dict[str, Any]:
        with self.lock:
            path = self._path(plugin)
            if not path.is_file():
                raise PluginViewError(f"plugin view definition not found: {plugin}")
            definition = self._read_json(path)
            validate_plugin_view(definition, expected_plugin=plugin)
            return definition

    def save(self, plugin: str, definition: Any) -> dict[str, Any]:
        validate_plugin_view(definition, expected_plugin=plugin)
        with self.lock:
            target = self._path(plugin)
            target.parent.mkdir(parents=True, exist_ok=True)
            temporary = target.with_name(f".{target.name}.{uuid4().hex}.tmp")
            try:
                with temporary.open("w", encoding="utf-8", newline="\n") as output:
                    output.write(json.dumps(definition, ensure_ascii=False, indent=2) + "\n")
                    output.flush()
                    os.fsync(output.fileno())
                os.replace(temporary, target)
            finally:
                temporary.unlink(missing_ok=True)
            return self.load(plugin)

    def _path(self, plugin: str) -> Path:
        if not _PLUGIN_NAME.fullmatch(plugin):
            raise PluginViewError(f"invalid plugin name: {plugin}")
        if self.directory:
            return self.directory / f"{plugin}.json"
        return self.plugin_source_root / plugin / "result_view.json"

    @staticmethod
    def _read_json(path: Path) -> dict[str, Any]:
        try:
            value = json.loads(path.read_text(encoding="utf-8-sig"))
        except (OSError, json.JSONDecodeError) as exc:
            raise PluginViewError(f"cannot read plugin view definition: {path.name}") from exc
        if not isinstance(value, dict):
            raise PluginViewError(f"plugin view definition must be an object: {path.name}")
        return value


def validate_plugin_view(value: Any, expected_plugin: str | None = None) -> None:
    if not isinstance(value, dict):
        raise PluginViewError("plugin view definition must be an object")
    _only_keys(
        value,
        {"schema_version", "plugin", "title", "description", "primary_metric",
         "widgets", "empty_states"},
        "view",
    )
    _required(value, {"schema_version", "plugin", "title", "widgets"}, "view")
    if value["schema_version"] != SCHEMA_VERSION:
        raise PluginViewError(f"schema_version must be {SCHEMA_VERSION}")
    plugin = _short_string(value["plugin"], "view.plugin", 64)
    if not _PLUGIN_NAME.fullmatch(plugin):
        raise PluginViewError("view.plugin contains invalid characters")
    if expected_plugin is not None and plugin != expected_plugin:
        raise PluginViewError(f"view.plugin must be {expected_plugin}")
    _short_string(value["title"], "view.title", 120)
    if "description" in value:
        _short_string(value["description"], "view.description", 500, allow_empty=True)
    if "primary_metric" in value:
        _validate_metric(value["primary_metric"], "view.primary_metric")
    widgets = value["widgets"]
    if not isinstance(widgets, list) or len(widgets) > 12:
        raise PluginViewError("view.widgets must be an array with at most 12 items")
    widget_ids: set[str] = set()
    for index, widget in enumerate(widgets):
        _validate_widget(widget, f"view.widgets[{index}]")
        if widget["id"] in widget_ids:
            raise PluginViewError(f"duplicate widget id: {widget['id']}")
        widget_ids.add(widget["id"])
    if "empty_states" in value:
        states = value["empty_states"]
        if not isinstance(states, dict) or not set(states).issubset(_EMPTY_STATES):
            raise PluginViewError("view.empty_states contains an unsupported status")
        for key, text in states.items():
            _short_string(text, f"view.empty_states.{key}", 300)


def _validate_widget(value: Any, location: str) -> None:
    if not isinstance(value, dict):
        raise PluginViewError(f"{location} must be an object")
    _required(value, {"id", "type", "title"}, location)
    widget_id = _short_string(value["id"], f"{location}.id", 64)
    if not _PLUGIN_NAME.fullmatch(widget_id):
        raise PluginViewError(f"{location}.id contains invalid characters")
    widget_type = value["type"]
    if widget_type in {"metrics", "key_value"}:
        _only_keys(value, _COMMON_WIDGET_KEYS | {"items"}, location)
        _validate_items(value.get("items"), location, _validate_metric)
    elif widget_type == "progress":
        _only_keys(value, _COMMON_WIDGET_KEYS | {"items"}, location)
        _validate_items(value.get("items"), location, _validate_progress)
    elif widget_type == "data_table":
        _only_keys(value, _COMMON_WIDGET_KEYS | {"source", "columns", "empty_text"}, location)
        _value_path(value.get("source"), f"{location}.source")
        _validate_items(value.get("columns"), location, _validate_column, key="columns")
        if "empty_text" in value:
            _short_string(value["empty_text"], f"{location}.empty_text", 300)
    else:
        raise PluginViewError(f"{location}.type is not supported")
    _short_string(value["title"], f"{location}.title", 120)
    if "description" in value:
        _short_string(value["description"], f"{location}.description", 500, allow_empty=True)


def _validate_items(value: Any, location: str, validator, key: str = "items") -> None:
    if not isinstance(value, list) or not value or len(value) > 24:
        raise PluginViewError(f"{location}.{key} must contain between 1 and 24 items")
    for index, item in enumerate(value):
        validator(item, f"{location}.{key}[{index}]")


def _validate_metric(value: Any, location: str) -> None:
    if not isinstance(value, dict):
        raise PluginViewError(f"{location} must be an object")
    _only_keys(value, {"label", "value", "format", "suffix", "tone", "empty"}, location)
    _required(value, {"label", "value"}, location)
    _short_string(value["label"], f"{location}.label", 100)
    _value_path(value["value"], f"{location}.value")
    _optional_format_tone(value, location)
    for key in ("suffix", "empty"):
        if key in value:
            _short_string(value[key], f"{location}.{key}", 40, allow_empty=True)


def _validate_progress(value: Any, location: str) -> None:
    if not isinstance(value, dict):
        raise PluginViewError(f"{location} must be an object")
    _only_keys(value, {"label", "value", "total", "format", "tone"}, location)
    _required(value, {"label", "value", "total"}, location)
    _short_string(value["label"], f"{location}.label", 100)
    _value_path(value["value"], f"{location}.value")
    _value_path(value["total"], f"{location}.total")
    _optional_format_tone(value, location)


def _validate_column(value: Any, location: str) -> None:
    if not isinstance(value, dict):
        raise PluginViewError(f"{location} must be an object")
    _only_keys(value, {"label", "value", "format"}, location)
    _required(value, {"label", "value"}, location)
    _short_string(value["label"], f"{location}.label", 100)
    field = value["value"]
    if not isinstance(field, str) or not re.fullmatch(
            r"[A-Za-z_][A-Za-z0-9_]*(\.[A-Za-z_][A-Za-z0-9_]*)*", field):
        raise PluginViewError(f"{location}.value must be a row-relative data path")
    if "format" in value and value["format"] not in _FORMATS:
        raise PluginViewError(f"{location}.format is not supported")


def _optional_format_tone(value: dict[str, Any], location: str) -> None:
    if "format" in value and value["format"] not in _FORMATS:
        raise PluginViewError(f"{location}.format is not supported")
    if "tone" in value and value["tone"] not in _TONES:
        raise PluginViewError(f"{location}.tone is not supported")


def _value_path(value: Any, location: str) -> None:
    if not isinstance(value, str) or not _VALUE_PATH.fullmatch(value):
        raise PluginViewError(
            f"{location} must reference run, analysis, result_completeness or outcomes data"
        )


def _short_string(value: Any, location: str, maximum: int, allow_empty: bool = False) -> str:
    if not isinstance(value, str) or len(value) > maximum or (not allow_empty and not value.strip()):
        raise PluginViewError(f"{location} must be a string with at most {maximum} characters")
    return value


def _required(value: dict[str, Any], keys: set[str], location: str) -> None:
    missing = keys - set(value)
    if missing:
        raise PluginViewError(f"{location} is missing required field: {sorted(missing)[0]}")


def _only_keys(value: dict[str, Any], keys: set[str], location: str) -> None:
    unknown = set(value) - keys
    if unknown:
        raise PluginViewError(f"{location} contains unsupported field: {sorted(unknown)[0]}")
