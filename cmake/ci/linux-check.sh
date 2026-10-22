#!/bin/sh
# SPDX-FileCopyrightText: 2026 Gauvain "GovanifY" Roussel-Tarbouriech <gauvain@govanify.com>
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

root='/opt/ekphore relocated'
"$root/ekphore-ui" --help
if [ ! -x /checks/ekphore-linux-smoke ]; then
    exit 0
fi

for format in clap lv2 vst3; do
    isolated=$(mktemp -d /tmp/ekphore-plugin.XXXXXX)
    case "$format" in
        clap) cp -a "$root/ekphore-clap" "$isolated/" ;;
        lv2) cp -a "$root/ekphore.lv2" "$isolated/" ;;
        vst3) cp -a "$root/ekphore.vst3" "$isolated/" ;;
    esac
    /checks/ekphore-linux-smoke "$isolated" "$format"
done
