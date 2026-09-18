from __future__ import annotations

import importlib.util
import json
import re
from pathlib import Path
from types import ModuleType
from typing import Any, Mapping, Protocol

from .views import PluginViewStore


class QueryConnection(Protocol):
    def execute(self, query: str, params: tuple[Any, ...] = ()) -> Any: ...


class ResultPluginError(RuntimeError):
    """A plugin-owned result query could not be completed."""


class ResultAdapter(Protocol):
    configured: bool

    def summary(self, connection: QueryConnection, run_id: str) -> dict[str, Any]: ...


class ResultPlugin(Protocol):
    name: str
    configured: bool

    def summary(self, connection: QueryConnection, run_id: str) -> dict[str, Any]: ...

    def view_definition(self) -> dict[str, Any]: ...

    def save_view_definition(self, value: Any) -> dict[str, Any]: ...


class ViewOnlyResultAdapter:
    configured = False

    def summary(self, connection: QueryConnection, run_id: str) -> dict[str, Any]:
        return {}


class PluginBundle:
    def __init__(self, name: str, adapter: ResultAdapter, view_store: PluginViewStore):
        self.name = name
        self.adapter = adapter
        self.view_store = view_store

    @property
    def configured(self) -> bool:
        return bool(self.adapter.configured)

    def summary(self, connection: QueryConnection, run_id: str) -> dict[str, Any]:
        try:
            value = self.adapter.summary(connection, run_id)
        except ResultPluginError:
            raise
        except Exception as exc:
            raise ResultPluginError(f"{self.name} result query failed") from exc
        if not isinstance(value, dict):
            raise ResultPluginError(f"{self.name} result adapter must return an object")
        return value

    def view_definition(self) -> dict[str, Any]:
        return self.view_store.load(self.name)

    def save_view_definition(self, value: Any) -> dict[str, Any]:
        return self.view_store.save(self.name, value)


class PluginAdapterStore:
    """Load trusted, packaged Python result adapters owned by business plugins."""

    def __init__(self, directory: str | Path | None = None):
        self.directory = Path(directory).resolve() if directory else None
        self.plugin_source_root = Path(__file__).resolve().parents[2] / "plugins"

    def load(self, plugin: str, options: Mapping[str, Any]) -> ResultAdapter:
        path = self._path(plugin)
        if not path.is_file():
            return ViewOnlyResultAdapter()
        module = self._load_module(plugin, path)
        factory = getattr(module, "create_result_adapter", None)
        if not callable(factory):
            raise ResultPluginError(
                f"plugin result adapter must define create_result_adapter: {plugin}"
            )
        adapter = factory(dict(options))
        if not isinstance(getattr(adapter, "configured", None), bool):
            raise ResultPluginError(f"plugin result adapter has invalid configured flag: {plugin}")
        if not callable(getattr(adapter, "summary", None)):
            raise ResultPluginError(f"plugin result adapter has no summary method: {plugin}")
        return adapter

    def _path(self, plugin: str) -> Path:
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_-]*", plugin):
            raise ResultPluginError(f"invalid plugin name: {plugin}")
        if self.directory:
            return self.directory / f"{plugin}.py"
        return self.plugin_source_root / plugin / "result_adapter.py"

    @staticmethod
    def _load_module(plugin: str, path: Path) -> ModuleType:
        module_name = f"testtracking_result_adapter_{plugin.replace('-', '_')}"
        spec = importlib.util.spec_from_file_location(module_name, path)
        if spec is None or spec.loader is None:
            raise ResultPluginError(f"cannot load plugin result adapter: {plugin}")
        module = importlib.util.module_from_spec(spec)
        try:
            spec.loader.exec_module(module)
        except Exception as exc:
            raise ResultPluginError(f"cannot load plugin result adapter: {plugin}") from exc
        return module


def _validate_packaged_resources(views: PluginViewStore, adapters: PluginAdapterStore,
                                 report_templates_dir: str | None) -> None:
    # Source discovery and external view-only plugins need no installation
    # registry. A built package declares the resources it actually ships so a
    # missing file cannot silently change that package's available analysis.
    if views.directory is None:
        return
    manifest = views.directory / "resources.manifest"
    if not manifest.exists():
        return
    try:
        definition = json.loads(manifest.read_text(encoding="utf-8-sig"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ResultPluginError(f"cannot read plugin resource manifest: {manifest}") from exc
    if (not isinstance(definition, dict) or definition.get("schema_version") != 1
            or not isinstance(definition.get("plugins"), dict)):
        raise ResultPluginError(f"invalid plugin resource manifest: {manifest}")
    for plugin, resources in definition["plugins"].items():
        if (not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_-]*", plugin)
                or not isinstance(resources, list) or "view" not in resources
                or any(item not in ("view", "adapter", "report_template") for item in resources)):
            raise ResultPluginError(f"invalid resource declaration for plugin {plugin}: {manifest}")
        paths = {
            "view": views._path(plugin),
            "adapter": adapters._path(plugin),
            "report_template": (Path(report_templates_dir) / f"{plugin}.py"
                                if report_templates_dir else
                                views.plugin_source_root / plugin / "result_report.py"),
        }
        for resource in resources:
            if not paths[resource].is_file():
                raise ResultPluginError(
                    f"plugin {plugin} is missing declared {resource}: {paths[resource]}"
                )


def default_plugins(plugin_options: Mapping[str, Mapping[str, Any]] | None = None,
                    views_dir: str | None = None,
                    adapters_dir: str | None = None,
                    report_templates_dir: str | None = None) -> dict[str, ResultPlugin]:
    views = PluginViewStore(views_dir)
    adapters = PluginAdapterStore(adapters_dir)
    _validate_packaged_resources(views, adapters, report_templates_dir)
    options = plugin_options or {}
    plugins = {
        name: PluginBundle(name, adapters.load(name, options.get(name, {})), views)
        for name in views.plugin_names()
    }
    if not plugins:
        raise ResultPluginError("no plugin result views were found")
    return plugins
