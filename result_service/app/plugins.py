from __future__ import annotations

import importlib.util
import hashlib
import json
import re
from pathlib import Path
from types import ModuleType
from typing import Any, Mapping, Protocol

from .views import PluginViewError, PluginViewStore, validate_plugin_view, view_revision


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


def versioned_plugin_view(plugin: ResultPlugin) -> dict[str, Any]:
    """Add API concurrency metadata without changing the plugin-owned schema."""
    with PluginViewStore.lock:
        definition = plugin.view_definition()
        return {**definition, "revision": view_revision(definition)}


def save_versioned_plugin_view(plugin: ResultPlugin, value: Any) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise PluginViewError("plugin view definition must be an object")
    definition = {key: item for key, item in value.items() if key != "revision"}
    validate_plugin_view(definition, expected_plugin=plugin.name)
    expected = value.get("revision")
    if not isinstance(expected, str) or not re.fullmatch(r"[0-9a-f]{64}", expected):
        raise PluginViewError("revision is required; reload the plugin view before saving")
    with PluginViewStore.lock:
        current = view_revision(plugin.view_definition())
        if expected != current:
            raise PluginViewError(
                "展示已被其他使用者修改。当前草稿已保留，请读取最新版本后合并修改。",
                409, current_revision=current,
            )
        saved = plugin.save_view_definition(definition)
        return {**saved, "revision": view_revision(saved)}


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


class PluginAIContextStore:
    """Load immutable, plugin-owned background material for DSH."""

    MAX_BYTES = 64 * 1024

    def __init__(self, directory: str | Path | None = None):
        self.directory = Path(directory).resolve() if directory else None
        self.plugin_source_root = Path(__file__).resolve().parents[2] / "plugins"

    def _path(self, plugin: str) -> Path:
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_-]*", plugin):
            raise ResultPluginError(f"invalid plugin name: {plugin}")
        if self.directory:
            return self.directory / f"{plugin}.md"
        return self.plugin_source_root / plugin / "AI_CONTEXT.md"

    def load(self, plugin: str) -> dict[str, Any]:
        path = self._path(plugin)
        try:
            raw = path.read_bytes()
        except OSError as exc:
            raise ResultPluginError(f"plugin AI context not found: {plugin}") from exc
        if not raw or len(raw) > self.MAX_BYTES:
            raise ResultPluginError(f"plugin AI context has invalid size: {plugin}")
        try:
            content = raw.decode("utf-8-sig")
        except UnicodeDecodeError as exc:
            raise ResultPluginError(f"plugin AI context must be UTF-8: {plugin}") from exc
        if not content.strip():
            raise ResultPluginError(f"plugin AI context is empty: {plugin}")
        return {
            "plugin": plugin,
            "content": content,
            "sha256": hashlib.sha256(raw).hexdigest(),
        }


def _validate_packaged_resources(views: PluginViewStore, adapters: PluginAdapterStore,
                                 contexts: PluginAIContextStore,
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
                or any(item not in ("view", "adapter", "report_template", "ai_context")
                       for item in resources)):
            raise ResultPluginError(f"invalid resource declaration for plugin {plugin}: {manifest}")
        paths = {
            "view": views._path(plugin),
            "adapter": adapters._path(plugin),
            "report_template": (Path(report_templates_dir) / f"{plugin}.py"
                                if report_templates_dir else
                                views.plugin_source_root / plugin / "result_report.py"),
            "ai_context": contexts._path(plugin),
        }
        for resource in resources:
            if not paths[resource].is_file():
                raise ResultPluginError(
                    f"plugin {plugin} is missing declared {resource}: {paths[resource]}"
                )


def default_plugins(plugin_options: Mapping[str, Mapping[str, Any]] | None = None,
                    views_dir: str | None = None,
                    adapters_dir: str | None = None,
                    report_templates_dir: str | None = None,
                    ai_contexts_dir: str | None = None) -> dict[str, ResultPlugin]:
    views = PluginViewStore(views_dir)
    adapters = PluginAdapterStore(adapters_dir)
    contexts = PluginAIContextStore(ai_contexts_dir)
    _validate_packaged_resources(views, adapters, contexts, report_templates_dir)
    options = plugin_options or {}
    plugins = {
        name: PluginBundle(name, adapters.load(name, options.get(name, {})), views)
        for name in views.plugin_names()
    }
    if not plugins:
        raise ResultPluginError("no plugin result views were found")
    return plugins
