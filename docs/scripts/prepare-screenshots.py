"""Prepare a separate portable IBStart profile with fictional documentation data."""

import argparse
import json
import shutil
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", required=True, type=Path, help="Current IBStart.exe")
    parser.add_argument("--output", required=True, type=Path, help="New demo directory")
    args = parser.parse_args()
    executable = args.exe.resolve(strict=True)
    output = args.output.resolve()
    # Refuse to overwrite an existing profile, including a previous capture session.
    output.mkdir(parents=True, exist_ok=False)
    profile = output / "data"
    profile.mkdir()
    shutil.copy2(executable, output / "IBStart.exe")
    (output / "IBStart.portable").touch()
    catalog = output / "demo-ibases.v8i"
    catalog.write_text(
        '[Демонстрационные базы]\nFolder=/\nOrderInTree=1\n\n'
        '[Бухгалтерия — учебная]\n'
        'Connect=File="C:\\Demo\\Bases\\Accounting";\n'
        'ID=ibstart-docs-accounting\nFolder=/Демонстрационные базы\n'
        'OrderInTree=1\nVersion=8.5.1\nApp=ThinClient\nAppArch=x86_64\n'
        'AdditionalParameters=/L ru /DisableStartupMessages\n\n'
        '[Управление торговлей — тестовая]\n'
        'Connect=Srvr="demo-server";Ref="trade-demo";\n'
        'ID=ibstart-docs-trade\nFolder=/Демонстрационные базы\nOrderInTree=2\n\n'
        '[Веб-клиент — пример]\nConnect=ws="https://example.com/demo";\n'
        'ID=ibstart-docs-web\nFolder=/Демонстрационные базы\nOrderInTree=3\n',
        encoding="utf-8-sig",
    )
    settings = {
        "active_ibases": str(catalog),
        "selected_entry": "Бухгалтерия — учебная",
        "window_x": 80, "window_y": 80,
        "window_width": 1180, "window_height": 720,
        "credentials_schema_version": 1,
        "credentials": [{
            "id": "ibstart-docs-account", "title": "Учебные базы",
            "user_name": "Демонстрация", "password": "",
            "scope": {"mode": "all", "targets": []},
        }],
    }
    state = {
        "favorites": [{"favorite": "ibstart-docs-accounting"}],
        "tags": [
            {"tag_id": "ibstart-docs-accounting", "values": ["Учебная"]},
            {"tag_id": "ibstart-docs-trade", "values": ["Тестовая"]},
        ],
    }
    for name, data in (("settings.json", settings), ("catalog-state.json", state)):
        (profile / name).write_text(json.dumps(data, ensure_ascii=False, indent=2), encoding="utf-8")
    print(output / "IBStart.exe")


if __name__ == "__main__":
    main()
