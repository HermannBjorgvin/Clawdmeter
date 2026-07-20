#!/bin/bash
# Сборка calnext — EventKit-хелпера календарных напоминаний (см. calnext.swift).
# Встраивает calnext.plist в секцию __info_plist и подписывает ad-hoc: обе
# части обязательны, иначе macOS не покажет диалог доступа к календарю.
set -euo pipefail
cd "$(dirname "$0")"
swiftc calnext.swift -O -o calnext \
    -Xlinker -sectcreate -Xlinker __TEXT -Xlinker __info_plist -Xlinker calnext.plist
codesign --force --sign - calnext
echo "OK: $(pwd)/calnext"
