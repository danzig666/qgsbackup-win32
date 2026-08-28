"""Executed by QGIS through --code after a project has loaded."""
import json
import sys

from qgis.core import QgsProject

report_path = sys.argv[-1]
layers = []
for layer in QgsProject.instance().mapLayers().values():
    layers.append({
        "name": layer.name(),
        "valid": layer.isValid(),
        "provider": layer.providerType(),
        "source": layer.source(),
    })

with open(report_path, "w", encoding="utf-8") as report:
    json.dump({"project": QgsProject.instance().fileName(), "layers": layers}, report, ensure_ascii=False, indent=2)
