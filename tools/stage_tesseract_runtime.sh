#!/bin/sh
# Package the native OCR dependencies in a release. No Python at runtime or
# during staging. Linux uses the distribution's installed native libraries.
set -eu
release=$1
binary="$release/bin/samosa-ocr"
data=$("$binary" --data-path)
test -f "$data/eng.traineddata" || { echo "missing Tesseract English data: $data" >&2; exit 1; }
mkdir -p "$release/share/tessdata"
cp "$data/eng.traineddata" "$release/share/tessdata/eng.traineddata"
if [ "$(uname -s)" = Darwin ]; then
  mkdir -p "$release/lib/ocr"
  # A subshell gives each recursive invocation its own shell variables.
  bundle() (
    target=$1
    relative=$2
    original=${3:-$1}
    origin=${original%/*}
    for dependency in $(otool -L "$original" | tail -n +2 | awk '{print $1}'); do
      case "$dependency" in /usr/lib/*|/System/*) continue ;; esac
      [ "$dependency" != "$original" ] || continue
      name=${dependency##*/}
      resolved=$dependency
      case "$dependency" in
        @loader_path/*) resolved="$origin/${dependency#@loader_path/}" ;;
        @rpath/*)
          resolved="$origin/$name"
          if [ ! -f "$resolved" ]; then
            for rpath in $(otool -l "$original" | awk '$1=="cmd" && $2=="LC_RPATH" {r=1} r && $1=="path" {print $2; r=0}'); do
              case "$rpath" in @loader_path*) rpath="$origin${rpath#@loader_path}" ;; esac
              if [ -f "$rpath/$name" ]; then resolved="$rpath/$name"; break; fi
            done
          fi ;;
      esac
      test -f "$resolved" || { echo "unresolved OCR dependency: $dependency in $original" >&2; exit 1; }
      destination="$release/lib/ocr/$name"
      if [ ! -f "$destination" ]; then
        cp "$resolved" "$destination"
        chmod u+w "$destination"
        install_name_tool -id "@loader_path/$name" "$destination"
        bundle "$destination" '@loader_path' "$resolved"
      fi
      install_name_tool -change "$dependency" "$relative/$name" "$target"
    done
    codesign --force --sign - "$target" >/dev/null 2>&1
  )
  bundle "$binary" '@loader_path/../lib/ocr'
fi
"$binary" --check >/dev/null
