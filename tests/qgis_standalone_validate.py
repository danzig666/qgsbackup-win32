"""Headless integration check using the PyQGIS runtime bundled with QGIS."""
import json
import sys

from qgis.core import QgsApplication, QgsProject

project_path, report_path = sys.argv[1:3]
app = QgsApplication([], False)
app.initQgis()
project = QgsProject()
loaded = project.read(project_path)
layers = [
    {
        "name": layer.name(),
        "valid": layer.isValid(),
        "provider": layer.providerType(),
        "source": layer.source(),
    }
    for layer in project.mapLayers().values()
]
with open(report_path, "w", encoding="utf-8") as report:
    json.dump({"loaded": loaded, "project": project.fileName(), "layers": layers}, report, ensure_ascii=False, indent=2)
project.clear()
app.exitQgis()
raise SystemExit(0 if loaded else 2)
